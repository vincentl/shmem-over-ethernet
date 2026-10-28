/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Team creation (spec §9.4.6-§9.4.8; phase 1 step 7 slice 2): the splits, and
 * every team routine and collective on the teams they create.
 *
 * Each phase makes teams and checks what a member can ask of each - its size
 * and rank, every rank translated to and from the world and the parent,
 * shmem_team_ptr, the configuration it was given, a context it cannot have -
 * then runs every collective on it: a sync, a broadcast from its last rank,
 * sum and maximum reductions, one of a vector past an internal-region slot, an
 * fcollect, a collect of every member's own length and an alltoall, checking
 * every value against the members the phase expects. The phases:
 *
 * - halves: the world split in two, the second half's rank 0 not world PE 0,
 *   and each half split again into its even and its odd ranks: a split of a
 *   split, the two halves' evens holding one number, as their odds do, since
 *   no PE is in two of them (the team redesign's step D, D1);
 * - evens and odds of the world;
 * - one PE, by a size of 1 and by a stride of 0;
 * - the world reversed, by a stride of -1, and split again into its even and
 *   odd ranks, strides of -2 in the world's, and into a grid: split_2d of a
 *   parent other than the world;
 * - grids of the world: split_2d over rows of two, and over rows longer than
 *   the world, the x-axis team given a configuration and the y-axis team none;
 * - SHMEM_TEAM_SHARED split: the node again, and reversed - a table of ranks
 *   where the node's ranks interleave with another node's (0,1,2,0,1,0);
 * - two teams whose local leads coincide, {0, 1} and {0, 2}, reducing and
 *   gathering back to back with PE 1 late to the first, so that PE 2's piece
 *   for the second reaches PE 0 before PE 1's for the first: the case each
 *   team number's own internal-region block exists for (internal-region.h);
 * - splits that must fail, returning nonzero and SHMEM_TEAM_INVALID on every
 *   PE.
 *
 * Wherever a phase's teams partition the world - halves and each half's evens
 * and odds, the world's evens and odds, the reversed world and its evens and
 * odds, a grid's rows and its columns, each node's SHARED split - every PE
 * syncs its own team with its arrival staggered by world rank and checks that
 * it left after every member of its team, and only those, had arrived
 * (arrivals.h): no team counts another's arrivals, which counters shared
 * between two teams would make it do, evens and odds arriving in turn.
 *
 * Modes instead test what a split needs and what a number is (step D); each
 * has PE 0 say last that every check passed, since each is judged by what PE
 * 0 says rather than by the exit status:
 * - OSOE_TEST_SPLIT=exhaust, four PEs under OSOE_MAX_TEAMS=8, five numbers a
 *   PE may hold beyond the predefined three: two teams with no PE in common
 *   hold one number; a grid takes two; a split_2d needing two numbers where
 *   every PE has one fails alike, PE 0 saying so; a team of PE 0 alone takes
 *   PE 0's last, and the world again then finds none free on PE 0, nor after
 *   one of the two teams under one number is destroyed, but after both it
 *   takes their number, while the teams made still work;
 * - OSOE_TEST_SPLIT=too-many, four PEs under OSOE_MAX_TEAMS=4, one number: a
 *   split_2d, which needs two, fails, and the one number serves the split
 *   after it, and again once that team is destroyed;
 * - OSOE_TEST_SPLIT=dry, three PEs of one node under OSOE_TEAM_BLOCKS=8, five
 *   blocks for created teams: five teams take them, and six teams of one
 *   split even so, claiming none (P7); a sixth fails alike, the lead that
 *   found the pool dry saying so; one destroyed, a split_2d whose lead needs
 *   two blocks fails alike too, holding the one block it claimed - which the
 *   first design took for a block on its way back for ever; the destroyed
 *   team's block serves the next; and with the pool full, a team two PEs
 *   destroy late serves a split PE 0 leads and calls at once;
 * - OSOE_TEST_SPLIT=stale-handle: a destroyed team's handle used after its
 *   number names another team ends the job by name, rather than reaching the
 *   new team;
 * - OSOE_TEST_SPLIT=generations, under OSOE_TEST_TEAM_GENERATION just below
 *   2^32 (P5): the world again eight times under one number, its generation
 *   crossing 32 bits - every handle another, and the first, destroyed, still
 *   ending the job by name;
 * - OSOE_TEST_SPLIT=divergent and divergent-2d (P6): PE 1 passing another
 *   size, or another xrange, than the other PEs - undefined - ends the job by
 *   name in the split's reduction, on every parent PE;
 * - OSOE_TEST_SPLIT=reuse: the world again, destroyed, and then a team of
 *   every PE but world PE 0, whose lead on PE 0's node is another PE: on that
 *   node the second team takes the first's block under another name, at
 *   another generation, which the progress engine and the collectives follow;
 * - OSOE_TEST_SPLIT=full, under a small SHMEM_SYMMETRIC_SIZE: the heap filled
 *   to its last block, and a split still succeeds on every PE, since it
 *   borrows nothing from the heap (phase 1 step 8 slice 2).
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "arrivals.h"
#include "check.h"
#include "invariants.h"

enum { MAX_PES = 64, WORDS = 4096 };

static int me, npes;
static long *source, *dest, *exchange;

/* A team as the test expects it: its members' world ranks, by team rank. */
struct expected {
    int members;
    int world[MAX_PES];
};

static struct expected world_expected;

/* The team a triplet makes of `parent`'s ranks (spec §9.4.6). */
static struct expected strided(const struct expected *parent, int start, int stride, int size)
{
    struct expected expected = { .members = size };
    for (int i = 0; i < size; i++)
        expected.world[i] = parent->world[start + stride * i];
    return expected;
}

/* Whether world rank `world` is in `expected`, and as which rank. */
static bool rank_in(const struct expected *expected, int world, int *rank)
{
    for (int r = 0; r < expected->members; r++)
        if (expected->world[r] == world) {
            if (rank)
                *rank = r;
            return true;
        }
    return false;
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/*
 * What a member can ask of `team`, made of `parent`'s ranks as `expected`
 * says, which was created asking for `contexts` contexts: its shape, both
 * ways, against the world and against its parent.
 */
static void check_team(shmem_team_t team, const struct expected *expected, shmem_team_t parent,
                       const struct expected *parent_expected, int contexts)
{
    CHECK_FATAL(team != SHMEM_TEAM_INVALID);
    int rank = -1;
    CHECK(rank_in(expected, me, &rank));
    CHECK(shmem_team_n_pes(team) == expected->members);
    CHECK(shmem_team_my_pe(team) == rank);
    for (int r = 0; r < expected->members; r++) {
        int in_parent = -1;
        CHECK(rank_in(parent_expected, expected->world[r], &in_parent));
        CHECK(shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD) == expected->world[r]);
        CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, expected->world[r], team) == r);
        CHECK(shmem_team_translate_pe(team, r, parent) == in_parent);
        CHECK(shmem_team_translate_pe(parent, in_parent, team) == r);
        CHECK(shmem_team_ptr(team, source, r) == shmem_ptr(source, expected->world[r]));
    }
    for (int w = 0; w < npes; w++)
        if (!rank_in(expected, w, NULL))
            CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, w, team) == -1);
    CHECK(shmem_team_translate_pe(team, expected->members, SHMEM_TEAM_WORLD) == -1);
    CHECK(shmem_team_translate_pe(team, -1, SHMEM_TEAM_WORLD) == -1);
    CHECK(shmem_team_translate_pe(team, 0, SHMEM_TEAM_INVALID) == -1);
    CHECK(shmem_team_ptr(team, source, expected->members) == NULL);

    /* §9.4.4, with the mask and without: the configuration it was given. */
    shmem_team_config_t config = { .num_contexts = -5 };
    CHECK(shmem_team_get_config(team, SHMEM_TEAM_NUM_CONTEXTS, &config) == 0);
    CHECK(config.num_contexts == contexts);
    config.num_contexts = -5;
    CHECK(shmem_team_get_config(team, 0, &config) == 0);
    CHECK(config.num_contexts == -5);
    /* A context from the team (§9.5.2; phase 1 step 8), naming it. */
    shmem_ctx_t ctx = SHMEM_CTX_INVALID;
    shmem_team_t of = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_create_ctx(team, 0, &ctx) == 0 && ctx != SHMEM_CTX_INVALID);
    CHECK(shmem_ctx_get_team(ctx, &of) == 0 && of == team);
    shmem_ctx_destroy(ctx);
}

/*
 * A sync on `team` with this PE's arrival staggered by its world rank: it
 * must leave after every member of its own team arrived. Every PE of the
 * world calls this in the same round, each for the team of its own that a
 * phase's partition gives it.
 */
static void staggered_sync(shmem_team_t team, const struct expected *expected)
{
    arrivals_arrive(me);
    CHECK(shmem_team_sync(team) == 0);
    double left = arrivals_now();
    const double *row = arrivals_row();
    arrivals_round++;           /* before reading: arrivals.h says why */
    for (int r = 0; r < expected->members; r++) {
        double arrived = row[expected->world[r]];
        if (arrived == 0.0 || left < arrived)
            fprintf(stderr, "PE %d: left a sync of %d members before its member PE %d "
                    "arrived\n", me, expected->members, expected->world[r]);
        CHECK(arrived != 0.0 && left >= arrived);
    }
}

/* Every collective on `team`, as `expected` says it is made, each checked. */
static void exercise(shmem_team_t team, const struct expected *expected)
{
    int rank = shmem_team_my_pe(team), n = expected->members;
    CHECK(shmem_team_sync(team) == 0);

    /* A broadcast from the last rank, which is not rank 0 but in a team of one. */
    for (int i = 0; i < 4; i++) {
        source[i] = 1000L * me + i;
        dest[i] = -1;
    }
    CHECK(shmem_long_broadcast(team, dest, source, 4, n - 1) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(dest[i] == 1000L * expected->world[n - 1] + i);

    long sum = 0, most = 0, base = 0;
    for (int r = 0; r < n; r++) {
        sum += expected->world[r] + 1;
        most = expected->world[r] + 1 > most ? expected->world[r] + 1 : most;
        base += expected->world[r];
    }
    for (int i = 0; i < 4; i++)
        source[i] = (long)(me + 1) * (i + 1);
    CHECK(shmem_long_sum_reduce(team, dest, source, 4) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(dest[i] == sum * (i + 1));
    CHECK(shmem_long_max_reduce(team, dest, source, 4) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(dest[i] == most * (i + 1));

    /* 32 KiB: pieces through the internal region, one after another. */
    for (int i = 0; i < WORDS; i++)
        source[i] = me + i;
    CHECK(shmem_long_sum_reduce(team, dest, source, WORDS) == 0);
    int wrong = 0;
    for (int i = 0; i < WORDS; i++)
        wrong += dest[i] != base + (long)n * i;
    CHECK(wrong == 0);

    source[0] = 10L * me;
    source[1] = 10L * me + 1;
    CHECK(shmem_long_fcollect(team, dest, source, 2) == 0);
    for (int r = 0; r < n; r++)
        CHECK(dest[2 * r] == 10L * expected->world[r] &&
              dest[2 * r + 1] == 10L * expected->world[r] + 1);

    /* Member r's rank + 1 words, at the sum of the lengths before it. */
    for (int j = 0; j <= rank; j++)
        source[j] = 100L * me + j;
    CHECK(shmem_long_collect(team, dest, source, (size_t)rank + 1) == 0);
    for (int r = 0, at = 0; r < n; r++)
        for (int j = 0; j <= r; j++)
            CHECK(dest[at++] == 100L * expected->world[r] + j);

    /* Into an array no other collective here writes, whose puts land in the
     * other members' copies; the sync after keeps a fast member's next
     * alltoall out of a copy still being checked. */
    for (int j = 0; j < n; j++)
        for (int k = 0; k < 2; k++)
            source[2 * j + k] = 1000L * me + 10 * j + k;
    CHECK(shmem_long_alltoall(team, exchange, source, 2) == 0);
    for (int r = 0; r < n; r++)
        for (int k = 0; k < 2; k++)
            CHECK(exchange[2 * r + k] == 1000L * expected->world[r] + 10 * rank + k);
    CHECK(shmem_team_sync(team) == 0);
}

/*
 * `parent`'s even ranks and its odd ranks, split from it, each checked and
 * run, and synced with the arrival table: every member of `parent` is in one
 * of the two, and every PE of the world calls this for the parent it is in at
 * the same time, so the calls partition the world. For a parent other than the
 * world, a split of a split.
 */
static void evens_and_odds_of(shmem_team_t parent, const struct expected *parent_expected)
{
    int members = parent_expected->members;
    bool even = shmem_team_my_pe(parent) % 2 == 0;
    shmem_team_t evens = SHMEM_TEAM_INVALID, odds = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(parent, 0, 2, (members + 1) / 2, NULL, 0, &evens) == 0);
    if (members > 1)
        CHECK(shmem_team_split_strided(parent, 1, 2, members / 2, NULL, 0, &odds) == 0);
    CHECK((evens != SHMEM_TEAM_INVALID) == even);
    CHECK((odds != SHMEM_TEAM_INVALID) == !even);
    shmem_team_t part = even ? evens : odds;
    struct expected part_expected = strided(parent_expected, even ? 0 : 1, 2,
                                            even ? (members + 1) / 2 : members / 2);
    check_team(part, &part_expected, parent, parent_expected, 0);
    staggered_sync(part, &part_expected);
    exercise(part, &part_expected);
    shmem_barrier_all();
    shmem_team_destroy(evens);
    shmem_team_destroy(odds);
}

/*
 * split_2d of `parent` over rows of `xrange` (spec §9.4.7): the x-axis team is
 * this PE's row, and its rank there its x; the y-axis team its column, rank y.
 * Every team checked and run, rows and then columns synced with the arrival
 * table: like evens_and_odds_of, called by every PE at once for a parent that
 * partitions the world.
 */
static void grid_of(shmem_team_t parent, const struct expected *parent_expected, int xrange)
{
    int members = parent_expected->members, rank = shmem_team_my_pe(parent);
    shmem_team_config_t x_config = { .num_contexts = 3 };
    shmem_team_t row = SHMEM_TEAM_INVALID, column = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_2d(parent, xrange, &x_config, SHMEM_TEAM_NUM_CONTEXTS, &row, NULL, 0,
                              &column) == 0);
    int columns = xrange < members ? xrange : members;
    int x = rank % columns, y = rank / columns;
    int row_size = members - y * columns < columns ? members - y * columns : columns;
    struct expected row_expected = strided(parent_expected, y * columns, 1, row_size);
    struct expected column_expected = strided(parent_expected, x, columns,
                                              (members - x + columns - 1) / columns);
    check_team(row, &row_expected, parent, parent_expected, 3);
    check_team(column, &column_expected, parent, parent_expected, 0);
    CHECK(shmem_team_my_pe(row) == x);
    CHECK(shmem_team_my_pe(column) == y);
    staggered_sync(row, &row_expected);
    staggered_sync(column, &column_expected);
    exercise(row, &row_expected);
    shmem_barrier_all();
    exercise(column, &column_expected);
    shmem_barrier_all();
    shmem_team_destroy(row);
    shmem_team_destroy(column);
}

static void halves(void)
{
    int split_at = npes > 1 ? npes / 2 : 1;
    bool in_first = me < split_at;
    shmem_team_t first = SHMEM_TEAM_INVALID, second = SHMEM_TEAM_INVALID;
    shmem_team_config_t config = { .num_contexts = 2 };
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, split_at, &config,
                                   SHMEM_TEAM_NUM_CONTEXTS, &first) == 0);
    if (npes > 1)
        CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, split_at, 1, npes - split_at, NULL, 0,
                                       &second) == 0);
    CHECK((first != SHMEM_TEAM_INVALID) == in_first);
    CHECK((second != SHMEM_TEAM_INVALID) == !in_first);
    shmem_team_t half = in_first ? first : second;
    struct expected half_expected = strided(&world_expected, in_first ? 0 : split_at, 1,
                                            in_first ? split_at : npes - split_at);
    check_team(half, &half_expected, SHMEM_TEAM_WORLD, &world_expected, in_first ? 2 : 0);
    staggered_sync(half, &half_expected);
    exercise(half, &half_expected);

    /* Each half's even and odd ranks, split from the half, whose rank 0
     * claims: the second half's is not world PE 0. */
    evens_and_odds_of(half, &half_expected);
    shmem_team_destroy(half);
}

static void one_pe(void)
{
    shmem_team_t last = SHMEM_TEAM_INVALID, first = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, npes - 1, 1, 1, NULL, 0, &last) == 0);
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 0, 1, NULL, 0, &first) == 0);
    CHECK((last != SHMEM_TEAM_INVALID) == (me == npes - 1));
    CHECK((first != SHMEM_TEAM_INVALID) == (me == 0));
    if (last != SHMEM_TEAM_INVALID) {
        struct expected last_expected = strided(&world_expected, npes - 1, 1, 1);
        check_team(last, &last_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
        exercise(last, &last_expected);
    }
    if (first != SHMEM_TEAM_INVALID) {
        struct expected first_expected = strided(&world_expected, 0, 0, 1);
        check_team(first, &first_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
        exercise(first, &first_expected);
    }
    shmem_barrier_all();
    shmem_team_destroy(last);
    shmem_team_destroy(first);
}

/* The world reversed, and split again: its even and odd ranks - world ranks
 * npes - 1, npes - 3, ... and npes - 2, ... - and a grid of it, split_2d of a
 * parent other than the world. */
static void reversed(void)
{
    shmem_team_t back = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, npes - 1, -1, npes, NULL, 0, &back) == 0);
    struct expected back_expected = strided(&world_expected, npes - 1, -1, npes);
    check_team(back, &back_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    staggered_sync(back, &back_expected);
    exercise(back, &back_expected);
    evens_and_odds_of(back, &back_expected);
    grid_of(back, &back_expected, 2);
    shmem_team_destroy(back);
}

/* SHMEM_TEAM_SHARED split, on every node at once: the node again, and
 * reversed. Every PE is in its node's, so the first partitions the world. */
static void shared_splits(void)
{
    struct expected node = { .members = shmem_team_n_pes(SHMEM_TEAM_SHARED) };
    for (int r = 0; r < node.members; r++)
        node.world[r] = shmem_team_translate_pe(SHMEM_TEAM_SHARED, r, SHMEM_TEAM_WORLD);
    shmem_team_t again = SHMEM_TEAM_INVALID, back = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_SHARED, 0, 1, node.members, NULL, 0, &again) == 0);
    CHECK(shmem_team_split_strided(SHMEM_TEAM_SHARED, node.members - 1, -1, node.members, NULL,
                                   0, &back) == 0);
    struct expected reverse = strided(&node, node.members - 1, -1, node.members);
    check_team(again, &node, SHMEM_TEAM_SHARED, &node, 0);
    check_team(back, &reverse, SHMEM_TEAM_SHARED, &node, 0);
    staggered_sync(again, &node);
    exercise(again, &node);
    shmem_barrier_all();
    exercise(back, &reverse);
    shmem_barrier_all();
    shmem_team_destroy(back);
    shmem_team_destroy(again);
}

/*
 * Two teams whose local leads coincide: {0, 1} and {0, 2}, PE 0 the lowest
 * rank of both on its node. On a map with the three on three nodes PE 0 is the
 * root of both teams' trees, with PE 1's node and PE 2's each the child at
 * position 0: PE 1, late, puts its piece for the first team after PE 2 has put
 * its piece for the second, while PE 0 still waits in the first. Two teams
 * sharing slots would give the second team the first's piece.
 */
static void coinciding_leads(void)
{
    if (npes < 3)
        return;
    shmem_team_t a = SHMEM_TEAM_INVALID, b = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 2, NULL, 0, &a) == 0);
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, 2, NULL, 0, &b) == 0);
    CHECK((a != SHMEM_TEAM_INVALID) == (me == 0 || me == 1));
    CHECK((b != SHMEM_TEAM_INVALID) == (me == 0 || me == 2));
    for (int round = 0; round < 6; round++) {
        if (a != SHMEM_TEAM_INVALID) {
            if (me == 1)
                sleep_milliseconds(20);
            source[0] = me + 1 + round;
            CHECK(shmem_long_sum_reduce(a, dest, source, 1) == 0);
            CHECK(dest[0] == 1 + 2 + 2 * round);
        }
        if (b != SHMEM_TEAM_INVALID) {
            source[0] = 100L * (me + 1) + round;
            CHECK(shmem_long_sum_reduce(b, dest, source, 1) == 0);
            if (dest[0] != 100 + 300 + 2 * round)
                fprintf(stderr, "PE %d: team {0, 2}'s sum in round %d is %ld, not %d\n", me,
                        round, dest[0], 100 + 300 + 2 * round);
            CHECK(dest[0] == 100 + 300 + 2 * round);
        }
    }
    for (int round = 0; round < 6; round++) {
        if (a != SHMEM_TEAM_INVALID) {
            if (me == 1)
                sleep_milliseconds(20);
            source[0] = 10L * me + round;
            CHECK(shmem_long_fcollect(a, dest, source, 1) == 0);
            CHECK(dest[0] == round && dest[1] == 10 + round);
        }
        if (b != SHMEM_TEAM_INVALID) {
            source[0] = 1000L * me + round;
            CHECK(shmem_long_fcollect(b, dest, source, 1) == 0);
            CHECK(dest[0] == round && dest[1] == 2000 + round);
        }
    }
    shmem_barrier_all();
    shmem_team_destroy(a);
    shmem_team_destroy(b);
}

/* Splits that must fail: nonzero and SHMEM_TEAM_INVALID on every PE (spec
 * §9.4.6, §9.4.7), with nothing claimed or made. */
static void invalid_splits(void)
{
    const int triplets[][3] = {
        { 0, 1, 0 },                /* size not positive */
        { 0, 1, -1 },
        { -1, 1, 1 },               /* start outside the parent */
        { npes, 1, 1 },
        { 0, -1, 2 },               /* a rank below the parent's first */
        { 1, 1, npes },             /* and past its last */
        { 0, 1, npes + 1 },
        { 1, 2, npes },             /* a sequence that would wrap around */
    };
    for (size_t i = 0; i < sizeof triplets / sizeof triplets[0]; i++) {
        shmem_team_t made = SHMEM_TEAM_WORLD;
        CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, triplets[i][0], triplets[i][1],
                                       triplets[i][2], NULL, 0, &made) != 0);
        CHECK(made == SHMEM_TEAM_INVALID);
    }
    shmem_team_t made = SHMEM_TEAM_WORLD;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_INVALID, 0, 1, 1, NULL, 0, &made) != 0);
    CHECK(made == SHMEM_TEAM_INVALID);
    shmem_team_t x = SHMEM_TEAM_WORLD, y = SHMEM_TEAM_WORLD;
    CHECK(shmem_team_split_2d(SHMEM_TEAM_INVALID, 1, NULL, 0, &x, NULL, 0, &y) != 0);
    CHECK(x == SHMEM_TEAM_INVALID && y == SHMEM_TEAM_INVALID);
}

/* Whether every PE's split returned the same: none failed, or all did. */
static bool statuses_agree(int status)
{
    source[0] = status != 0;
    CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, 1) == 0);
    return dest[0] == 0 || dest[0] == npes;
}

/* Every check of a mode passed, said by PE 0 last: what its registration's
 * regular expression needs, since it does not judge by the exit status. */
static void mode_passed(void)
{
    shmem_barrier_all();
    if (me == 0 && CHECK_DONE() == 0)
        fprintf(stderr, "PE 0: every check of this mode passed\n");
}

/* Whether a split's status says it made the team, alike on every PE. A
 * macro, so that the split returns before its handle is read: as arguments
 * of one call the two are indeterminately sequenced (C11 6.5.2.2p10), and
 * GCC for x86-64 reads the handle first. */
#define check_made(split, team) do {                                           \
        int split_status = (split);                                             \
        check_made_status(split_status, (team));                                \
    } while (0)
static void check_made_status(int status, shmem_team_t team)
{
    CHECK(status == 0 && team != SHMEM_TEAM_INVALID && statuses_agree(status));
}

/* Whether a split's status says it failed, alike on every PE, and left the
 * handles it was given SHMEM_TEAM_INVALID; a macro as check_made is. */
#define check_failed(split, x, y) do {                                         \
        int split_status = (split);                                             \
        check_failed_status(split_status, (x), (y));                            \
    } while (0)
static void check_failed_status(int status, shmem_team_t x, shmem_team_t y)
{
    CHECK(status != 0 && x == SHMEM_TEAM_INVALID && y == SHMEM_TEAM_INVALID &&
          statuses_agree(status));
}

/* The block this PE's team under `number` holds on its node, or UINT32_MAX
 * when it holds none (invariants.h, the PE's published epoch word). */
static uint32_t block_of(uint32_t number)
{
    return osoe_test_team_block(number);
}

/*
 * OSOE_TEST_SPLIT=exhaust, four PEs under OSOE_MAX_TEAMS=8: five numbers a PE
 * may hold beyond the predefined three (the file's comment).
 */
static void exhaust(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t made = SHMEM_TEAM_INVALID, evens = SHMEM_TEAM_INVALID;
    shmem_team_t odds = SHMEM_TEAM_INVALID, again = SHMEM_TEAM_INVALID;
    /* 3 everywhere; then the evens and the odds, sharing no PE, both 4. */
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made), made);
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, 2, NULL, 0, &evens);
    CHECK(status == 0 && statuses_agree(status));
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 2, 2, NULL, 0, &odds);
    CHECK(status == 0 && statuses_agree(status));
    CHECK((me % 2 == 0 ? evens : odds) != SHMEM_TEAM_INVALID);
    CHECK(block_of(4) != UINT32_MAX);
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);

    /* A grid of two by two takes 5 for its rows and 6 for its columns. */
    shmem_team_t x = SHMEM_TEAM_INVALID, y = SHMEM_TEAM_INVALID;
    status = shmem_team_split_2d(SHMEM_TEAM_WORLD, 2, NULL, 0, &x, NULL, 0, &y);
    CHECK(status == 0 && x != SHMEM_TEAM_INVALID && y != SHMEM_TEAM_INVALID &&
          statuses_agree(status));
    CHECK(block_of(5) != UINT32_MAX && block_of(6) != UINT32_MAX);

    /* One number left on every PE, 7: a split_2d needs two, and fails. */
    shmem_team_t x2 = SHMEM_TEAM_WORLD, y2 = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_2d(made, 1, NULL, 0, &x2, NULL, 0, &y2), x2, y2);

    /* PE 0 alone takes its 7; the world again then finds none on PE 0. */
    shmem_team_t first = SHMEM_TEAM_INVALID;
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 1, NULL, 0, &first);
    CHECK(status == 0 && statuses_agree(status) && (me == 0) == (first != SHMEM_TEAM_INVALID));
    shmem_team_t more = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &more), more,
                 SHMEM_TEAM_INVALID);

    /* The evens give 4 back, the odds still hold it: none on every PE yet. */
    shmem_team_destroy(evens);
    more = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &more), more,
                 SHMEM_TEAM_INVALID);
    /* The odds too: 4 is free everywhere, and the world again takes it. */
    shmem_team_destroy(odds);
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &again), again);
    CHECK(block_of(4) != UINT32_MAX);
    check_team(again, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    exercise(again, &world_expected);

    /* The teams made still work. */
    exercise(made, &world_expected);
    CHECK(shmem_team_sync(x) == 0 && shmem_team_sync(y) == 0);
    shmem_barrier_all();
    shmem_team_destroy(first);
    shmem_team_destroy(again);
    shmem_team_destroy(y);
    shmem_team_destroy(x);
    shmem_team_destroy(made);
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=too-many, four PEs under OSOE_MAX_TEAMS=4: one number a PE
 * may hold. A split_2d needs two, and fails alike, which PE 0 says; the one
 * number serves the split after it, and a split of that team finds none; and
 * once the team is destroyed the number serves again.
 */
static void too_many(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t x = SHMEM_TEAM_WORLD, y = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_2d(SHMEM_TEAM_WORLD, 1, NULL, 0, &x, NULL, 0, &y), x, y);
    shmem_team_t made = SHMEM_TEAM_INVALID;
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made), made);
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    exercise(made, &world_expected);
    shmem_team_t more = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_strided(made, 0, 1, 1, NULL, 0, &more), more,
                 SHMEM_TEAM_INVALID);
    shmem_team_destroy(made);
    made = SHMEM_TEAM_INVALID;
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made), made);
    exercise(made, &world_expected);
    shmem_team_destroy(made);
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=dry, three PEs of one node under OSOE_TEAM_BLOCKS=8: five
 * blocks for created teams. Four teams of all three and one of PEs 1 and 2
 * take them. With the pool full, teams of one still split - six, two a PE,
 * more than the pool has blocks - since a team of one claims none (P7): each
 * works, its epoch word naming no block. A sixth team of all three finds the
 * pool dry and fails alike, PE 0 - the lead that would have claimed - saying
 * so; one of the four destroyed gives its block back. A split_2d of the three
 * by rows of two - rows {0, 1} and {2}, columns {0, 2} and {1} - needs two
 * blocks, both PE 0's to claim, the row and the column of one needing none:
 * PE 0 finds one, and its split fails alike, PE 0 saying so - the adversarial
 * read of 2026-10-03 found the first design ran its rounds for ever, the
 * block PE 0 had claimed taken for one on its way back. The sixth then takes
 * the block given back. Last, with the pool full again, PEs 1 and 2 destroy
 * their team late, and a split of the world that PE 0 leads, called by PE 0
 * at once, succeeds: PE 0's first claim comes before the pair has given the
 * block back, and the pair gives it back before it arrives in the split's
 * reduction, so the split's next round finds it - a pool found empty in the
 * first round makes the round busy, not dry (tools/modelcheck/pool_dry_model.py).
 * Until P7 a team of one took a block like any other, and the six failed.
 */
static void dry(void)
{
    CHECK_FATAL(npes == 3);
    enum { BLOCKS = 5, ALONE = 6 };
    shmem_team_t made[BLOCKS + 1], pair = SHMEM_TEAM_INVALID;
    for (int i = 0; i < BLOCKS - 1; i++) {
        made[i] = SHMEM_TEAM_INVALID;
        check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made[i]),
                   made[i]);
    }
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 1, 2, NULL, 0, &pair);
    CHECK(status == 0 && statuses_agree(status) && (pair != SHMEM_TEAM_INVALID) == (me != 0));

    /* Six teams of one with the pool full, world PE i % 3 alone in the i-th. */
    shmem_team_t alone[ALONE];
    int held = 0;
    for (int i = 0; i < ALONE; i++) {
        shmem_team_t team = SHMEM_TEAM_INVALID;
        status = shmem_team_split_strided(SHMEM_TEAM_WORLD, i % npes, 1, 1, NULL, 0, &team);
        CHECK(status == 0 && statuses_agree(status) &&
              (team != SHMEM_TEAM_INVALID) == (me == i % npes));
        if (team != SHMEM_TEAM_INVALID)
            alone[held++] = team;
    }
    CHECK(held == ALONE / npes);
    struct expected own = strided(&world_expected, me, 1, 1);
    for (int i = 0; i < held; i++) {
        check_team(alone[i], &own, SHMEM_TEAM_WORLD, &world_expected, 0);
        exercise(alone[i], &own);
    }
    CHECK(osoe_test_teams_without_block() == held);

    made[BLOCKS - 1] = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0,
                                          &made[BLOCKS - 1]), made[BLOCKS - 1], SHMEM_TEAM_INVALID);
    shmem_team_destroy(made[2]);
    made[2] = SHMEM_TEAM_INVALID;
    shmem_team_t x = SHMEM_TEAM_WORLD, y = SHMEM_TEAM_WORLD;
    check_failed(shmem_team_split_2d(SHMEM_TEAM_WORLD, 2, NULL, 0, &x, NULL, 0, &y), x, y);
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0,
                                        &made[BLOCKS - 1]), made[BLOCKS - 1]);
    if (me != 0) {
        sleep_milliseconds(100);
        shmem_team_destroy(pair);
    }
    made[BLOCKS] = SHMEM_TEAM_INVALID;
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made[BLOCKS]),
               made[BLOCKS]);
    for (int i = 0; i <= BLOCKS; i++)
        if (made[i] != SHMEM_TEAM_INVALID) {
            exercise(made[i], &world_expected);
            shmem_team_destroy(made[i]);
        }
    for (int i = 0; i < held; i++)
        shmem_team_destroy(alone[i]);
    CHECK(osoe_test_teams_without_block() == 0);
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=stale-handle: the world again, destroyed, and the world
 * again once more under the same number - the lowest free - with another
 * handle; the first handle then names a destroyed team, which ends the job by
 * name (team.c, D6), rather than reaching the team now under its number.
 */
static void stale_handle(void)
{
    shmem_team_t old = SHMEM_TEAM_INVALID, now = SHMEM_TEAM_INVALID;
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &old), old);
    uint32_t first_block = block_of(3);
    shmem_team_destroy(old);
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &now), now);
    CHECK(now != old);
    CHECK(block_of(3) != UINT32_MAX && first_block != UINT32_MAX);
    exercise(now, &world_expected);
    shmem_barrier_all();
    if (me == 0 && CHECK_DONE() == 0)
        fprintf(stderr, "PE 0: every check before the stale handle passed\n");
    shmem_barrier_all();
    (void)shmem_team_my_pe(old);
    CHECK(!"a destroyed handle was accepted");
}

/*
 * OSOE_TEST_SPLIT=generations, under OSOE_TEST_TEAM_GENERATION=4294967292,
 * 2^32 - 4 (P5): the world again and destroyed eight times under one number,
 * the lowest free, its generation - which a handle carries (team.c,
 * handle_of) - from 2^32 - 3 to 2^32 + 4. Each team works and has a handle no
 * other had; the first's, destroyed, then names a number another team holds,
 * which ends the job by name, as stale-handle's does below 2^32. A generation
 * of 32 bits ended the job at the fourth split instead, and one that wrapped
 * would have given the first team's handle to the seventh.
 */
static void generations(void)
{
    enum { CYCLES = 8 };
    shmem_team_t handle[CYCLES];
    for (int i = 0; i < CYCLES; i++) {
        handle[i] = SHMEM_TEAM_INVALID;
        check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &handle[i]),
                   handle[i]);
        exercise(handle[i], &world_expected);
        for (int j = 0; j < i; j++)
            CHECK(handle[i] != handle[j]);
        if (i < CYCLES - 1)
            shmem_team_destroy(handle[i]);
    }
    shmem_barrier_all();
    if (me == 0 && CHECK_DONE() == 0)
        fprintf(stderr, "PE 0: every check before the stale handle passed\n");
    shmem_barrier_all();
    (void)shmem_team_my_pe(handle[0]);
    CHECK(!"a destroyed handle was accepted");
}

/*
 * OSOE_TEST_SPLIT=divergent and divergent-2d (P6): the world split with PE 1
 * passing a size one less than the other PEs' - or, in two dimensions, an
 * xrange of 1 where the others pass 2 - which the specification leaves
 * undefined. Every argument is valid on its own PE, so every PE reaches the
 * split's reduction, which compares them and ends the job by name; before,
 * PE 1 left the new team out of its view of it and the others waited.
 */
static void divergent(bool two_dimensions)
{
    shmem_team_t a = SHMEM_TEAM_INVALID, b = SHMEM_TEAM_INVALID;
    shmem_barrier_all();
    if (two_dimensions)
        (void)shmem_team_split_2d(SHMEM_TEAM_WORLD, me == 1 ? 1 : 2, NULL, 0, &a, NULL, 0, &b);
    else
        (void)shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, me == 1 ? npes - 1 : npes, NULL,
                                       0, &a);
    CHECK(!"a split whose PEs passed different arguments returned");
}

/*
 * OSOE_TEST_SPLIT=reuse: the world again, its block on each node noted, then
 * destroyed; then every PE but world PE 0, whose lead on PE 0's node is the
 * node's next PE - so that, on a node of several PEs, the second team's name
 * there is another than the first's. Blocks are claimed the highest free
 * first, so it takes the first team's block, at a generation two past the
 * first's (node.h): the engine and the collectives must follow it.
 */
static void reuse(void)
{
    shmem_team_t first = SHMEM_TEAM_INVALID, second = SHMEM_TEAM_INVALID;
    check_made(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &first), first);
    exercise(first, &world_expected);
    uint32_t block = block_of(3);
    uint64_t generation = 0;
    CHECK(block != UINT32_MAX &&
          osoe_test_scoped_metric("team.name.generation", OSOE_MSCOPE_TEAM, block,
                                  &generation));
    shmem_team_destroy(first);
    /* Every member has left the first team's blocks once every PE is past its
     * destroy, and the last on each node has given its block back: the
     * barrier, so that the second team's lead finds it free when it claims,
     * before the second split's reduction. */
    shmem_barrier_all();
    if (npes < 2) {
        mode_passed();
        return;
    }
    struct expected rest = strided(&world_expected, 1, 1, npes - 1);
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 1, npes - 1, NULL, 0, &second);
    CHECK(status == 0 && statuses_agree(status) && (me == 0) == (second == SHMEM_TEAM_INVALID));
    if (second != SHMEM_TEAM_INVALID) {
        check_team(second, &rest, SHMEM_TEAM_WORLD, &world_expected, 0);
        /* The same block, and the generation moved on by two - given back,
         * then written again. */
        uint64_t now = 0;
        CHECK(block_of(3) == block);
        CHECK(osoe_test_scoped_metric("team.name.generation", OSOE_MSCOPE_TEAM, block, &now) &&
              now == generation + 2);
        exercise(second, &rest);
        shmem_team_destroy(second);
    }
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=full, under a small SHMEM_SYMMETRIC_SIZE: the heap filled
 * block by block, halving the block as each stops fitting, until not even the
 * allocator's smallest does; a split then succeeds on every PE all the same.
 * Until phase 1 step 8 slice 2 it borrowed its word from the heap and failed
 * here; now nothing it needs comes from the heap.
 */
static void heap_full(void)
{
    enum { MAX_BLOCKS = 128 };
    void *blocks[MAX_BLOCKS];
    int count = 0;
    for (size_t size = (size_t)1 << 30; size >= 16 && count < MAX_BLOCKS;) {
        void *block = shmem_malloc(size);
        if (block)
            blocks[count++] = block;
        else
            size /= 2;
    }
    CHECK_FATAL(count < MAX_BLOCKS);
    /* Since phase 1 step 8 slice 2 a split borrows nothing from the heap -
     * since step D its reduction runs in the parent number's own lines of the
     * internal region - so a full heap no longer stops it. */
    shmem_team_t made = SHMEM_TEAM_INVALID;
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made);
    CHECK(status == 0 && statuses_agree(status));
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    exercise(made, &world_expected);
    shmem_team_destroy(made);
    while (count > 0)
        shmem_free(blocks[--count]);
    mode_passed();
}

int main(void)
{
    arrivals_open();
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(npes <= MAX_PES);
    source = shmem_malloc(sizeof *source * WORDS);
    dest = shmem_malloc(sizeof *dest * WORDS);
    exchange = shmem_malloc(sizeof *exchange * 2 * MAX_PES);
    CHECK_FATAL(source && dest && exchange);
    world_expected.members = npes;
    for (int r = 0; r < npes; r++)
        world_expected.world[r] = r;

    const char *mode = getenv("OSOE_TEST_SPLIT");
    if (mode && strcmp(mode, "exhaust") == 0) {
        exhaust();
    } else if (mode && strcmp(mode, "too-many") == 0) {
        too_many();
    } else if (mode && strcmp(mode, "dry") == 0) {
        dry();
    } else if (mode && strcmp(mode, "stale-handle") == 0) {
        stale_handle();
    } else if (mode && strcmp(mode, "divergent") == 0) {
        divergent(false);
    } else if (mode && strcmp(mode, "divergent-2d") == 0) {
        divergent(true);
    } else if (mode && strcmp(mode, "generations") == 0) {
        generations();
    } else if (mode && strcmp(mode, "reuse") == 0) {
        reuse();
    } else if (mode && strcmp(mode, "full") == 0) {
        heap_full();
    } else {
        halves();
        evens_and_odds_of(SHMEM_TEAM_WORLD, &world_expected);
        one_pe();
        reversed();
        grid_of(SHMEM_TEAM_WORLD, &world_expected, 2);
        grid_of(SHMEM_TEAM_WORLD, &world_expected, npes + 1);
        shared_splits();
        coinciding_leads();
        invalid_splits();
    }

    shmem_free(exchange);
    shmem_free(dest);
    shmem_free(source);
    CHECK_INVARIANTS();
    shmem_finalize();
    arrivals_close();
    return CHECK_DONE();
}
