/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * scan-skew: the cost of step E part 2's scan grant when one member enters a
 * large scan late (team-scans adversarial review, the timing finding).
 *
 * Every PE: barrier, then the PE whose world rank is LATE (an interior member
 * of the world's scan tree, or the root, or a leaf; -1 for none) spins for
 * DELAY_US, then everyone calls shmem_long_sum_inscan on SHMEM_TEAM_WORLD over
 * N longs. Each PE times barrier exit to scan return; the job's figure for an
 * iteration is the largest over PEs minus DELAY_US (when LATE >= 0) - what the
 * scan cost past the late member's entry. The median over ITERATIONS is
 * printed.
 *
 * Build with oshcc on D's tree (main at 158c13e) and on team-scans; run
 * alternately on the Pi at 4 x 4:
 *   srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 ./scan-skew 256 5 1000 300
 *     (2 KiB of longs, one piece on both trees, so that only the grant
 *      differs; world rank 5 late by 1 ms - with 16 PEs and fan-out 4, rank
 *      5 is the root's second child, on node 1, with children 6 and 7 there
 *      and 8 on node 2)
 *   ... 256 -1 0 300     (nobody late)
 *   ... 256 15 1000 300  (the last rank, a leaf, late)
 *   ... 256 0 1000 300   (the root late)
 * Expected if the review's simulation holds: E minus D about one hop with
 * nobody late, about two with rank 5 or the root late, about none with the
 * leaf late. At sizes where the two trees cut different pieces, compare the
 * differences instead: E(rank 5 late) - E(nobody late) exceeds D's by about
 * a hop.
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
    shmem_init();
    int me = shmem_my_pe();
    size_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 2048;
    int late = argc > 2 ? atoi(argv[2]) : 1;
    double delay = argc > 3 ? atof(argv[3]) : 1000.0;
    int iterations = argc > 4 ? atoi(argv[4]) : 300;
    long *source = shmem_malloc(n * sizeof *source);
    long *dest = shmem_malloc(n * sizeof *dest);
    double *took = shmem_malloc(sizeof *took), *most = shmem_malloc(sizeof *most);
    double *runs = malloc((size_t)iterations * sizeof *runs);
    if (!source || !dest || !took || !most || !runs)
        shmem_global_exit(1);
    for (size_t i = 0; i < n; i++)
        source[i] = me + (long)i;
    for (int warm = 0; warm < 20; warm++)
        shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, n);
    for (int it = 0; it < iterations; it++) {
        shmem_barrier_all();
        double start = now_us();
        if (me == late)
            while (now_us() - start < delay)
                ;
        shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, n);
        *took = now_us() - start;
        shmem_double_max_reduce(SHMEM_TEAM_WORLD, most, took, 1);
        runs[it] = *most - (late >= 0 ? delay : 0.0);
    }
    if (me == 0) {
        qsort(runs, (size_t)iterations, sizeof *runs, compare);
        printf("scan-skew: %zu bytes, PE %d late by %.0f us: median %.1f us past its entry "
               "(p10 %.1f, p90 %.1f)\n", n * sizeof(long), late, late >= 0 ? delay : 0.0,
               runs[iterations / 2], runs[iterations / 10], runs[iterations * 9 / 10]);
    }
    shmem_finalize();
    return 0;
}
