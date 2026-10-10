/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 0: what a fetch-add waits behind a put stream on the same
 * connection (head-of-line blocking, §3.2), against the connection buffer's
 * size. Four PEs, two a node, in blocks: PE 0 streams 1 MiB puts to PE 2 while
 * PE 1 times fetch-adds to PE 3, whose requests share node 0's connection to
 * node 1 with the stream. First with no stream, then under it.
 * usage: hol [SECONDS]   (the stream's length; default 10) */
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
    printf("hol phase=%s fetch_adds=%zu median_us=%.0f p90_us=%.0f p99_us=%.0f max_us=%.0f "
           "stream_mib_per_s=%.0f\n", phase, n, us[n / 2], us[n * 9 / 10], us[n * 99 / 100],
           us[n - 1], extra);
    fflush(stdout);
}

enum { BYTES = 1 << 20, MOST = 200000 };

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 10;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (npes != 4) {
        if (me == 0)
            fprintf(stderr, "hol: needs 4 PEs, two a node\n");
        shmem_global_exit(2);
    }
    static long cell;
    static long done;
    static double stream_rate;
    unsigned char *window = shmem_malloc(BYTES);
    unsigned char *source = malloc(BYTES);
    double *us = malloc(MOST * sizeof *us);
    memset(source, 7, BYTES);
    shmem_barrier_all();

    /* Phase 1: fetch-adds alone, for two seconds. */
    if (me == 1) {
        size_t n = 0;
        double end = now() + 2;
        while (now() < end && n < MOST) {
            double t = now();
            (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
            us[n++] = (now() - t) * 1e6;
        }
        report("alone", us, n, 0);
    }
    shmem_barrier_all();

    /* Phase 2: under PE 0's stream to PE 2; PE 0 tells PE 1 when it ends. */
    if (me == 0) {
        double start = now();
        size_t total = 0;
        while (now() - start < seconds) {
            for (int i = 0; i < 64; i++)
                shmem_putmem(window, source, BYTES, 2);
            shmem_quiet();
            total += (size_t)64 * BYTES;
        }
        stream_rate = (double)total / (1 << 20) / (now() - start);
        shmem_double_p(&stream_rate, stream_rate, 1);
        shmem_quiet();
        shmem_long_atomic_set(&done, 1, 1);
    } else if (me == 1) {
        size_t n = 0;
        double warm = now() + 0.5;      /* let the stream fill the pipe first */
        while (now() < warm)
            (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
        while (shmem_long_atomic_fetch(&done, 1) == 0 && n < MOST) {
            double t = now();
            (void)shmem_long_atomic_fetch_add(&cell, 1, 3);
            us[n++] = (now() - t) * 1e6;
        }
        shmem_long_wait_until(&done, SHMEM_CMP_EQ, 1);
        report("stream", us, n, stream_rate);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(source);
    free(us);
    shmem_finalize();
    return 0;
}
