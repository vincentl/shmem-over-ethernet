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
 * the heap still can (below).
 *
 * Its layout is the generic internode phase's (internode-generic.c), which is
 * its only user, and since phase 1 step 7 it is **a block per team number**, so
 * that two teams' collectives are disjoint in memory by construction, whatever
 * their trees share - two teams whose local leads coincide on a node put into
 * different slots and count on different words. First the control words: per
 * team number, two eight-byte words, each on its own cache line, counting the
 * internode phase's pieces arrived and released; then, per team number,
 * `fanout + 1` slots of `slots_per_child` pieces each - one for each child's
 * partial result and one for what the parent sends down. Eight bytes because
 * stock RDMA verbs have no four-byte atomics, so a four-byte word would put
 * every arrival of a RoCE backend on its slow path.
 *
 * The words come first (phase 1 step 7's brief), and so a program's store that
 * runs off the end of its heap meets the world team's arrival word, which the
 * next collective would wait on forever; until step 7 the slots came first,
 * and such a store met scratch that means something only while a collective
 * has it. The order is internode-generic.c's offsets and this file's sizes,
 * and nothing else depends on it.
 *
 * Every PE's heap carries one, and a team touches only its own words' lines
 * and slots, and only on the local leads of its nodes; the pages nobody
 * writes cost address space and the /dev/shm capacity check's share, not
 * memory - about 2.5 MiB a PE at the default 32 teams and a fan-out of 4. Its
 * size is fixed by the fan-out and OSOE_MAX_TEAMS, which every node agrees on
 * (§5.3, §5.7), and by the slot and the words area's rounding, which are
 * constants rather than settings for the same reason: two nodes that
 * disagreed about any of them would put into each other's slots at different
 * offsets.
 */
#ifndef OSOE_INTERNAL_REGION_H
#define OSOE_INTERNAL_REGION_H

#include <stdint.h>

#include "base/cacheline.h"

enum {
    OSOE_INTERNAL_SLOT_BYTES = 16 << 10,        /* one piece of a data collective */
    /* How many pieces each child's slot holds, and the parent's: one until
     * a data collective's pieces are pipelined up and down the tree. */
    OSOE_INTERNAL_SLOTS_PER_CHILD = 1,
    /* The words area rounds up to this: a constant and not the running
     * kernel's page size, which can differ between nodes (4 or 16 KiB on
     * aarch64) that must all compute the same offsets. */
    OSOE_INTERNAL_WORDS_ALIGN = 4096,
};

/* A team number's arrival word and release word, a cache line each. */
#define OSOE_INTERNAL_TEAM_WORDS_BYTES (2u * OSOE_CACHELINE)

/* The words area, every team number's words, rounded up. */
static inline uint64_t osoe_internal_words_bytes(uint32_t max_teams)
{
    uint64_t words = (uint64_t)max_teams * OSOE_INTERNAL_TEAM_WORDS_BYTES;
    return (words + OSOE_INTERNAL_WORDS_ALIGN - 1) / OSOE_INTERNAL_WORDS_ALIGN *
           OSOE_INTERNAL_WORDS_ALIGN;
}

/* One team number's block of slots, for a tree of fan-out `fanout`. */
static inline uint64_t osoe_internal_team_slots_bytes(uint32_t fanout)
{
    return (uint64_t)(fanout + 1) * OSOE_INTERNAL_SLOTS_PER_CHILD * OSOE_INTERNAL_SLOT_BYTES;
}

/* The region's size for `max_teams` team numbers and a tree of fan-out
 * `fanout`, before page rounding. */
static inline uint64_t osoe_internal_region_bytes(uint32_t fanout, uint32_t max_teams)
{
    return osoe_internal_words_bytes(max_teams) +
           (uint64_t)max_teams * osoe_internal_team_slots_bytes(fanout);
}

#endif /* OSOE_INTERNAL_REGION_H */
