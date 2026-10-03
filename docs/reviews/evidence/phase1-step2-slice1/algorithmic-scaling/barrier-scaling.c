/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Barrier and sync latency as a function of the node map.
 *
 * Review evidence (phase1-step2-slice1, algorithmic-scaling). Not part of the
 * suite. Every PE times ROUNDS calls of shmem_sync_all (no quiet) after a
 * warm-up, and PE 0 prints the mean and the maximum per-call time it saw.
 *
 * All numbers from this program were taken in the shared 4-core review VM and
 * are "measured under contention"; with more PEs than cores they measure the
 * scheduler as much as the algorithm. What they can show is the *shape*: how
 * the cost moves with the number of logical nodes at one PE per node (the
 * internode star of progress.c) and with the number of PEs on one node (the
 * flat counter and the wake-all of barrier.c).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <shmem.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 2000;
    shmem_init();
    int me = shmem_my_pe();
    int n = shmem_n_pes();

    for (int i = 0; i < 200; i++)
        shmem_sync_all();

    double worst = 0, t0 = now_us();
    for (int i = 0; i < rounds; i++) {
        double a = now_us();
        shmem_sync_all();
        double d = now_us() - a;
        if (d > worst)
            worst = d;
    }
    double mean = (now_us() - t0) / rounds;
    if (me == 0)
        printf("pes %d rounds %d mean_us %.1f max_us %.1f\n", n, rounds, mean, worst);
    shmem_finalize();
    return 0;
}
