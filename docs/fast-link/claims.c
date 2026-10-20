/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Leads claiming team blocks at once (docs/fast-link.md, §18; P9 of
 * docs/prompts/v0.2-open-items.md). OpenSHMEM 1.5 has no team of one PE, so
 * each PE first makes its own, its row of a split of the world in two
 * dimensions with rows of one PE (shmem_team_split_2d, xrange 1). Then every
 * PE splits its own team of one KEEP times - a split whose parent is itself,
 * and whose new team it leads on its node and so claims a block of the
 * node's pool for (node.c, osoe_node_team_claim) - all of a node's PEs at
 * once, ITERATIONS times, the teams destroyed between iterations. Before P9
 * every claim tried the pool's top word first and again after each lost swap;
 * since, a lost swap moves it to a word chosen by the PE's local id.
 *
 * Each iteration starts at a time PE 0 sets 2 ms ahead, which every PE waits
 * for by the clock, so that the splits begin together and not in the order a
 * barrier lets the PEs go. A PE's time is from that start to its last split's
 * return; an iteration's, the last PE's. PE 0 prints the median and the 90th
 * percentile of the iterations' times, and percentiles of every PE's time a
 * split.
 *
 * usage: claims ITERATIONS KEEP
 */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { KEEP_MOST = 8 };   /* 576 of a default pool's 1,021 blocks at 64 PEs */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int ascending(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The value at fraction `f` of `n` sorted values. */
static double at(const double *sorted, long n, double f)
{
    long i = (long)(f * (double)(n - 1) + 0.5);
    return sorted[i < 0 ? 0 : i >= n ? n - 1 : i];
}

int main(int argc, char **argv)
{
    long iterations = argc > 1 ? atol(argv[1]) : 1000;
    int keep = argc > 2 ? atoi(argv[2]) : 1;
    if (iterations < 1 || keep < 1 || keep > KEEP_MOST) {
        fprintf(stderr, "usage: claims ITERATIONS KEEP (1 to %d)\n", KEEP_MOST);
        return 2;
    }
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    static double start;                       /* PE 0's, read by every PE */
    double *ends = shmem_malloc((size_t)iterations * sizeof *ends);
    double *last = shmem_malloc((size_t)iterations * sizeof *last);
    double *splits = shmem_malloc((size_t)iterations * (size_t)npes * sizeof *splits);
    double *own = malloc((size_t)iterations * sizeof *own);
    if (!ends || !last || !splits || !own) {
        fprintf(stderr, "claims: PE %d: no room for %ld iterations\n", me, iterations);
        shmem_global_exit(1);
    }
    shmem_team_t alone, column, teams[KEEP_MOST];
    if (shmem_team_split_2d(SHMEM_TEAM_WORLD, 1, NULL, 0, &alone, NULL, 0, &column) != 0 ||
        alone == SHMEM_TEAM_INVALID) {
        fprintf(stderr, "claims: PE %d: no team of its own\n", me);
        shmem_global_exit(1);
    }
    shmem_team_destroy(column);
    for (long i = 0; i < iterations; i++) {
        if (me == 0)
            start = now() + 0.002;
        shmem_barrier_all();
        double begin = shmem_double_g(&start, 0);
        while (now() < begin)
            ;
        double t0 = now();
        for (int k = 0; k < keep; k++)
            if (shmem_team_split_strided(alone, 0, 1, 1, NULL, 0, &teams[k]) != 0 ||
                teams[k] == SHMEM_TEAM_INVALID) {
                fprintf(stderr, "claims: PE %d: split %d of iteration %ld failed\n", me, k, i);
                shmem_global_exit(1);
            }
        double t1 = now();
        ends[i] = t1 - begin;
        own[i] = (t1 - t0) / keep;
        shmem_barrier_all();
        for (int k = 0; k < keep; k++)
            shmem_team_destroy(teams[k]);
    }
    /* Every PE's split times to PE 0, and the iterations' last ends. */
    shmem_putmem(splits + (size_t)me * (size_t)iterations, own,
                 (size_t)iterations * sizeof *own, 0);
    shmem_barrier_all();
    shmem_double_max_reduce(SHMEM_TEAM_WORLD, last, ends, (size_t)iterations);
    if (me == 0) {
        long all = iterations * npes;
        qsort(last, (size_t)iterations, sizeof *last, ascending);
        qsort(splits, (size_t)all, sizeof *splits, ascending);
        printf("claims pes=%d keep=%d iterations=%ld last_us=%.1f last_p90_us=%.1f "
               "split_us=%.2f split_p90_us=%.2f split_p99_us=%.2f\n", npes, keep, iterations,
               at(last, iterations, 0.5) * 1e6, at(last, iterations, 0.9) * 1e6,
               at(splits, all, 0.5) * 1e6, at(splits, all, 0.9) * 1e6,
               at(splits, all, 0.99) * 1e6);
    }
    shmem_barrier_all();
    shmem_team_destroy(alone);
    shmem_free(splits);
    shmem_free(last);
    shmem_free(ends);
    free(own);
    shmem_finalize();
    return 0;
}
