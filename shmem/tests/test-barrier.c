/*
 * shmem_barrier_all blocks every PE until all have arrived (spec §9.10.1), and
 * every shmem_finalize is a barrier (§9.1.4). See arrivals.h for the check.
 */
#define _GNU_SOURCE
#include <shmem.h>

#include "arrivals.h"
#include "check.h"
#include "invariants.h"

enum { QUICK = 2000, STAGGERED = 5 };

int main(void)
{
    arrivals_open();
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();

    /* Many epochs without delay. */
    for (int i = 0; i < QUICK; i++)
        shmem_barrier_all();

    /* Skewed arrivals. */
    for (int i = 0; i < STAGGERED; i++) {
        arrivals_arrive(me);
        shmem_barrier_all();
        CHECK(arrivals_left_after_all(n));
    }

    CHECK_INVARIANTS();

    /* A non-last finalize is a barrier. */
    shmem_init();
    arrivals_arrive(me);
    shmem_finalize();
    CHECK(arrivals_left_after_all(n));

    /* So is the last one. */
    arrivals_arrive(me);
    shmem_finalize();
    CHECK(arrivals_left_after_all(n));

    arrivals_close();
    return CHECK_DONE();
}
