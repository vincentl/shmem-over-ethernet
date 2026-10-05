/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * piece-edge: e-band (../e-band.c) at the sizes either side of a reduction's
 * and a broadcast's piece - a lane's 16 KiB slot less its 16-byte header,
 * 16,368 bytes - so that a power-of-two size's extra piece shows: 16,368 and
 * 16,384, 32,736 and 32,768, 65,472 and 65,536 bytes, each PE's median over
 * 200 calls between barriers, the slowest PE's printed (round six's
 * scale-and-performance review, U1; the lead's run).
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
    static const size_t sizes[] = { 16368, 16384, 32736, 32768, 65472, 65536 };
    for (int op = REDUCE; op <= BROADCAST; op++)
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
            measure((enum operation)op, sizes[s], 200);
    shmem_free(pwork);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return 0;
}
