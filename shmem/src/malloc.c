/* shmem_malloc and shmem_free (spec §9.3.1, §9.3.2; design §4.4). */
#include <stddef.h>

#include <shmem.h>

#include "alloc.h"
#include "barrier.h"
#include "export.h"
#include "fatal.h"
#include "state.h"
#include "sym.h"

/* Size 0 returns NULL with no barrier: every PE makes the same call, so none
 * waits. Otherwise the barrier is on exit, after the allocation, so memory
 * returned on one PE is usable on all as soon as it returns - even when the
 * result is NULL, since every PE fails identically. */
OSOE_API void *shmem_malloc(size_t size)
{
    if (size == 0)
        return NULL;
    void *p = NULL;
    uint64_t offset;
    switch (osoe_alloc_take(&osoe.alloc, size, &offset)) {
    case 0:
        p = (char *)osoe.node.heap_of[osoe.boot.local_id] + offset;
        break;
    case 1:
        break;
    default:
        /* Failing on some PEs and not others would desynchronize the heaps. */
        osoe_fatal("shmem_malloc: out of private memory for heap metadata");
    }
    osoe_barrier_all("shmem_malloc");
    return p;
}

/* NULL does nothing, with no barrier. Otherwise the barrier is on entry, so no
 * PE frees memory another PE is still using in a collective sense. */
OSOE_API void shmem_free(void *ptr)
{
    if (!ptr)
        return;
    osoe_barrier_all("shmem_free");
    uint64_t offset;
    if (!osoe_sym_heap_offset(ptr, 0, &offset) || osoe_alloc_give(&osoe.alloc, offset) != 0)
        osoe_fatal("shmem_free: %p is not the address of a block allocated from the "
                   "symmetric heap", ptr);
}
