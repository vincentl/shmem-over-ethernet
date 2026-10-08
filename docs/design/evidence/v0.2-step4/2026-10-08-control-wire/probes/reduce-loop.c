/* v0.2 step 4 part 2: a loop of 8-byte sum reductions over the world, one PE a
 * node - the small tree on the data plane, which Part 2's control wire slowed
 * at 64 logical nodes while a barrier did not move. PE 0 reports the mean.
 * usage: reduce-loop [COUNT] */
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
    long count = argc > 1 ? atol(argv[1]) : 20000;
    shmem_init();
    int me = shmem_my_pe();
    static long source, dest;
    source = me;
    for (int i = 0; i < 100; i++)
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, &dest, &source, 1);
    double t = now();
    for (long i = 0; i < count; i++)
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, &dest, &source, 1);
    double each = (now() - t) / (double)count;
    if (me == 0)
        printf("reduce-loop pes=%d count=%ld reduce8_us=%.1f\n", shmem_n_pes(), count,
               each * 1e6);
    shmem_finalize();
    return 0;
}
