/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * threads - SHMEM_THREAD_MULTIPLE (proposal §4.7;
 * docs/prompts/task-stress-phase2.md): T worker threads a PE, each working on
 * four contexts in turn and checking every result it can see:
 *
 *   private     its own shmem_ctx_create(SHMEM_CTX_PRIVATE), each slice inside
 *               a session (SHMEM_CTX_SESSION_BATCH)
 *   serialized  its own shmem_ctx_create(SHMEM_CTX_SERIALIZED)
 *   default     SHMEM_CTX_DEFAULT, every thread of the PE issuing and
 *               quieting on it at once - the lock-free shared path
 *   team        a context of the world reversed, which the PE's threads share,
 *               so that its PE numbers go through a team's numbering
 *
 * and on each, a slice's work:
 *
 *   a row      its own words to the next PE, the same to the previous one,
 *              a quiet - or on odd slices a pe_quiet naming only the next,
 *              which leaves the other put outstanding - and a get of the row
 *              back: a quiet on a shared context must complete this thread's
 *              puts whatever the others issue and quiet meanwhile, the
 *              promise OSOE_CHECK_QUIET checks inside the library;
 *   a table    nonblocking gets of a table the next PE wrote once, completed
 *              by one quiet, checked word by word;
 *   counters   fetch-adds on counters spread over every PE, each thread
 *              keeping what it added where: at each checkpoint the sums over
 *              the team of what was added must be what the counters hold.
 *
 * The driver stays on the main thread: step releases the workers for `units`
 * slices each and waits for all of them; checkpoint, the oracle's reduction
 * and parallel_print run with the workers parked. Initialized with
 * SHMEM_THREAD_MULTIPLE, so named only, as osoe-bench's threads is; the debug
 * and asan tests run it with OSOE_CHECK_QUIET=1 and OSOE_TEST_PERTURB=1.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { ROW = 16, TABLE = 16, GETS = 4, COUNTERS = 8, ADDS = 4, CONTEXTS = 4 };
enum kind { PRIVATE, SERIALIZED, DEFAULT, TEAM };
static const char *const context_names[CONTEXTS] = { "private", "serialized", "default", "team" };
static const char *const phase_names[] = { "private", "serialized", "default", "team", NULL };

struct worker {
    pthread_t thread;
    int index;
    shmem_ctx_t own[2];             /* private, serialized */
    uint64_t *added;                /* [n][COUNTERS]: what this thread added where */
    double operations, seconds[CONTEXTS];
    int failed_to_start;
};

static struct {
    /* the plan */
    int count;                      /* T */
    uint64_t seed;
    int plant_row, plant_table, plant_count;
    /* the team, and the world reversed for the team context */
    shmem_team_t team, reversed;
    shmem_ctx_t team_context;
    int n, me, me_world;
    int next, previous;             /* team ranks */
    int next_world, previous_world;
    int next_reversed, previous_reversed;
    int *world;                     /* team rank to world PE */
    int *reversed_of;               /* team rank to its number in `reversed` */
    /* symmetric */
    uint64_t *rows;                 /* [T][CONTEXTS][ROW], written by the previous PE */
    uint64_t *rows_back;            /* the same, written by the next PE */
    uint64_t *table;                /* [T][TABLE], written once */
    uint64_t *counters;             /* [COUNTERS] */
    uint64_t *reduce_source, *reduce_total;   /* [n][COUNTERS] */
    /* private */
    uint64_t *added;                /* [n][COUNTERS], every thread's since the start */
    struct worker *workers;
    pthread_mutex_t lock;
    pthread_cond_t start, done;
    long generation, units, slice;
    int finished, stop;
} threads;

static uint64_t threads_memory(int npes, const struct pacer_options *options, char *fields,
                               size_t size)
{
    uint64_t n = (uint64_t)npes, t = (uint64_t)options->threads;
    uint64_t symmetric = 8 * (2 * t * CONTEXTS * ROW + t * TABLE + COUNTERS + 2 * n * COUNTERS);
    uint64_t private_bytes = 8 * (t + 1) * n * COUNTERS + 8 * n;
    if (symmetric > options->memory || private_bytes > options->memory) {
        snprintf(fields, size, "%d threads on %d PEs need %llu bytes", options->threads, npes,
                 (unsigned long long)(symmetric > private_bytes ? symmetric : private_bytes));
        return UINT64_MAX;
    }
    threads.count = options->threads;
    threads.seed = options->seed;
    threads.plant_row = options->plant == PACER_PLANT_THREADS_ROW;
    threads.plant_table = options->plant == PACER_PLANT_THREADS_TABLE;
    threads.plant_count = options->plant == PACER_PLANT_THREADS_COUNT;
    snprintf(fields, size, "threads=%d contexts=private,serialized,default,team row=%d gets=%d "
             "counters=%d", threads.count, ROW * 8, GETS, COUNTERS);
    return symmetric;
}

static uint64_t table_word(int pe, int thread, int i)
{
    return random_mix(threads.seed ^ random_name("threads"), (uint64_t)pe, (uint64_t)thread,
                      (uint64_t)i);
}

static uint64_t row_word(int pe, int thread, int context, long slice, int i)
{
    return random_mix(threads.seed ^ random_name("rows"), (uint64_t)pe,
                      ((uint64_t)thread << 32) | (uint64_t)context,
                      ((uint64_t)slice << 8) | (uint64_t)i);
}

/* ---- one slice of one worker, on one context -------------------------------- */

static void on_context(struct worker *w, enum kind k, shmem_ctx_t ctx, long slice,
                       uint64_t *state)
{
    int t = w->index, team_numbering = k == TEAM;
    int next = team_numbering ? threads.next_reversed : threads.next_world;
    int previous = team_numbering ? threads.previous_reversed : threads.previous_world;
    size_t place = ((size_t)t * CONTEXTS + (size_t)k) * ROW;
    double started = pacer_now();

    /* The row: out to both neighbours, a quiet or a pe_quiet naming only the
     * next, and back from the next. */
    uint64_t row[ROW], back[ROW];
    for (int i = 0; i < ROW; i++)
        row[i] = row_word(threads.me_world, t, (int)k, slice, i);
    uint64_t sent = row[3];
    if (t == 0 && threads.me_world == 1 && threads.plant_row) {    /* thread 0's flag alone */
        row[3] ^= 1;                /* the planted defect: one word not what it checks */
        threads.plant_row = 0;
    }
    shmem_ctx_putmem(ctx, threads.rows + place, row, sizeof row, next);
    shmem_ctx_fence(ctx);
    shmem_ctx_putmem(ctx, threads.rows_back + place, row, sizeof row, previous);
    if (slice % 2)
        shmem_ctx_pe_quiet(ctx, &next, 1);
    else
        shmem_ctx_quiet(ctx);
    shmem_ctx_getmem(ctx, back, threads.rows + place, sizeof back, next);
    row[3] = sent;
    for (int i = 0; i < ROW; i++)
        if (back[i] != row[i]) {
            pacer_failure("threads round=%ld %s row: thread %d read back word %d as %016llx, "
                          "not %016llx", slice, context_names[k], t, i,
                          (unsigned long long)back[i], (unsigned long long)row[i]);
            break;
        }

    /* The table: several nonblocking gets, one quiet. The table never changes,
     * so what lands is cleared first: a get the quiet left in flight must not
     * pass on the last slice's words. */
    uint64_t got[GETS][TABLE];
    memset(got, 0xa5, sizeof got);
    for (int g = 0; g < GETS; g++)
        shmem_ctx_getmem_nbi(ctx, got[g], threads.table + (size_t)((t + g) % threads.count) * TABLE,
                             sizeof got[g], next);
    shmem_ctx_quiet(ctx);
    for (int g = 0; g < GETS; g++) {
        int of = (t + g) % threads.count;
        for (int i = 0; i < TABLE; i++)
            if (got[g][i] != table_word(threads.next_world, of, i)) {
                pacer_failure("threads round=%ld %s table: thread %d got word %d of PE %d's row "
                              "%d as %016llx", slice, context_names[k], t, i, threads.next_world,
                              of, (unsigned long long)got[g][i]);
                g = GETS;
                break;
            }
    }

    /* Counters: fetch-adds anywhere, each remembered. */
    for (int a = 0; a < ADDS; a++) {
        uint64_t r = random_next(state);
        int target = (int)(r % (uint64_t)threads.n);
        int counter = (int)((r >> 32) % COUNTERS);
        uint64_t amount = 1 + (r >> 48) % 7;
        int pe = team_numbering ? threads.reversed_of[target] : threads.world[target];
        (void)shmem_ctx_uint64_atomic_fetch_add(ctx, threads.counters + counter, amount, pe);
        if (t == 0 && threads.me_world == 1 && threads.plant_count) {
            amount--;               /* the planted defect: one add counted short */
            threads.plant_count = 0;
        }
        w->added[(size_t)target * COUNTERS + (size_t)counter] += amount;
    }
    w->operations += 3 + GETS + ADDS;
    w->seconds[k] += pacer_now() - started;
}

static void slice_of(struct worker *w, long slice)
{
    uint64_t state = random_mix(threads.seed, random_name("threads"),
                                (uint64_t)threads.me_world, ((uint64_t)w->index << 40) | (uint64_t)slice);
    shmem_ctx_session_config_t config = { .total_ops = 3 + GETS + ADDS };
    shmem_ctx_session_start(w->own[0], SHMEM_CTX_SESSION_BATCH, &config,
                            SHMEM_CTX_SESSION_TOTAL_OPS);
    on_context(w, PRIVATE, w->own[0], slice, &state);
    shmem_ctx_session_stop(w->own[0]);
    on_context(w, SERIALIZED, w->own[1], slice, &state);
    on_context(w, DEFAULT, SHMEM_CTX_DEFAULT, slice, &state);
    on_context(w, TEAM, threads.team_context, slice, &state);
}

static void *worker_main(void *argument)
{
    struct worker *w = argument;
    int created = shmem_ctx_create(SHMEM_CTX_PRIVATE, &w->own[0]) == 0;
    created = created && shmem_ctx_create(SHMEM_CTX_SERIALIZED, &w->own[1]) == 0;
    long seen = 0;
    pthread_mutex_lock(&threads.lock);
    if (!created)
        w->failed_to_start = 1;
    for (;;) {
        while (threads.generation == seen && !threads.stop)
            pthread_cond_wait(&threads.start, &threads.lock);
        if (threads.stop)
            break;
        seen = threads.generation;
        long first = threads.slice, units = threads.units;
        pthread_mutex_unlock(&threads.lock);
        if (!w->failed_to_start)
            for (long u = 0; u < units; u++)
                slice_of(w, first + u);
        pthread_mutex_lock(&threads.lock);
        if (++threads.finished == threads.count)
            pthread_cond_signal(&threads.done);
    }
    pthread_mutex_unlock(&threads.lock);
    if (created) {
        shmem_ctx_destroy(w->own[1]);
        shmem_ctx_destroy(w->own[0]);
    }
    return NULL;
}

/* ---- the exercise ------------------------------------------------------------ */

static int threads_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team), t = threads.count;
    threads.team = team;
    threads.n = n;
    threads.me = shmem_team_my_pe(team);
    threads.me_world = shmem_my_pe();
    threads.next = (threads.me + 1) % n;
    threads.previous = (threads.me + n - 1) % n;
    if (shmem_team_split_strided(team, n - 1, -1, n, NULL, 0, &threads.reversed) != 0 ||
        shmem_team_create_ctx(threads.reversed, 0, &threads.team_context) != 0)
        return -1;
    threads.world = malloc(sizeof *threads.world * (size_t)n);
    threads.reversed_of = malloc(sizeof *threads.reversed_of * (size_t)n);
    threads.added = calloc((size_t)n * COUNTERS, sizeof *threads.added);
    threads.workers = calloc((size_t)t, sizeof *threads.workers);
    if (!threads.world || !threads.reversed_of || !threads.added || !threads.workers)
        return -1;
    for (int j = 0; j < n; j++) {
        threads.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
        threads.reversed_of[j] = shmem_team_translate_pe(team, j, threads.reversed);
    }
    threads.next_world = threads.world[threads.next];
    threads.previous_world = threads.world[threads.previous];
    threads.next_reversed = threads.reversed_of[threads.next];
    threads.previous_reversed = threads.reversed_of[threads.previous];

    size_t rows = (size_t)t * CONTEXTS * ROW;
    threads.rows = shmem_calloc(rows, sizeof *threads.rows);
    threads.rows_back = shmem_calloc(rows, sizeof *threads.rows_back);
    threads.table = shmem_malloc(sizeof *threads.table * (size_t)t * TABLE);
    threads.counters = shmem_calloc(COUNTERS, sizeof *threads.counters);
    threads.reduce_source = shmem_malloc(sizeof *threads.reduce_source * (size_t)n * COUNTERS);
    threads.reduce_total = shmem_malloc(sizeof *threads.reduce_total * (size_t)n * COUNTERS);
    if (!threads.rows || !threads.rows_back || !threads.table || !threads.counters ||
        !threads.reduce_source || !threads.reduce_total)
        return -1;
    for (int th = 0; th < t; th++)
        for (int i = 0; i < TABLE; i++)
            threads.table[(size_t)th * TABLE + i] = table_word(threads.me_world, th, i);
    if (threads.plant_table && threads.me_world == 1)
        threads.table[5] ^= 1;      /* the planted defect: one word of the table */
    shmem_quiet();
    shmem_team_sync(team);          /* every table written before any worker reads one */

    pthread_mutex_init(&threads.lock, NULL);
    pthread_cond_init(&threads.start, NULL);
    pthread_cond_init(&threads.done, NULL);
    threads.generation = threads.slice = 0;
    threads.stop = 0;
    for (int th = 0; th < t; th++) {
        struct worker *w = &threads.workers[th];
        w->index = th;
        w->added = calloc((size_t)n * COUNTERS, sizeof *w->added);
        if (!w->added || pthread_create(&w->thread, NULL, worker_main, w) != 0)
            return -1;
    }
    return 0;
}

static void threads_step(long units, int last)
{
    (void)last;
    pthread_mutex_lock(&threads.lock);
    threads.units = units;
    threads.finished = 0;
    threads.generation++;
    pthread_cond_broadcast(&threads.start);
    while (threads.finished < threads.count)
        pthread_cond_wait(&threads.done, &threads.lock);
    threads.slice += units;
    pthread_mutex_unlock(&threads.lock);
    for (int th = 0; th < threads.count; th++)
        if (threads.workers[th].failed_to_start) {
            pacer_failure("threads: thread %d could not create its contexts", th);
            threads.workers[th].failed_to_start = 0;
        }
}

/* With the workers parked: the counters against the sums over the team of
 * what every thread added to each. */
static void threads_checkpoint(struct pacer_counters *counters)
{
    int n = threads.n;
    size_t cells = (size_t)n * COUNTERS;
    double seconds[CONTEXTS] = { 0 }, operations = 0;
    for (int th = 0; th < threads.count; th++) {
        struct worker *w = &threads.workers[th];
        for (size_t c = 0; c < cells; c++) {
            threads.added[c] += w->added[c];
            w->added[c] = 0;
        }
        for (int k = 0; k < CONTEXTS; k++) {
            seconds[k] += w->seconds[k];
            w->seconds[k] = 0;
        }
        operations += w->operations;
        w->operations = 0;
    }
    memcpy(threads.reduce_source, threads.added, sizeof *threads.added * cells);
    shmem_uint64_sum_reduce(threads.team, threads.reduce_total, threads.reduce_source, cells);
    for (int k = 0; k < COUNTERS; k++) {
        uint64_t want = threads.reduce_total[(size_t)threads.me * COUNTERS + (size_t)k];
        uint64_t have = shmem_uint64_atomic_fetch(threads.counters + k, threads.me_world);
        if (have != want)
            pacer_failure("threads round=%ld counter %d: holds %llu, and %llu were added",
                          threads.slice - 1, k, (unsigned long long)have,
                          (unsigned long long)want);
    }
    counters->work = operations;
    for (int k = 0; k < CONTEXTS; k++)
        counters->phase_seconds[k] = seconds[k];
}

static void threads_teardown(void)
{
    pthread_mutex_lock(&threads.lock);
    threads.stop = 1;
    pthread_cond_broadcast(&threads.start);
    pthread_mutex_unlock(&threads.lock);
    for (int th = 0; th < threads.count; th++) {
        pthread_join(threads.workers[th].thread, NULL);
        free(threads.workers[th].added);
    }
    pthread_cond_destroy(&threads.done);
    pthread_cond_destroy(&threads.start);
    pthread_mutex_destroy(&threads.lock);
    shmem_quiet();
    shmem_team_sync(threads.team);
    shmem_ctx_destroy(threads.team_context);
    shmem_team_destroy(threads.reversed);
    shmem_free(threads.reduce_total);
    shmem_free(threads.reduce_source);
    shmem_free(threads.counters);
    shmem_free(threads.table);
    shmem_free(threads.rows_back);
    shmem_free(threads.rows);
    free(threads.workers);
    free(threads.added);
    free(threads.reversed_of);
    free(threads.world);
}

const struct pacer_exercise pacer_threads = {
    .name = "threads",
    .work = "operations",
    .phases = phase_names,
    .quick_rounds = 8,
    .multiple = 1,
    .memory = threads_memory,
    .setup = threads_setup,
    .step = threads_step,
    .checkpoint = threads_checkpoint,
    .teardown = threads_teardown,
};
