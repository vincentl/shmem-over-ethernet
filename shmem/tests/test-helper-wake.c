/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A PE's wake reaches the owner of its wire (v0.2 step 4, Part 3; owner-map.h;
 * lead_wake_model.py's mark_wake_apart).
 *
 * With progress helpers, a PE that commits into a connection marks the
 * connection's dirty bit in its owner's bitmap and wakes that owner if it
 * sleeps (osoe_connection_area_mark, osoe_progress_host_wake). A wake that went
 * to another owner would show nowhere but in latency: the owner that holds the
 * wire still finds the bytes at its next sweep, a tenth of a second on. So this
 * counts each owner's wakes - the futex completions on its ring that saw its
 * wake word move (transport.am.wakes, layout 36) - while PE 0, alone on its
 * node, puts to each peer node in turn with a quiet after each put, and every
 * other PE waits in a barrier: the owner of that peer's wire sleeps between the
 * puts, so the puts wake it, and the owner the map names must be woken, and
 * the node's other helper not at all.
 *
 * Run at OSOE_PROGRESS_HELPERS=2 on four nodes of one PE each: node 0's peers
 * are nodes 1, 2 and 3 in slots 0, 1 and 2, whose wires are helper 1's, helper
 * 2's and helper 1's. The deadlock detector, whose requests nudge every
 * helper, is off (OSOE_DEADLOCK_SECONDS=0).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

/* A put and a quiet a round. The owner is asleep at most rounds' put, but not
 * at every one - it may still be finishing the last round's pass - so the
 * bound is a tenth of the rounds; a wake sent elsewhere leaves it at none. */
enum { ROUNDS = 100, WOKEN_AT_LEAST = ROUNDS / 10, OTHER_AT_MOST = 2, HELPERS = 2 };

static uint64_t wakes(uint32_t owner)
{
    uint64_t value = 0;
    CHECK_FATAL(osoe_test_scoped_metric("transport.am.wakes", OSOE_MSCOPE_OWNER, owner, &value));
    return value;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    long *x = shmem_malloc(sizeof *x);
    *x = 0;
    CHECK_FATAL(npes == 4);
    shmem_barrier_all();

    if (me == 0) {
        for (int node = 1; node < npes; node++) {
            uint32_t slot = (uint32_t)node - 1, owner = 1 + slot % HELPERS;
            uint64_t before[HELPERS + 1], after[HELPERS + 1];
            for (uint32_t o = 1; o <= HELPERS; o++)
                before[o] = wakes(o);
            for (long round = 1; round <= ROUNDS; round++) {
                shmem_putmem(x, &round, sizeof round, node);
                shmem_quiet();
            }
            for (uint32_t o = 1; o <= HELPERS; o++)
                after[o] = wakes(o);
            for (uint32_t o = 1; o <= HELPERS; o++) {
                uint64_t woken = after[o] - before[o];
                printf("node %d's wire, helper %u's: helper %u woken %llu times in %d puts\n",
                       node, owner, o, (unsigned long long)woken, ROUNDS);
                CHECK(o == owner ? woken >= WOKEN_AT_LEAST : woken <= OTHER_AT_MOST);
            }
        }
    }

    shmem_barrier_all();
    if (me != 0)
        CHECK(*x == ROUNDS);
    CHECK_INVARIANTS();
    shmem_free(x);
    shmem_finalize();
    return CHECK_DONE();
}
