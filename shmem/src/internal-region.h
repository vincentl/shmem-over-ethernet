/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
 * Past each team number's counters, **two lines for its splits** (§5.7; the
 * team redesign's step D, docs/design/team-resources-proposal.md D2): the words
 * a split over the team as parent reduces in place, every parent PE's free mask
 * of team numbers - a bit per number, OSOE_MAX_TEAMS bits - and then one word
 * of marks (team.c). Seventeen words at the largest OSOE_MAX_TEAMS, 1,024: two
 * lines hold them, and every team number has the two, whatever OSOE_MAX_TEAMS
 * is, so that nothing else's place depends on it more than it did. Until step
 * D a line past every team number's words held the count of team numbers
 * claimed, on world PE 0, and the split's first number travelled in a spare
 * word of the release counter's line; both are gone.
 *
 * Last, **the allgather's words** (§5.8, "An allgather for fcollect"; phase 1
 * step 5 slice 6): per team number, a READY counter for each of the gather's
 * steps and then a DATA counter for each, eight bytes each and each alone on
 * its line, as the tree's are. READY[s] at a lead counts the entries of the
 * lead it writes to at step s, DATA[s] the steps s of the lead that writes to
 * it (internode-generic.c); READY[0]'s line also holds, past the counter, the
 * shape of the gather that lead entered, 24 bytes, put before the add. A team over N nodes takes ceil(log2 N) steps, so
 * every team number has room for ceil(log2 N) steps of the job's N nodes,
 * which every node agrees on - the one part of the layout the node count
 * sizes, by its logarithm. They came after the counter's line, so that nothing
 * before them moved when they were added, and come after the words since the
 * line went.
 *
 * **What a team number owns here is the team's while one of this PE's teams
 * holds the number** (step D): a PE gives a number back when it destroys its
 * team, and takes it again for a later one, and as it takes it zeroes every
 * counter of the number in its own region (osoe_internode_number_taken), before
 * the split's sync, after which alone anybody writes them for the new team.
 * Nothing of the old team is still to arrive by then: every write into a PE's
 * slots and counters is one the PE waits for before it returns from the
 * collective, and the destroy's sync follows every member's return from the
 * team's last collective (the proposal's "Why giving a number back is safe").
 * The slots are not zeroed: a slot is read only once a counter says it is
 * written.
 *
 * Every PE's heap carries one, and a team touches only its own slots and its
 * words' lines, and only on the leads of its nodes - but for its scans, whose
 * slots are on every member (below); the pages nobody writes
 * cost address space and the /dev/shm capacity check's share, not memory -
 * about 10 MiB of slots a PE at the default 64 teams, a fan-out of 4 and D = 2,
 * and 2.6 MiB of the scans' (below); both are half of that at D = 1; the allgather's words add 2.5 KiB a team number at a thousand
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
    /* One piece of a prefix scan: smaller than a data collective's, since a
     * scan's slots are every member's and not only its node's lead's, and no
     * program in the corpus scans much (the scans' area, below). */
    OSOE_INTERNAL_SCAN_SLOT_BYTES = 4 << 10,
};

/* One team number's block of slots, for a tree of fan-out `fanout` whose leads
 * keep `pipeline` pieces in flight: a row of `pipeline` per child position,
 * then the row the parent's releases land in. */
static inline uint64_t osoe_internal_team_slots_bytes(uint32_t fanout, uint32_t pipeline)
{
    return (uint64_t)(fanout + 1) * pipeline * OSOE_INTERNAL_SLOT_BYTES;
}

/* The lines of a team number's words that hold its splits' masks and marks
 * (the file's comment): room for 1,024 numbers' bits and the marks. */
enum { OSOE_INTERNAL_SPLIT_LINES = 2 };

/* One team number's words: an arrival counter per child position, then the
 * release counter, each alone on its line; then its splits' lines. */
static inline uint64_t osoe_internal_team_words_bytes(uint32_t fanout)
{
    return (uint64_t)(fanout + 1 + OSOE_INTERNAL_SPLIT_LINES) * OSOE_INTERNAL_LINE_BYTES;
}

/* Where the words start: past every team number's slots. */
static inline uint64_t osoe_internal_words_offset(uint32_t fanout, uint32_t pipeline,
                                                  uint32_t max_teams)
{
    return (uint64_t)max_teams * osoe_internal_team_slots_bytes(fanout, pipeline);
}

/*
 * Spare words of a team number's release counter line, past its first 64
 * bytes (phase 1 step 8 slice 2): what a collective of the SHMEM layer needs
 * symmetric by construction rather than by the allocator's history, since
 * under THREAD_MULTIPLE a borrow from the heap may race an allocation on
 * another thread, in a different order on each PE. Each is the team number's
 * own, and one team's collectives are never concurrent (spec §9.4), so a word
 * needs no more room than one collective at a time. The line's counter is
 * written only by remote adds to its first eight bytes; these only by the
 * collective that owns them, with plain stores, and read by node-mates.
 */
enum osoe_internal_team_spare {
    OSOE_INTERNAL_COLLECT_LENGTH,       /* a member's collect length, for its lead */
    OSOE_INTERNAL_COLLECT_TOTAL,        /* the lead's total of them, for its members */
};

static inline uint64_t osoe_internal_team_spare_word(uint32_t fanout, uint32_t pipeline,
                                                     uint32_t max_teams, uint32_t team,
                                                     enum osoe_internal_team_spare which)
{
    return osoe_internal_words_offset(fanout, pipeline, max_teams) +
           (uint64_t)team * osoe_internal_team_words_bytes(fanout) +
           (uint64_t)fanout * OSOE_INTERNAL_LINE_BYTES + 64 + (uint64_t)which * 8;
}

/* The words a split over team number `team` reduces (the file's comment):
 * the mask's, then the marks word, in the number's split lines. */
static inline uint64_t osoe_internal_team_split_offset(uint32_t fanout, uint32_t pipeline,
                                                       uint32_t max_teams, uint32_t team)
{
    return osoe_internal_words_offset(fanout, pipeline, max_teams) +
           (uint64_t)team * osoe_internal_team_words_bytes(fanout) +
           (uint64_t)(fanout + 1) * OSOE_INTERNAL_LINE_BYTES;
}

/* How many words a free mask of `max_teams` numbers takes, a bit a number. */
static inline uint32_t osoe_internal_mask_words(uint32_t max_teams)
{
    return (max_teams + 63) / 64;
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

/* Where the allgather's words start: past every team number's words. */
static inline uint64_t osoe_internal_gather_offset(uint32_t fanout, uint32_t pipeline,
                                                   uint32_t max_teams)
{
    return osoe_internal_words_offset(fanout, pipeline, max_teams) +
           (uint64_t)max_teams * osoe_internal_team_words_bytes(fanout);
}

/*
 * Last, **the scans' area** (spec §9.10.10; phase 1 step 10 slice 1,
 * docs/prompts/done/phase1-step10-slice1-prompt.md): per team number, like
 * everything else here, and on every member's region rather than its node's
 * lead's alone, since every member of a scan holds a place of its own in the
 * scan's tree - a row of D up-slots per child position and a row of D
 * down-slots, D being the pieces a scan keeps in flight as the reductions do
 * (OSOE_COLLECTIVE_PIPELINE; the author's decision of 2026-09-27), piece g of
 * the team's scans in slot g mod D; then a counter for each row, alone on its
 * line. Not the
 * reductions' rows: a node that finished a scan may start a reduction and put
 * into a row its tree parent, still in the scan, would be reading - different
 * collectives have different trees. Per team number and not the PE's alone,
 * shared by every team: under THREAD_MULTIPLE two threads of a PE may scan two
 * teams at once (spec §9.4), and with a team's own slots consecutive scans on
 * it are safe by the tree alone (tools/modelcheck/scan_model.py, per_team;
 * the review of the slice), and with D in flight as well
 * (scan_pipeline_model.py). 2.6 MiB at the defaults, touched only by scans.
 */
static inline uint64_t osoe_internal_scan_offset(uint32_t fanout, uint32_t pipeline,
                                                 uint32_t max_teams, uint32_t nodes)
{
    return osoe_internal_gather_offset(fanout, pipeline, max_teams) +
           (uint64_t)max_teams *
               osoe_internal_team_gather_bytes(osoe_internal_gather_steps(nodes));
}

/* One team number's scan rows, `pipeline` slots each, and their counters. */
static inline uint64_t osoe_internal_team_scan_bytes(uint32_t fanout, uint32_t pipeline)
{
    return (uint64_t)(fanout + 1) *
           ((uint64_t)pipeline * OSOE_INTERNAL_SCAN_SLOT_BYTES + OSOE_INTERNAL_LINE_BYTES);
}

static inline uint64_t osoe_internal_scan_bytes(uint32_t fanout, uint32_t pipeline,
                                                uint32_t max_teams)
{
    return (uint64_t)max_teams * osoe_internal_team_scan_bytes(fanout, pipeline);
}

/* The region's size for `max_teams` team numbers, a tree of fan-out `fanout`,
 * `pipeline` pieces in flight and a job of `nodes` nodes, before page
 * rounding: the slots, the words, the allgather's words, then the scans'
 * area. */
static inline uint64_t osoe_internal_region_bytes(uint32_t fanout, uint32_t pipeline,
                                                  uint32_t max_teams, uint32_t nodes)
{
    return osoe_internal_scan_offset(fanout, pipeline, max_teams, nodes) +
           osoe_internal_scan_bytes(fanout, pipeline, max_teams);
}

#endif /* OSOE_INTERNAL_REGION_H */
