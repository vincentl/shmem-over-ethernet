/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * teams - teams of every shape, their queries, and collectives on many teams
 * at once (proposal §4.3; docs/prompts/task-stress-phase2.md).
 *
 * **Made once, from a budget.** A team's number is never reused (design §5.7):
 * a job creates OSOE_MAX_TEAMS - 3 teams in its life, 61 at the default, and a
 * split_2d spends one number a row and one a column. So this exercise makes
 * its teams at setup, from a budget of 16 numbers, and keeps them: a strided
 * team of random start, stride and size; one with a negative stride; a team
 * of one, by the stride 0 the spec allows it; a split of the first; and a grid
 * - split_2d - of the whole team when its rows and columns fit what is left,
 * else of a strided team of its first PEs. The header says what it spent.
 * `--teams-exhaust` is the other mode: it splits until a split returns
 * nonzero, checks that it did on every PE and that the job goes on, and stops.
 *
 * **Every team is a progression of world ranks** - a split of a progression
 * is one, and so is a grid's row or column - so every answer is arithmetic.
 * Checked at setup, for each team: SHMEM_TEAM_INVALID exactly outside it;
 * team_n_pes, team_my_pe; team_translate_pe to the world for every member and
 * from the world for every PE, and between a grid's row and column;
 * team_get_config; team_ptr non-NULL exactly for node-mates; a context of the
 * team whose ctx_get_team names it.
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
 * buffer another member is still checking.
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

enum { BUDGET = 16, ELEMENTS = 4, STRIDED = 4, GRID_ROW = STRIDED, GRID_COLUMN, TEAMS };
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
static const char *const phase_names[] = { "grid", "strided", "check", NULL };

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
    int plant_query, plant_collective, plant_exhaust;
    int grid_size, columns, rows;   /* the grid's parent size, and its shape */
    int grid_row_index;             /* this PE's row in the grid, or -1 */
    int spent;                      /* team numbers */
    /* the teams */
    shmem_team_t team, grid_parent;
    int n, me_world;
    struct team teams[TEAMS];
    long *ring;                     /* symmetric [TEAMS]: the contexts' round trip */
    long round;
    double collectives, phase[3];
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

/* The grid of `size` PEs with the fewest numbers: columns near the square root. */
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
    if (teams.exhaust) {
        snprintf(fields, size, "mode=exhaust");
        return 0;
    }
    /* The grid: the whole team's if its rows and columns fit what the four
     * strided teams leave, else of its first PEs, one number more. */
    int columns, rows, grid = npes;
    grid_shape(grid, &columns, &rows);
    if (STRIDED + columns + rows > BUDGET) {
        while (STRIDED + 1 + columns + rows > BUDGET) {
            grid--;
            grid_shape(grid, &columns, &rows);
        }
    }
    teams.grid_size = grid;
    teams.columns = columns;
    teams.rows = rows;
    teams.spent = STRIDED + columns + rows + (grid < npes);
    uint64_t n = (uint64_t)npes, buffers = 0;
    for (int k = 0; k < STRIDED; k++)
        buffers += 2 * (3 * ELEMENTS + n) * 8 * 3;
    buffers += 2 * 2 * (3 * ELEMENTS + (uint64_t)(rows > columns ? rows : columns) * ELEMENTS) * 8 * 3;
    uint64_t symmetric = buffers + TEAMS * 8;
    if (symmetric > options->memory) {
        snprintf(fields, size, "%d PEs' team buffers need %llu bytes", npes,
                 (unsigned long long)symmetric);
        return UINT64_MAX;
    }
    snprintf(fields, size, "numbers=%d strided=%d grid=%dx%d%s kinds=%d", teams.spent, STRIDED,
             rows, columns, grid < npes ? " grid_of_first" : "", KINDS);
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

/* Every query against arithmetic. */
static void check_queries(struct team *t, const struct team *world_team)
{
    const struct pacer_layout *layout = pacer_layout();
    int rank;
    int member = member_of(t, teams.me_world, &rank);
    if (member != (t->handle != SHMEM_TEAM_INVALID)) {
        pacer_failure("teams %s: this PE %s a member, and its handle %s SHMEM_TEAM_INVALID",
                      t->name, member ? "is" : "is not", member ? "is" : "is not");
        return;
    }
    t->rank = rank;
    if (!member)
        return;
    int want_rank = rank;
    if (teams.plant_query && teams.me_world == 1) {
        want_rank++;                /* the planted defect: one answer not what it asks */
        teams.plant_query = 0;
    }
    int n = shmem_team_n_pes(t->handle), my = shmem_team_my_pe(t->handle);
    if (n != t->size || my != want_rank)
        pacer_failure("teams %s: team_n_pes %d and team_my_pe %d, expected %d and %d", t->name,
                      n, my, t->size, want_rank);
    for (int r = 0; r < t->size; r++) {
        int world = shmem_team_translate_pe(t->handle, r, SHMEM_TEAM_WORLD);
        if (world != t->start + r * t->stride) {
            pacer_failure("teams %s: rank %d translates to world PE %d, expected %d", t->name, r,
                          world, t->start + r * t->stride);
            break;
        }
        void *near = shmem_team_ptr(t->handle, t->longs[0], r);
        int mate = layout->node[world] == layout->node[teams.me_world];
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
        int got = shmem_team_translate_pe(SHMEM_TEAM_WORLD, w, t->handle);
        if (got != expected) {
            pacer_failure("teams %s: world PE %d translates to rank %d, expected %d", t->name, w,
                          got, expected);
            break;
        }
    }
    shmem_team_config_t config = { .num_contexts = -1 };
    if (shmem_team_get_config(t->handle, SHMEM_TEAM_NUM_CONTEXTS, &config) != 0 ||
        config.num_contexts != 0)
        pacer_failure("teams %s: team_get_config gave num_contexts %d, expected 0", t->name,
                      config.num_contexts);
    shmem_team_t named = SHMEM_TEAM_INVALID;
    t->has_ctx = shmem_team_create_ctx(t->handle, 0, &t->ctx) == 0;
    if (!t->has_ctx || shmem_ctx_get_team(t->ctx, &named) != 0 || named != t->handle)
        pacer_failure("teams %s: a context of the team %s", t->name,
                      t->has_ctx ? "names another team" : "could not be created");
}

static int exhaust(void)
{
    /* Splits of the whole team until one returns nonzero. Each PE's count of
     * successes must be every PE's, and the job must go on. */
    enum { MOST = 1024 };
    shmem_team_t *made = calloc(MOST, sizeof *made);
    long *counts = shmem_malloc(2 * sizeof *counts);
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
    if (shmem_team_my_pe(teams.team) == 0) {
        printf("teams exhausted after %d splits of the whole team, then the split returned "
               "nonzero on every PE\n", count);
        fflush(stdout);
    }
    for (int k = 0; k < count; k++)
        shmem_team_destroy(made[k]);
    free(made);
    shmem_free(counts);
    return 0;
}

static int teams_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    teams.team = team;
    teams.n = shmem_team_n_pes(team);
    teams.me_world = shmem_my_pe();
    teams.round = 0;
    teams.collectives = 0;
    memset(teams.phase, 0, sizeof teams.phase);
    if (teams.exhaust)
        return exhaust();

    int n = teams.n;
    struct team world_team = { .name = "world", .handle = team, .start = 0, .stride = 1, .size = n };
    world_team.start = shmem_team_translate_pe(team, 0, SHMEM_TEAM_WORLD);
    uint64_t state = random_mix(teams.seed, random_name("teams"), 0, 0);
    struct team *t = teams.teams;

    /* A: random start, stride and size. */
    int stride = 1 + (int)(random_next(&state) % (uint64_t)(n > 3 ? n / 3 : 1));
    int span = (n - 1) / stride + 1;
    int size = 1 + (int)(random_next(&state) % (uint64_t)span);
    int start = (int)(random_next(&state) % (uint64_t)(n - (size - 1) * stride));
    t[0] = (struct team){ .name = "strided" };
    progression(&t[0], &world_team, start, stride, size);
    /* B: a negative stride, from the top. */
    int down = 1 + (int)(random_next(&state) % (uint64_t)(n > 2 ? n / 2 : 1));
    int down_size = 1 + (int)(random_next(&state) % (uint64_t)((n - 1) / down + 1));
    int down_start = (down_size - 1) * down +
                     (int)(random_next(&state) % (uint64_t)(n - (down_size - 1) * down));
    t[1] = (struct team){ .name = "negative" };
    progression(&t[1], &world_team, down_start, -down, down_size);
    /* C: a team of one, by stride 0. */
    int one = (int)(random_next(&state) % (uint64_t)n);
    t[2] = (struct team){ .name = "one" };
    progression(&t[2], &world_team, one, 0, 1);
    if (shmem_team_split_strided(team, t[0].start - world_team.start, stride, size, NULL, 0,
                                 &t[0].handle) != 0 ||
        shmem_team_split_strided(team, down_start, -down, down_size, NULL, 0, &t[1].handle) != 0 ||
        shmem_team_split_strided(team, one, 0, 1, NULL, 0, &t[2].handle) != 0) {
        fprintf(stderr, "osoe-pacer: teams: a split failed; OSOE_MAX_TEAMS may be too small "
                "for the %d numbers teams spends\n", teams.spent);
        return -1;
    }
    /* D: a split of A, every other member of it from its second. */
    t[3] = (struct team){ .name = "split", .handle = SHMEM_TEAM_INVALID };
    int inner_size = size > 1 ? size / 2 : 1;
    progression(&t[3], &t[0], size > 1 ? 1 : 0, size > 1 ? 2 : 1, inner_size);
    if (t[0].handle != SHMEM_TEAM_INVALID &&
        shmem_team_split_strided(t[0].handle, size > 1 ? 1 : 0, size > 1 ? 2 : 1, inner_size,
                                 NULL, 0, &t[3].handle) != 0)
        return -1;
    /* The grid, of the whole team or of its first PEs. */
    struct team grid_parent = { .name = "grid" };
    progression(&grid_parent, &world_team, 0, 1, teams.grid_size);
    teams.grid_parent = team;
    if (teams.grid_size < n &&
        shmem_team_split_strided(team, 0, 1, teams.grid_size, NULL, 0, &teams.grid_parent) != 0)
        return -1;
    t[GRID_ROW] = (struct team){ .name = "row", .handle = SHMEM_TEAM_INVALID };
    t[GRID_COLUMN] = (struct team){ .name = "column", .handle = SHMEM_TEAM_INVALID };
    if (teams.grid_parent != SHMEM_TEAM_INVALID &&
        shmem_team_split_2d(teams.grid_parent, teams.columns, NULL, 0, &t[GRID_ROW].handle, NULL,
                            0, &t[GRID_COLUMN].handle) != 0)
        return -1;
    /* This PE's row and column, as progressions, if it is in the grid. */
    int grid_rank = teams.me_world - grid_parent.start;
    teams.grid_row_index = -1;
    if (grid_rank >= 0 && grid_rank < teams.grid_size) {
        int x = grid_rank % teams.columns, y = grid_rank / teams.columns;
        teams.grid_row_index = y;
        int row_size = teams.grid_size - y * teams.columns < teams.columns
                       ? teams.grid_size - y * teams.columns : teams.columns;
        progression(&t[GRID_ROW], &grid_parent, y * teams.columns, 1, row_size);
        progression(&t[GRID_COLUMN], &grid_parent, x, teams.columns,
                    (teams.grid_size - x + teams.columns - 1) / teams.columns);
    } else {
        t[GRID_ROW].size = t[GRID_COLUMN].size = 0;
        t[GRID_ROW].start = t[GRID_COLUMN].start = -1;
        t[GRID_ROW].stride = t[GRID_COLUMN].stride = 1;
    }

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
        check_queries(&t[k], &world_team);
    /* Between the grid's row and column: the corner they share. */
    if (t[GRID_ROW].rank >= 0 && t[GRID_COLUMN].rank >= 0) {
        int through = shmem_team_translate_pe(t[GRID_ROW].handle, t[GRID_ROW].rank,
                                              t[GRID_COLUMN].handle);
        if (through != t[GRID_COLUMN].rank)
            pacer_failure("teams row: this PE's row rank %d translates to column rank %d, "
                          "expected %d", t[GRID_ROW].rank, through, t[GRID_COLUMN].rank);
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
    if (teams.plant_collective && teams.me_world == 1 && t == &teams.teams[GRID_ROW] &&
        k == SUM) {
        source[0] += 1;             /* the planted defect: a wrong input, once */
        teams.plant_collective = 0;
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
            dest[m + j] = input(teams.me_world, round, j);
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
        if (me != root)
            for (int e = 0; e < ELEMENTS; e++)
                if (dest[e] != input(world_of(t, root), round, e))
                    wrong(t, k, round, e, (double)dest[e],
                          (double)input(world_of(t, root), round, e));
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
    if (back != value)
        pacer_failure("teams round=%ld %s context: read back %ld from rank %d, not %ld", round,
                      t->name, back, next, value);
}

static void teams_step(long units, int last)
{
    (void)last;
    if (teams.exhaust)
        return;
    struct team *t = teams.teams;
    for (long u = 0; u < units; u++, teams.round++) {
        long round = teams.round;
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
    counters->work = teams.collectives;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = teams.phase[k];
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
    if (teams.grid_parent != teams.team && teams.grid_parent != SHMEM_TEAM_INVALID)
        shmem_team_destroy(teams.grid_parent);
    shmem_free(teams.ring);
    for (int k = TEAMS - 1; k >= 0; k--)
        for (int p = 1; p >= 0; p--) {
            shmem_free(t[k].doubles[p]);
            shmem_free(t[k].words[p]);
            shmem_free(t[k].longs[p]);
        }
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
