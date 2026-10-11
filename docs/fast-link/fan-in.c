/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Fan-in (docs/fast-link.md, §9; the scale investigator's probe of the review
 * of v0.2 step 4's merge point, S3): every PE not on PE 0's node puts COUNT
 * blocks of BYTES into PE 0 at once, ITERATIONS times with a quiet and a barrier
 * between - a node's fan-in from all its peers, which the shared receive pool of
 * the owner that holds each peer's wire takes. PE 0 prints the rate.
 *
 * FAN_IN_STAT, if set, is osoe-stat's path: PE 0 prints its node's counters
 * (osoe-stat -d -j) before and after the iterations, between marker lines, for
 * the receive pool's -ENOBUFS completions and re-arms.
 *
 * usage: fan-in BYTES COUNT ITERATIONS
 */
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

/* PE 0's node's counters, between two marker lines on PE 0's output, when
 * FAN_IN_STAT names osoe-stat: `osoe-stat -d -j JOB` finds the job's segments
 * on this node through its live PEs, and the output goes back with the job's
 * own, so nothing is left on a node without a shared file system. */
static void dump(const char *which)
{
    const char *stat = getenv("FAN_IN_STAT");
    const char *job = getenv("SLURM_JOB_ID") ? getenv("SLURM_JOB_ID") : getenv("OSOE_JOB_ID");
    if (!stat || !job)
        return;
    printf("counters %s begin\n", which);
    fflush(stdout);
    char command[4096];
    snprintf(command, sizeof command, "%s -d -j %s 2>&1", stat, job);
    if (system(command) != 0)
        printf("counters %s: %s failed\n", which, command);
    fflush(stdout);
    printf("counters %s end\n", which);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    size_t bytes = argc > 1 ? (size_t)strtoull(argv[1], NULL, 0) : 65536;
    long count = argc > 2 ? atol(argv[2]) : 16;
    long iterations = argc > 3 ? atol(argv[3]) : 20;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    unsigned char *window = shmem_malloc(bytes * (size_t)npes);
    unsigned char *source = malloc(bytes);
    if (!window || !source) {
        fprintf(stderr, "fan-in: PE %d: no room for %zu bytes a PE\n", me, bytes);
        shmem_global_exit(2);
    }
    memset(source, 1, bytes);
    int sender = me != 0 && shmem_ptr(window, 0) == NULL;
    static long senders;
    shmem_long_atomic_add(&senders, sender, 0);
    shmem_barrier_all();
    if (me == 0)
        dump("start");
    shmem_barrier_all();
    double start = now();
    for (long it = 0; it < iterations; it++) {
        if (sender)
            for (long j = 0; j < count; j++)
                shmem_putmem(window + (size_t)me * bytes, source, bytes, 0);
        shmem_quiet();
        shmem_barrier_all();
    }
    double t = now() - start;
    if (me == 0) {
        dump("end");
        long n = shmem_long_atomic_fetch(&senders, 0);
        double total = (double)n * (double)count * (double)iterations * (double)bytes;
        printf("fan-in pes=%d senders=%ld bytes=%zu count=%ld iterations=%ld seconds=%.3f "
               "mib=%.0f mib_per_s=%.1f\n", npes, n, bytes, count, iterations, t,
               total / (1 << 20), total / (1 << 20) / t);
        fflush(stdout);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(source);
    shmem_finalize();
    return 0;
}
