/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * teams - teams of every shape, their queries, and collectives on many teams
 * at once (proposal §4.3; docs/prompts/task-stress-phase2.md).
 *
 * **Made at setup, and renewed as it runs.** A team's number is each PE's own,
 * and a destroyed team gives it back (design §5.7), so what the exercise costs
 * a PE is what it holds at once: STRIDED numbers for the strided teams - one
 * of random start, stride and size; one with a negative stride; a team of one,
 * by the stride 0 the spec allows it; a split of the first - and two for the
 * grid, which is always of the whole team: split_2d takes two numbers of every
 * PE, its row's and its column's, however large the grid. The header's
 * `numbers=` is the most one PE holds. Every RENEW_EVERY rounds the exercise
 * renews one strided team, rotating which: it destroys the context it made of
 * the team, frees the team's buffers, destroys the team, and splits another of
 * the same kind in its place - a fresh start, stride and size drawn from the
 * stream setup draws from, alike on every PE - so that the library gives
 * numbers and nodes' blocks back and takes them again while the other teams'
 * collectives run. `--teams-exhaust` is the other mode: it splits until a
 * split returns nonzero, checks that it did on every PE and that the job goes
 * on, destroys what it made, checks that a split of the whole team then
 * succeeds on every PE - the numbers are back - and stops.
 *
 * **Every team is a progression of world ranks** - a split of a progression
 * is one, and so is a grid's row or column - so every answer is arithmetic.
 * Checked for each team as it is made, at setup or by a renewal:
 * SHMEM_TEAM_INVALID exactly outside it; team_n_pes, team_my_pe;
 * team_translate_pe to the world for every member and from the world for every
 * PE, and between a grid's row and column; team_get_config; team_ptr non-NULL
 * exactly for node-mates; a context of the team whose ctx_get_team names it.
 *
 * **Rounds of collectives on many teams at once.** Each round every grid
 * member does one collective on its row and one on its column - the even rows
 * row first, the odd rows column first, so rows' and columns' collectives run
 * at once and no wait closes a cycle - and then every PE one on each strided
 * team it belongs to, in the teams' order, and a put and get back through
 * each such team's context. The grid's collectives take turns over sum, max,
 * min and product reductions of longs, and, or and xor of 64-bit words, a sum
 * of doubles, a broadcast from a rotating root, collect with ragged lengths
 * including zero, fcollect, alltoall, the inclusive and exclusive sum scans and
 * team_sync; the strided teams' over the ones whose memory is O(1) a member,
 * or O(size) for fcollect. Every input is (world PE, round, element)-
 * determined, so every member computes every result. A team's buffers come in
 * two, by round parity, so that no member's next collective on it writes a
 * buffer another member is still checking; a renewal frees a team's and
 * allocates its successor's, on every PE, members or not, as setup does.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

/* TEAMS - the strided teams, a row and a column - are the most a PE is in at
 * once, and each holds one of its numbers. A strided team is renewed every
 * RENEW_EVERY rounds, in turn: three, so that --quick's 15 rounds renew all
 * four. */
enum { ELEMENTS = 4, STRIDED = 4, GRID_ROW = STRIDED, GRID_COLUMN, TEAMS, RENEW_EVERY = 3 };
enum kind {
    SUM, MAX, MIN, PRODUCT, AND, OR, XOR, DOUBLE_SUM, BROADCAST, COLLECT, FCOLLECT, ALLTOALL,
    INSCAN, EXSCAN, SYNC, KINDS
};
static const char *const kind_names[KINDS] = {
    "sum_reduce", "max_reduce", "min_reduce", "prod_reduce", "and_reduce", "or_reduce",
    "xor_reduce", "double_sum_reduce", "broadcast", "collect", "fcollect", "alltoall",
    "sum_inscan", "sum_exscan", "team_sync"
};
/* The strided teams' turns: each O(1) a member but fcollect's O(size). */
static const enum kind strided_kinds[] = { SUM, MAX, XOR, BROADCAST, FCOLLECT, INSCAN, SYNC };
static const char *const phase_names[] = { "grid", "strided", "check", "renew", NULL };

struct team {
    const char *name;
    shmem_team_t handle;            /* SHMEM_TEAM_INVALID outside it */
    int start, stride, size;        /* in world ranks */
    int rank;                       /* this PE's, or -1 */
    shmem_ctx_t ctx;
    int has_ctx;
    long *longs[2];                 /* symmetric, [capacity] each: sources and results */
    uint64_t *words[2];
    double *doubles[2];
    size_t capacity;                /* longs a buffer, at least 3 * ELEMENTS + size * ELEMENTS */
};

static struct {
    /* the plan */
    uint64_t seed;
    int exhaust;
    int plant_query, plant_collective, plant_exhaust, plant_member, plant_ring;
    int plant_renew, plant_back;
    int query_planted, collective_planted[KINDS];
    int columns, rows;              /* the grid's shape: of the whole team */
    int grid_row_index;             /* this PE's row in the grid */
    /* the teams */
    shmem_team_t team;
    int n, me_world;
    struct team world;              /* the exercise's team, as the strided teams' parent */
    struct team teams[TEAMS];
    long *ring;                     /* symmetric [TEAMS]: the contexts' round trip */
    uint64_t state;                 /* the strided teams' draws: the same stream on every PE */
    long round;
    long renewals;                  /* strided teams renewed, the same on every PE */
    int last;                       /* the step in hand is the last */
    double collectives, phase[4];   /* seconds: grid, strided, check, renew */
} teams;

static int member_of(const struct team *t, int world, int *rank)
{
    int offset = world - t->start;
    if (t->stride == 0) {
        *rank = offset == 0 ? 0 : -1;
        return offset == 0;
    }
    if (offset % t->stride != 0 || offset / t->stride < 0 || offset / t->stride >= t->size) {
        *rank = -1;
        return 0;
    }
    *rank = offset / t->stride;
    return 1;
}

/* The grid of `size` PEs: columns near the square root, so that rows and
 * columns are about as long as each other. */
static void grid_shape(int size, int *columns, int *rows)
{
    int c = (int)ceil(sqrt((double)size));
    *columns = c < 1 ? 1 : c;
    *rows = (size + *columns - 1) / *columns;
}

static uint64_t teams_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    teams.seed = options->seed;
    teams.exhaust = options->teams_exhaust;
    teams.plant_query = options->plant == PACER_PLANT_TEAMS_QUERY;
    teams.plant_collective = options->plant == PACER_PLANT_TEAMS_COLLECTIVE;
    teams.plant_exhaust = options->plant == PACER_PLANT_TEAMS_EXHAUST;
    teams.plant_member = options->plant == PACER_PLANT_TEAMS_MEMBER;
    teams.plant_ring = options->plant == PACER_PLANT_TEAMS_RING;
    teams.plant_renew = options->plant == PACER_PLANT_TEAMS_RENEW;
    teams.plant_back = options->plant == PACER_PLANT_TEAMS_BACK;
    if (teams.exhaust) {
        snprintf(fields, size, "mode=exhaust");
        return 0;
    }
    /* The grid is of the whole team: split_2d takes two numbers of every PE,
     * whatever the grid's size. */
    grid_shape(npes, &teams.columns, &teams.rows);
    uint64_t n = (uint64_t)npes, buffers = 0;
    uint64_t side = (uint64_t)(teams.rows > teams.columns ? teams.rows : teams.columns);
    for (int k = 0; k < STRIDED; k++)
        buffers += 2 * (3 * ELEMENTS + n) * 8 * 3;
    buffers += 2 * 2 * (3 * ELEMENTS + side * ELEMENTS) * 8 * 3;
    uint64_t symmetric = buffers + TEAMS * 8;
    if (symmetric > options->memory) {
        snprintf(fields, size, "%d PEs' team buffers need %llu bytes", npes,
                 (unsigned long long)symmetric);
        return UINT64_MAX;
    }
    snprintf(fields, size, "numbers=%d strided=%d grid=%dx%d renew_every=%d kinds=%d", TEAMS,
             STRIDED, teams.rows, teams.columns, RENEW_EVERY, KINDS);
    return symmetric;
}

/* A team's world progression from its parent's and its triplet in the parent. */
static void progression(struct team *t, const struct team *parent, int start, int stride,
                        int size)
{
    t->start = parent->start + start * parent->stride;
    t->stride = stride * parent->stride;
    t->size = size;
}

static int allocate_buffers(struct team *t, size_t members)
{
    t->capacity = 3 * ELEMENTS + members * ELEMENTS;
    for (int p = 0; p < 2; p++) {
        t->longs[p] = shmem_malloc(sizeof(long) * t->capacity);
        t->words[p] = shmem_malloc(sizeof(uint64_t) * t->capacity);
        t->doubles[p] = shmem_malloc(sizeof(double) * t->capacity);
        if (!t->longs[p] || !t->words[p] || !t->doubles[p])
            return -1;
    }
    return 0;
}

/* A team's buffers back, in the reverse of the order they came. shmem_free is
 * collective, so every PE frees them alike, members of the team or not. */
static void release_buffers(struct team *t)
{
    for (int p = 1; p >= 0; p--) {
        shmem_free(t->doubles[p]);
        shmem_free(t->words[p]);
        shmem_free(t->longs[p]);
        t->doubles[p] = NULL;
        t->words[p] = NULL;
        t->longs[p] = NULL;
    }
}

/*
 * Strided team k, made: its triplet in the whole team drawn from the stream -
 * three words for the first and the negative, one for the team of one, none for
 * the split, which takes every other member of the first from its second - its
 * progression in world ranks, and the split. Setup draws the four in turn, and
 * a renewal the one it renews, so that every PE, which draws and splits alike,
 * is given the same teams. A member gets its handle, any other PE
 * SHMEM_TEAM_INVALID. Nonzero when the split is.
 */
static int make_strided(int k)
{
    static const char *const names[STRIDED] = { "strided", "negative", "one", "split" };
    struct team *t = &teams.teams[k];
    const struct team *parent = &teams.world;
    shmem_team_t parent_handle = teams.team;
    uint64_t *state = &teams.state;
    int n = teams.n, start, stride, size;
    switch (k) {
    case 0:                         /* random start, stride and size */
        stride = 1 + (int)(random_next(state) % (uint64_t)(n > 3 ? n / 3 : 1));
        size = 1 + (int)(random_next(state) % (uint64_t)((n - 1) / stride + 1));
        start = (int)(random_next(state) % (uint64_t)(n - (size - 1) * stride));
        break;
    case 1: {                       /* a negative stride, from the top */
        int down = 1 + (int)(random_next(state) % (uint64_t)(n > 2 ? n / 2 : 1));
        size = 1 + (int)(random_next(state) % (uint64_t)((n - 1) / down + 1));
        start = (size - 1) * down +
                (int)(random_next(state) % (uint64_t)(n - (size - 1) * down));
        stride = -down;
        break;
    }
    case 2:                         /* a team of one, by stride 0 */
        start = (int)(random_next(state) % (uint64_t)n);
        stride = 0;
        size = 1;
        break;
    default:                        /* a split of the first, every other member from its second */
        parent = &teams.teams[0];
        parent_handle = parent->handle;
        start = parent->size > 1 ? 1 : 0;
        stride = parent->size > 1 ? 2 : 1;
        size = parent->size > 1 ? parent->size / 2 : 1;
        break;
    }
    *t = (struct team){ .name = names[k], .handle = SHMEM_TEAM_INVALID };
    progression(t, parent, start, stride, size);
    if (parent_handle == SHMEM_TEAM_INVALID)    /* not a member of the parent: no split to join */
        return 0;
    return shmem_team_split_strided(parent_handle, start, stride, size, NULL, 0, &t->handle);
}

/* Every query against arithmetic. */
static void check_queries(struct team *t, const struct team *world_team)
{
    const struct pacer_layout *layout = pacer_layout();
    int rank;
    int member = member_of(t, teams.me_world, &rank);
    int believed = member;
    if (teams.plant_member && teams.me_world == 1 && t == &teams.teams[2]) {
        believed = !member;         /* the planted defect: membership of "one" misjudged */
        teams.plant_member = 0;
    }
    if (believed != (t->handle != SHMEM_TEAM_INVALID)) {
        pacer_failure("teams %s: this PE %s a member, and its handle %s SHMEM_TEAM_INVALID",
                      t->name, believed ? "is" : "is not", believed ? "is" : "is not");
        t->rank = member ? rank : -1;
        return;
    }
    t->rank = rank;
    if (!member)
        return;
    /* The planted defect: every expectation of one team off by one, so that
     * each query's check fires once. */
    int plant = teams.plant_query && teams.me_world == 1 && !teams.query_planted;
    if (plant)
        teams.query_planted = 1;
    int want_rank = rank + plant;
    int n = shmem_team_n_pes(t->handle), my = shmem_team_my_pe(t->handle);
    if (n != t->size || my != want_rank)
        pacer_failure("teams %s: team_n_pes %d and team_my_pe %d, expected %d and %d", t->name,
                      n, my, t->size, want_rank);
    for (int r = 0; r < t->size; r++) {
        int world = shmem_team_translate_pe(t->handle, r, SHMEM_TEAM_WORLD);
        int want_world = t->start + r * t->stride + (plant && r == 0);
        if (world != want_world) {
            pacer_failure("teams %s: rank %d translates to world PE %d, expected %d", t->name, r,
                          world, want_world);
            break;
        }
    }
    for (int r = 0; r < t->size; r++) {     /* apart, so each check stops only itself */
        int world = t->start + r * t->stride;
        void *near = shmem_team_ptr(t->handle, t->longs[0], r);
        int mate = (layout->node[world] == layout->node[teams.me_world]) != (plant && r == 0);
        if ((near != NULL) != mate) {
            pacer_failure("teams %s: team_ptr to rank %d, world PE %d, is %s; it is %s", t->name,
                          r, world, near ? "not NULL" : "NULL",
                          mate ? "a node-mate" : "on another node");
            break;
        }
    }
    for (int w = 0; w < world_team->size; w++) {
        int expected;
        member_of(t, w, &expected);
        expected += plant && w == 0;
        int got = shmem_team_translate_pe(SHMEM_TEAM_WORLD, w, t->handle);
        if (got != expected) {
            pacer_failure("teams %s: world PE %d translates to rank %d, expected %d", t->name, w,
                          got, expected);
            break;
        }
    }
    shmem_team_config_t config = { .num_contexts = -1 };
    if (shmem_team_get_config(t->handle, SHMEM_TEAM_NUM_CONTEXTS, &config) != 0 ||
        config.num_contexts != plant)
        pacer_failure("teams %s: team_get_config gave num_contexts %d, expected %d", t->name,
                      config.num_contexts, plant);
    shmem_team_t named = SHMEM_TEAM_INVALID;
    t->has_ctx = shmem_team_create_ctx(t->handle, 0, &t->ctx) == 0;
    shmem_team_t expected_team = plant ? SHMEM_TEAM_WORLD : t->handle;
    if (!t->has_ctx || shmem_ctx_get_team(t->ctx, &named) != 0 || named != expected_team)
        pacer_failure("teams %s: a context of the team %s", t->name,
                      t->has_ctx ? "names another team" : "could not be created");
}

static int exhaust(void)
{
    /* Splits of the whole team until one returns nonzero. Each PE's count of
     * successes must be every PE's, the job must go on, and the numbers the
     * splits took must come back with the teams: a split of the whole team
     * after they are destroyed succeeds on every PE. */
    enum { MOST = 1024 };
    shmem_team_t *made = calloc(MOST, sizeof *made);
    long *counts = shmem_malloc(4 * sizeof *counts);
    if (!made || !counts)
        return -1;
    int count = 0;
    while (count < MOST &&
           shmem_team_split_strided(teams.team, 0, 1, teams.n, NULL, 0, &made[count]) == 0)
        count++;
    counts[0] = count;
    if (teams.plant_exhaust && teams.me_world == 1)
        counts[0]++;                /* the planted defect: one split more than it made */
    shmem_long_max_reduce(teams.team, counts + 1, counts, 1);
    long most = counts[1];
    if (most != counts[0])
        pacer_failure("teams exhaust: this PE made %ld teams before a split failed, another %ld",
                      counts[0], most);
    shmem_team_sync(teams.team);    /* the job goes on */
    for (int k = 0; k < count; k++)
        shmem_team_destroy(made[k]);
    /* The numbers, and the nodes' blocks, are back. */
    shmem_team_t again = SHMEM_TEAM_INVALID;
    int status = shmem_team_split_strided(teams.team, 0, 1, teams.n, NULL, 0, &again);
    if (teams.plant_back && teams.me_world == 1)
        status = 1;                 /* the planted defect: the split's status misread */
    int back = status == 0 && again != SHMEM_TEAM_INVALID;
    if (!back)
        pacer_failure("teams exhaust: a split of the whole team after the %d teams made were "
                      "destroyed returned %d: their numbers did not come back", count, status);
    shmem_team_destroy(again);
    counts[2] = back;
    shmem_long_sum_reduce(teams.team, counts + 3, counts + 2, 1);
    if (shmem_team_my_pe(teams.team) == 0) {
        char outcome[96] = "succeeded on every PE, and was destroyed";
        if (counts[3] != teams.n)
            snprintf(outcome, sizeof outcome, "failed on %ld of %d PEs", teams.n - counts[3],
                     teams.n);
        printf("teams exhausted after %d splits of the whole team, then the split returned "
               "nonzero on every PE; the teams destroyed, one more split of the whole team %s\n",
               count, outcome);
        fflush(stdout);
    }
    free(made);
    shmem_free(counts);
    return 0;
}

/*
 * Renew a strided team: the one after the last renewed, in turn. The context
 * the exercise made of the team goes first, then the team's buffers - every PE
 * frees them, members or not, as it allocated them - then the team, a
 * collective of its members. Its number goes back to each member, and its
 * node's block to the pool when the last member there has left. Then another
 * of the same kind takes its place, drawn from the stream, with buffers and
 * queries as at setup. Every PE comes here between the same two rounds, having
 * done every collective of the first, so that the renewal's own - the frees,
 * the destroy, the split, the allocations - are called by every PE alike and
 * in the same order.
 */
static void renew(long round)
{
    double started = pacer_now();
    int k = (int)(teams.renewals % STRIDED);
    struct team *t = &teams.teams[k];
    if (t->has_ctx)
        shmem_ctx_destroy(t->ctx);
    release_buffers(t);
    if (t->handle != SHMEM_TEAM_INVALID)
        shmem_team_destroy(t->handle);
    int status = make_strided(k), seen = status;
    if (teams.plant_renew && teams.me_world == 1) {
        seen = 1;                   /* the planted defect: the status misread, and only said */
        teams.plant_renew = 0;
    }
    if (seen != 0)
        pacer_failure("teams round=%ld renew %s: a split returned %d: a number or a block was "
                      "not given back, or OSOE_MAX_TEAMS is too small", round, t->name, seen);
    if (status != 0) {
        /* A split of the kind just given up fails only if its number or its
         * block did not come back, or OSOE_MAX_TEAMS is below what the
         * exercise needs. The slot goes empty, on every PE alike. */
        t->handle = SHMEM_TEAM_INVALID;
        t->start = -1;
        t->stride = 1;
        t->size = 0;
        t->rank = -1;
    }
    if (allocate_buffers(t, (size_t)teams.n) != 0) {
        fprintf(stderr, "osoe-pacer: PE %d: teams could not allocate the buffers of the %s team "
                "it renewed in round %ld\n", teams.me_world, t->name, round);
        shmem_global_exit(2);
    }
    if (status == 0)
        check_queries(t, &teams.world);
    teams.renewals++;
    teams.phase[3] += pacer_now() - started;
}

static int teams_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    teams.team = team;
    teams.n = shmem_team_n_pes(team);
    teams.me_world = shmem_my_pe();
    teams.round = 0;
    teams.renewals = 0;
    teams.last = 0;
    teams.collectives = 0;
    memset(teams.phase, 0, sizeof teams.phase);
    if (teams.exhaust)
        return exhaust();

    int n = teams.n;
    teams.world = (struct team){ .name = "world", .handle = team, .start = 0, .stride = 1,
                                 .size = n };
    teams.world.start = shmem_team_translate_pe(team, 0, SHMEM_TEAM_WORLD);
    teams.state = random_mix(teams.seed, random_name("teams"), 0, 0);
    struct team *t = teams.teams;

    /* The strided teams, in turn: each drawn, then split. */
    for (int k = 0; k < STRIDED; k++)
        if (make_strided(k) != 0) {
            fprintf(stderr, "osoe-pacer: teams: a split failed; OSOE_MAX_TEAMS may be too small: "
                    "teams holds %d numbers a PE at once, beside the predefined teams' three, and "
                    "a split needs a number free on every member of the new team\n", TEAMS);
            return -1;
        }
    /* The grid, of the whole team: every PE is in a row and a column. */
    t[GRID_ROW] = (struct team){ .name = "row", .handle = SHMEM_TEAM_INVALID };
    t[GRID_COLUMN] = (struct team){ .name = "column", .handle = SHMEM_TEAM_INVALID };
    if (shmem_team_split_2d(team, teams.columns, NULL, 0, &t[GRID_ROW].handle, NULL, 0,
                            &t[GRID_COLUMN].handle) != 0)
        return -1;
    /* This PE's row and column, as progressions. */
    int grid_rank = teams.me_world - teams.world.start;
    int x = grid_rank % teams.columns, y = grid_rank / teams.columns;
    teams.grid_row_index = y;
    int row_size = n - y * teams.columns < teams.columns ? n - y * teams.columns : teams.columns;
    progression(&t[GRID_ROW], &teams.world, y * teams.columns, 1, row_size);
    progression(&t[GRID_COLUMN], &teams.world, x, teams.columns,
                (n - x + teams.columns - 1) / teams.columns);

    /* Buffers, collectively and alike on every PE: the strided teams sized for
     * the world, the grid's for its longer side. */
    for (int k = 0; k < STRIDED; k++)
        if (allocate_buffers(&t[k], (size_t)n) != 0)
            return -1;
    size_t side = (size_t)(teams.rows > teams.columns ? teams.rows : teams.columns);
    if (allocate_buffers(&t[GRID_ROW], side) != 0 || allocate_buffers(&t[GRID_COLUMN], side) != 0)
        return -1;
    teams.ring = shmem_calloc(TEAMS, sizeof *teams.ring);
    if (!teams.ring)
        return -1;
    for (int k = 0; k < TEAMS; k++)
        check_queries(&t[k], &teams.world);
    /* Between the grid's row and column: the corner they share. */
    if (t[GRID_ROW].rank >= 0 && t[GRID_COLUMN].rank >= 0) {
        int through = shmem_team_translate_pe(t[GRID_ROW].handle, t[GRID_ROW].rank,
                                              t[GRID_COLUMN].handle);
        int want = t[GRID_COLUMN].rank + (teams.plant_query && teams.me_world == 1);
        if (through != want)
            pacer_failure("teams row: this PE's row rank %d translates to column rank %d, "
                          "expected %d", t[GRID_ROW].rank, through, want);
    }
    shmem_team_sync(team);
    return 0;
}

/* ---- one collective on one team, checked ----------------------------------- */

static long input(int world, long round, int e)
{
    return (long)(random_mix(teams.seed, (uint64_t)world, (uint64_t)round, (uint64_t)e) % 1000);
}

static uint64_t word_input(int world, long round, int e)
{
    return random_mix(teams.seed ^ 1, (uint64_t)world, (uint64_t)round, (uint64_t)e);
}

/* The world PE of team rank r. */
static int world_of(const struct team *t, int r)
{
    return t->start + r * t->stride;
}

static void wrong(const struct team *t, enum kind k, long round, int e, double got, double want)
{
    pacer_failure("teams round=%ld %s %s: element %d is %.17g, expected %.17g", round, t->name,
                  kind_names[k], e, got, want);
}

static void collective(struct team *t, enum kind k, long round)
{
    int parity = (int)(round % 2), m = t->size, me = t->rank;
    long *source = t->longs[parity], *dest = source + ELEMENTS;
    uint64_t *words = t->words[parity], *word_dest = words + ELEMENTS;
    double *doubles = t->doubles[parity], *double_dest = doubles + ELEMENTS;
    for (int e = 0; e < ELEMENTS; e++) {
        source[e] = input(teams.me_world, round, e);
        words[e] = word_input(teams.me_world, round, e);
        doubles[e] = (double)source[e];
    }
    if (k == PRODUCT)
        for (int e = 0; e < ELEMENTS; e++)
            source[e] = 1 + source[e] % 2;              /* no overflow: 2^m */
    /* The planted defect: PE 1's input to its row wrong once in every kind, by
     * a change each kind's result cannot absorb - past every input for max,
     * below for min, 0 for and, all ones for or; and for a broadcast or an
     * exclusive scan, whose result PE 1's input may not reach, PE 1's own
     * expectation. team_sync has no result to check. */
    int plant = teams.plant_collective && teams.me_world == 1 && t == &teams.teams[GRID_ROW] &&
                !teams.collective_planted[k] && k != SYNC;
    if (plant) {
        teams.collective_planted[k] = 1;
        if (k == MAX)
            source[0] = 1000;
        else if (k == MIN)
            source[0] = -1;
        else if (k == PRODUCT)
            source[0] = source[0] == 1 ? 2 : 1;
        else if (k != BROADCAST && k != EXSCAN && k != ALLTOALL)
            source[0] += 1;
        words[0] = k == AND ? 0 : k == OR ? UINT64_MAX : words[0] ^ 1;
        doubles[0] += 1;
    }
    double started = pacer_now();
    switch (k) {
    case SUM: shmem_long_sum_reduce(t->handle, dest, source, ELEMENTS); break;
    case MAX: shmem_long_max_reduce(t->handle, dest, source, ELEMENTS); break;
    case MIN: shmem_long_min_reduce(t->handle, dest, source, ELEMENTS); break;
    case PRODUCT: shmem_long_prod_reduce(t->handle, dest, source, ELEMENTS); break;
    case AND: shmem_uint64_and_reduce(t->handle, word_dest, words, ELEMENTS); break;
    case OR: shmem_uint64_or_reduce(t->handle, word_dest, words, ELEMENTS); break;
    case XOR: shmem_uint64_xor_reduce(t->handle, word_dest, words, ELEMENTS); break;
    case DOUBLE_SUM: shmem_double_sum_reduce(t->handle, double_dest, doubles, ELEMENTS); break;
    case BROADCAST:
        shmem_long_broadcast(t->handle, dest, source, ELEMENTS, (int)(round % m));
        break;
    case COLLECT:
        shmem_long_collect(t->handle, dest, source, (size_t)((teams.me_world + round) % 3));
        break;
    case FCOLLECT: shmem_long_fcollect(t->handle, dest, source, 1); break;
    case ALLTOALL:
        for (int j = 0; j < m; j++)             /* block j for rank j: one element */
            dest[m + j] = input(teams.me_world, round, j) + (plant && j == 0);
        shmem_long_alltoall(t->handle, dest, dest + m, 1);
        break;
    case INSCAN: shmem_long_sum_inscan(t->handle, dest, source, ELEMENTS); break;
    case EXSCAN: shmem_long_sum_exscan(t->handle, dest, source, ELEMENTS); break;
    case SYNC: shmem_team_sync(t->handle); break;
    default: break;
    }
    double checking = pacer_now();
    teams.phase[t == &teams.teams[GRID_ROW] || t == &teams.teams[GRID_COLUMN] ? 0 : 1] +=
        checking - started;
    teams.collectives++;

    /* The expected results, from every member's inputs. */
    switch (k) {
    case SUM: case MAX: case MIN: case PRODUCT: case DOUBLE_SUM:
    case INSCAN: case EXSCAN:
        for (int e = 0; e < ELEMENTS; e++) {
            long want = k == PRODUCT ? 1 : k == MAX ? -1 : k == MIN ? 1000 : 0;
            /* The exclusive scan's is the sum below this rank: 0 at rank 0,
             * as §9.10.10 defines it. */
            int last = k == INSCAN ? me : k == EXSCAN ? me - 1 : m - 1;
            for (int r = 0; r <= last; r++) {
                long v = input(world_of(t, r), round, e);
                if (k == PRODUCT)
                    want *= 1 + v % 2;
                else if (k == MAX)
                    want = v > want ? v : want;
                else if (k == MIN)
                    want = v < want ? v : want;
                else
                    want += v;
            }
            if (plant && k == EXSCAN && e == 0)
                want++;
            if (k == DOUBLE_SUM) {
                if (double_dest[e] != (double)want)
                    wrong(t, k, round, e, double_dest[e], (double)want);
            } else if (dest[e] != want) {
                wrong(t, k, round, e, (double)dest[e], (double)want);
            }
        }
        break;
    case AND: case OR: case XOR:
        for (int e = 0; e < ELEMENTS; e++) {
            uint64_t want = k == AND ? UINT64_MAX : 0;
            for (int r = 0; r < m; r++) {
                uint64_t v = word_input(world_of(t, r), round, e);
                want = k == AND ? want & v : k == OR ? want | v : want ^ v;
            }
            if (word_dest[e] != want)
                pacer_failure("teams round=%ld %s %s: element %d is %016llx, expected %016llx",
                              round, t->name, kind_names[k], e,
                              (unsigned long long)word_dest[e], (unsigned long long)want);
        }
        break;
    case BROADCAST: {
        int root = (int)(round % m);
        for (int e = 0; e < ELEMENTS; e++) {
            long want = input(world_of(t, root), round, e) + (plant && e == 0);
            if ((me != root || plant) && (me == root ? source[e] : dest[e]) != want)
                wrong(t, k, round, e, (double)(me == root ? source[e] : dest[e]), (double)want);
        }
        break;
    }
    case COLLECT: {
        int at = 0;
        for (int r = 0; r < m; r++) {
            int w = world_of(t, r), count = (int)((w + round) % 3);
            for (int e = 0; e < count; e++, at++)
                if (dest[at] != input(w, round, e))
                    wrong(t, k, round, at, (double)dest[at], (double)input(w, round, e));
        }
        break;
    }
    case FCOLLECT:
        for (int r = 0; r < m; r++)
            if (dest[r] != input(world_of(t, r), round, 0))
                wrong(t, k, round, r, (double)dest[r], (double)input(world_of(t, r), round, 0));
        break;
    case ALLTOALL:
        for (int r = 0; r < m; r++)             /* rank r's block for this PE */
            if (dest[r] != input(world_of(t, r), round, me))
                wrong(t, k, round, r, (double)dest[r], (double)input(world_of(t, r), round, me));
        break;
    default:
        break;
    }
    teams.phase[2] += pacer_now() - checking;
}

/* A put and a get back through a team's context, to its next member. */
static void round_trip(struct team *t, int index, long round)
{
    if (!t->has_ctx || t->size < 1)
        return;
    int next = (t->rank + 1) % t->size;
    long value = ((long)teams.me_world << 32) | (round & 0xffffffff);
    shmem_ctx_long_p(t->ctx, &teams.ring[index], value, next);
    shmem_ctx_quiet(t->ctx);
    long back = shmem_ctx_long_g(t->ctx, &teams.ring[index], next);
    if (teams.plant_ring && teams.me_world == 1) {
        value++;                    /* the planted defect: one round trip misjudged */
        teams.plant_ring = 0;
    }
    if (back != value)
        pacer_failure("teams round=%ld %s context: read back %ld from rank %d, not %ld", round,
                      t->name, back, next, value);
}

static void teams_step(long units, int last)
{
    if (teams.exhaust)
        return;
    teams.last = last;
    struct team *t = teams.teams;
    for (long u = 0; u < units; u++, teams.round++) {
        long round = teams.round;
        if (round > 0 && round % RENEW_EVERY == 0)      /* between rounds, on every PE */
            renew(round);
        enum kind k = (enum kind)(round % KINDS);
        if (t[GRID_ROW].rank >= 0) {
            int first = teams.grid_row_index % 2 ? GRID_COLUMN : GRID_ROW;
            collective(&t[first], k, round);
            collective(&t[first == GRID_ROW ? GRID_COLUMN : GRID_ROW], k, round);
        }
        enum kind small = strided_kinds[round % (long)(sizeof strided_kinds / sizeof *strided_kinds)];
        for (int s = 0; s < STRIDED; s++)
            if (t[s].rank >= 0) {
                collective(&t[s], small, round);
                round_trip(&t[s], s, round);
            }
    }
}

static void teams_checkpoint(struct pacer_counters *counters)
{
    counters->finished = teams.exhaust;     /* exhausting is done at setup */
    counters->work = teams.collectives;
    for (int k = 0; k < (int)(sizeof teams.phase / sizeof *teams.phase); k++)
        counters->phase_seconds[k] = teams.phase[k];
    /* The renewals are the same on every PE: rank 0 says them, at the end. */
    if (teams.last && shmem_team_my_pe(teams.team) == 0)
        pacer_summary("summary teams renewals=%ld renew_every=%d", teams.renewals, RENEW_EVERY);
    teams.collectives = 0;
    memset(teams.phase, 0, sizeof teams.phase);
}

static void teams_teardown(void)
{
    if (teams.exhaust)
        return;
    struct team *t = teams.teams;
    shmem_team_sync(teams.team);
    for (int k = TEAMS - 1; k >= 0; k--) {
        if (t[k].has_ctx)
            shmem_ctx_destroy(t[k].ctx);
        if (t[k].handle != SHMEM_TEAM_INVALID)
            shmem_team_destroy(t[k].handle);
    }
    shmem_free(teams.ring);
    for (int k = TEAMS - 1; k >= 0; k--)
        release_buffers(&t[k]);
}

const struct pacer_exercise pacer_teams = {
    .name = "teams",
    .work = "collectives",
    .phases = phase_names,
    .quick_rounds = KINDS,          /* every collective once on the grid */
    .memory = teams_memory,
    .setup = teams_setup,
    .step = teams_step,
    .checkpoint = teams_checkpoint,
    .teardown = teams_teardown,
};
