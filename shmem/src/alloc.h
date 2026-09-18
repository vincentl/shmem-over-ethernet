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
 * OSOE_ALLOC_ALIGN, which suits any object type (spec §9.3.1).
 *
 * Hinted allocations (spec §9.3.5) will grow from the opposite end of the heap
 * (§4.10), which this structure allows without change.
 */
#ifndef OSOE_ALLOC_H
#define OSOE_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSOE_ALLOC_ALIGN UINT64_C(16)

_Static_assert(OSOE_ALLOC_ALIGN >= _Alignof(max_align_t),
               "heap blocks must be aligned for any object type");

struct osoe_alloc_block {
    uint64_t offset;
    uint64_t size;
    bool free;
};

struct osoe_alloc {
    uint64_t capacity;
    struct osoe_alloc_block *blocks;    /* ordered by offset, contiguous */
    size_t count;
    size_t room;
};

/* 0, or -1 if private memory for the metadata ran out. */
int osoe_alloc_init(struct osoe_alloc *a, uint64_t capacity);
void osoe_alloc_fini(struct osoe_alloc *a);

/* Allocate size > 0 bytes. 0 and *offset set; 1 if no free block fits;
 * -1 if private memory for the metadata ran out. */
int osoe_alloc_take(struct osoe_alloc *a, uint64_t size, uint64_t *offset);

/* Free the block starting at offset. 0, or -1 if no allocated block starts
 * there. */
int osoe_alloc_give(struct osoe_alloc *a, uint64_t offset);

/* Bytes currently allocated, and the largest single block still available -
 * the two numbers that distinguish "the heap is full" from "the heap is
 * fragmented" (§3.14 question 4). O(blocks), so call it once per allocation,
 * never in a loop. */
void osoe_alloc_levels(const struct osoe_alloc *a, uint64_t *in_use, uint64_t *largest_free);

#endif /* OSOE_ALLOC_H */
