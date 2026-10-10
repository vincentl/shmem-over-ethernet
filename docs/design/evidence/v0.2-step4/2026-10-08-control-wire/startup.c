/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 2: how long shmem_init takes, which the mesh's two wires a
 * pair lengthen (the brief's Part 2, item 5). Each PE times its own init; PE 0
 * reports the slowest and its own. usage: startup */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdio.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(void)
{
    double start = now();
    shmem_init();
    double took = (now() - start) * 1e3;
    static double each[1024];
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (npes <= 1024)
        shmem_double_p(&each[me], took, 0);
    shmem_barrier_all();
    if (me == 0) {
        double most = 0;
        for (int i = 0; i < npes && i < 1024; i++)
            if (each[i] > most)
                most = each[i];
        printf("startup pes=%d init_ms_pe0=%.1f init_ms_slowest=%.1f\n", npes, took, most);
    }
    shmem_finalize();
    return 0;
}
