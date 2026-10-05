/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * scan-odd: the scans' piece rule at team-scans 29f5d41 and after, for counts
 * on either side of a multiple of D (team-scans adversarial review).
 *
 * share = ceil(bytes / D), piece = clamp(share, 8 KiB, 16 KiB), and then
 * per_piece = floor(piece / width): for 2- to 16-byte elements, a count not
 * divisible by D whose bytes lie in (8 KiB * D, 16 KiB * D] is cut into D + 1
 * pieces, the last of (count mod D) elements, which waits for the first
 * piece's down (the window). At the default D = 2: longs from 2,049 to 4,095,
 * every odd count.
 *
 * Each PE times shmem_long_sum_inscan on SHMEM_TEAM_WORLD between barriers;
 * the job's figure for an iteration is the slowest PE's; the median of
 * ITERATIONS is printed for each count. Run with the library's
 * pe.scan.most_pieces_in_flight unchanged (it reads min(D, pieces) at a
 * leaf either way); osoe-stat is not needed.
 *
 *   srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 ./scan-odd 300
 *
 * Expected if the review's arithmetic holds: 2,049 and 3,001 longs about 1.7
 * to 1.9 times 2,048 and 3,000; 4,096 and 1,024 as a control (even).
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <shmem.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    static const size_t counts[] = { 1024, 2048, 2049, 3000, 3001, 4094, 4095, 4096 };
    enum { COUNTS = sizeof counts / sizeof counts[0], MOST = 4096 };
    shmem_init();
    int iterations = argc > 1 ? atoi(argv[1]) : 300;
    long *source = shmem_malloc(MOST * sizeof *source);
    long *dest = shmem_malloc(MOST * sizeof *dest);
    double *took = shmem_malloc(sizeof *took), *most = shmem_malloc(sizeof *most);
    double *runs = malloc((size_t)iterations * sizeof *runs);
    if (!source || !dest || !took || !most || !runs)
        shmem_global_exit(1);
    for (size_t i = 0; i < MOST; i++)
        source[i] = shmem_my_pe() + (long)i;
    for (int c = 0; c < COUNTS; c++) {
        size_t n = counts[c];
        for (int warm = 0; warm < 20; warm++)
            shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, n);
        for (int it = 0; it < iterations; it++) {
            shmem_barrier_all();
            double start = now_us();
            shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, n);
            *took = now_us() - start;
            shmem_double_max_reduce(SHMEM_TEAM_WORLD, most, took, 1);
            runs[it] = *most;
        }
        if (shmem_my_pe() == 0) {
            qsort(runs, (size_t)iterations, sizeof *runs, compare);
            printf("scan-odd: %4zu longs (%6zu bytes): median %8.1f us (p10 %8.1f, p90 %8.1f)\n",
                   n, n * sizeof(long), runs[iterations / 2], runs[iterations / 10],
                   runs[iterations * 9 / 10]);
        }
    }
    shmem_finalize();
    return 0;
}
