/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The hold of a team's local release (design §3.8; the step D review, correctness
 * G1; tools/modelcheck/team_block_model.py, QUEUED).
 *
 * A team's release goes down its tree as RELEASEs queued toward its children,
 * and the node's own members are let go last, and only once none of the team's
 * RELEASEs is still in a control queue (progress.c, release_local). Let go
 * sooner, the members leave the team and give its block back, and a team made
 * at once takes the block again - the pool hands out its highest free block
 * first - which the engine then finds at another generation with the old team's
 * RELEASE still counted: it ends the job over a valid program (known_team,
 * "holds another team ... RELEASEs are queued"). Removed, the hold failed no
 * test: a RELEASE leaves its queue on the pass that queued it unless the wire's
 * last segment is still going out, the review's probe stopped a node under 32
 * MiB of puts, and a control wire carries a few bytes a barrier.
 *
 * OSOE_TEST_HOLD_RELEASE=0:1 does what a slow peer would: once node 0 queues a
 * RELEASE of a created team toward node 1, node 0's control wire to node 1 seals
 * nothing for half a second (progress.c, release_held).
 *
 * Three nodes, a PE each. T is split from the world every round, PE 0 and PE 1
 * its members, nodes 0 and 1 with node 0 its root, and destroyed: the destroy's
 * sync closes on node 0 with its RELEASE for node 1 queued behind the window.
 * Q, split from the world once, is PE 0 and PE 2, nodes 0 and 2, so that a team
 * can be made and synchronized without node 1, whose PE is still in the destroy,
 * and without the wire the window shuts: PE 0 and PE 2 split T2 from Q, which on
 * node 0 takes T's block, and synchronize it. With the hold PE 0 is let out of
 * the destroy as the window ends, T2 is made after it, and node 0 has counted
 * the release held (transport.am.releases_held). Without it PE 0 is out of the
 * destroy at once, T2 takes the block inside the window, and T2's first
 * arrival ends the job on node 0 in the first round.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 3 };

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    CHECK_FATAL(shmem_n_pes() == 3);    /* map 0,1,2: a PE a node */

    shmem_team_t across_zero_and_two = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, 2, NULL, 0,
                                         &across_zero_and_two) == 0);
    CHECK((across_zero_and_two != SHMEM_TEAM_INVALID) == (me != 1));

    uint64_t held_before = 0, held_after = 0;
    if (me == 0)
        CHECK_FATAL(osoe_test_node_metric("transport.am.releases_held", &held_before));
    shmem_barrier_all();

    for (int round = 0; round < ROUNDS; round++) {
        shmem_team_t zero_and_one = SHMEM_TEAM_INVALID;
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 2, NULL, 0,
                                             &zero_and_one) == 0);
        CHECK((zero_and_one != SHMEM_TEAM_INVALID) == (me != 2));
        if (me != 2) {
            double start = now();
            shmem_team_destroy(zero_and_one);
            if (me == 0)
                fprintf(stderr, "PE 0: round %d: the destroy of the team over nodes 0 and 1 "
                                "took %.0f ms\n", round, (now() - start) * 1000.0);
        }
        if (me != 1) {
            shmem_team_t again = SHMEM_TEAM_INVALID;
            CHECK_FATAL(shmem_team_split_strided(across_zero_and_two, 0, 1, 2, NULL, 0,
                                                 &again) == 0);
            CHECK_FATAL(shmem_team_sync(again) == 0);
            shmem_team_destroy(again);
        }
        shmem_barrier_all();
    }

    if (me == 0) {
        CHECK_FATAL(osoe_test_node_metric("transport.am.releases_held", &held_after));
        fprintf(stderr, "PE 0: node 0 held a team's local release %llu times in %d rounds\n",
                (unsigned long long)(held_after - held_before), ROUNDS);
        CHECK(held_after > held_before);
    }
    shmem_team_destroy(across_zero_and_two);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
