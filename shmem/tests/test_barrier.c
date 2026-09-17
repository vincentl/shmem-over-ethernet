/*
 * shmem_barrier_all blocks every PE until all have arrived (spec §9.10.1), and
 * every shmem_finalize is a barrier (§9.1.4). PE r delays r * STAGGER before
 * arriving, so every PE must leave no earlier than (L - 1) * STAGGER after the
 * iteration began.
 */
#include <stdint.h>
#include <time.h>

#include <shmem.h>

#include "check.h"

enum { STAGGER_MS = 30, QUICK = 2000, STAGGERED = 5 };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stagger(int me)
{
    struct timespec ts = { 0, (long)me * STAGGER_MS * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    double need = (n - 1) * STAGGER_MS / 1000.0 * 0.95;

    /* Many epochs without delay. */
    for (int i = 0; i < QUICK; i++)
        shmem_barrier_all();

    /* Skewed arrivals. */
    for (int i = 0; i < STAGGERED; i++) {
        shmem_barrier_all();            /* align the start */
        double t0 = now();
        stagger(me);
        shmem_barrier_all();
        CHECK(now() - t0 >= need);
    }

    /* A non-last finalize is a barrier. */
    shmem_init();
    shmem_barrier_all();
    double t0 = now();
    stagger(me);
    shmem_finalize();
    CHECK(now() - t0 >= need);

    /* So is the last one. */
    shmem_barrier_all();
    t0 = now();
    stagger(me);
    shmem_finalize();
    CHECK(now() - t0 >= need);
    return CHECK_DONE();
}
