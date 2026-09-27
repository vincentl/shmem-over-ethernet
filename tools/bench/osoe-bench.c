/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe-bench - what the library costs on a cluster, one measurement a line.
 *
 *   srun -N4 --ntasks-per-node=4 osoe-bench [WHAT]...
 *   osoe-launch -n 8 -m 0,0,0,0,1,1,1,1 -- osoe-bench [WHAT]...
 *
 * WHAT is any of latency, bandwidth, atomic, barrier, collectives and pairs;
 * all of them by default. And threads, only when named, since it initializes
 * with SHMEM_THREAD_MULTIPLE, which the others should not pay for. `quick` takes a tenth of the trips and a sixteenth of
 * the streams, for the smoke test that runs in every preset. PE 0 measures
 * against two targets - the first PE on another node, and the first other PE on
 * its own node, if there is one - while every other PE waits in a barrier, so
 * its agent serves the traffic and nothing else runs:
 *
 *   latency    a put and quiet, and a get, for sizes from 8 bytes to 1 MiB:
 *              the median and the fastest of many round trips
 *   bandwidth  a stream of puts of each size, quieted at the end, in MiB/s
 *   atomic     a 64-bit fetch-add's median and fastest round trip
 *   barrier    shmem_barrier_all across every PE: the median and fastest
 *   collectives a sum reduction and a broadcast from PE 0 across every PE, of
 *              8 bytes and of 64 KiB; and an fcollect and an alltoall of a
 *              block of 8 bytes and of 4 KiB from every PE, where the job's
 *              blocks fit the window: the median and fastest of each
 *   pairs      every PE at once streams to the PE half the job away - on
 *              another node when the job is a block layout with the same count
 *              on every node - each PE's MiB/s, and the whole job's: every
 *              byte over the slowest PE's time, what the switch and every link
 *              carried together
 *   threads    PE 0 alone, with 1, 2 and 4 threads sharing the default
 *              context: each issues 8-byte puts and nonblocking gets in turn
 *              to the PE on another node, quieting after every 32 pairs, and
 *              the line gives every thread's operations a second together -
 *              how issue from several threads on one context scales (step 8
 *              slice 3's speed claim, measured before and after)
 *
 * Lines start with the measurement's name and give key=value fields, so a
 * shell can pick them apart; times are microseconds. Design §8.4 has what the
 * library should cost against the floor: shmem/tests/test-floor.c measures that
 * ratio between two nodes directly, against an io_uring echo on the same link.
 * A name it does not know is refused, rather than run as nothing.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

enum { LATENCY_TRIPS = 2000, WARMUP = 200, STREAM_BYTES = 64 << 20, MAX_BYTES = 4 << 20 };

static const size_t latency_sizes[] = { 8, 64, 512, 4096, 32768, 262144, 1048576 };
static const size_t stream_sizes[] = { 4096, 65536, 262144, 1048576, 4194304 };

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Median and fastest of n trips, in microseconds. */
static void summarize(uint64_t *trips, int n, double *median, double *fastest)
{
    qsort(trips, (size_t)n, sizeof *trips, ascending);
    *median = (double)trips[n / 2] / 1e3;
    *fastest = (double)trips[0] / 1e3;
}

static int trips_per_size = LATENCY_TRIPS;         /* `quick`: a tenth */
static size_t stream_bytes = STREAM_BYTES;          /* `quick`: a sixteenth */

static unsigned char *window;           /* symmetric, MAX_BYTES */
static unsigned char *source;           /* private, MAX_BYTES */
static uint64_t *counter;               /* symmetric */
static double *results;                 /* symmetric, one stream's seconds per PE */

static void latency(int target, const char *where)
{
    uint64_t *trips = malloc(sizeof *trips * (size_t)trips_per_size);
    for (size_t s = 0; s < sizeof latency_sizes / sizeof *latency_sizes; s++) {
        size_t bytes = latency_sizes[s];
        for (int i = -WARMUP; i < trips_per_size; i++) {
            uint64_t t = now_ns();
            shmem_putmem(window, source, bytes, target);
            shmem_quiet();
            if (i >= 0)
                trips[i] = now_ns() - t;
        }
        double median, fastest;
        summarize(trips, trips_per_size, &median, &fastest);
        printf("latency put+quiet target=%d where=%s bytes=%zu median_us=%.1f fastest_us=%.1f\n",
               target, where, bytes, median, fastest);
        for (int i = -WARMUP; i < trips_per_size; i++) {
            uint64_t t = now_ns();
            shmem_getmem(source, window, bytes, target);
            if (i >= 0)
                trips[i] = now_ns() - t;
        }
        summarize(trips, trips_per_size, &median, &fastest);
        printf("latency get target=%d where=%s bytes=%zu median_us=%.1f fastest_us=%.1f\n",
               target, where, bytes, median, fastest);
        fflush(stdout);
    }
    free(trips);
}

static void bandwidth(int target, const char *where)
{
    for (size_t s = 0; s < sizeof stream_sizes / sizeof *stream_sizes; s++) {
        size_t bytes = stream_sizes[s];
        size_t count = stream_bytes / bytes ? stream_bytes / bytes : 1;
        shmem_putmem(window, source, bytes, target);    /* the connection warm */
        shmem_quiet();
        uint64_t t = now_ns();
        for (size_t i = 0; i < count; i++)
            shmem_putmem(window, source, bytes, target);
        shmem_quiet();
        double seconds = (double)(now_ns() - t) / 1e9;
        printf("bandwidth put target=%d where=%s bytes=%zu mib_per_s=%.1f\n", target, where,
               bytes, (double)(count * bytes) / (1 << 20) / seconds);
        fflush(stdout);
    }
}

static void atomic(int target, const char *where)
{
    uint64_t *trips = malloc(sizeof *trips * (size_t)trips_per_size);
    for (int i = -WARMUP; i < trips_per_size; i++) {
        uint64_t t = now_ns();
        (void)shmem_uint64_atomic_fetch_add(counter, 1, target);
        if (i >= 0)
            trips[i] = now_ns() - t;
    }
    double median, fastest;
    summarize(trips, trips_per_size, &median, &fastest);
    printf("atomic fetch-add target=%d where=%s median_us=%.1f fastest_us=%.1f\n", target,
           where, median, fastest);
    fflush(stdout);
    free(trips);
}

/* Every PE runs this, so every PE is in the barrier each trip. */
static void barrier(int me, int npes)
{
    uint64_t *trips = malloc(sizeof *trips * (size_t)trips_per_size);
    for (int i = -WARMUP; i < trips_per_size; i++) {
        uint64_t t = now_ns();
        shmem_barrier_all();
        if (i >= 0)
            trips[i] = now_ns() - t;
    }
    if (me == 0) {
        double median, fastest;
        summarize(trips, trips_per_size, &median, &fastest);
        printf("barrier pes=%d median_us=%.1f fastest_us=%.1f\n", npes, median, fastest);
        fflush(stdout);
    }
    free(trips);
}

/*
 * Every PE runs this: a sum reduction and a broadcast from PE 0, of one long
 * and of 64 KiB - four of the leads' pieces (collectives.c) - over the world
 * team. PE 0 reports each one's median and fastest, as for the barrier.
 */
static void collectives(int me, int npes)
{
    static const size_t sizes[] = { 8, 65536 };
    long *src = (long *)window;
    long *dst = (long *)(window + MAX_BYTES / 2);
    memset(src, me + 1, 65536);
    uint64_t *trips = malloc(sizeof *trips * (size_t)trips_per_size);
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s] / sizeof *src;
        for (int kind = 0; kind < 2; kind++) {
            shmem_barrier_all();
            for (int i = -WARMUP; i < trips_per_size; i++) {
                uint64_t t = now_ns();
                if (kind == 0)
                    shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, n);
                else
                    shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, n, 0);
                if (i >= 0)
                    trips[i] = now_ns() - t;
            }
            if (me == 0) {
                double median, fastest;
                summarize(trips, trips_per_size, &median, &fastest);
                printf("%s pes=%d bytes=%zu median_us=%.1f fastest_us=%.1f\n",
                       kind == 0 ? "reduce" : "broadcast", npes, sizes[s], median, fastest);
                fflush(stdout);
            }
        }
    }

    /* fcollect and alltoall, a block from every PE - ISx's end-of-run gather
     * of one double is the first - in the window's two halves; every PE skips
     * alike a size whose job's blocks do not fit. */
    static const size_t blocks[] = { 8, 4096 };
    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
        if ((uint64_t)npes * blocks[b] > MAX_BYTES / 2)
            continue;
        for (int kind = 0; kind < 2; kind++) {
            shmem_barrier_all();
            for (int i = -WARMUP; i < trips_per_size; i++) {
                uint64_t t = now_ns();
                if (kind == 0)
                    shmem_fcollectmem(SHMEM_TEAM_WORLD, dst, src, blocks[b]);
                else
                    shmem_alltoallmem(SHMEM_TEAM_WORLD, dst, src, blocks[b]);
                if (i >= 0)
                    trips[i] = now_ns() - t;
            }
            if (me == 0) {
                double median, fastest;
                summarize(trips, trips_per_size, &median, &fastest);
                printf("%s pes=%d block=%zu median_us=%.1f fastest_us=%.1f\n",
                       kind == 0 ? "fcollect" : "alltoall", npes, blocks[b], median, fastest);
                fflush(stdout);
            }
        }
    }
    free(trips);
}

/* Every PE streams to the PE half the job away, all at once. */
static void pairs(int me, int npes)
{
    int target = (me + npes / 2) % npes;
    size_t bytes = 1 << 20, count = stream_bytes / bytes ? stream_bytes / bytes : 1;
    shmem_barrier_all();
    uint64_t t = now_ns();
    for (size_t i = 0; i < count; i++)
        shmem_putmem(window, source, bytes, target);
    shmem_quiet();
    double seconds = (double)(now_ns() - t) / 1e9;
    shmem_putmem(&results[me], &seconds, sizeof seconds, 0);
    shmem_quiet();
    shmem_barrier_all();
    if (me == 0) {
        double mib = (double)(count * bytes) / (1 << 20), slowest = 0;
        for (int p = 0; p < npes; p++) {
            printf("pairs pe=%d target=%d bytes=%zu mib_per_s=%.1f\n", p, (p + npes / 2) % npes,
                   bytes, mib / results[p]);
            if (results[p] > slowest)
                slowest = results[p];
        }
        /* Every byte over the slowest PE's time: what the whole job moved, not
         * a sum of rates each over its own time, which overstates it. */
        printf("pairs pes=%d total_mib_per_s=%.1f\n", npes, mib * npes / slowest);
        fflush(stdout);
    }
}

enum { THREAD_PAIRS = 20000, QUIET_EVERY = 32, MAX_BENCH_THREADS = 4 };
static int thread_pairs = THREAD_PAIRS;         /* `quick`: a tenth */
static int thread_target;

static void *issue(void *argument)
{
    size_t slot = (size_t)(uintptr_t)argument * 64;   /* each thread its own line */
    uint64_t word = 0;
    for (int i = 0; i < thread_pairs; i++) {
        shmem_putmem(window + slot, source, 8, thread_target);
        shmem_getmem_nbi(&word, window + slot + 8, 8, thread_target);
        if ((i + 1) % QUIET_EVERY == 0)
            shmem_quiet();
    }
    shmem_quiet();
    return NULL;
}

static void threads(int me, int remote)
{
    shmem_barrier_all();
    if (me == 0 && remote >= 0) {
        thread_target = remote;
        for (int n = 1; n <= MAX_BENCH_THREADS; n *= 2) {
            pthread_t t[MAX_BENCH_THREADS];
            uint64_t start = now_ns();
            for (int k = 0; k < n; k++)
                if (pthread_create(&t[k], NULL, issue, (void *)(uintptr_t)k) != 0) {
                    fprintf(stderr, "osoe-bench: no thread\n");
                    shmem_global_exit(1);
                }
            for (int k = 0; k < n; k++)
                pthread_join(t[k], NULL);
            double seconds = (double)(now_ns() - start) / 1e9;
            printf("threads n=%d target=%d ops=%d ops_per_s=%.0f\n", n, remote,
                   2 * n * thread_pairs, 2.0 * n * thread_pairs / seconds);
            fflush(stdout);
        }
    }
    shmem_barrier_all();
}

static int wanted(int argc, char **argv, const char *what)
{
    int named = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "quick") == 0)
            continue;
        named = 1;
        if (strcmp(argv[i], what) == 0)
            return 1;
    }
    return !named && strcmp(what, "threads") != 0;   /* nothing named: all but threads */
}

int main(int argc, char **argv)
{
    static const char *const known[] = { "latency", "bandwidth", "atomic", "barrier",
                                         "collectives", "pairs", "threads", "quick" };
    for (int i = 1; i < argc; i++) {
        size_t k = 0;
        while (k < sizeof known / sizeof *known && strcmp(argv[i], known[k]) != 0)
            k++;
        if (k == sizeof known / sizeof *known) {
            fprintf(stderr, "osoe-bench: no measurement '%s': latency, bandwidth, atomic, "
                    "barrier, collectives, pairs, threads, quick\n", argv[i]);
            return 2;
        }
    }
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "quick") == 0) {
            trips_per_size = LATENCY_TRIPS / 10;
            stream_bytes = STREAM_BYTES / 16;
            thread_pairs = THREAD_PAIRS / 10;
        }
    if (wanted(argc, argv, "threads")) {
        int provided = -1;
        shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided);
        if (provided != SHMEM_THREAD_MULTIPLE) {
            fprintf(stderr, "osoe-bench: threads needs SHMEM_THREAD_MULTIPLE, got %d\n", provided);
            shmem_global_exit(1);
        }
    } else {
        shmem_init();
    }
    int me = shmem_my_pe(), npes = shmem_n_pes();
    window = shmem_malloc(MAX_BYTES);
    counter = shmem_calloc(1, sizeof *counter);
    results = shmem_calloc((size_t)npes, sizeof *results);
    source = malloc(MAX_BYTES);
    if (!window || !counter || !results || !source) {
        fprintf(stderr, "osoe-bench: PE %d: out of memory\n", me);
        shmem_global_exit(1);
    }
    memset(source, me + 1, MAX_BYTES);

    /* A PE on another node, and one on this node, as PE 0 sees them: a
     * node-mate's heap is mapped here, so shmem_ptr finds it (§4.1). */
    int remote = -1, local = -1;
    for (int p = 1; p < npes; p++) {
        if (shmem_ptr(window, p) == NULL && remote < 0)
            remote = p;
        if (shmem_ptr(window, p) != NULL && local < 0)
            local = p;
    }
    if (me == 0)
        printf("osoe-bench pes=%d remote_target=%d local_target=%d\n", npes, remote, local);

    const char *kinds[] = { "latency", "bandwidth", "atomic" };
    void (*measure[])(int, const char *) = { latency, bandwidth, atomic };
    for (size_t k = 0; k < 3; k++) {
        if (!wanted(argc, argv, kinds[k]))
            continue;
        shmem_barrier_all();
        if (me == 0) {
            if (remote >= 0)
                measure[k](remote, "remote");
            if (local >= 0)
                measure[k](local, "local");
        }
        shmem_barrier_all();
    }
    if (wanted(argc, argv, "barrier"))
        barrier(me, npes);
    if (wanted(argc, argv, "collectives"))
        collectives(me, npes);
    if (wanted(argc, argv, "pairs") && npes > 1)
        pairs(me, npes);
    if (wanted(argc, argv, "threads"))
        threads(me, remote);

    shmem_free(results);
    shmem_free(counter);
    shmem_free(window);
    free(source);
    shmem_finalize();
    return 0;
}
