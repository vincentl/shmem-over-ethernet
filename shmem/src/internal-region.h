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
 * Its layout is the generic internode phase's (internode-generic.c), which is
 * its only user, and since phase 1 step 7 it is **a block per team number**, so
 * that two teams' collectives are disjoint in memory by construction, whatever
 * their trees share - two teams whose leads coincide on a node put into
 * different slots and count on different words. First the slots: per team
 * number, `fanout + 1` rows of D, D being OSOE_COLLECTIVE_PIPELINE, the pieces
 * each lead keeps in flight (§5.8, "Pipelined pieces"; phase 1 step 5 slice
 * 5) - a row for each child position's partial results and one for what the
 * parent sends down, piece k in the row's slot k mod D. Then the control words:
 * per team number, an arrival counter for each child position and then a
 * release counter, eight bytes each, each on a line of its own - one counter
 * per position and not one per node, since with pieces in flight a fast
 * child's second arrival must not stand in for a slow child's first. Eight
 * bytes because stock RDMA verbs have no four-byte atomics, so a four-byte word
 * would put every arrival of a RoCE backend on its slow path. The slots first
 * because a program's store that runs off the end of its heap then lands in
 * scratch that means something only while a collective has it, rather than in
 * a count the next collective would wait on forever.
 *
 * Then one more line: **the count of team numbers claimed** (§5.7; phase 1
 * step 7 slice 2), an eight-byte word at the line's start. Every PE's region
 * has the line and only world PE 0's is used: a split's parent rank 0 claims
 * the numbers its call creates by compare-and-swap there (team.c), so numbers
 * are job-wide and never reused within a session. It is not a team number's
 * word, and no collective counts on it.
 *
 * Last, **the allgather's words** (§5.8, "An allgather for fcollect"; phase 1
 * step 5 slice 6): per team number, a READY counter for each of the gather's
 * steps and then a DATA counter for each, eight bytes each and each alone on
 * its line, as the tree's are. READY[s] at a lead counts the entries of the
 * lead it writes to at step s, DATA[s] the steps s of the lead that writes to
 * it (internode-generic.c). A team over N nodes takes ceil(log2 N) steps, so
 * every team number has room for ceil(log2 N) steps of the job's N nodes,
 * which every node agrees on - the one part of the layout the node count
 * sizes, by its logarithm. They come after the counter's line so that nothing
 * before them moved when they were added.
 *
 * Every PE's heap carries one, and a team touches only its own slots and its
 * words' lines, and only on the leads of its nodes; the pages nobody writes
 * cost address space and the /dev/shm capacity check's share, not memory -
 * about 5 MiB a PE at the default 32 teams, a fan-out of 4 and D = 2, half of
 * that at D = 1; the allgather's words add 2.5 KiB a team number at a thousand
 * nodes. **Every node computes the same offsets**: the layout is fixed by the
 * fan-out, OSOE_MAX_TEAMS, D and the job's node count, which every node agrees
 * on (§5.3, §5.7, §5.8, §6.2), and by the slot and the words' line, which are
 * constants rather than settings, and constants rather than the compiler's
 * cache line - 128 bytes on aarch64 and 64 on x86-64 - so that nodes of
 * different architectures in one job agree too. Two nodes that disagreed about
 * any of them would put into each other's slots at different offsets.
 */
#ifndef OSOE_INTERNAL_REGION_H
#define OSOE_INTERNAL_REGION_H

#include <stdint.h>

enum {
    OSOE_INTERNAL_SLOT_BYTES = 16 << 10,        /* one piece of a data collective */
    /*
     * The line each control word sits alone on: 128 bytes whatever the
     * compiler's cache line (base/cacheline.h), since the offsets must be
     * every node's alike, and 128 separates the words on every core osoe runs
     * on - the Apple cores the development VM runs on have 128-byte lines, a
     * Pi's Cortex-A72 and A76 and x86-64 have 64.
     */
    OSOE_INTERNAL_LINE_BYTES = 128,
};

/* One team number's block of slots, for a tree of fan-out `fanout` whose leads
 * keep `pipeline` pieces in flight: a row of `pipeline` per child position,
 * then the row the parent's releases land in. */
static inline uint64_t osoe_internal_team_slots_bytes(uint32_t fanout, uint32_t pipeline)
{
    return (uint64_t)(fanout + 1) * pipeline * OSOE_INTERNAL_SLOT_BYTES;
}

/* One team number's words: an arrival counter per child position, then the
 * release counter, each alone on its line. */
static inline uint64_t osoe_internal_team_words_bytes(uint32_t fanout)
{
    return (uint64_t)(fanout + 1) * OSOE_INTERNAL_LINE_BYTES;
}

/* Where the words start: past every team number's slots. */
static inline uint64_t osoe_internal_words_offset(uint32_t fanout, uint32_t pipeline,
                                                  uint32_t max_teams)
{
    return (uint64_t)max_teams * osoe_internal_team_slots_bytes(fanout, pipeline);
}

/* Where the count of team numbers claimed is: the line past every team
 * number's words. */
static inline uint64_t osoe_internal_team_counter_offset(uint32_t fanout, uint32_t pipeline,
                                                         uint32_t max_teams)
{
    return osoe_internal_words_offset(fanout, pipeline, max_teams) +
           (uint64_t)max_teams * osoe_internal_team_words_bytes(fanout);
}

/* The allgather's steps over `nodes` nodes: ceil(log2 nodes), 0 for one. */
static inline uint32_t osoe_internal_gather_steps(uint32_t nodes)
{
    uint32_t steps = 0;
    while ((UINT64_C(1) << steps) < nodes)
        steps++;
    return steps;
}

/* One team number's allgather words for a job whose teams take up to `steps`
 * steps: READY for each step, then DATA for each, each alone on its line. */
static inline uint64_t osoe_internal_team_gather_bytes(uint32_t steps)
{
    return (uint64_t)2 * steps * OSOE_INTERNAL_LINE_BYTES;
}

/* Where the allgather's words start: past the counter's line. */
static inline uint64_t osoe_internal_gather_offset(uint32_t fanout, uint32_t pipeline,
                                                   uint32_t max_teams)
{
    return osoe_internal_team_counter_offset(fanout, pipeline, max_teams) +
           OSOE_INTERNAL_LINE_BYTES;
}

/* The region's size for `max_teams` team numbers, a tree of fan-out `fanout`,
 * `pipeline` pieces in flight and a job of `nodes` nodes, before page
 * rounding: the slots, the words, the counter's line, then the allgather's
 * words. */
static inline uint64_t osoe_internal_region_bytes(uint32_t fanout, uint32_t pipeline,
                                                  uint32_t max_teams, uint32_t nodes)
{
    return osoe_internal_gather_offset(fanout, pipeline, max_teams) +
           (uint64_t)max_teams *
               osoe_internal_team_gather_bytes(osoe_internal_gather_steps(nodes));
}

#endif /* OSOE_INTERNAL_REGION_H */
