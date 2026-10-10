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
 * the heap still can, which is why scratch comes first (below).
 *
 * Its layout is the generic internode phase's (internode-generic.c), which is
 * its only user. Since the team redesign's step E
 * (docs/design/team-resources-proposal.md §4) it is in two kinds of place: **a
 * block per team number**, holding the team's counters and its small slots, so
 * that two teams' collectives count on different words whatever their trees
 * share; and **a lane per thread record**, holding the slots of the large
 * collectives, lent to one collective at a time by the thread that leads it.
 *
 * A data collective is *small* when its bytes are at most
 * OSOE_SMALL_COLLECTIVE_BYTES and *large* otherwise (osoe_collective_class,
 * below), which every lead decides alike from the call's bytes. A small one is
 * a single piece through the team number's own small slots; a large one goes
 * in pieces of a slot of 16 KiB, D of them in flight (D being
 * OSOE_COLLECTIVE_PIPELINE, §5.8, "Pipelined pieces"), through the lanes of the
 * threads that lead it, a thread being in one collective at a time.
 *
 * In order:
 *
 * **The team numbers' small rows** - per number, `fanout + 1` rows, one for
 * each child position and a last for what the parent sends down. A row starts
 * with a lane word: in a child position's row, the lane of the child at that
 * position in the large collective under way, which the child puts there
 * before its first arrival of the collective (E4); the release row's is
 * unused. Then the small slot, a piece's header and OSOE_SMALL_COLLECTIVE_BYTES
 * of data, the row rounded up to the region's line. A small collective's piece
 * goes into the small slot of its sender's position, and its result down into
 * the release row's. Scratch, which is why it comes first: a program's store
 * that runs off the end of its heap lands where a header is checked before
 * anything is read, rather than in a count the next collective would wait on
 * forever.
 *
 * **The team numbers' words**: per number, an arrival counter for each child
 * position and then the release counter, eight bytes each, each on a line of
 * its own - one counter per position and not one per node, since with pieces
 * in flight a fast child's second arrival must not stand in for a slow
 * child's first; then **the grant line**, whose counter a parent adds to once
 * it has put, past the counter, the grant's words: the epoch and the identity
 * of the large reduction it has entered, its bytes, and the lane it lends to
 * it (E4). Eight-byte counters because stock RDMA verbs have no four-byte
 * atomics, so a four-byte word would put every arrival of a RoCE backend on
 * its slow path. The counters are contiguous, so that a lead's one watch
 * covers every word it waits on (internode-generic.c), and no put writes
 * inside it: the lane words and the grant's words, which are put, lie outside
 * - the lane words in the rows above, the grant's past its counter at the
 * watch's end - so that no put wakes a lead to find its counters unmoved. (The
 * release counter's line also holds collect's spare words, below, which a
 * node-mate writes with plain stores, waking nobody.)
 * The release counter's line keeps two spare words past its first 64 bytes
 * (below).
 *
 * Past each number's grant line, **two lines for its splits** (§5.7; the team
 * redesign's step D, D2): the words a split over the team as parent reduces in
 * place, every parent PE's free mask of team numbers - a bit per number,
 * OSOE_MAX_TEAMS bits - and then one word of marks (team.c). Seventeen words
 * at the largest OSOE_MAX_TEAMS, 1,024: two lines hold them, and every team
 * number has the two, whatever OSOE_MAX_TEAMS is, so that nothing else's place
 * depends on it more than it did.
 *
 * Then **the allgather's words** (§5.8, "An allgather for fcollect"; phase 1
 * step 5 slice 6): per team number, a READY counter for each of the gather's
 * steps and then a DATA counter for each, eight bytes each and each alone on
 * its line, as the tree's are. READY[s] at a lead counts the entries of the
 * lead it writes to at step s, DATA[s] the steps s of the lead that writes to
 * it (internode-generic.c); READY[0]'s line also holds, past the counter, the
 * shape of the gather that lead entered, 24 bytes, put before the add. A team
 * over N nodes takes ceil(log2 N) steps, so every team number has room for
 * ceil(log2 N) steps of the job's N nodes, which every node agrees on - with
 * the reduce-scatter's words, the parts of the layout the node count sizes, by
 * its logarithm.
 *
 * Then **the reduce-scatter's words** (v0.2 step 3; below), sized the same way:
 * per team number, a line for each of the ceil(log2 N) rows of Träff's
 * reduce-scatter and allgather, three more, and the grant's lines - one up to
 * five rows, so four more lines up to 32 nodes, five to 1,024, six to 32,768
 * and seven past.
 *
 * Then **the scans' area** (below), and last **the lanes**: one per thread
 * record of the PE (node.h), each `fanout + 1` rows of D slots, a piece's
 * header and its 16 KiB each (OSOE_INTERNAL_SLOT_BYTES) - a
 * row for each child position's pieces and one for what the parent sends
 * down, piece k in the row's slot k mod D - a lane's place its index times its
 * size. Last, so that no other place depends on how many there are: a lane is
 * found by an index that travels (E4), and the index alone, with what every
 * node agrees on, places it.
 *
 * **What a team number owns here is the team's while one of this PE's teams
 * holds the number** (step D): a PE gives a number back when it destroys its
 * team, and takes it again for a later one, and as it takes it zeroes every
 * counter of the number in its own region and its grants' words
 * (osoe_internode_number_taken), before the split's sync, after which alone
 * anybody writes them for the new team. Nothing of the old team is still to
 * arrive by then: every write into a PE's slots and counters is one the PE
 * waits for before it returns from the collective, and the destroy's sync
 * follows every member's return from the team's last collective (the
 * proposal's "Why giving a number back is safe"). The slots and the lane words
 * are not zeroed: each is read only once a counter says it is written. **A
 * lane is a collective's from its thread's entry to its return** (E5): every
 * write into a lane is one its owner waits for before it returns, so at return
 * nothing addressed to it is still to come, and the thread's next collective,
 * on any team, may have it.
 *
 * Every PE's heap carries one region. A team touches only its own number's
 * rows and words, on the leads of its nodes - but for its scans, on every
 * member (below) - and a large collective a lane of each lead's thread, or
 * for a scan of each member's; the pages
 * nobody writes cost address space and the /dev/shm capacity check's share,
 * not memory. **Every node computes the same offsets**: the layout is fixed by
 * the fan-out, OSOE_MAX_TEAMS, D, OSOE_SMALL_COLLECTIVE_BYTES and the job's
 * node count, which every node agrees on (§5.3, §5.7, §5.8, §6.2), and by the
 * slot and the line, which are constants rather than settings, and constants
 * rather than the compiler's cache line - 128 bytes on aarch64 and 64 on
 * x86-64 - so that nodes of different architectures in one job agree too. Two
 * nodes that disagreed about any of them would put into each other's slots at
 * different offsets. How many lanes a PE has - its thread records - places
 * nothing else, and needs no agreement.
 */
#ifndef OSOE_INTERNAL_REGION_H
#define OSOE_INTERNAL_REGION_H

#include <stdint.h>

enum {
    OSOE_INTERNAL_PIECE_BYTES = 16 << 10,       /* the most a large collective's piece carries */
    /*
     * The line each control word sits alone on: 128 bytes whatever the
     * compiler's cache line (base/cacheline.h), since the offsets must be
     * every node's alike, and 128 separates the words on every core osoe runs
     * on - the Apple cores the development VM runs on have 128-byte lines, a
     * Pi's Cortex-A72 and A76 and x86-64 have 64.
     */
    OSOE_INTERNAL_LINE_BYTES = 128,
    /* What a piece carries ahead of its bytes (internode-generic.c), sixteen
     * bytes so that the bytes after it stay aligned for every element. */
    OSOE_INTERNAL_PIECE_HEADER_BYTES = 16,
    /* A small row's lane word, and the room it takes ahead of the small slot:
     * sixteen bytes, so that the slot's header and data stay aligned. */
    OSOE_INTERNAL_LANE_WORD_BYTES = 16,
    /* A lane's slot: a piece's header, then the piece's 16 KiB, so that a
     * power-of-two size from 16 KiB goes in whole pieces - with the header
     * inside 16 KiB a piece held 16,368 bytes, and 32 KiB went in three pieces
     * against a window of two (round six's U1; the author, 2026-10-04 item 10).
     * A multiple of sixteen, so that every slot's bytes stay aligned. */
    OSOE_INTERNAL_SLOT_BYTES = OSOE_INTERNAL_PIECE_HEADER_BYTES + OSOE_INTERNAL_PIECE_BYTES,
};

/* Which way a data collective moves (the file's comment). */
enum osoe_collective_class {
    OSOE_COLLECTIVE_SMALL,
    OSOE_COLLECTIVE_LARGE,
};

/* The class of a data collective of `bytes`, as every lead decides it: small
 * when the bytes fit `small_collective_bytes`, OSOE_SMALL_COLLECTIVE_BYTES. */
static inline enum osoe_collective_class osoe_collective_class(uint64_t bytes,
                                                               uint32_t small_collective_bytes)
{
    return bytes <= small_collective_bytes ? OSOE_COLLECTIVE_SMALL : OSOE_COLLECTIVE_LARGE;
}

/* One small row: its lane word, then the small slot - a piece's header and
 * `small_collective_bytes` - rounded up to the line. */
static inline uint64_t osoe_internal_small_row_bytes(uint32_t small_collective_bytes)
{
    uint64_t bytes = OSOE_INTERNAL_LANE_WORD_BYTES + OSOE_INTERNAL_PIECE_HEADER_BYTES +
                     (uint64_t)small_collective_bytes;
    return (bytes + OSOE_INTERNAL_LINE_BYTES - 1) / OSOE_INTERNAL_LINE_BYTES *
           OSOE_INTERNAL_LINE_BYTES;
}

/* One team number's small rows: a row per child position, then the release
 * row. */
static inline uint64_t osoe_internal_team_small_bytes(uint32_t fanout,
                                                      uint32_t small_collective_bytes)
{
    return (uint64_t)(fanout + 1) * osoe_internal_small_row_bytes(small_collective_bytes);
}

/* The lines of a team number's words that hold its splits' masks and marks
 * (the file's comment): room for 1,024 numbers' bits and the marks. */
enum { OSOE_INTERNAL_SPLIT_LINES = 2 };

/* One team number's words: an arrival counter per child position, the release
 * counter and the grant counter, each alone on its line; then its splits'
 * lines. */
static inline uint64_t osoe_internal_team_words_bytes(uint32_t fanout)
{
    return (uint64_t)(fanout + 2 + OSOE_INTERNAL_SPLIT_LINES) * OSOE_INTERNAL_LINE_BYTES;
}

/* Where the words start: past every team number's small rows. */
static inline uint64_t osoe_internal_words_offset(uint32_t fanout, uint32_t max_teams,
                                                  uint32_t small_collective_bytes)
{
    return (uint64_t)max_teams * osoe_internal_team_small_bytes(fanout, small_collective_bytes);
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

static inline uint64_t osoe_internal_team_spare_word(uint32_t fanout, uint32_t max_teams,
                                                     uint32_t small_collective_bytes,
                                                     uint32_t team,
                                                     enum osoe_internal_team_spare which)
{
    return osoe_internal_words_offset(fanout, max_teams, small_collective_bytes) +
           (uint64_t)team * osoe_internal_team_words_bytes(fanout) +
           (uint64_t)fanout * OSOE_INTERNAL_LINE_BYTES + 64 + (uint64_t)which * 8;
}

/* The words a split over team number `team` reduces (the file's comment):
 * the mask's, then the marks word, in the number's split lines. */
static inline uint64_t osoe_internal_team_split_offset(uint32_t fanout, uint32_t max_teams,
                                                       uint32_t small_collective_bytes,
                                                       uint32_t team)
{
    return osoe_internal_words_offset(fanout, max_teams, small_collective_bytes) +
           (uint64_t)team * osoe_internal_team_words_bytes(fanout) +
           (uint64_t)(fanout + 2) * OSOE_INTERNAL_LINE_BYTES;
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
static inline uint64_t osoe_internal_gather_offset(uint32_t fanout, uint32_t max_teams,
                                                   uint32_t small_collective_bytes)
{
    return osoe_internal_words_offset(fanout, max_teams, small_collective_bytes) +
           (uint64_t)max_teams * osoe_internal_team_words_bytes(fanout);
}

/*
 * Then **the reduce-scatter's words** (v0.2 step 3; docs/decisions/
 * 2026-10-07-circulant.md, which the author accepted on 2026-10-07): what
 * Träff's circulant reduce-scatter and allgather among a team's node leads
 * (collective-circulant.h; internode-generic.c, in_rounds) counts on, per team
 * number, in this order, each counter alone on its line:
 *   - an **arrival counter per row**, ceil(log2 N) of them: the pieces of the
 *     round that takes the row of this lead's lane, in either half;
 *   - the **credit counter**: one for each piece this lead's receivers have
 *     taken that its window waits on;
 *   - the **allgather's grant counter**: one for each of its allgather
 *     receivers whose row its reduce-scatter has emptied;
 *   - the **reduce-scatter's grant lines**: its counter, one for each
 *     reduce-scatter receiver that has entered, then a field per round, where
 *     that round's receiver alone puts the epoch, identity and bytes of the
 *     collective it entered and the lane it lends - each receiver's own, so
 *     that no word has two writers, which a head that every receiver put had
 *     with progress helpers, and the lead checks every receiver's call, not the
 *     last to land (the review of v0.2 step 4's merge point, correctness Q2-4);
 *     as many lines as the counter and the fields take, one up to five rows;
 *   - the **senders' lanes line**: a lane and an epoch per row, where each
 *     reduce-scatter round's sender puts its own lane before its first piece's
 *     add, for the allgather back.
 * So every put lands outside the counters a lead watches - the grant's fields
 * past its counter, the senders' lanes on a line of their own. Its own, and
 * no other collective's: the allgather ends with no step every lead waits
 * for, so a lead that has left can start a small collective whose child adds
 * to its parent's counters at once while another lead still waits on its last
 * pieces (tools/modelcheck/circulant_model.py, shared_counters). Sized like
 * the gather's by the job's node count, ceil(log2 N) + 3 lines and the grant's,
 * and no more rows than OSOE_INTERNAL_CIRCULANT_ROWS_MOST, which the senders'
 * lanes line holds: a team of more than 65,536 nodes takes the trees.
 */
enum { OSOE_INTERNAL_CIRCULANT_ROWS_MOST = 16 };

/* A reduce-scatter grant's field: the epoch, identity and bytes of the
 * receiver's collective and the lane it lends, and room to keep the next
 * field's bytes aligned (internode-generic.c, struct circulant_grant_field). */
enum { OSOE_INTERNAL_CIRCULANT_GRANT_FIELD_BYTES = 24 };

/* The reduce-scatter's grant lines for `rows` rows: the counter's 8 bytes and
 * a field a row. */
static inline uint32_t osoe_internal_circulant_grant_lines(uint32_t rows)
{
    uint64_t bytes = 8 + (uint64_t)rows * OSOE_INTERNAL_CIRCULANT_GRANT_FIELD_BYTES;
    return (uint32_t)((bytes + OSOE_INTERNAL_LINE_BYTES - 1) / OSOE_INTERNAL_LINE_BYTES);
}

/* The rows of the reduce-scatter's words for a job of `nodes` nodes:
 * ceil(log2 nodes), at most OSOE_INTERNAL_CIRCULANT_ROWS_MOST, 0 for one. */
static inline uint32_t osoe_internal_circulant_rows(uint32_t nodes)
{
    uint32_t rows = osoe_internal_gather_steps(nodes);
    return rows < OSOE_INTERNAL_CIRCULANT_ROWS_MOST ? rows : OSOE_INTERNAL_CIRCULANT_ROWS_MOST;
}

/* One team number's reduce-scatter words: a line for each row, the credit
 * counter's and the allgather grant counter's, the grant's lines and the
 * senders' lanes line, none on one node. */
static inline uint64_t osoe_internal_team_circulant_bytes(uint32_t rows)
{
    return rows == 0 ? 0
                     : (uint64_t)(rows + 3 + osoe_internal_circulant_grant_lines(rows)) *
                           OSOE_INTERNAL_LINE_BYTES;
}

/* Where the reduce-scatter's words start: past every team number's allgather
 * words. */
static inline uint64_t osoe_internal_circulant_offset(uint32_t fanout, uint32_t max_teams,
                                                      uint32_t small_collective_bytes,
                                                      uint32_t nodes)
{
    return osoe_internal_gather_offset(fanout, max_teams, small_collective_bytes) +
           (uint64_t)max_teams *
               osoe_internal_team_gather_bytes(osoe_internal_gather_steps(nodes));
}

/*
 * Then **the scans' area** (spec §9.10.10; phase 1 step 10 slice 1,
 * docs/prompts/done/phase1-step10-slice1-prompt.md; the team redesign's step
 * E, E6): per team number, like everything else here, and on every member's
 * region rather than its node's lead's alone, since every member of a scan
 * holds a place of its own in the scan's tree. Shaped as the reductions' are:
 * a small row per child position and one for the down - a lane word, then a
 * small slot of OSOE_SMALL_COLLECTIVE_BYTES, with no piece header, as a scan's
 * pieces carry none - rounded up to the line; then a counter for each row and
 * the scans' grant counter, each alone on its line and contiguous, the
 * grant's words past it. A small scan is one piece through the small slots; a
 * large one goes through the members' lanes in pieces sized to the scan, at
 * most a lane's 16 KiB slot, D of them in flight (OSOE_COLLECTIVE_PIPELINE; the author's decision of
 * 2026-09-27), piece g of the team's scans in slot g mod D, each member
 * granting its tree children as it enters. Not the reductions' rows and
 * counters: a node that finished a scan may start a reduction and put into a
 * row its tree parent, still in the scan, would be reading - different
 * collectives have different trees. Per team number and not the PE's alone,
 * so that two threads of a PE may scan two teams at once (spec §9.4), and
 * consecutive scans on a team are safe by the tree alone
 * (tools/modelcheck/scan_model.py, per_team; scan_pipeline_model.py, with D in
 * flight, the lanes and the small scans).
 */
static inline uint64_t osoe_internal_scan_row_bytes(uint32_t small_collective_bytes)
{
    uint64_t bytes = OSOE_INTERNAL_LANE_WORD_BYTES + (uint64_t)small_collective_bytes;
    return (bytes + OSOE_INTERNAL_LINE_BYTES - 1) / OSOE_INTERNAL_LINE_BYTES *
           OSOE_INTERNAL_LINE_BYTES;
}

static inline uint64_t osoe_internal_scan_offset(uint32_t fanout, uint32_t max_teams,
                                                 uint32_t small_collective_bytes,
                                                 uint32_t nodes)
{
    return osoe_internal_circulant_offset(fanout, max_teams, small_collective_bytes, nodes) +
           (uint64_t)max_teams *
               osoe_internal_team_circulant_bytes(osoe_internal_circulant_rows(nodes));
}

/* One team number's scan rows, and then their counters and the scans' grant
 * line. */
static inline uint64_t osoe_internal_team_scan_bytes(uint32_t fanout,
                                                     uint32_t small_collective_bytes)
{
    return (uint64_t)(fanout + 1) * osoe_internal_scan_row_bytes(small_collective_bytes) +
           (uint64_t)(fanout + 2) * OSOE_INTERNAL_LINE_BYTES;
}

static inline uint64_t osoe_internal_scan_bytes(uint32_t fanout, uint32_t max_teams,
                                                uint32_t small_collective_bytes)
{
    return (uint64_t)max_teams * osoe_internal_team_scan_bytes(fanout, small_collective_bytes);
}

/* One lane: a row of `pipeline` slots per child position, then the release
 * row. */
static inline uint64_t osoe_internal_lane_bytes(uint32_t fanout, uint32_t pipeline)
{
    return (uint64_t)(fanout + 1) * pipeline * OSOE_INTERNAL_SLOT_BYTES;
}

/* Where the lanes start: past the scans' area. */
static inline uint64_t osoe_internal_lanes_offset(uint32_t fanout, uint32_t max_teams,
                                                  uint32_t small_collective_bytes,
                                                  uint32_t nodes)
{
    return osoe_internal_scan_offset(fanout, max_teams, small_collective_bytes, nodes) +
           osoe_internal_scan_bytes(fanout, max_teams, small_collective_bytes);
}

/* The region's size for `max_teams` team numbers and `lanes` lanes, a tree of
 * fan-out `fanout`, `pipeline` pieces in flight, small collectives of up to
 * `small_collective_bytes` and a job of `nodes` nodes, before page rounding:
 * the small rows, the words, the allgather's words, the reduce-scatter's
 * words, the scans' area, then the lanes. */
static inline uint64_t osoe_internal_region_bytes(uint32_t fanout, uint32_t pipeline,
                                                  uint32_t max_teams,
                                                  uint32_t small_collective_bytes,
                                                  uint32_t nodes, uint32_t lanes)
{
    return osoe_internal_lanes_offset(fanout, max_teams, small_collective_bytes, nodes) +
           (uint64_t)lanes * osoe_internal_lane_bytes(fanout, pipeline);
}

#endif /* OSOE_INTERNAL_REGION_H */
