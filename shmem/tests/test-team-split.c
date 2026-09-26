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
 *   so its claim of a number is a compare-and-swap from another PE - another
 *   node on most maps - and each half split again into its even and its odd
 *   ranks: a split of a split;
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
 * Three modes, on four PEs, instead run out of what a split needs; each has
 * PE 0 say last that every check passed, since each is judged by what PE 0
 * says rather than by the exit status:
 * - OSOE_TEST_SPLIT=exhaust, under OSOE_MAX_TEAMS=8, five numbers for created
 *   teams: two splits take two, and two split_2d calls needing four and five
 *   fail at the claim and take none, so the numbers left still serve three
 *   splits that fit; then every split fails, a destroyed team's number not
 *   given back, while the teams made still work;
 * - OSOE_TEST_SPLIT=too-many, under OSOE_MAX_TEAMS=4: a split_2d creating five
 *   teams is refused before anything is claimed, and the one number serves
 *   the split after it;
 * - OSOE_TEST_SPLIT=full, under a small SHMEM_SYMMETRIC_SIZE: the heap filled
 *   to its last block, a split returns nonzero on every PE, PE 0 saying the
 *   heap had no room, and succeeds once the blocks are given back.
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

/*
 * OSOE_TEST_SPLIT=exhaust, four PEs under OSOE_MAX_TEAMS=8: five numbers for
 * created teams. Two splits take two. A split_2d needing four, and one of the
 * first team needing five, fail at the claim and take none - PE 0's
 * diagnostics say 2 were claimed before each - so a split that fits still
 * succeeds, and two more use the numbers up; then every split fails, a
 * destroyed team's number not given back, and PE 0 says 5 were claimed.
 */
static void exhaust(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t made = SHMEM_TEAM_INVALID, evens = SHMEM_TEAM_INVALID;
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made);
    CHECK(status == 0 && statuses_agree(status));
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, 2, NULL, 0, &evens);
    CHECK(status == 0 && statuses_agree(status));
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);

    /* Three left: a grid of two rows and two columns needs four, and one of
     * rows of one - four rows, a column - needs five. */
    shmem_team_t x = SHMEM_TEAM_WORLD, y = SHMEM_TEAM_WORLD;
    status = shmem_team_split_2d(SHMEM_TEAM_WORLD, 2, NULL, 0, &x, NULL, 0, &y);
    CHECK(status != 0 && x == SHMEM_TEAM_INVALID && y == SHMEM_TEAM_INVALID &&
          statuses_agree(status));
    x = y = SHMEM_TEAM_WORLD;
    status = shmem_team_split_2d(made, 1, NULL, 0, &x, NULL, 0, &y);
    CHECK(status != 0 && x == SHMEM_TEAM_INVALID && y == SHMEM_TEAM_INVALID &&
          statuses_agree(status));

    /* The three are still there: the team made, reversed, takes one. */
    shmem_team_t back = SHMEM_TEAM_INVALID;
    status = shmem_team_split_strided(made, npes - 1, -1, npes, NULL, 0, &back);
    CHECK(status == 0 && statuses_agree(status));
    struct expected back_expected = strided(&world_expected, npes - 1, -1, npes);
    check_team(back, &back_expected, made, &world_expected, 0);
    exercise(back, &back_expected);
    shmem_team_t first = SHMEM_TEAM_INVALID, last = SHMEM_TEAM_INVALID;
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 1, NULL, 0, &first);
    CHECK(status == 0 && statuses_agree(status));
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, npes - 1, 1, 1, NULL, 0, &last);
    CHECK(status == 0 && statuses_agree(status));

    /* None left, and a destroyed team gives none back. */
    shmem_team_t more = SHMEM_TEAM_WORLD;
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 1, NULL, 0, &more);
    CHECK(status != 0 && more == SHMEM_TEAM_INVALID && statuses_agree(status));
    shmem_team_destroy(evens);
    more = SHMEM_TEAM_WORLD;
    status = shmem_team_split_strided(made, 0, 1, npes, NULL, 0, &more);
    CHECK(status != 0 && more == SHMEM_TEAM_INVALID && statuses_agree(status));

    /* The teams made still work. */
    exercise(made, &world_expected);
    shmem_barrier_all();
    shmem_team_destroy(first);
    shmem_team_destroy(last);
    shmem_team_destroy(back);
    shmem_team_destroy(made);
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=too-many, four PEs under OSOE_MAX_TEAMS=4: one number. A
 * split_2d creating five teams is refused before anything is claimed, which
 * PE 0 says, and the one number is still there for the split after it.
 */
static void too_many(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t x = SHMEM_TEAM_WORLD, y = SHMEM_TEAM_WORLD;
    int status = shmem_team_split_2d(SHMEM_TEAM_WORLD, 1, NULL, 0, &x, NULL, 0, &y);
    CHECK(status != 0 && x == SHMEM_TEAM_INVALID && y == SHMEM_TEAM_INVALID &&
          statuses_agree(status));
    shmem_team_t made = SHMEM_TEAM_INVALID;
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made);
    CHECK(status == 0 && statuses_agree(status));
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    exercise(made, &world_expected);
    shmem_team_t more = SHMEM_TEAM_WORLD;
    status = shmem_team_split_strided(made, 0, 1, 1, NULL, 0, &more);
    CHECK(status != 0 && more == SHMEM_TEAM_INVALID && statuses_agree(status));
    shmem_team_destroy(made);
    mode_passed();
}

/*
 * OSOE_TEST_SPLIT=full, under a small SHMEM_SYMMETRIC_SIZE: the heap filled
 * block by block, halving the block as each stops fitting, until not even the
 * allocator's smallest does; a split then returns nonzero on every PE, PE 0
 * saying why, and succeeds once the blocks are given back.
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
    shmem_team_t made = SHMEM_TEAM_WORLD;
    int status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made);
    CHECK(status != 0 && made == SHMEM_TEAM_INVALID && statuses_agree(status));
    while (count > 0)
        shmem_free(blocks[--count]);
    made = SHMEM_TEAM_INVALID;
    status = shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made);
    CHECK(status == 0 && statuses_agree(status));
    check_team(made, &world_expected, SHMEM_TEAM_WORLD, &world_expected, 0);
    exercise(made, &world_expected);
    shmem_team_destroy(made);
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
