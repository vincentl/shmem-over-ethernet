/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Symmetric memory management (spec §9.3; design §4.4).
 *
 * Every one of these is a collective operation on the world team, and what
 * distinguishes them is which barriers they take and when:
 *
 *   malloc, calloc, align, malloc_with_hints   barrier on exit
 *   free                                       barrier on entry
 *   realloc                                    barrier on entry *and* exit
 *
 * realloc takes both because it is free and malloc in one call: it releases a
 * block no PE may still be reading, which is the entry barrier, and publishes
 * one every PE must see before any returns, which is the exit barrier. spec
 * §9.3.3 permits both and this needs both. Its degenerate forms take only the
 * barrier they really are - a null ptr is shmem_malloc, a zero size is
 * shmem_free.
 *
 * Nothing takes a barrier when it does nothing: a zero size, a zero count, or a
 * null pointer to free returns at once. Every PE makes the same call with the
 * same arguments (the spec requires it), so none is left waiting. A failed
 * allocation is not "nothing": every PE fails identically, so the exit barrier
 * still runs and all of them return a null pointer together.
 */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pshmem.h>
#include <shmem.h>

#include "allocator.h"
#include "barrier.h"
#include "export.h"
#include "fatal.h"
#include "metrics.h"
#include "state.h"
#include "symmetric.h"
#include "transport.h"

static void *heap_at(uint64_t offset)
{
    return (char *)osoe.node.heap_of[osoe.bootstrap.local_id] + offset;
}

/*
 * Tell the transport what a hinted block is for (§4.4, §4.10). Spec §9.3.5's
 * hints are the only thing that says which bytes may carry remote atomics or
 * signals, and a backend that must prepare memory before the network can touch
 * it has no other source for it.
 *
 * Not a collective and it does not block: every PE reaches this with identical
 * arguments because the allocator is a pure function of the call sequence
 * (§4.4, invariant 6), not because anything coordinated. Unhinted blocks say
 * nothing, which is what an unhinted allocation means.
 *
 * Bits this build does not recognize are carried in the hints but name no
 * attribute, since there is nothing to tell a backend about them.
 */
static void declare_range(uint64_t offset, uint64_t size, long hints)
{
    unsigned attrs = 0;
    if (hints & SHMEM_MALLOC_ATOMICS_REMOTE)
        attrs |= OSOE_TRANSPORT_RANGE_ATOMICS;
    if (hints & SHMEM_MALLOC_SIGNAL_REMOTE)
        attrs |= OSOE_TRANSPORT_RANGE_SIGNAL;
    if (attrs)
        osoe_transport_set_range(OSOE_REGION_HEAP, offset, size, attrs);
}

/*
 * The body every allocating routine shares: take a block, publish the heap
 * levels, barrier on exit. `zero` is shmem_calloc's initialization, done before
 * the barrier so the memory is zero everywhere by the time any PE returns.
 */
static void *allocate(const char *routine, uint64_t size, uint64_t align, long hints,
                      bool zero)
{
    osoe_require_initialized(routine);
    osoe_count(OSOE_C_SHMEM_MALLOC_CALLS, 1);
    void *p = NULL;
    uint64_t offset;
    switch (osoe_allocator_take(&osoe.allocator, size, align, hints, &offset)) {
    case 0:
        p = heap_at(offset);
        declare_range(offset, size, hints);
        if (zero)
            memset(p, 0, size);
        break;
    case 1:
        break;                          /* no block fits: every PE returns NULL */
    default:
        /* Failing on some PEs and not others would desynchronize the heaps. */
        osoe_fatal("%s: out of private memory for heap metadata", routine);
    }
    osoe_publish_heap_levels();
    osoe_barrier_all(routine);
    return p;
}

OSOE_API void *pshmem_malloc(size_t size)
{
    if (size == 0)
        return NULL;
    return allocate("shmem_malloc", size, OSOE_ALLOCATOR_ALIGN, 0, false);
}

/*
 * spec §9.3.5's hints are advisory: they change nothing on one node, where
 * every PE's memory is already load/store accessible from every other. They are
 * recorded rather than dropped because §9.3.3 makes shmem_realloc preserve
 * them, and because §4.10's set_range will want them (§4.4). Bits this build
 * does not recognize are kept as given - a hint is a statement about how the
 * program will use the memory, and this library's not knowing a particular one
 * does not make it untrue.
 */
OSOE_API void *pshmem_malloc_with_hints(size_t size, long hints)
{
    if (size == 0)
        return NULL;
    return allocate("shmem_malloc_with_hints", size, OSOE_ALLOCATOR_ALIGN, hints, false);
}

/*
 * spec §9.3.6: a count or size of 0 returns NULL without a barrier, and
 * count * size overflowing is a failed allocation - which does barrier, since
 * every PE overflows identically.
 */
OSOE_API void *pshmem_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0)
        return NULL;
    if (size > SIZE_MAX / count)
        return allocate("shmem_calloc", UINT64_MAX, OSOE_ALLOCATOR_ALIGN, 0, false);
    return allocate("shmem_calloc", (uint64_t)count * size, OSOE_ALLOCATOR_ALIGN, 0, true);
}

/*
 * spec §9.3.4: `alignment` shall be a power of two and a multiple of
 * sizeof(void *), and anything else is undefined. Undefined is where a library
 * gets to choose, and the choice here is the project's usual one - say what is
 * wrong and stop - because the alternative is a symmetric object that is not
 * where the program thinks it is on some PEs and not others.
 */
OSOE_API void *pshmem_align(size_t alignment, size_t size)
{
    if (size == 0)
        return NULL;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        alignment % sizeof(void *) != 0)
        osoe_fatal("shmem_align: alignment %zu is not a power of two that is a multiple "
                   "of sizeof(void *) (%zu)", alignment, sizeof(void *));
    /*
     * The allocator aligns heap *offsets*, and the caller was promised an
     * aligned *address*. The two agree only while this PE's heap base is itself
     * a multiple of the alignment: the node's B, which every candidate base is
     * for anything up to 4 GiB (§4.1), plus - under §8.2's heap-base shift -
     * this PE's rank times OSOE_HEAP_SHIFT. Beyond that, say the allocation
     * failed rather than return an address that is not what was asked for; a
     * failure is a return value the spec defines. It is decided from B and the
     * shift, which every PE shares, and not from this PE's own base, which the
     * shift makes different on every PE: read from that, a 4 MiB alignment
     * under a 2 MiB shift failed on the odd ranks and succeeded on the even.
     */
    uint64_t base = osoe.node.hdr->heap_base, shift = osoe.bootstrap.heap_shift;
    if (base % alignment != 0 || shift % alignment != 0)
        return allocate("shmem_align", UINT64_MAX, OSOE_ALLOCATOR_ALIGN, 0, false);
    return allocate("shmem_align", size, alignment, 0, false);
}

/* NULL does nothing, with no barrier. Otherwise the barrier is on entry, so no
 * PE frees memory another PE is still using in a collective sense. */
OSOE_API void pshmem_free(void *ptr)
{
    if (!ptr)
        return;
    osoe_require_initialized("shmem_free");
    osoe_count(OSOE_C_SHMEM_FREE_CALLS, 1);
    osoe_barrier_all("shmem_free");
    uint64_t offset;
    if (!osoe_symmetric_heap_offset(ptr, 0, &offset) || osoe_allocator_give(&osoe.allocator, offset) != 0)
        osoe_fatal("shmem_free: %p is not the address of a block allocated from the "
                   "symmetric heap", ptr);
    osoe_publish_heap_levels();
}

/*
 * spec §9.3.3. The hints of the old block are carried to the new one, and if a
 * block carrying them cannot be found the call fails with the old block
 * untouched - "if the space cannot be allocated or if hints cannot be
 * preserved". Those are one case here rather than two, because a hinted
 * allocation is placed from the hinted end of the heap (§4.4): running out of
 * room at that end *is* failing to preserve the hint, and the alternative -
 * quietly placing it at the other end - would lose the property the program
 * asked for without saying so.
 *
 * Resizing in place is tried first, and not as an optimization: growing a block
 * by allocating a second one and copying needs both to fit in the heap at once,
 * so a program that grows one large array would fail at half the heap rather
 * than at all of it.
 */
OSOE_API void *pshmem_realloc(void *ptr, size_t size)
{
    osoe_require_initialized("shmem_realloc");
    osoe_count(OSOE_C_SHMEM_REALLOC_CALLS, 1);
    if (!ptr)
        return pshmem_malloc(size);      /* shmem_malloc: exit barrier only */
    if (size == 0) {
        pshmem_free(ptr);                /* shmem_free: entry barrier only */
        return NULL;
    }

    osoe_barrier_all("shmem_realloc");  /* entry: nobody is still reading it */

    uint64_t offset, old_size;
    long hints;
    if (!osoe_symmetric_heap_offset(ptr, 0, &offset) ||
        osoe_allocator_query(&osoe.allocator, offset, &old_size, &hints) != 0)
        osoe_fatal("shmem_realloc: %p is not the address of a block allocated from the "
                   "symmetric heap", ptr);

    void *result = ptr;
    switch (osoe_allocator_resize(&osoe.allocator, offset, size)) {
    case 0:
        /* Same address, contents intact - but possibly more bytes than were
         * declared before, and §4.10 takes the union, so say it again. */
        declare_range(offset, size, hints);
        break;
    case 1: {
        uint64_t moved;
        switch (osoe_allocator_take(&osoe.allocator, size, OSOE_ALLOCATOR_ALIGN, hints, &moved)) {
        case 0:
            memcpy(heap_at(moved), ptr, old_size < size ? old_size : size);
            if (osoe_allocator_give(&osoe.allocator, offset) != 0)
                osoe_fatal("shmem_realloc: the block at %p vanished", ptr);
            declare_range(moved, size, hints);   /* spec §9.3.3 preserves hints */
            result = heap_at(moved);
            break;
        case 1:
            result = NULL;              /* old block unchanged (spec §9.3.3) */
            break;
        default:
            osoe_fatal("shmem_realloc: out of private memory for heap metadata");
        }
        break;
    }
    default:
        osoe_fatal("shmem_realloc: out of private memory for heap metadata");
    }

    osoe_publish_heap_levels();
    osoe_barrier_all("shmem_realloc");  /* exit: every PE sees the new block */
    return result;
}

/* Per PE, not per thread: a level cannot be summed across threads, and under
 * SHMEM_THREAD_SERIALIZED whichever thread is inside the allocator is the only
 * writer (§3.14). */
void osoe_publish_heap_levels(void)
{
    uint64_t in_use, largest_free;
    osoe_allocator_levels(&osoe.allocator, &in_use, &largest_free);

    struct osoe_pe_slot *me = &osoe.node.slots[osoe.bootstrap.local_id];
    atomic_store_explicit(&me->heap_in_use, in_use, memory_order_release);
    atomic_store_explicit(&me->heap_largest_free, largest_free, memory_order_release);
    if (in_use > atomic_load_explicit(&me->heap_high_water, memory_order_acquire))
        atomic_store_explicit(&me->heap_high_water, in_use, memory_order_release);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(malloc)
OSOE_ALIAS(malloc_with_hints)
OSOE_ALIAS(calloc)
OSOE_ALIAS(align)
OSOE_ALIAS(free)
OSOE_ALIAS(realloc)
