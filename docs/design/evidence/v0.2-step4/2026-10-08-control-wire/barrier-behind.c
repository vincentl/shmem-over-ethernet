/* v0.2 step 4 part 2: what a barrier waits behind a put stream between the
 * same two nodes - the control plane's own number (the brief's Part 2, item 5).
 * Four PEs, two a node, in blocks: PE 0 streams 1 MiB puts to PE 2 while PEs 1
 * and 3, a team across the two nodes, time shmem_team_sync - whose arrival and
 * release cross between the nodes, on the stream's connection before the
 * control wire and on the control wire after it. First with no stream, then
 * under it. usage: barrier-behind [SYNCS]   (each phase's syncs; default 2000) */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report(const char *phase, double *us, size_t n, double extra)
{
    qsort(us, n, sizeof *us, compare);
    printf("barrier-behind phase=%s syncs=%zu median_us=%.0f p90_us=%.0f p99_us=%.0f "
           "max_us=%.0f stream_mib_per_s=%.0f\n", phase, n, us[n / 2], us[n * 9 / 10],
           us[n * 99 / 100], us[n - 1], extra);
    fflush(stdout);
}

enum { BYTES = 1 << 20 };

static void syncs(shmem_team_t team, double *us, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        double t = now();
        shmem_team_sync(team);
        us[i] = (now() - t) * 1e6;
    }
}

int main(int argc, char **argv)
{
    size_t count = argc > 1 ? (size_t)atol(argv[1]) : 2000;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (npes != 4) {
        if (me == 0)
            fprintf(stderr, "barrier-behind: needs 4 PEs, two a node\n");
        shmem_global_exit(2);
    }
    static long stop;
    static double stream_rate;
    unsigned char *window = shmem_malloc(BYTES);
    unsigned char *source = malloc(BYTES);
    double *us = malloc(count * sizeof *us);
    memset(source, 7, BYTES);
    /* PEs 1 and 3: one on each node, neither of them the stream's. */
    shmem_team_t pair = SHMEM_TEAM_INVALID;
    shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 2, 2, NULL, 0, &pair);
    shmem_barrier_all();

    /* Phase 1: syncs alone. */
    if (pair != SHMEM_TEAM_INVALID) {
        syncs(pair, us, count);
        if (me == 1)
            report("alone", us, count, 0);
    }
    shmem_barrier_all();

    /* Phase 2: under PE 0's stream to PE 2, which runs until PE 1 says the
     * syncs are done; the pair waits half a second for the stream to fill the
     * pipe first. */
    if (me == 0) {
        double start = now();
        size_t total = 0;
        while (shmem_long_atomic_fetch(&stop, 0) == 0) {
            for (int i = 0; i < 16; i++)
                shmem_putmem(window, source, BYTES, 2);
            shmem_quiet();
            total += (size_t)16 * BYTES;
        }
        stream_rate = (double)total / (1 << 20) / (now() - start);
        shmem_double_p(&stream_rate, stream_rate, 1);
        shmem_quiet();
    } else if (pair != SHMEM_TEAM_INVALID) {
        struct timespec warm = { 0, 500000000 };
        nanosleep(&warm, NULL);
        syncs(pair, us, count);
        if (me == 1)
            shmem_long_atomic_set(&stop, 1, 0);
    }
    shmem_barrier_all();
    if (me == 1)
        report("stream", us, count, stream_rate);
    shmem_barrier_all();
    if (pair != SHMEM_TEAM_INVALID)
        shmem_team_destroy(pair);
    shmem_free(window);
    free(source);
    free(us);
    shmem_finalize();
    return 0;
}
