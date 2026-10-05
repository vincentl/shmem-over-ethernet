/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The team redesign's step E against step D (the brief's Part 2, "the
 * measurements"; the review of D's merge point, scale, the band): the time of a
 * barrier, and of a long sum reduction, a broadcast and an inclusive sum scan on
 * the world at 8 B, 1 KiB, the 2 to 16 KiB band, 32 and 64 KiB and 1 MiB - each
 * PE's median over ITERATIONS calls, each call between two barriers, and the
 * slowest PE's median printed by PE 0, one line a measurement, as the review's
 * q3d-band printed its own. Built with each tree's oshcc and run alternately.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

enum { WARMUP = 20 };

static long *source, *dest;
static long *pwork;             /* symmetric scratch for the slowest median */
static int me, npes;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

enum operation { BARRIER, REDUCE, BROADCAST, SCAN };
static const char *const names[] = { "barrier", "reduce", "broadcast", "scan" };

static void call(enum operation op, size_t n)
{
    switch (op) {
    case BARRIER: shmem_barrier_all(); break;
    case REDUCE: shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n); break;
    case BROADCAST: shmem_long_broadcast(SHMEM_TEAM_WORLD, dest, source, n, 0); break;
    case SCAN: shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, n); break;
    }
}

static void measure(enum operation op, size_t bytes, int iterations)
{
    size_t n = bytes / sizeof(long);
    uint64_t *times = malloc(sizeof *times * iterations);
    for (int i = 0; i < WARMUP + iterations; i++) {
        shmem_barrier_all();
        uint64_t t0 = now_ns();
        call(op, n);
        uint64_t t1 = now_ns();
        if (i >= WARMUP)
            times[i - WARMUP] = t1 - t0;
    }
    qsort(times, iterations, sizeof *times, compare);
    pwork[0] = (long)times[iterations / 2];
    free(times);
    shmem_barrier_all();
    shmem_long_max_reduce(SHMEM_TEAM_WORLD, pwork + 1, pwork, 1);
    if (me == 0)
        printf("e-band pes=%d %s bytes=%zu slowest_pe_median_us=%.1f\n", npes, names[op],
               op == BARRIER ? (size_t)0 : bytes, pwork[1] / 1000.0);
    fflush(stdout);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    source = shmem_malloc(1 << 20);
    dest = shmem_malloc(1 << 20);
    pwork = shmem_malloc(2 * sizeof *pwork);
    if (!source || !dest || !pwork)
        return 1;
    for (size_t i = 0; i < (1 << 20) / sizeof(long); i++)
        source[i] = me + (long)i;
    static const size_t sizes[] = { 8, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 1 << 20 };
    measure(BARRIER, 0, 400);
    for (int op = REDUCE; op <= SCAN; op++)
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
            measure((enum operation)op, sizes[s], sizes[s] >= 65536 ? 50 : 200);
    shmem_free(pwork);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return 0;
}
