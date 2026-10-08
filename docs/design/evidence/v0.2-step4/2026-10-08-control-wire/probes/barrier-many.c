/* v0.2 step 4 part 2: the control plane's cost at many logical nodes, one PE a
 * node - shmem_init (the mesh, two wires a pair after Part 2), a barrier (the
 * tree's ARRIVE and RELEASE, over the control wires after Part 2), a reduction
 * of 8 bytes (the small tree), a put and quiet to the next PE (the data plane
 * alone), and shmem_finalize (each wire's close). PE 0 reports the means.
 * usage: barrier-many [COUNT] */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? atol(argv[1]) : 1000;
    double t = now();
    shmem_init();
    double init = now() - t;
    int me = shmem_my_pe(), npes = shmem_n_pes();
    static long source, dest, word;
    for (int i = 0; i < 100; i++)
        shmem_barrier_all();

    t = now();
    for (long i = 0; i < count; i++)
        shmem_barrier_all();
    double barrier = (now() - t) / (double)count;

    source = me;
    t = now();
    for (long i = 0; i < count; i++)
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, &dest, &source, 1);
    double reduce = (now() - t) / (double)count;

    shmem_barrier_all();
    t = now();
    for (long i = 0; i < count; i++) {
        shmem_long_p(&word, i, (me + 1) % npes);
        shmem_quiet();
    }
    double put = (now() - t) / (double)count;
    shmem_barrier_all();

    t = now();
    shmem_finalize();
    double finalize = now() - t;
    if (me == 0)
        printf("barrier-many pes=%d count=%ld init_ms=%.1f barrier_us=%.1f reduce8_us=%.1f "
               "put_quiet_us=%.1f finalize_ms=%.1f\n", npes, count, init * 1e3, barrier * 1e6,
               reduce * 1e6, put * 1e6, finalize * 1e3);
    return 0;
}
