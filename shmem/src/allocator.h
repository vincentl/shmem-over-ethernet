/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The symmetric heap allocator (§4.4, invariant 6).
 *
 * A pure, deterministic function of its call sequence: first fit over an
 * address-ordered block list, splitting on allocation and coalescing with both
 * neighbours on free. It consults nothing that varies across PEs - no rank, no
 * address, no time - so every PE issuing the same sequence of calls holds
 * bit-identical state and returns the same offsets. Metadata lives in private
 * memory, never in the heap, so there is no cross-process lock.
 *
 * Offsets are from the heap's start; every block starts at a multiple of
 * OSOE_ALLOCATOR_ALIGN, which suits any object type (spec §9.3.1).
 *
 * **Hints are recorded, not discarded** (§4.4). spec §9.3.5's hints are
 * advisory and change nothing on one node, but §9.3.3 requires shmem_realloc to
 * *preserve* an allocation's hints and to return a null pointer rather than
 * lose them, so the hint has to have been kept. A hinted allocation is placed
 * from the **high end** of the heap and an unhinted one from the low end, which
 * is what makes "the hints cannot be preserved" a case that exists: a hinted
 * block that cannot be grown at the hinted end fails the call instead of
 * silently migrating. Both ends are a pure function of the call sequence, so
 * determinism is untouched.
 */
#ifndef OSOE_ALLOCATOR_H
#define OSOE_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSOE_ALLOCATOR_ALIGN UINT64_C(16)

_Static_assert(OSOE_ALLOCATOR_ALIGN >= _Alignof(max_align_t),
               "heap blocks must be aligned for any object type");

struct osoe_allocator_block {
    uint64_t offset;
    uint64_t size;
    long hints;                         /* spec §9.3.5; 0 on a free block */
    bool free;
};

struct osoe_allocator {
    uint64_t capacity;
    struct osoe_allocator_block *blocks;    /* ordered by offset, contiguous */
    size_t count;
    size_t room;
};

/* 0, or -1 if private memory for the metadata ran out. */
int osoe_allocator_init(struct osoe_allocator *a, uint64_t capacity);
void osoe_allocator_fini(struct osoe_allocator *a);

/*
 * Allocate size > 0 bytes at a multiple of `align`, which must be a power of
 * two; anything below OSOE_ALLOCATOR_ALIGN is raised to it. `hints` is spec
 * §9.3.5's bit array, recorded with the block and deciding which end of the
 * heap it comes from.
 *
 * 0 and *offset set; 1 if no free block fits; -1 if private memory for the
 * metadata ran out.
 */
int osoe_allocator_take(struct osoe_allocator *a, uint64_t size, uint64_t align, long hints,
                    uint64_t *offset);

/* Free the block starting at offset. 0, or -1 if no allocated block starts
 * there. */
int osoe_allocator_give(struct osoe_allocator *a, uint64_t offset);

/* The size and hints of the allocated block starting at offset. 0, or -1 if no
 * allocated block starts there. `size` is the rounded size the allocator
 * holds, which is at least what was asked for. */
int osoe_allocator_query(const struct osoe_allocator *a, uint64_t offset, uint64_t *size,
                     long *hints);

/*
 * Resize the block at `offset` without moving it: shrink always works, grow
 * only into a free neighbour above. 0 on success, 1 if it cannot be done
 * without moving, -1 if no allocated block starts there or the metadata could
 * not grow. The block is unchanged unless 0 is returned.
 *
 * This exists so that shmem_realloc's common case - a block grown with nothing
 * live above it - does not need both the old and the new block to fit in the
 * heap at once.
 */
int osoe_allocator_resize(struct osoe_allocator *a, uint64_t offset, uint64_t size);

/* Bytes currently allocated, and the largest single block still available -
 * the two numbers that distinguish "the heap is full" from "the heap is
 * fragmented" (§3.14 question 4). O(blocks), so call it once per allocation,
 * never in a loop. */
void osoe_allocator_levels(const struct osoe_allocator *a, uint64_t *in_use, uint64_t *largest_free);

/* The bytes from `offset` to the end of the allocated block that holds it:
 * 0, or -1 if no allocated block holds it (phase 1 step 8 slice 2: how a
 * gather names a dest too short for its result before writing past it). */
int osoe_allocator_room(const struct osoe_allocator *a, uint64_t offset, uint64_t *room);

#endif /* OSOE_ALLOCATOR_H */
