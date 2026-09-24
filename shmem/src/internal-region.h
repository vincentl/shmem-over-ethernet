/*
 * OSOE_REGION_INTERNAL (§5.8; phase 1 step 5): library scratch and collective
 * control words, "never visible to the application" (transport.h).
 *
 * It is the tail of every PE's heap mapping: the heap object grows past the
 * page-rounded SHMEM_SYMMETRIC_SIZE by this region, so it needs no mapping, no
 * name in /dev/shm and no teardown of its own, and a backend that registers
 * the heap registers it too. The allocator is given the heap's size and never
 * reaches it, and every API check bounds an address by that size, so neither
 * shmem_ptr nor any RMA, AMO or wait names it. A plain store past the end of
 * the heap still can, which is why the slots come first (below).
 *
 * Its layout is the collectives' (collectives.c): fan-out + 1 slots, one for
 * each child's partial result and one for what the parent sends down, then a
 * page of control words - per team, two eight-byte words, each on its own
 * cache line, counting the internode phase's pieces arrived and released.
 * Eight bytes because stock RDMA verbs have no four-byte atomics, so a
 * four-byte word would put every arrival of a RoCE backend on its slow path.
 * The slots first because a program's store that runs off the end of its heap
 * then lands in scratch that means something only while a collective has it,
 * rather than in a count the next collective would wait on forever. Its size is
 * fixed by the fan-out, which every node agrees on (§5.3), and by the slot,
 * which is a constant rather than a setting for the same reason: two nodes that
 * disagreed about either would put into each other's slots at different
 * offsets.
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
