/*
 * SHMEM_THREAD_MULTIPLE (spec §9.2; design §4.11; phase 1 step 8 slice 2):
 * several threads of each PE in the library at once, each checking its own
 * results - under tsan, the one tool that sees a race inside a process.
 *
 * THREADS threads a PE, in two waves, so that the second wave's threads take
 * the counter blocks and records the first wave's gave back at their exit (a
 * claim takes the lowest free block). Each worker, ROUNDS times:
 *   - on a private context of its own: a put of its own words to the right
 *     neighbour, a blocking get and a nonblocking get of the right
 *     neighbour's fixed table, a fetching AMO, a quiet - each result checked
 *     by value;
 *     several nonblocking gets at once (PIECES of them, more than a small
 *     landing zone's extents, so that each thread's own pieces overfill it
 *     however the scheduler runs the threads, and an issue must copy out
 *     landed pieces, its own or other threads', and wait for room);
 *   - on the default context, which every thread shares: a fetch-add on one
 *     counter at PE 0, a blocking get and a nonblocking one;
 *   - a distributed lock, one per worker number, which the same-numbered
 *     workers of every PE contend for, around a get and a put of a count at
 *     PE 0 - so several threads of one PE hold different locks at once;
 *   - a wait: it waits for its own word, which the same-numbered worker of
 *     its left neighbour sets with an AMO, several workers of one PE waiting
 *     at once (§4.11 item 3's records); and two workers of each PE wait on
 *     one shared word, which PE 0's main thread sets once, and both wake.
 * Then PE 0's counter is every increment of every worker.
 *
 * OSOE_TEST_MULTIPLE=exhaust: OSOE_MAX_THREADS=2, the main thread and two
 * workers in the library at once (the agent's block is not among the two),
 * each worker in a wait nobody ends: the third application thread ends the job
 * naming OSOE_MAX_THREADS (decided 2026-09-25).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

enum { THREADS = 4, ROUNDS = 50, WORDS = 8, PIECES = 12 };

static int me, npes, left, right;
static long *words;             /* [THREADS][WORDS]: each worker's own */
static long *fixed;             /* [THREADS][WORDS]: written once, before the workers */
static long *landing;           /* [THREADS][WORDS]: where the left neighbour puts */
static uint64_t *flags;         /* [THREADS]: each worker's wait word */
static uint64_t *shared_flag;   /* one word two workers of each PE wait on */
static uint64_t *counter;       /* at PE 0: every worker's increments */
static long *locks;             /* [THREADS]: one lock per worker number */
static long *locked_count;      /* [THREADS], at PE 0: counted under the locks */
static pthread_mutex_t report = PTHREAD_MUTEX_INITIALIZER;
static int failures;

/* This PE's total for a thread-scope counter, over every thread's block
 * (test-context.c's). */
static uint64_t counter_total(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    const char *local = osoe_test_launcher("LOCALID");
    uint32_t local_id = (uint32_t)atoi(local ? local : "0");
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, local_id * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return total;
}

static long value(int pe, int thread, int round, int i)
{
    return ((long)pe << 32) | ((long)thread << 24) | ((long)round << 8) | i;
}

static void fail(const char *what, int thread, int round, long got, long want)
{
    pthread_mutex_lock(&report);
    fprintf(stderr, "PE %d thread %d round %d: %s read %ld, not %ld\n", me, thread, round, what,
            got, want);
    failures++;
    pthread_mutex_unlock(&report);
}

struct worker {
    int thread;
    int wave;
};

static void *work(void *argument)
{
    const struct worker *w = argument;
    int t = w->thread;
    shmem_ctx_t ctx = SHMEM_CTX_INVALID;
    if (shmem_ctx_create(SHMEM_CTX_PRIVATE, &ctx) != 0) {
        fail("shmem_ctx_create", t, 0, 1, 0);
        return NULL;
    }
    for (int round = 0; round < ROUNDS; round++) {
        long mine[WORDS], got[PIECES][WORDS];
        for (int i = 0; i < WORDS; i++)
            mine[i] = value(me, t, round, i);
        /* This worker's words, then the neighbour's copy of them. Each
         * worker writes only its own row, so nothing it checks is another's. */
        memcpy(&words[t * WORDS], mine, sizeof mine);
        shmem_ctx_long_put(ctx, &landing[t * WORDS], mine, WORDS, right);
        shmem_ctx_quiet(ctx);
        long fetched = shmem_ctx_long_atomic_fetch(ctx, &fixed[t * WORDS + 2], right);
        if (fetched != value(right, t, 0, 2))
            fail("a fetching AMO of the right neighbour's table", t, round, fetched,
                 value(right, t, 0, 2));
        long one = shmem_ctx_long_g(ctx, &fixed[t * WORDS + 1], right);
        if (one != value(right, t, 0, 1))
            fail("a blocking get of the right neighbour's table", t, round, one,
                 value(right, t, 0, 1));
        memset(got, 0, sizeof got);
        for (int k = 0; k < PIECES; k++)
            shmem_ctx_long_get_nbi(ctx, got[k], &fixed[((t + k) % THREADS) * WORDS], WORDS,
                                   right);
        shmem_ctx_quiet(ctx);
        for (int k = 0; k < PIECES; k++)
            for (int i = 0; i < WORDS; i++)
                if (got[k][i] != value(right, (t + k) % THREADS, 0, i)) {
                    fail("a nonblocking get of the right neighbour's table", t, round,
                         got[k][i], value(right, (t + k) % THREADS, 0, i));
                    break;
                }
        /* The shared default context: an increment at PE 0 and a get. */
        shmem_uint64_atomic_inc(counter, 0);
        long shared_get = shmem_long_g(&fixed[t * WORDS + 3], right);
        if (shared_get != value(right, t, 0, 3))
            fail("a get on the default context", t, round, shared_get, value(right, t, 0, 3));
        long shared_nbi[WORDS];
        memset(shared_nbi, 0, sizeof shared_nbi);
        shmem_long_get_nbi(shared_nbi, &fixed[t * WORDS], WORDS, left);
        shmem_quiet();
        for (int i = 0; i < WORDS; i++)
            if (shared_nbi[i] != value(left, t, 0, i)) {
                fail("a nonblocking get on the default context", t, round, shared_nbi[i],
                     value(left, t, 0, i));
                break;
            }
        /* A count kept by a get and a put, under this worker number's lock. */
        shmem_set_lock(&locks[t]);
        long count = shmem_long_g(&locked_count[t], 0);
        shmem_long_p(&locked_count[t], count + 1, 0);
        shmem_quiet();
        shmem_clear_lock(&locks[t]);
    }
    shmem_quiet();
    /* The wait: this worker's word, set by the left neighbour's same worker;
     * then its own set at the right neighbour. The set comes before the wait,
     * so no worker waits for one that waits in turn. */
    shmem_uint64_atomic_set(&flags[t], (uint64_t)w->wave + 1, right);
    shmem_uint64_wait_until(&flags[t], SHMEM_CMP_EQ, (uint64_t)w->wave + 1);
    /* Two workers of each PE on one word, which PE 0's main thread sets. */
    if (t < 2)
        shmem_uint64_wait_until(shared_flag, SHMEM_CMP_GE, (uint64_t)w->wave + 1);
    shmem_ctx_destroy(ctx);
    return NULL;
}

/* A worker that stays in the library: a wait nobody ends. */
static void *sit(void *argument)
{
    (void)argument;
    shmem_uint64_wait_until(&flags[THREADS - 1], SHMEM_CMP_EQ, UINT64_MAX);
    return NULL;
}

static void exhaust(void)
{
    /* The main thread holds one of the two blocks; two workers in a wait
     * at once need two more. */
    pthread_t a, b;
    CHECK_FATAL(pthread_create(&a, NULL, sit, NULL) == 0);
    CHECK_FATAL(pthread_create(&b, NULL, sit, NULL) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    fprintf(stderr, "PE %d: three threads ran on two blocks\n", me);
    exit(1);
}

int main(void)
{
    int provided = -1;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    left = (me + npes - 1) % npes;
    right = (me + 1) % npes;
    words = shmem_calloc(THREADS * WORDS, sizeof *words);
    fixed = shmem_calloc(THREADS * WORDS, sizeof *fixed);
    landing = shmem_calloc(THREADS * WORDS, sizeof *landing);
    flags = shmem_calloc(THREADS, sizeof *flags);
    shared_flag = shmem_calloc(1, sizeof *shared_flag);
    counter = shmem_calloc(1, sizeof *counter);
    locks = shmem_calloc(THREADS, sizeof *locks);
    locked_count = shmem_calloc(THREADS, sizeof *locked_count);
    CHECK_FATAL(words && fixed && landing && flags && shared_flag && counter && locks &&
                locked_count);
    for (int t = 0; t < THREADS; t++)
        for (int i = 0; i < WORDS; i++)
            fixed[t * WORDS + i] = value(me, t, 0, i);
    shmem_barrier_all();

    const char *mode = getenv("OSOE_TEST_MULTIPLE");
    if (mode && strcmp(mode, "exhaust") == 0)
        exhaust();

    for (int wave = 0; wave < 2; wave++) {
        pthread_t threads[THREADS];
        struct worker workers[THREADS];
        for (int t = 0; t < THREADS; t++) {
            workers[t] = (struct worker){ t, wave };
            CHECK_FATAL(pthread_create(&threads[t], NULL, work, &workers[t]) == 0);
        }
        /* Every PE's workers are past their rounds before PE 0 lets the two
         * shared waiters go: a barrier on the main thread, which the workers'
         * waits do not block. */
        shmem_barrier_all();
        if (me == 0)
            for (int pe = 0; pe < npes; pe++)
                shmem_uint64_atomic_set(shared_flag, (uint64_t)wave + 1, pe);
        for (int t = 0; t < THREADS; t++)
            CHECK_FATAL(pthread_join(threads[t], NULL) == 0);
        shmem_barrier_all();
        /* The landing rows hold the left neighbour's last round, by value. */
        for (int t = 0; t < THREADS; t++)
            for (int i = 0; i < WORDS; i++)
                if (landing[t * WORDS + i] != value(left, t, ROUNDS - 1, i)) {
                    fail("the left neighbour's put", t, ROUNDS - 1, landing[t * WORDS + i],
                         value(left, t, ROUNDS - 1, i));
                    break;
                }
        /* Every PE has checked its rows before any starts the next wave, whose
         * first puts would otherwise land in a slower neighbour's rows while
         * it still reads them. */
        shmem_barrier_all();
    }
    /* With a small landing zone and a neighbour on another node, each
     * thread's pieces overfilled it: an issue found no room at least once,
     * which is what makes the room-making path part of what this test ran.
     * Each thread's own PIECES exceed the zone, so this holds even when the
     * threads run one at a time - under load they did, and a pin that needed
     * them concurrent failed (the load loop of 2026-09-26). */
    const char *landing_bytes = getenv("OSOE_GET_LANDING_BYTES");
    if (landing_bytes && atol(landing_bytes) <= 8192 && shmem_ptr(fixed, right) == NULL)
        CHECK(counter_total("am.landing_full") > 0);
    shmem_barrier_all();
    if (me == 0 && *counter != (uint64_t)npes * THREADS * ROUNDS * 2)
        fail("PE 0's counter", -1, -1, (long)*counter, (long)npes * THREADS * ROUNDS * 2);
    for (int t = 0; me == 0 && t < THREADS; t++)
        if (locked_count[t] != (long)npes * ROUNDS * 2)
            fail("the count kept under a lock", t, -1, locked_count[t], (long)npes * ROUNDS * 2);
    CHECK(failures == 0);
    shmem_barrier_all();
    shmem_free(locked_count);
    shmem_free(locks);
    shmem_free(counter);
    shmem_free(shared_flag);
    shmem_free(flags);
    shmem_free(landing);
    shmem_free(fixed);
    shmem_free(words);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
