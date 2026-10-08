/* v0.2 step 4 part 2: are a job's data-plane operations slower once its control
 * wires have carried barriers? One PE a node: COUNT 8-byte sum reductions,
 * COUNT barriers, COUNT reductions again, and COUNT puts with a quiet to the
 * next PE before and after a second COUNT barriers. PE 0 reports each mean.
 * usage: after-barriers [COUNT] */
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

static long source, dest, word;
static int me, npes;

static double reductions(long count)
{
    double t = now();
    for (long i = 0; i < count; i++)
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, &dest, &source, 1);
    return (now() - t) / (double)count * 1e6;
}

static double barriers(long count)
{
    double t = now();
    for (long i = 0; i < count; i++)
        shmem_barrier_all();
    return (now() - t) / (double)count * 1e6;
}

static double put_quiets(long count)
{
    shmem_barrier_all();
    double t = now();
    for (long i = 0; i < count; i++) {
        shmem_long_p(&word, i, (me + 1) % npes);
        shmem_quiet();
    }
    double each = (now() - t) / (double)count * 1e6;
    shmem_barrier_all();
    return each;
}

int main(int argc, char **argv)
{
    long count = argc > 1 ? atol(argv[1]) : 1000;
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    source = me;
    (void)reductions(100);
    double r1 = reductions(count), p1 = put_quiets(count);
    double b1 = barriers(count);
    double r2 = reductions(count), p2 = put_quiets(count);
    double b2 = barriers(count);
    double r3 = reductions(count), p3 = put_quiets(count);
    if (me == 0)
        printf("after-barriers pes=%d count=%ld reduce_us=%.1f,%.1f,%.1f put_quiet_us=%.1f,%.1f,%.1f "
               "barrier_us=%.1f,%.1f\n", npes, count, r1, r2, r3, p1, p2, p3, b1, b2);
    shmem_finalize();
    return 0;
}
