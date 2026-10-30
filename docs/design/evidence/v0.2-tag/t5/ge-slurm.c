/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * T5, a check of the 0.2 tag, as 0.1.0's (docs/decisions/2026-10-05-release-point.md):
 * shmem_global_exit under Slurm's srun -K1, the caller's atexit handler taking a given time.
 *   ge-slurm CALLER HANDLER_MS STATUS
 * PE CALLER registers a handler that sleeps HANDLER_MS and then says it finished, and ends the
 * job with STATUS while every other PE computes; srun must return STATUS and the handler finish.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <shmem.h>

static long handler_ms;
static int me;

static void handler(void)
{
    struct timespec t = { handler_ms / 1000, (handler_ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
    printf("PE %d: handler finished after %ld ms\n", me, handler_ms);
}

int main(int argc, char **argv)
{
    int caller = argc > 1 ? atoi(argv[1]) : 0;
    handler_ms = argc > 2 ? atol(argv[2]) : 0;
    int status = argc > 3 ? atoi(argv[3]) : 7;
    shmem_init();
    me = shmem_my_pe();
    if (me == caller)
        atexit(handler);
    shmem_barrier_all();
    if (me == caller) {
        struct timespec settle = { 0, 200 * 1000 * 1000 };
        nanosleep(&settle, NULL);
        shmem_global_exit(status);
    }
    volatile unsigned long spins = 0;
    for (;;)
        spins++;
}
