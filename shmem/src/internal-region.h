/*
 * OSOE_REGION_INTERNAL (§5.8; phase 1 step 5): library scratch and collective
 * control words, "never visible to the application" (transport.h).
 *
 * It is the tail of every PE's heap mapping: the heap object grows past the
 * page-rounded SHMEM_SYMMETRIC_SIZE by this region, so it needs no mapping, no
 * name in /dev/shm and no teardown of its own, and a backend that registers
 * the heap registers it too. The allocator is given the heap's size and never
 * reaches it, and every API check bounds an address by that size, so neither
 * shmem_ptr nor a stray application pointer can name it.
 *
 * Its layout is the collectives' (collectives.c): a page of control words -
 * per team, a cache line each for the counts that its internode phase's pieces
 * have arrived and been released - then fan-out + 1 slots, one for each child's
 * partial result and one for what the parent sends down. Its size is fixed by
 * the fan-out, which every node agrees on (§5.3), and by the slot, which is a
 * constant rather than a setting for the same reason: two nodes that disagreed
 * about either would put into each other's slots at different offsets.
 */
#ifndef OSOE_INTERNAL_REGION_H
#define OSOE_INTERNAL_REGION_H

#include <stdint.h>

enum {
    OSOE_INTERNAL_WORDS_BYTES = 4096,           /* the control words' page */
    OSOE_INTERNAL_SLOT_BYTES = 16 << 10,        /* one piece of a data collective */
};

/* The region's size for a tree of fan-out `fanout`, before page rounding. */
static inline uint64_t osoe_internal_region_bytes(uint32_t fanout)
{
    return OSOE_INTERNAL_WORDS_BYTES + (uint64_t)(fanout + 1) * OSOE_INTERNAL_SLOT_BYTES;
}

#endif /* OSOE_INTERNAL_REGION_H */
