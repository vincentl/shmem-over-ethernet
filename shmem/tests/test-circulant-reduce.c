/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Träff's circulant reduce-scatter and allgather on the data path (tier 2;
 * v0.2 step 3, Part 2; internode-generic.c's in_rounds).
 *
 *   test-circulant-reduce [threads | lanes | diverge | tiny | helpers-differ]
 *
 * Run over logical nodes, the link set by OSOE_LINK_* so that the choice is
 * known: the test asks the chooser itself (collective-shape.c, compiled in)
 * what each call takes, from what the data path asks it with (shape_of), and
 * holds what this node's lead published to it. In order:
 *   1. sizes: a byte sum over the world at sizes from the small class's edge
 *      to 1 MiB and an odd tail, either side of the chooser's crossover: each
 *      result right; each call's algorithm the chooser's; a Träff's call's
 *      rounds twice ceil(log2 N), counted as they ran, its tree children none,
 *      and the most pieces its lead had put in a round and not yet held the
 *      credit for at most the window - the credits' pin, where a sender that
 *      ran ahead of them would overwrite a slot only in an unlucky run;
 *   2. types: every typed reduction - every operation on every type the
 *      specification gives it - at the smallest size that takes Träff's and at
 *      one of several pieces, each of them with a dest of its own, whose source
 *      the call must leave as it was, and in place, of values whose result no
 *      order of combining can round: small integers, and for the products 1, -1
 *      and 2;
 *   3. in place: source == dest, a byte sum;
 *   4. staggered: two Träff's reductions in a row, the PEs entering each some
 *      milliseconds apart, in one order and then the other, so that some lead
 *      is ready for its reduce-scatter while its receivers have still to
 *      enter - what its grants hold it back from;
 *   5. back to back: Träff's reductions followed at once, no barrier between,
 *      by small reductions, broadcasts and the trees' large reductions, on the
 *      world and on a copy of it whose leads are the world's - the
 *      reduce-scatter's counters its own, which a small reduction's child that
 *      has left adds to at once;
 *   6. reuse: a copy of the world made, given two Träff's reductions and
 *      destroyed, three times over, each new copy taking the number its last
 *      gave back - whose words every lead must have zeroed for it;
 *   7. few: long double sums of fewer elements than nodes, where some blocks
 *      are empty and some rounds send or take nothing - Träff's only where the
 *      link makes a few hundred bytes worth it, as shmem.circulant.16.tiny's
 *      does;
 *   8. teams: Träff's on teams that are not the world, each reduction by the
 *      team's members alone - the evens and the odds, all but the first PE, all
 *      but the last, every third PE, the world in reverse rank order, and a
 *      grid's rows and columns - teams whose nodes' places among their own are
 *      not their numbers, and whose leads are not all the world's. Byte sums
 *      at five sizes from just past the small class to 256 KiB, in place and
 *      not: each right on every member, nothing written past its end, the
 *      source left as it was, and each call's algorithm the chooser's for the
 *      nodes the team spans, read from the team's own block of this node, after
 *      a small reduction on the team that leaves the block saying a tree - so
 *      that a large call that published nothing is not taken for one that took
 *      Träff's. The block's record says the team's nodes and this node's place
 *      among them;
 *   9. the deprecated shmem_TYPE_OP_to_all over the whole job, which is the
 *      world's reduction, at the sizes of 2: every operation on every type of
 *      Table 11, in place and not, and each one's algorithm the chooser's, read
 *      as in 8;
 *  10. rounding: sums of doubles, floats and complex doubles whose result
 *      depends on the order they are made in, on the world and on each team of
 *      8, at the least size that takes Träff's over the team's nodes and at
 *      four pieces, in place and not: a result within what any order's rounding
 *      allows of the sum, from values whose sums in two orders the test checks
 *      differ, and every member holding the same bits - their hashes, reduced
 *      by a maximum and a minimum over the team, are alike.
 * With `tiny`, the few-elements sums of 7 are required to have taken Träff's,
 * which the link of the case makes them. With `helpers-differ`, every phase on
 * nodes whose progress helpers differ in number, each node's set with
 * `-e RANK:OSOE_PROGRESS_HELPERS=n` for each of its ranks: the test requires
 * that they do, from the owners each node's lead holds in its control segment.
 * With `threads`, under SHMEM_THREAD_MULTIPLE: two threads a PE, each with a
 * copy of the world of its own, making Träff's reductions on it at once - two
 * teams' reduce-scatters through two lanes of each lead; then both on one copy
 * in turn, each round's reduction led by the other thread, so that the lane a
 * lead lends changes from call to call; at 1 MiB, so that every round credits.
 * With `lanes`, under SHMEM_THREAD_MULTIPLE and one PE a node: on node i, i
 * helper threads claim thread records first and hold them, and then a new
 * thread makes the reductions of phase 1, so that every node's lead lends a
 * lane of its own - which the single-threaded phases cannot arrange, every lead
 * lending its main thread's - and a sender that put into another round's lane,
 * or its own index, would land in slots nobody lent it; the leads' lanes are
 * checked to differ. With `diverge`, PE 1 reduces a byte more than the others:
 * the job must end naming the difference. Under
 * OSOE_COLLECTIVE_SHAPE=tree every call must take a tree: the setting turns
 * Träff's off by name. And where the rounds outnumber the rows a lane of the
 * fan-out ceiling's + 1 would hold - nine nodes and more at a ceiling of 2, a
 * lane of three (P3) - the chooser must still take Träff's at some size: a lane
 * has as many rows as the rounds need, and a test that took none of it would
 * pass on the trees alone.
 *
 * Every result is checked as the call returns; at the end every PE's failures
 * are summed, and the job's last line says how many calls took Träff's.
 */
#define _GNU_SOURCE
#include <complex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "collective-circulant.h"
#include "collective-shape.h"
#include "internal-region.h"
#include "invariants.h"
#include "launcher.h"

static int me, npes, failures, circulant_calls;
static int team_wanted, team_calls;     /* calls on teams that are not the world: those the
                                         * chooser gave Träff's, and those that took it */
static int few_calls;                   /* fewer-elements sums that took Träff's */
static int per_node;                    /* PEs a node: the maps here are blocks of one size */
static uint64_t nodes;
static struct osoe_link link;
static uint64_t ceiling, capacity, small;

static uint64_t setting(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    return value ? strtoull(value, NULL, 10) : fallback;
}

/* What the data path asks the chooser for a reduction of `bytes` over a team
 * that spans `team_nodes` of the job's nodes (internode-generic.c, shape_of):
 * Träff's allowed under the shape `link` - not `tree` - where its rows fit the
 * reduce-scatter's words, which the layout sizes for the job's nodes, in a lane
 * of the rows the job's nodes and the ceiling give it. */
static struct osoe_collective_shape chosen_over(uint64_t team_nodes, uint64_t bytes)
{
    const char *shape = getenv("OSOE_COLLECTIVE_SHAPE");
    bool allowed = (!shape || strcmp(shape, "link") == 0) &&
                   osoe_circulant_rows((uint32_t)team_nodes) <=
                       osoe_internal_circulant_rows((uint32_t)nodes);
    return osoe_shape_choose(OSOE_SHAPE_REDUCE, (uint32_t)team_nodes, bytes, &link,
                             (uint32_t)ceiling,
                             osoe_internal_lane_rows((uint32_t)ceiling, (uint32_t)nodes),
                             (uint32_t)capacity, (uint32_t)small, OSOE_INTERNAL_PIECE_BYTES,
                             allowed);
}

/* The same over the world. */
static struct osoe_collective_shape chosen(uint64_t bytes)
{
    return chosen_over(nodes, bytes);
}

/* The smallest of a ladder of sizes that takes Träff's over `team_nodes`
 * nodes, or 0. */
static uint64_t smallest_circulant_over(uint64_t team_nodes)
{
    for (uint64_t bytes = 2048; bytes <= (UINT64_C(1) << 20); bytes *= 2)
        if (chosen_over(team_nodes, bytes).algorithm == OSOE_SHAPE_CIRCULANT)
            return bytes;
    return 0;
}

static uint64_t smallest_circulant(void)
{
    return smallest_circulant_over(nodes);
}

/*
 * What this node's lead published for the last reduction of the team whose
 * block of the node's pool is `block` (node.h): the algorithm the chooser gives
 * for `bytes` over the `team_nodes` nodes the team spans, and for Träff's its
 * rounds, no tree children and at most the window in flight. Every PE reads
 * its own node's lead, so every node is held to it. True where the chooser
 * wanted Träff's and the call took it as it should have.
 */
static bool published_on(uint32_t block, uint64_t team_nodes, uint64_t bytes, const char *what)
{
    struct osoe_collective_shape want = chosen_over(team_nodes, bytes);
    uint64_t algorithm = 0, rounds = 0, children = 0, most = 0, window = 0;
    CHECK(osoe_test_scoped_metric("team.tree.algorithm", OSOE_MSCOPE_TEAM, block, &algorithm));
    CHECK(osoe_test_scoped_metric("team.tree.circulant_rounds", OSOE_MSCOPE_TEAM, block,
                                  &rounds));
    CHECK(osoe_test_scoped_metric("team.tree.shape_children", OSOE_MSCOPE_TEAM, block,
                                  &children));
    CHECK(osoe_test_scoped_metric("team.tree.most_pieces_in_flight", OSOE_MSCOPE_TEAM, block,
                                  &most));
    CHECK(osoe_test_scoped_metric("team.tree.shape_window", OSOE_MSCOPE_TEAM, block, &window));
    if (bytes <= small)
        return false;           /* a small call is the tree's, whatever the link says */
    if (algorithm != (uint64_t)want.algorithm) {
        fprintf(stderr, "PE %d: %s of %llu bytes on %llu nodes took algorithm %llu, the "
                "chooser's %d\n", me, what, (unsigned long long)bytes,
                (unsigned long long)team_nodes, (unsigned long long)algorithm,
                (int)want.algorithm);
        failures++;
        return false;
    }
    if (want.algorithm != OSOE_SHAPE_CIRCULANT)
        return false;
    uint64_t want_rounds = osoe_circulant_rounds((uint32_t)team_nodes);
    if (rounds != want_rounds || children != 0 || window != want.window || most < 1 ||
        most > window) {
        fprintf(stderr, "PE %d: %s of %llu bytes on %llu nodes took Träff's with %llu rounds "
                "(%llu wanted), %llu children, a window of %llu (%u wanted) and %llu pieces "
                "in flight at most\n", me, what, (unsigned long long)bytes,
                (unsigned long long)team_nodes, (unsigned long long)rounds,
                (unsigned long long)want_rounds, (unsigned long long)children,
                (unsigned long long)window, want.window, (unsigned long long)most);
        failures++;
    }
    return true;
}

/* The same for the world's, whose block is the pool's first. */
static void published(uint64_t bytes, const char *what)
{
    if (published_on(0, nodes, bytes, what))
        circulant_calls++;
}

/* ---- groups: a team, and who is in it --------------------------------------- */

/*
 * The room kept past the end of a vector, filled with one byte before a call
 * and required to hold it after: a block that ran past its end would write
 * there.
 */
enum { GUARD = 64, GUARD_BYTE = 0xA5 };

/*
 * A team, and who is in it, as every PE of the job knows it. `rank` is this
 * PE's in the team, or -1 for a PE outside it. `pes`, the members' PEs by
 * rank, is NULL for a team of every PE in rank order: the world, or a copy.
 */
struct group {
    const char *name;
    shmem_team_t team;
    uint32_t number;            /* what its members hold it under (team.h): a PE's own */
    int size;
    int rank;
    int *pes;
    int64_t pe_sum;             /* the members' PEs, summed */
    uint64_t span;              /* the nodes they are on */
    uint64_t position;          /* this PE's node's place among those, in node order */
};

static int member_pe(const struct group *g, int rank)
{
    return g->pes ? g->pes[rank] : rank;
}

/* Fill in what a group's members say of it: this PE's rank, their sum, the
 * nodes they span and this node's place among them (the maps here are blocks
 * of per_node PEs). */
static void measure(struct group *g)
{
    unsigned char *on = calloc((size_t)nodes, 1);
    CHECK_FATAL(on != NULL);
    g->rank = -1;
    g->pe_sum = 0;
    for (int r = 0; r < g->size; r++) {
        int pe = member_pe(g, r);
        if (pe == me)
            g->rank = r;
        g->pe_sum += pe;
        on[pe / per_node] = 1;
    }
    g->span = 0;
    g->position = 0;
    for (uint64_t node = 0; node < nodes; node++) {
        if (node == (uint64_t)(me / per_node))
            g->position = g->span;
        g->span += on[node];
    }
    free(on);
}

/* The world: a team of every PE, in rank order, under number 0. */
static struct group everyone;

/* The same members under another team: a copy of the world. */
static struct group copy_of_everyone(shmem_team_t team)
{
    struct group g = everyone;
    g.team = team;
    return g;
}

/* What every mode knows of the job once it has started: this PE, how many
 * there are, and the world as a group. */
static void world_known(void)
{
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(nodes > 0 && (uint64_t)npes % nodes == 0);
    per_node = (int)((uint64_t)npes / nodes);
    everyone = (struct group){ .name = "the world", .team = SHMEM_TEAM_WORLD, .number = 0,
                               .size = npes };
    measure(&everyone);
}

static long *tiny_source, *tiny_dest;   /* a small reduction's words, symmetric */

/*
 * A small reduction on `g`, which leaves its block of the node's pool saying a
 * tree and the small class (internode-generic.c, in_pieces publishes either
 * way), so that what the next large reduction publishes is that one's and no
 * earlier one's: a call that published nothing would otherwise be read as the
 * call before it, which took Träff's too. Every member of a node enters the
 * large reduction before its lead can publish it (the lead waits for them),
 * so none of them reads the cleared word after it is written over.
 */
static void clear_published(const struct group *g)
{
    *tiny_source = me;
    CHECK(shmem_long_sum_reduce(g->team, tiny_dest, tiny_source, 1) == 0);
    if (g->span < 2)
        return;                 /* no internode phase: nothing published */
    uint32_t block = osoe_test_team_block(g->number);
    uint64_t algorithm = UINT64_MAX;
    CHECK(block != UINT32_MAX);
    if (block != UINT32_MAX &&
        osoe_test_scoped_metric("team.tree.algorithm", OSOE_MSCOPE_TEAM, block, &algorithm) &&
        algorithm != OSOE_SHAPE_TREE) {
        fprintf(stderr, "PE %d: a small reduction on %s left algorithm %llu published, not a "
                "tree\n", me, g->name, (unsigned long long)algorithm);
        failures++;
    }
}

/* Whether the last reduction of `bytes` on `g` took what the chooser gives for
 * the nodes it spans, read from its block of this node: true where that was
 * Träff's and it did. */
static bool published_by(const struct group *g, uint64_t bytes, const char *what)
{
    if (g->span < 2)
        return false;           /* no internode phase: nothing published */
    uint32_t block = osoe_test_team_block(g->number);
    CHECK(block != UINT32_MAX);
    return block != UINT32_MAX && published_on(block, g->span, bytes, what);
}

/* The team's record on this node says the nodes it spans and this node's place
 * among them, in node order - what Träff's rounds count from, and every lead
 * must reckon alike. */
static void check_record(const struct group *g)
{
    if (g->span < 2)
        return;
    uint32_t block = osoe_test_team_block(g->number);
    uint64_t record_nodes = UINT64_MAX, record_index = UINT64_MAX;
    CHECK(block != UINT32_MAX);
    if (block == UINT32_MAX)
        return;
    CHECK(osoe_test_scoped_metric("team.record.nodes", OSOE_MSCOPE_TEAM, block, &record_nodes));
    CHECK(osoe_test_scoped_metric("team.record.index", OSOE_MSCOPE_TEAM, block, &record_index));
    if (record_nodes != g->span || record_index != g->position) {
        fprintf(stderr, "PE %d: the record of %s on this node says %llu nodes and this one the "
                "%lluth, not %llu and %llu\n", me, g->name, (unsigned long long)record_nodes,
                (unsigned long long)record_index, (unsigned long long)g->span,
                (unsigned long long)g->position);
        failures++;
    }
}

/* ---- 1, 3, 4, 5 and 8: byte sums --------------------------------------------- */

static uint8_t byte_value(int pe, size_t i, int round)
{
    return (uint8_t)(pe * 37 + (int)(i % 251) * 11 + round * 5 + (int)(i / 251));
}

/* Byte i of the sum of the group's members' values, in closed form: the
 * values summed mod 256, which is what the sum of their bytes is. */
static uint8_t byte_sum_want(const struct group *g, size_t i, int round)
{
    int64_t sum = 37 * g->pe_sum + (int64_t)g->size * ((int64_t)(i % 251) * 11 + round * 5 +
                                                       (int64_t)(i / 251));
    return (uint8_t)sum;
}

static void check_byte_sum(const struct group *g, const uint8_t *dest, size_t n, int round,
                           const char *what)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t want = byte_sum_want(g, i, round);
        if (dest[i] != want) {
            fprintf(stderr, "PE %d: %s of %zu bytes, round %d: byte %zu is %u, not %u\n", me,
                    what, n, round, i, dest[i], want);
            failures++;
            return;
        }
    }
}

/* A byte sum of `n` over the group's team, by a member, checked: each byte
 * right, nothing written past the vector's end, and the source as it was
 * unless `in_place` makes it dest. */
static void group_byte_sum(const struct group *g, uint8_t *dest, uint8_t *source, size_t n,
                           int round, bool in_place, const char *what)
{
    uint8_t *from = in_place ? dest : source;
    for (size_t i = 0; i < n; i++) {
        from[i] = byte_value(me, i, round);
        if (!in_place)
            dest[i] = 0;
    }
    memset(dest + n, GUARD_BYTE, GUARD);
    CHECK(shmem_uint8_sum_reduce(g->team, dest, from, n) == 0);
    check_byte_sum(g, dest, n, round, what);
    for (size_t b = 0; b < GUARD; b++)
        if (dest[n + b] != GUARD_BYTE) {
            fprintf(stderr, "PE %d: %s of %zu bytes wrote past its end, to byte %zu\n", me, what,
                    n, n + b);
            failures++;
            break;
        }
    for (size_t i = 0; !in_place && i < n; i++)
        if (source[i] != byte_value(me, i, round)) {
            fprintf(stderr, "PE %d: %s of %zu bytes changed its source, at byte %zu\n", me, what,
                    n, i);
            failures++;
            break;
        }
}

/* A byte sum of `n` over `team`, which holds every PE, checked; `in_place`
 * makes source dest. */
static void byte_sum(shmem_team_t team, uint8_t *dest, uint8_t *source, size_t n, int round,
                     bool in_place, const char *what)
{
    struct group g = copy_of_everyone(team);
    group_byte_sum(&g, dest, source, n, round, in_place, what);
}

static const size_t SIZES[] = { 1025, 2048, 4096, 8192, 16384, 16385, 32768, 65536, 131072,
                                262144, 524288, 1048576, 1048589 };
enum { SIZE_COUNT = sizeof SIZES / sizeof SIZES[0] };

static void sizes(uint8_t *dest, uint8_t *source)
{
    for (int s = 0; s < SIZE_COUNT; s++) {
        shmem_barrier_all();
        byte_sum(SHMEM_TEAM_WORLD, dest, source, SIZES[s], s, false, "a byte sum");
        published(SIZES[s], "a byte sum");
    }
}

static void in_place(uint8_t *dest, size_t n)
{
    shmem_barrier_all();
    byte_sum(SHMEM_TEAM_WORLD, dest, NULL, n, 100, true, "a byte sum in place");
    published(n, "a byte sum in place");
}

/* Sleep `ms` milliseconds. */
static void pause_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void staggered(uint8_t *dest, uint8_t *source, size_t n)
{
    shmem_barrier_all();
    pause_ms(me * 3);
    byte_sum(SHMEM_TEAM_WORLD, dest, source, n, 200, false, "a staggered byte sum");
    pause_ms((npes - 1 - me) * 3);
    byte_sum(SHMEM_TEAM_WORLD, dest, source, n, 201, false, "a second staggered byte sum");
    published(n, "a staggered byte sum");
}

static void back_to_back(shmem_team_t copy, uint8_t *dest[4], uint8_t *source[4], size_t large)
{
    shmem_barrier_all();
    /* The largest size the trees take, if any: a large reduction through them. */
    size_t tree = 0;
    for (int s = 0; s < SIZE_COUNT; s++)
        if (SIZES[s] > small && chosen(SIZES[s]).algorithm == OSOE_SHAPE_TREE)
            tree = SIZES[s];
    for (int round = 0; round < 8; round++) {
        int b = round % 4;
        byte_sum(SHMEM_TEAM_WORLD, dest[b], source[b], large, 300 + round, false,
                 "back to back, Träff's on the world");
        byte_sum(copy, dest[(b + 1) % 4], source[(b + 1) % 4], 40, 400 + round, false,
                 "back to back, a small sum on the copy");
        byte_sum(SHMEM_TEAM_WORLD, dest[(b + 2) % 4], source[(b + 2) % 4], 40, 500 + round,
                 false, "back to back, a small sum on the world");
        int root = round % npes;
        for (size_t i = 0; i < 64; i++)
            source[(b + 3) % 4][i] = byte_value(me, i, 600 + round);
        CHECK(shmem_broadcastmem(SHMEM_TEAM_WORLD, dest[(b + 3) % 4], source[(b + 3) % 4], 64,
                                 root) == 0);
        for (size_t i = 0; i < 64; i++)
            if (dest[(b + 3) % 4][i] != byte_value(root, i, 600 + round)) {
                fprintf(stderr, "PE %d: back to back, round %d: a broadcast's byte %zu is "
                        "wrong\n", me, round, i);
                failures++;
                break;
            }
        byte_sum(copy, dest[b], source[b], large, 700 + round, false,
                 "back to back, Träff's on the copy");
        if (tree)
            byte_sum(SHMEM_TEAM_WORLD, dest[(b + 1) % 4], source[(b + 1) % 4], tree,
                     800 + round, false, "back to back, a tree's large sum on the world");
        byte_sum(SHMEM_TEAM_WORLD, dest[(b + 2) % 4], source[(b + 2) % 4], large, 900 + round,
                 false, "back to back, Träff's on the world again");
    }
}

static void reuse(uint8_t *dest, uint8_t *source, size_t large)
{
    for (int round = 0; round < 3; round++) {
        shmem_team_t team = SHMEM_TEAM_INVALID;
        shmem_barrier_all();
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &team) ==
                    0);
        byte_sum(team, dest, source, large, 1100 + 2 * round, false,
                 "Träff's on a number taken again");
        byte_sum(team, dest, source, large, 1101 + 2 * round, false,
                 "Träff's again on a number taken again");
        shmem_team_destroy(team);
    }
}

/* Long double sums of `n` elements, fewer than the nodes: each element the sum
 * of small integers, exact in any order. */
static void few(long double *dest, long double *source, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        source[i] = (long double)(me * 3 + (int)i);
        dest[i] = 0;
    }
    shmem_barrier_all();
    CHECK(shmem_longdouble_sum_reduce(SHMEM_TEAM_WORLD, dest, source, (size_t)n) == 0);
    for (uint64_t i = 0; i < n; i++) {
        long double want = 0;
        for (int p = 0; p < npes; p++)
            want += (long double)(p * 3 + (int)i);
        if (dest[i] != want) {
            fprintf(stderr, "PE %d: a long double sum of %llu elements: element %llu is "
                    "wrong\n", me, (unsigned long long)n, (unsigned long long)i);
            failures++;
            break;
        }
    }
    if (published_on(0, nodes, n * sizeof(long double),
                     "a long double sum of fewer elements than nodes")) {
        circulant_calls++;
        few_calls++;
    }
}

/* ---- 2 and 9: every typed reduction, and the deprecated to_all -------------- */

/* Values a reduction of every type takes exactly, whatever order the nodes'
 * partials combine in. */
static int bits_value(int p, size_t i) { return (13 * p + 7 * (int)(i % 4093)) & 0x7f; }
static int order_value(int p, size_t i) { return (7 * p + 3 * (int)(i % 23)) % 23 - 11; }
static int sum_value(int p, size_t i) { return (p + 2 * (int)(i % 5)) % 3 - 1; }
static int sign_value(int p, size_t i) { return (p + (int)(i % 2)) % 2 ? -1 : 1; }
static int product_value(int p, size_t i)
{
    return p % 7 == 0 && i % 3 == 0 ? 2 : sign_value(p, i);
}

static void *type_source, *type_dest;
static size_t type_bytes;
static bool type_in_place;

/* The deprecated reductions' pSync and pWrk (spec Annex F): symmetric, pSync
 * SHMEM_SYNC_VALUE between calls, and pWrk of the specification's size for
 * the most elements a call makes, which the library never touches
 * (reductions.c). */
static long *to_all_sync;
static void *to_all_work;

static void type_failed(const char *name, const char *what, size_t i)
{
    fprintf(stderr, "PE %d: %s over %zu bytes%s: %s %zu\n", me, name, type_bytes,
            type_in_place ? ", in place" : "", what, i);
    failures++;
}

#define DIFFERENT_REAL(a, b) ((a) != (b))
#define DIFFERENT_COMPLEX(a, b) (creal(a) != creal(b) || cimag(a) != cimag(b))

/*
 * One case: a vector of type T of VALUE's, reduced by CALL - an expression
 * that makes the call from `dest`, `source` and `n`, and is true where it
 * returned as it should - compared with FOLD of every PE's, in the world's
 * order, element by element; then nothing written past the vector's end and,
 * with a dest of its own, the source as it was. VALUE depends on the element
 * by its remainder mod PERIOD alone, so the fold is made once for each
 * remainder, not for each element.
 */
#define REDUCTION_CASE(FUNCTION, TEXT, T, VALUE, PERIOD, FOLD, DIFFERENT, CALL)       \
    static void FUNCTION(void)                                                        \
    {                                                                                 \
        size_t n = type_bytes / sizeof(T);                                            \
        T *source = type_source, *dest = type_in_place ? type_source : type_dest;     \
        T *folded = malloc(PERIOD * sizeof *folded);                                  \
        CHECK_FATAL(folded != NULL);                                                  \
        for (size_t i = 0; i < n; i++)                                                \
            source[i] = (T)VALUE(me, i);                                              \
        memset(dest + n, GUARD_BYTE, GUARD);                                          \
        CHECK(CALL);                                                                  \
        for (size_t i = 0; i < PERIOD && i < n; i++) {                                \
            T acc = (T)VALUE(0, i);                                                   \
            for (int p = 1; p < npes; p++) {                                          \
                T v = (T)VALUE(p, i);                                                 \
                acc = (T)(FOLD);                                                      \
            }                                                                         \
            folded[i] = acc;                                                          \
        }                                                                             \
        for (size_t i = 0; i < n; i++)                                                \
            if (DIFFERENT(dest[i], folded[i % PERIOD])) {                             \
                type_failed(TEXT, "element", i);                                      \
                break;                                                                \
            }                                                                         \
        for (size_t b = 0; b < GUARD; b++)                                            \
            if (((unsigned char *)(dest + n))[b] != GUARD_BYTE) {                     \
                type_failed(TEXT, "wrote past its end, at byte", n * sizeof(T) + b);  \
                break;                                                                \
            }                                                                         \
        for (size_t i = 0; !type_in_place && i < n; i++)                              \
            if (DIFFERENT(source[i], (T)VALUE(me, i))) {                              \
                type_failed(TEXT, "the source changed at element", i);                \
                break;                                                                \
            }                                                                         \
        free(folded);                                                                 \
    }

/* The remainders VALUE depends on its element by, and the least period of
 * each; 4093 is bits_value's, 23 order_value's, 5 sum_value's, and the sign
 * and the product's 2 and 6. */
#define PERIOD_bits_value 4093
#define PERIOD_order_value 23
#define PERIOD_sum_value 5
#define PERIOD_sign_value 2
#define PERIOD_product_value 6
#define PERIOD_complex_sum_value 5
#define PERIOD_complex_product_value 4

#define INTEGER_CASE(T, NAME, OPNAME, VALUE, FOLD)                                    \
    REDUCTION_CASE(NAME##_##OPNAME, "shmem_" #NAME "_" #OPNAME "_reduce", T, VALUE,   \
                   PERIOD_##VALUE, FOLD, DIFFERENT_REAL,                              \
                   shmem_##NAME##_##OPNAME##_reduce(SHMEM_TEAM_WORLD, dest, source, n) == 0)

#define BITWISE_CASES(T, NAME)                                                    \
    INTEGER_CASE(T, NAME, and, bits_value, acc & v)                               \
    INTEGER_CASE(T, NAME, or, bits_value, acc | v)                                \
    INTEGER_CASE(T, NAME, xor, bits_value, acc ^ v)
#define ORDERED_CASES(T, NAME)                                                    \
    INTEGER_CASE(T, NAME, max, order_value, v > acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, min, order_value, v < acc ? v : acc)
#define INTEGER_ARITHMETIC_CASES(T, NAME)                                         \
    INTEGER_CASE(T, NAME, sum, sum_value, acc + v)                                \
    INTEGER_CASE(T, NAME, prod, sign_value, acc * v)
#define FLOATING_CASES(T, NAME)                                                   \
    INTEGER_CASE(T, NAME, max, order_value, v > acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, min, order_value, v < acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, sum, sum_value, acc + v)                                \
    INTEGER_CASE(T, NAME, prod, product_value, acc * v)

OSOE_REDUCE_BITWISE_TYPES(BITWISE_CASES)
OSOE_REDUCE_INTEGER_TYPES(ORDERED_CASES)
OSOE_REDUCE_INTEGER_TYPES(INTEGER_ARITHMETIC_CASES)
OSOE_REDUCE_FLOATING_TYPES(FLOATING_CASES)

/* The complex values: a sum's of small integers, a product's a power of i. */
static double complex complex_sum_value(int p, size_t i)
{
    return (double)sum_value(p, i) + (double)sum_value(p + 1, i) * I;
}

static double complex complex_product_value(int p, size_t i)
{
    static const double complex power[4] = { 1, I, -1, -I };
    return power[(p + (int)(i % 4)) % 4];
}

#define COMPLEX_CASE(T, NAME, OPNAME, VALUE, FOLD)                                    \
    REDUCTION_CASE(NAME##_##OPNAME, "shmem_" #NAME "_" #OPNAME "_reduce", T, VALUE,   \
                   PERIOD_##VALUE, FOLD, DIFFERENT_COMPLEX,                           \
                   shmem_##NAME##_##OPNAME##_reduce(SHMEM_TEAM_WORLD, dest, source, n) == 0)
#define COMPLEX_CASES(T, NAME)                                                    \
    COMPLEX_CASE(T, NAME, sum, complex_sum_value, acc + v)                        \
    COMPLEX_CASE(T, NAME, prod, complex_product_value, acc * v)
OSOE_REDUCE_COMPLEX_TYPES(COMPLEX_CASES)

/* The deprecated forms, Table 11's types and operations over the whole job
 * (reductions.c: the world's reduction), the same values and folds. */
#define TO_ALL_CASE(T, NAME, OPNAME, VALUE, FOLD, DIFFERENT)                          \
    REDUCTION_CASE(to_all_##NAME##_##OPNAME, "shmem_" #NAME "_" #OPNAME "_to_all", T, \
                   VALUE, PERIOD_##VALUE, FOLD, DIFFERENT,                            \
                   (shmem_##NAME##_##OPNAME##_to_all(dest, source, (int)n, 0, 0, npes,  \
                                                     to_all_work, to_all_sync), true))

#define TO_ALL_INTEGER_CASES(T, NAME)                                                 \
    TO_ALL_CASE(T, NAME, and, bits_value, acc & v, DIFFERENT_REAL)                    \
    TO_ALL_CASE(T, NAME, or, bits_value, acc | v, DIFFERENT_REAL)                     \
    TO_ALL_CASE(T, NAME, xor, bits_value, acc ^ v, DIFFERENT_REAL)                    \
    TO_ALL_CASE(T, NAME, max, order_value, v > acc ? v : acc, DIFFERENT_REAL)         \
    TO_ALL_CASE(T, NAME, min, order_value, v < acc ? v : acc, DIFFERENT_REAL)         \
    TO_ALL_CASE(T, NAME, sum, sum_value, acc + v, DIFFERENT_REAL)                     \
    TO_ALL_CASE(T, NAME, prod, sign_value, acc * v, DIFFERENT_REAL)
#define TO_ALL_FLOATING_CASES(T, NAME)                                                \
    TO_ALL_CASE(T, NAME, max, order_value, v > acc ? v : acc, DIFFERENT_REAL)         \
    TO_ALL_CASE(T, NAME, min, order_value, v < acc ? v : acc, DIFFERENT_REAL)         \
    TO_ALL_CASE(T, NAME, sum, sum_value, acc + v, DIFFERENT_REAL)                     \
    TO_ALL_CASE(T, NAME, prod, product_value, acc * v, DIFFERENT_REAL)
#define TO_ALL_COMPLEX_CASES(T, NAME)                                                 \
    TO_ALL_CASE(T, NAME, sum, complex_sum_value, acc + v, DIFFERENT_COMPLEX)          \
    TO_ALL_CASE(T, NAME, prod, complex_product_value, acc * v, DIFFERENT_COMPLEX)

OSOE_TO_ALL_INTEGER_TYPES(TO_ALL_INTEGER_CASES)
OSOE_TO_ALL_FLOATING_TYPES(TO_ALL_FLOATING_CASES)
OSOE_TO_ALL_COMPLEX_TYPES(TO_ALL_COMPLEX_CASES)

/* Every case, in a table: its function, its element's width, its name, and
 * whether it is a sum (the cases a size of several pieces is run for). */
struct type_case {
    void (*run)(void);
    size_t width;
    const char *name;
    bool sum;
};

#define BITWISE_ROWS(T, NAME)                                                     \
    { NAME##_and, sizeof(T), #NAME " and", false },                               \
    { NAME##_or, sizeof(T), #NAME " or", false },                                 \
    { NAME##_xor, sizeof(T), #NAME " xor", false },
#define INTEGER_ROWS(T, NAME)                                                     \
    { NAME##_max, sizeof(T), #NAME " max", false },                               \
    { NAME##_min, sizeof(T), #NAME " min", false },                               \
    { NAME##_sum, sizeof(T), #NAME " sum", true },                                \
    { NAME##_prod, sizeof(T), #NAME " prod", false },
#define COMPLEX_ROWS(T, NAME)                                                     \
    { NAME##_sum, sizeof(T), #NAME " sum", true },                                \
    { NAME##_prod, sizeof(T), #NAME " prod", false },

static const struct type_case TYPE_CASES[] = {
    OSOE_REDUCE_BITWISE_TYPES(BITWISE_ROWS)
    OSOE_REDUCE_INTEGER_TYPES(INTEGER_ROWS)
    OSOE_REDUCE_FLOATING_TYPES(INTEGER_ROWS)
    OSOE_REDUCE_COMPLEX_TYPES(COMPLEX_ROWS)
};
enum { TYPE_CASE_COUNT = sizeof TYPE_CASES / sizeof TYPE_CASES[0] };

#define TO_ALL_INTEGER_ROWS(T, NAME)                                              \
    { to_all_##NAME##_and, sizeof(T), #NAME " and to_all", false },               \
    { to_all_##NAME##_or, sizeof(T), #NAME " or to_all", false },                 \
    { to_all_##NAME##_xor, sizeof(T), #NAME " xor to_all", false },               \
    { to_all_##NAME##_max, sizeof(T), #NAME " max to_all", false },               \
    { to_all_##NAME##_min, sizeof(T), #NAME " min to_all", false },               \
    { to_all_##NAME##_sum, sizeof(T), #NAME " sum to_all", true },                \
    { to_all_##NAME##_prod, sizeof(T), #NAME " prod to_all", false },
#define TO_ALL_FLOATING_ROWS(T, NAME)                                             \
    { to_all_##NAME##_max, sizeof(T), #NAME " max to_all", false },               \
    { to_all_##NAME##_min, sizeof(T), #NAME " min to_all", false },               \
    { to_all_##NAME##_sum, sizeof(T), #NAME " sum to_all", true },                \
    { to_all_##NAME##_prod, sizeof(T), #NAME " prod to_all", false },
#define TO_ALL_COMPLEX_ROWS(T, NAME)                                              \
    { to_all_##NAME##_sum, sizeof(T), #NAME " sum to_all", true },                \
    { to_all_##NAME##_prod, sizeof(T), #NAME " prod to_all", false },

static const struct type_case TO_ALL_CASES[] = {
    OSOE_TO_ALL_INTEGER_TYPES(TO_ALL_INTEGER_ROWS)
    OSOE_TO_ALL_FLOATING_TYPES(TO_ALL_FLOATING_ROWS)
    OSOE_TO_ALL_COMPLEX_TYPES(TO_ALL_COMPLEX_ROWS)
};
enum { TO_ALL_CASE_COUNT = sizeof TO_ALL_CASES / sizeof TO_ALL_CASES[0] };

/* Every case of `table` at `bytes` - which takes Träff's, or the tree at a size
 * that does not - each of whole elements, with a dest of its own or in place,
 * and the sums alone where `sums_only`. Each is read as the world's reduction,
 * after a small one that clears what its block says, and which, being a
 * reduction over every PE, is also the barrier that keeps one PE's read of the
 * last case's block from a lead's publishing of this one's. */
static void run_cases(const struct type_case *table, int count, size_t bytes, bool in_place,
                      bool sums_only)
{
    type_in_place = in_place;
    shmem_barrier_all();
    for (int c = 0; c < count; c++) {
        if (sums_only && !table[c].sum)
            continue;
        type_bytes = bytes / table[c].width * table[c].width;
        clear_published(&everyone);
        table[c].run();
        published(type_bytes, table[c].name);
    }
}

static void types(size_t bytes, bool in_place, bool sums_only)
{
    run_cases(TYPE_CASES, TYPE_CASE_COUNT, bytes, in_place, sums_only);
}

/* The same through shmem_TYPE_OP_to_all; pSync must come back as it was. */
static void to_all(size_t bytes, bool in_place, bool sums_only)
{
    run_cases(TO_ALL_CASES, TO_ALL_CASE_COUNT, bytes, in_place, sums_only);
    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
        if (to_all_sync[i] != SHMEM_SYNC_VALUE) {
            fprintf(stderr, "PE %d: pSync[%d] is %ld after the _to_all calls, not %ld\n", me, i,
                    to_all_sync[i], (long)SHMEM_SYNC_VALUE);
            failures++;
            break;
        }
}

/* ---- threads ----------------------------------------------------------------- */

enum { THREADS = 2, THREAD_ROUNDS = 6 };

struct thread_work {
    shmem_team_t team;
    shmem_team_t shared;                /* the copy the threads take in turn */
    pthread_barrier_t *turns;
    uint8_t *dest, *source;
    size_t bytes;
    int index;
    int failed;
};

/* One byte sum on `team` from a thread, checked; tagged by `tag`. */
static void thread_sum(struct thread_work *w, shmem_team_t team, int tag, int round)
{
    for (size_t i = 0; i < w->bytes; i++) {
        w->source[i] = byte_value(me, i, tag);
        w->dest[i] = 0;
    }
    if (shmem_uint8_sum_reduce(team, w->dest, w->source, w->bytes) != 0)
        w->failed = 1;
    for (size_t i = 0; i < w->bytes && !w->failed; i++) {
        uint8_t want = 0;
        for (int p = 0; p < npes; p++)
            want = (uint8_t)(want + byte_value(p, i, tag));
        if (w->dest[i] != want) {
            fprintf(stderr, "PE %d, thread %d, round %d: byte %zu of %zu is %u, not %u\n", me,
                    w->index, round, i, w->bytes, w->dest[i], want);
            w->failed = 1;
        }
    }
}

static void *thread_main(void *argument)
{
    struct thread_work *w = argument;
    for (int round = 0; round < THREAD_ROUNDS && !w->failed; round++)
        thread_sum(w, w->team, 1000 + 10 * w->index + round, round);
    /* In turn on the shared copy: round r's reduction is thread r mod THREADS's
     * on every PE, the others waiting at the barrier, so one call at a time
     * on the team, as the specification asks, each from another thread. */
    for (int round = 0; round < 2 * THREADS * THREAD_ROUNDS; round++) {
        if (round % THREADS == w->index && !w->failed)
            thread_sum(w, w->shared, 2000 + round, round);
        pthread_barrier_wait(w->turns);
    }
    return NULL;
}

/* The lanes mode: helpers that claim a thread record each and hold it until
 * released, and the thread that then reduces. */
static pthread_barrier_t claimed, released;

static void *helper_main(void *argument)
{
    (void)argument;
    shmem_quiet();                      /* a counted call: claims this thread's record */
    pthread_barrier_wait(&claimed);
    pthread_barrier_wait(&released);
    return NULL;
}

static uint64_t reducer_lane = UINT64_MAX;

static void *reducer_main(void *argument)
{
    uint8_t **buffers = argument;
    sizes(buffers[0], buffers[1]);
    CHECK(osoe_test_scoped_metric("team.tree.lane", OSOE_MSCOPE_TEAM, 0, &reducer_lane));
    return NULL;
}

static void lanes(void)
{
    int provided = 0;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    world_known();
    CHECK_FATAL((uint64_t)npes == nodes);           /* one PE a node: its node is its rank */
    size_t most = SIZES[SIZE_COUNT - 1];
    uint8_t *buffers[2] = { shmem_malloc(most + GUARD), shmem_malloc(most + GUARD) };
    CHECK_FATAL(buffers[0] && buffers[1]);
    pthread_t helper[64], reducer;
    CHECK_FATAL(me < 64);
    CHECK_FATAL(pthread_barrier_init(&claimed, NULL, (unsigned)me + 1) == 0);
    CHECK_FATAL(pthread_barrier_init(&released, NULL, (unsigned)me + 1) == 0);
    for (int h = 0; h < me; h++)
        CHECK_FATAL(pthread_create(&helper[h], NULL, helper_main, NULL) == 0);
    pthread_barrier_wait(&claimed);
    shmem_barrier_all();
    CHECK_FATAL(pthread_create(&reducer, NULL, reducer_main, buffers) == 0);
    CHECK(pthread_join(reducer, NULL) == 0);
    pthread_barrier_wait(&released);
    for (int h = 0; h < me; h++)
        CHECK(pthread_join(helper[h], NULL) == 0);
    /* Every node's lead lent a lane of its own, or the test proved nothing. */
    uint64_t *seen = shmem_calloc((size_t)npes, sizeof *seen);
    CHECK_FATAL(seen != NULL);
    for (int p = 0; p < npes; p++)
        shmem_uint64_p(&seen[me], reducer_lane, p);
    shmem_barrier_all();
    for (int p = 0; p < npes; p++)
        for (int q = p + 1; q < npes; q++)
            if (seen[p] == seen[q]) {
                fprintf(stderr, "PE %d: the leads of nodes %d and %d lent the same lane, %llu\n",
                        me, p, q, (unsigned long long)seen[p]);
                failures++;
            }
    shmem_barrier_all();
    shmem_free(seen);
}

/* The diverge mode: PE 1 reduces a byte more than the others, at a size every
 * node takes Träff's for. */
static void diverge(uint64_t smallest)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(smallest != 0);
    size_t n = (size_t)smallest * 4 + (me == 1);
    /* Room for the larger on every PE: allocations of different sizes end the
     * job by name before the reduction is reached (P23). */
    size_t room = (size_t)smallest * 4 + 2;
    uint8_t *source = shmem_malloc(room), *dest = shmem_malloc(room);
    CHECK_FATAL(source && dest);
    memset(source, 1, n);
    shmem_uint8_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n);
    fprintf(stderr, "PE %d: a divergent reduction returned\n", me);
    CHECK(0);
}

static int threads(size_t bytes)
{
    int provided = 0;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    struct thread_work work[THREADS];
    pthread_t thread[THREADS];
    pthread_barrier_t turns;
    CHECK_FATAL(pthread_barrier_init(&turns, NULL, THREADS) == 0);
    shmem_team_t shared = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &shared) == 0);
    for (int t = 0; t < THREADS; t++) {
        work[t] = (struct thread_work){ SHMEM_TEAM_INVALID, shared, &turns, shmem_malloc(bytes),
                                        shmem_malloc(bytes), bytes, t, 0 };
        CHECK_FATAL(work[t].dest && work[t].source);
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0,
                                             &work[t].team) == 0);
    }
    shmem_barrier_all();
    for (int t = 0; t < THREADS; t++)
        CHECK_FATAL(pthread_create(&thread[t], NULL, thread_main, &work[t]) == 0);
    for (int t = 0; t < THREADS; t++) {
        CHECK(pthread_join(thread[t], NULL) == 0);
        failures += work[t].failed;
    }
    shmem_barrier_all();
    for (int t = 0; t < THREADS; t++)
        shmem_team_destroy(work[t].team);
    shmem_team_destroy(shared);
    pthread_barrier_destroy(&turns);
    return 0;
}

/* ---- 8 and 10: teams that are not the world, and sums that round ------------- */

/*
 * The values of 10: a table of 4,096 doubles of either sign and all 53 bits,
 * each PE and component taking one, scaled by a power of two, so that the
 * sums of different PEs' values depend on the order their parts are made in.
 * A complex number's two parts are components 2i and 2i + 1. Made once, as
 * the job starts: the check makes a value for every member and component of
 * every call.
 */
enum { ROUNDING_TABLE_SIZE = 4096 };
static double rounding_table[ROUNDING_TABLE_SIZE];

static void rounding_table_make(void)
{
    for (uint64_t k = 0; k < ROUNDING_TABLE_SIZE; k++) {
        uint64_t x = (k + 1) * UINT64_C(0x9e3779b97f4a7c15);
        x ^= x >> 31;
        x *= UINT64_C(0x94d049bb133111eb);
        x ^= x >> 29;
        rounding_table[k] = ((double)(x >> 11) / 9007199254740992.0 - 0.5) * 1e3;
    }
}

static double rounding_value(int pe, size_t component)
{
    static const double scale[8] = { 1.0, 0.25, 4.0, 1.0, 16.0, 0.5, 2.0, 8.0 };
    return rounding_table[(component + 1031 * (size_t)pe) % ROUNDING_TABLE_SIZE] *
           scale[(component + (size_t)pe) % 8];
}

/* a + b as a sum and the error of its rounding, exactly (Knuth's two-sum). */
static void two_sum(double a, double b, double *sum, double *error)
{
    double s = a + b, virtual_b = s - a;
    *error = (a - (s - virtual_b)) + (b - virtual_b);
    *sum = s;
}

/*
 * A result's components against the group's members' values: how many are
 * further from the exact sum than any order of rounding takes them (the bound
 * every order of n - 1 additions keeps, with room; the sum itself made exactly
 * enough with two-sums), and, in `sensitive`, how many of the members' values
 * sum to other bits in their order and in its reverse - the inputs must still
 * depend on the order, on a group of three or more, or alike bits would show
 * nothing.
 */
#define ROUNDING_CHECK(FUNCTION, T, EPSILON)                                          \
    static size_t FUNCTION(const struct group *g, const T *result, size_t components, \
                           size_t *sensitive)                                         \
    {                                                                                 \
        T *values = malloc((size_t)g->size * sizeof *values);                         \
        CHECK_FATAL(values != NULL);                                                  \
        size_t far = 0;                                                               \
        *sensitive = 0;                                                               \
        for (size_t k = 0; k < components; k++) {                                     \
            double high = 0, low = 0, magnitude = 0;                                  \
            for (int r = 0; r < g->size; r++) {                                       \
                double sum, error;                                                    \
                values[r] = (T)rounding_value(member_pe(g, r), k);                    \
                two_sum(high, (double)values[r], &sum, &error);                       \
                high = sum;                                                           \
                low += error;                                                         \
                magnitude += values[r] < 0 ? -(double)values[r] : (double)values[r];  \
            }                                                                         \
            T forward = values[0], backward = values[g->size - 1];                    \
            for (int r = 1; r < g->size; r++) {                                       \
                forward = (T)(forward + values[r]);                                   \
                backward = (T)(backward + values[g->size - 1 - r]);                   \
            }                                                                         \
            *sensitive += forward != backward;                                        \
            double difference = ((double)result[k] - high) - low;                     \
            if ((difference < 0 ? -difference : difference) >                         \
                (double)g->size * EPSILON * magnitude)                                \
                far++;                                                                \
        }                                                                             \
        free(values);                                                                 \
        return far;                                                                   \
    }

ROUNDING_CHECK(check_rounded_doubles, double, 2.3e-16)
ROUNDING_CHECK(check_rounded_floats, float, 1.3e-7)

static uint64_t *hash_in, *hash_most, *hash_least;      /* symmetric: a hash, and its extremes */

/* The members' results alike to the bit: each hashes its bytes, and the team's
 * greatest hash and its least must be one. */
static void members_agree(const struct group *g, const void *result, size_t bytes,
                          const char *what)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const unsigned char *p = result;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= p[i];
        hash *= UINT64_C(0x100000001b3);
    }
    *hash_in = hash;
    CHECK(shmem_uint64_max_reduce(g->team, hash_most, hash_in, 1) == 0);
    CHECK(shmem_uint64_min_reduce(g->team, hash_least, hash_in, 1) == 0);
    if (*hash_most != *hash_least) {
        fprintf(stderr, "PE %d: %s: the members' results differ in their bits\n", me, what);
        failures++;
    }
}

enum rounding_kind { ROUNDING_DOUBLE, ROUNDING_FLOAT, ROUNDING_COMPLEX, ROUNDING_KINDS };

static const char *const ROUNDING_NAMES[ROUNDING_KINDS] = {
    "a sum of doubles that rounds", "a sum of floats that rounds",
    "a sum of complex doubles that rounds"
};
static const size_t ROUNDING_WIDTHS[ROUNDING_KINDS] = { sizeof(double), sizeof(float),
                                                        sizeof(double complex) };

/* The call of a group's reduction that took the algorithm it should, counted:
 * the world's with Träff's, a team's apart, with how many the chooser gave
 * Träff's to. */
static void pin_call(const struct group *g, uint64_t bytes, const char *what)
{
    bool wanted = g->span >= 2 && bytes > small &&
                  chosen_over(g->span, bytes).algorithm == OSOE_SHAPE_CIRCULANT;
    bool took = published_by(g, bytes, what);
    if (g->number == 0) {
        circulant_calls += took;
        return;
    }
    team_wanted += wanted;
    team_calls += took;
}

/*
 * A sum of about `bytes` bytes of the kind, over the group's team, by a member:
 * each component within what rounding allows of the exact sum, nothing past
 * the vector's end, the algorithm the chooser's, and every member's bits the
 * same - the check of 10.
 */
static void rounding_sum(const struct group *g, enum rounding_kind kind, size_t bytes,
                         bool in_place, uint8_t *dest_bytes, uint8_t *source_bytes, int family)
{
    size_t width = ROUNDING_WIDTHS[kind], n = bytes / width;
    size_t components = kind == ROUNDING_COMPLEX ? 2 * n : n;
    unsigned char *source = source_bytes, *dest = in_place ? source_bytes : dest_bytes;
    char what[128];
    snprintf(what, sizeof what, "%s on %s", ROUNDING_NAMES[kind], g->name);
    for (size_t k = 0; k < components; k++) {
        if (kind == ROUNDING_FLOAT)
            ((float *)(void *)source)[k] = (float)rounding_value(me, k);
        else
            ((double *)(void *)source)[k] = rounding_value(me, k);
    }
    memset(dest + n * width, GUARD_BYTE, GUARD);
    clear_published(g);
    switch (kind) {
    case ROUNDING_DOUBLE:
        CHECK(shmem_double_sum_reduce(g->team, (double *)(void *)dest,
                                      (const double *)(void *)source, n) == 0);
        break;
    case ROUNDING_FLOAT:
        CHECK(shmem_float_sum_reduce(g->team, (float *)(void *)dest,
                                     (const float *)(void *)source, n) == 0);
        break;
    default:
        CHECK(shmem_complexd_sum_reduce(g->team, (double complex *)(void *)dest,
                                        (const double complex *)(void *)source, n) == 0);
    }
    pin_call(g, n * width, what);
    size_t sensitive = 0;
    size_t far = kind == ROUNDING_FLOAT
                     ? check_rounded_floats(g, (const float *)(void *)dest, components, &sensitive)
                     : check_rounded_doubles(g, (const double *)(void *)dest, components,
                                             &sensitive);
    if (far != 0) {
        fprintf(stderr, "PE %d: %s of %zu bytes, family %d: %zu of %zu components are further "
                "from the sum than rounding takes them\n", me, what, n * width, family, far,
                components);
        failures++;
    }
    if (g->size >= 3 && sensitive == 0) {
        fprintf(stderr, "PE %d: %s of %zu bytes: no component's sum depends on the order it is "
                "made in, so alike bits show nothing\n", me, what, n * width);
        failures++;
    }
    for (size_t b = 0; b < GUARD; b++)
        if (dest[n * width + b] != GUARD_BYTE) {
            fprintf(stderr, "PE %d: %s of %zu bytes wrote past its end\n", me, what, n * width);
            failures++;
            break;
        }
    members_agree(g, dest, n * width, what);
}

/*
 * The sizes of a group's byte sums, from the small class's edge up: a byte
 * past it; a byte past the least that takes Träff's over the group's nodes (a
 * tree's, 4 KiB and a byte, where none does); twice that and three; four
 * pieces and a byte; and sixteen and three bytes. All odd, so that an even
 * node count's blocks have remainders.
 */
enum { FAMILY_STEPS = 5 };

static uint64_t step_bytes(const struct group *g, int step)
{
    uint64_t smallest = smallest_circulant_over(g->span);
    uint64_t least = smallest ? smallest : 4096;
    switch (step) {
    case 0:
        return small + 1;
    case 1:
        return least + 1;
    case 2:
        return 2 * least + 3;
    case 3:
        return 4 * OSOE_INTERNAL_PIECE_BYTES + 1;
    default:
        return 16 * OSOE_INTERNAL_PIECE_BYTES + 3;
    }
}

/*
 * One family of teams with no PE in two of them, as every PE of the job
 * enters it: byte sums at the steps' sizes, then sums that round, by the PE's
 * own team of the family - `mine`, or NULL for a PE in none. The teams of a
 * family run their steps at once. A step's small reduction (clear_published)
 * is a reduction over the team's members, none of which returns before all
 * have entered it, and so after reading what the step before published; and
 * a node's lead publishes a large call once its node's members have entered
 * it, after that - so no lead's publication overtakes a member's read, and no
 * barrier over the job is needed between the steps.
 */
static void run_family(const struct group *mine, int family, uint8_t *dest, uint8_t *source)
{
    shmem_barrier_all();
    if (!mine)
        return;
    check_record(mine);
    for (int step = 0; step < FAMILY_STEPS; step++) {
        uint64_t bytes = step_bytes(mine, step);
        char what[128];
        snprintf(what, sizeof what, "a byte sum on %s", mine->name);
        clear_published(mine);
        group_byte_sum(mine, dest, source, bytes, 2000 + 16 * family + step,
                       (family + step) % 2 != 0, what);
        pin_call(mine, bytes, what);
    }
    /* The least size that takes Träff's, and four pieces. */
    static const int rounded_steps[] = { 1, 3 };
    for (int s = 0; s < 2; s++)
        for (int kind = 0; kind < ROUNDING_KINDS; kind++)
            rounding_sum(mine, (enum rounding_kind)kind, step_bytes(mine, rounded_steps[s]),
                         (family + s + kind) % 2 != 0, dest, source, family);
}

/* The group of a strided team of the world, made by every PE, for a PE in it;
 * a team of fewer than two is not made, and nobody is in it. */
static struct group made_strided(const char *name, int start, int stride, int size,
                                 uint32_t number)
{
    struct group g = { .name = name, .team = SHMEM_TEAM_INVALID, .number = number, .rank = -1 };
    if (size < 2)
        return g;
    g.size = size;
    g.pes = malloc((size_t)size * sizeof *g.pes);
    CHECK_FATAL(g.pes != NULL);
    for (int r = 0; r < size; r++)
        g.pes[r] = start + r * stride;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, start, stride, size, NULL, 0,
                                         &g.team) == 0);
    measure(&g);
    CHECK_FATAL((g.rank >= 0) == (g.team != SHMEM_TEAM_INVALID));
    if (g.rank >= 0) {
        CHECK_FATAL(shmem_team_n_pes(g.team) == size && shmem_team_my_pe(g.team) == g.rank);
        for (int r = 0; r < size; r++)
            CHECK_FATAL(shmem_team_translate_pe(g.team, r, SHMEM_TEAM_WORLD) == g.pes[r]);
    }
    return g;
}

/* This PE's row and column of a grid `xrange` PEs wide, from a split that
 * every PE makes; a row or a column of one PE is nobody's team here, though
 * the split makes it. The members as the arithmetic of a grid gives them are
 * checked against the library's. */
static void made_grid(int xrange, struct group *row, struct group *column)
{
    shmem_team_t x = SHMEM_TEAM_INVALID, y = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_2d(SHMEM_TEAM_WORLD, xrange, NULL, 0, &x, NULL, 0, &y) == 0);
    int row_first = me / xrange * xrange, column_first = me % xrange;
    *row = (struct group){ .name = "its row of a grid", .team = x, .number = 3,
                           .size = npes - row_first < xrange ? npes - row_first : xrange };
    *column = (struct group){ .name = "its column of a grid", .team = y, .number = 4,
                              .size = (npes - 1 - column_first) / xrange + 1 };
    row->pes = malloc((size_t)row->size * sizeof *row->pes);
    column->pes = malloc((size_t)column->size * sizeof *column->pes);
    CHECK_FATAL(row->pes != NULL && column->pes != NULL);
    for (int r = 0; r < row->size; r++)
        row->pes[r] = row_first + r;
    for (int r = 0; r < column->size; r++)
        column->pes[r] = column_first + r * xrange;
    CHECK_FATAL(shmem_team_n_pes(x) == row->size && shmem_team_n_pes(y) == column->size);
    for (int r = 0; r < row->size; r++)
        CHECK_FATAL(shmem_team_translate_pe(x, r, SHMEM_TEAM_WORLD) == row->pes[r]);
    for (int r = 0; r < column->size; r++)
        CHECK_FATAL(shmem_team_translate_pe(y, r, SHMEM_TEAM_WORLD) == column->pes[r]);
    measure(row);
    measure(column);
}

/* A group given up: its team destroyed by its member, its members forgotten. */
static void unmade(struct group *g)
{
    if (g->team != SHMEM_TEAM_INVALID)
        shmem_team_destroy(g->team);
    free(g->pes);
    g->pes = NULL;
    g->team = SHMEM_TEAM_INVALID;
}

/* The group of a pair that this PE is in, if any, and big enough to reduce on. */
static const struct group *member_of(const struct group *a, const struct group *b)
{
    if (a->rank >= 0)
        return a;
    return b && b->rank >= 0 ? b : NULL;
}

/*
 * 8 and 10: the world and then each family of teams, one at a time - made by
 * every PE, run, destroyed - so that a PE holds at most one team beyond the
 * predefined three (a grid's rows and columns, two, where OSOE_MAX_TEAMS lets
 * it) and the first takes number 3.
 */
static void teams_and_rounding(uint8_t *dest, uint8_t *source)
{
    int family = 0;
    run_family(&everyone, family++, dest, source);
    if (npes >= 4) {
        struct group evens = made_strided("the even PEs", 0, 2, (npes + 1) / 2, 3);
        struct group odds = made_strided("the odd PEs", 1, 2, npes / 2, 3);
        run_family(member_of(&evens, &odds), family++, dest, source);
        unmade(&evens);
        unmade(&odds);
    }
    if (npes >= 3) {
        struct group tail = made_strided("all but the first PE", 1, 1, npes - 1, 3);
        run_family(member_of(&tail, NULL), family++, dest, source);
        unmade(&tail);
        struct group head = made_strided("all but the last PE", 0, 1, npes - 1, 3);
        run_family(member_of(&head, NULL), family++, dest, source);
        unmade(&head);
    }
    if (npes >= 4) {
        struct group thirds = made_strided("every third PE", 0, 3, (npes + 2) / 3, 3);
        run_family(member_of(&thirds, NULL), family++, dest, source);
        unmade(&thirds);
    }
    struct group reversed = made_strided("the world in reverse", npes - 1, -1, npes, 3);
    run_family(member_of(&reversed, NULL), family++, dest, source);
    unmade(&reversed);
    /* A grid's rows and columns hold two numbers at once. */
    if (npes >= 4 && setting("OSOE_MAX_TEAMS", 256) >= 5) {
        int side = 1;
        while ((uint64_t)side * (uint64_t)side < nodes)
            side++;
        struct group row, column;
        made_grid(per_node * side, &row, &column);
        run_family(row.size >= 2 ? &row : NULL, family++, dest, source);
        run_family(column.size >= 2 ? &column : NULL, family++, dest, source);
        unmade(&row);
        unmade(&column);
    }
}

/* The progress helpers this PE's node runs: the owners its lead's control
 * segment holds (layout 36), less the agent's; -1 where they cannot be read. */
static int node_helpers(void)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "PE %d: %s\n", me, err);
        return -1;
    }
    int helpers = s.owner_count > 0 ? (int)s.owner_count - 1 : -1;
    osoe_segment_close(&s);
    return helpers;
}

/*
 * With `helpers-differ`: what each node runs is what its ranks' environment
 * says, the launcher's `-e RANK:OSOE_PROGRESS_HELPERS=n` for each rank of a
 * node alike, and two nodes run different numbers - or every phase below
 * would run on nodes alike and show nothing of the difference. Gives the
 * least and the greatest.
 */
static void helper_counts(int *least, int *greatest)
{
    int helpers = node_helpers();
    const char *said = getenv("OSOE_PROGRESS_HELPERS");
    int *counts = shmem_malloc(3 * sizeof *counts);
    CHECK_FATAL(counts != NULL);
    CHECK(helpers >= 0);
    CHECK(said != NULL && atoi(said) == helpers);
    counts[0] = helpers;
    CHECK(shmem_int_max_reduce(SHMEM_TEAM_WORLD, &counts[1], &counts[0], 1) == 0);
    CHECK(shmem_int_min_reduce(SHMEM_TEAM_WORLD, &counts[2], &counts[0], 1) == 0);
    CHECK(counts[1] != counts[2]);
    *greatest = counts[1];
    *least = counts[2];
    shmem_free(counts);
}

int main(int argc, char **argv)
{
    const char *mode_argument = argc > 1 ? argv[1] : "";
    bool threaded = strcmp(mode_argument, "threads") == 0;
    bool lanes_mode = strcmp(mode_argument, "lanes") == 0;
    bool diverge_mode = strcmp(mode_argument, "diverge") == 0;
    bool tiny = strcmp(mode_argument, "tiny") == 0;
    bool differ = strcmp(mode_argument, "helpers-differ") == 0;
    CHECK_FATAL(argc == 1 || threaded || lanes_mode || diverge_mode || tiny || differ);
    const char *step_nodes = osoe_test_launcher("STEP_NUM_NODES");
    nodes = strtoull(step_nodes ? step_nodes : "1", NULL, 10);
    link = (struct osoe_link){ setting("OSOE_LINK_HOP_NS", 0), setting("OSOE_LINK_MESSAGE_NS", 0),
                               setting("OSOE_LINK_BYTES_PER_SECOND", 0) };
    ceiling = setting("OSOE_COLLECTIVE_FANOUT", 8);
    capacity = setting("OSOE_COLLECTIVE_PIPELINE", 4);
    small = setting("OSOE_SMALL_COLLECTIVE_BYTES", 1024);
    CHECK_FATAL(link.bytes_per_second != 0);    /* the link is set, so the choice is known */
    uint64_t smallest = smallest_circulant();
    size_t large = smallest ? (size_t)smallest : (size_t)1 << 20;
    {
        const char *shape = getenv("OSOE_COLLECTIVE_SHAPE");
        CHECK_FATAL(smallest != 0 || (shape && strcmp(shape, "link") != 0) ||
                    osoe_circulant_rows((uint32_t)nodes) <= ceiling + 1);
    }
    int helpers_least = -1, helpers_greatest = -1;

    if (diverge_mode) {
        diverge(smallest);
    } else if (lanes_mode) {
        lanes();
    } else if (threaded) {
        /* At 1 MiB every round has more pieces than its window, so credits
         * flow. */
        threads((size_t)1 << 20);
    } else {
        shmem_init();
        world_known();
        size_t most = SIZES[SIZE_COUNT - 1];
        /* Several pieces of 16 KiB, for the typed cases and the to_all calls;
         * and the least size that takes Träff's, held to 256 KiB for the
         * typed cases in place and the to_all calls, since where nothing
         * takes it large is 1 MiB, and a tree takes long over a hundred such
         * calls. */
        size_t pieces = 3 * OSOE_INTERNAL_PIECE_BYTES + 1000;
        size_t least = large < 262144 ? large : 262144;
        size_t to_all_most = least > pieces ? least : pieces;
        uint8_t *dest[4], *source[4];
        for (int b = 0; b < 4; b++) {
            dest[b] = shmem_malloc(most + GUARD);
            source[b] = shmem_malloc(most + GUARD);
            CHECK_FATAL(dest[b] && source[b]);
        }
        tiny_source = shmem_malloc(sizeof *tiny_source);
        tiny_dest = shmem_malloc(sizeof *tiny_dest);
        hash_in = shmem_malloc(3 * sizeof *hash_in);
        to_all_sync = shmem_malloc(SHMEM_REDUCE_SYNC_SIZE * sizeof *to_all_sync);
        to_all_work = shmem_malloc(to_all_most / 2 + 4096 + 64);
        CHECK_FATAL(tiny_source && tiny_dest && hash_in && to_all_sync && to_all_work);
        hash_most = hash_in + 1;
        hash_least = hash_in + 2;
        rounding_table_make();
        for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
            to_all_sync[i] = SHMEM_SYNC_VALUE;
        type_source = source[0];
        type_dest = dest[0];
        CHECK(osoe_test_team_block(0) == 0);    /* the world's block is the pool's first */
        if (differ)
            helper_counts(&helpers_least, &helpers_greatest);
        shmem_team_t copy = SHMEM_TEAM_INVALID;
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &copy) == 0);
        sizes(dest[0], source[0]);
        types(large, false, false);
        types(least, true, false);
        types(pieces, false, true);
        types(pieces, true, true);
        to_all(least, false, false);
        to_all(least, true, false);
        to_all(pieces, false, true);
        to_all(pieces, true, true);
        in_place(dest[1], large + 3);
        staggered(dest[2], source[2], large);
        back_to_back(copy, dest, source, large);
        /* The copy gives its number back first, which the reuse takes, so
         * that it runs at OSOE_MAX_TEAMS=4 too. */
        shmem_barrier_all();
        shmem_team_destroy(copy);
        reuse(dest[3], source[3], large);
        long double *ld_source = (long double *)(void *)source[0];
        long double *ld_dest = (long double *)(void *)dest[0];
        if (nodes > 2) {
            few(ld_dest, ld_source, nodes - 1);
            few(ld_dest, ld_source, nodes / 2 + 1);
        }
        teams_and_rounding(dest[2], source[2]);
    }

    enum { COUNT_FAILURES, COUNT_CIRCULANT, COUNT_TEAM_WANTED, COUNT_TEAM_CALLS, COUNT_FEW,
           COUNTERS };
    int *mine = shmem_malloc(COUNTERS * sizeof *mine), *all = shmem_malloc(COUNTERS * sizeof *all);
    CHECK_FATAL(mine && all);
    mine[COUNT_FAILURES] = failures;
    mine[COUNT_CIRCULANT] = circulant_calls;
    mine[COUNT_TEAM_WANTED] = team_wanted;
    mine[COUNT_TEAM_CALLS] = team_calls;
    mine[COUNT_FEW] = few_calls;
    CHECK(shmem_int_sum_reduce(SHMEM_TEAM_WORLD, all, mine, COUNTERS) == 0);
    CHECK(all[COUNT_FAILURES] == 0);
    /* Träff's must have run wherever the chooser takes it at some size. */
    CHECK(threaded || smallest == 0 || all[COUNT_CIRCULANT] > 0);
    if (!threaded && !lanes_mode && !diverge_mode) {
        /* Every call the chooser gave Träff's, on a team, took it; and on four
         * nodes or more, where "all but the first PE" has three, some team
         * was given it, or the phase proved nothing. */
        const char *shape = getenv("OSOE_COLLECTIVE_SHAPE");
        CHECK(all[COUNT_TEAM_CALLS] == all[COUNT_TEAM_WANTED]);
        CHECK((shape && strcmp(shape, "link") != 0) || nodes < 4 || all[COUNT_TEAM_WANTED] > 0);
        /* And in the case of fewer elements than nodes, the sums of 7 took it. */
        CHECK(!tiny || all[COUNT_FEW] > 0);
    }
    const char *mode = threaded ? "two threads" : lanes_mode ? "lanes of their own"
                       : tiny ? "every phase, with fewer elements than nodes through Träff's"
                       : differ ? "every phase at differing helper counts" : "every phase";
    if (me == 0 && all[COUNT_FAILURES] == 0) {
        printf("CIRCULANT REDUCE: %s on %llu nodes, %d PEs, Träff's from %llu bytes, %d calls "
               "checked as Träff's, %d of them on teams that are not the world",
               mode, (unsigned long long)nodes, npes, (unsigned long long)smallest,
               all[COUNT_CIRCULANT] + all[COUNT_TEAM_CALLS], all[COUNT_TEAM_CALLS]);
        if (differ)
            printf(", helpers from %d to %d", helpers_least, helpers_greatest);
        printf("\n");
    }
    shmem_finalize();
    return CHECK_DONE();
}
