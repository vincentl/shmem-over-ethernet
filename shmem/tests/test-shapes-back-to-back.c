/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Collectives whose shape changes from one call to the next, back to back
 * (tier 2; v0.2 step 2, Part 2; the step's adversarial read, finding 2, whose
 * probe this is).
 *
 *   test-shapes-back-to-back ROUNDS
 *
 * Large reductions and broadcasts whose sizes cross the chooser's crossovers,
 * so that the fan-out differs from one call to the next, with no barrier
 * between calls - on the world, and on two strided teams, the evens and the
 * odds, whose node counts differ from the world's, so the same size takes
 * another fan-out there, and whose leads are the world's: on a map of two PEs
 * a node, the odds' local lead is local id 1. Broadcast roots rotate; a ring
 * of buffers keeps a call's from the next's. Every result is checked as the
 * call returns.
 *
 * What it holds: a large reduction's grants go to the children of this call's
 * tree - the tree the data path then waits on - and a child takes the grant
 * of this call's parent. Grant the children of another tree, as the
 * read's mutation did, and a child puts into a lane still lent to another
 * team's collective, which the next piece's header names, or finds nothing
 * where a piece should be; with a barrier before every call, as the shapes
 * test has, nothing comes early enough to show it.
 *
 * Since v0.2 step 3 the large reductions past the chooser's crossover take
 * Träff's circulant reduce-scatter and allgather, so its calls run back to
 * back with the trees' and the small ones: each PE counts the world's calls
 * its node's lead says took it (team.tree.algorithm), and the job's line says
 * how many - at least one, or the test fails.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static const size_t SIZES[] = { 40, 2048, 8192, 16384, 16385, 49152, 300000, 1048589, 1025, 4096 };
enum { SIZE_COUNT = sizeof SIZES / sizeof SIZES[0], RING = 4 };

static int me, npes, failures, circulant_calls;

static uint8_t value(int pe, size_t i, int round)
{
    return (uint8_t)(pe * 37 + i * 11 + (size_t)round * 5 + i / 251);
}

/* One reduction or broadcast on `team`, `n` bytes, checked as it returns. */
static void one(shmem_team_t team, int team_size, int my_rank, int (*world_of)(int), int round,
                int op, size_t n, uint8_t *dst, uint8_t *src, const char *label)
{
    for (size_t i = 0; i < n; i++) {
        src[i] = value(me, i, round);
        dst[i] = 0;
    }
    int root = round % team_size;
    if (op == 0)
        CHECK(shmem_uint8_sum_reduce(team, dst, src, n) == 0);
    else
        CHECK(shmem_broadcastmem(team, dst, src, n, root) == 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t want = 0;
        if (op == 0) {
            for (int r = 0; r < team_size; r++)
                want = (uint8_t)(want + value(world_of(r), i, round));
        } else {
            want = value(world_of(root), i, round);
        }
        if (dst[i] != want) {
            fprintf(stderr, "PE %d (rank %d of %s): round %d, a %s of %zu bytes: byte %zu is "
                    "%u, not %u\n", me, my_rank, label, round, op ? "broadcast" : "sum", n, i,
                    dst[i], want);
            failures++;
            return;
        }
    }
}

static int world_identity(int r) { return r; }
static int world_of_even(int r) { return 2 * r; }
static int world_of_odd(int r) { return 2 * r + 1; }

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2);
    int rounds = atoi(argv[1]);
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(npes >= 4);
    size_t most = 0;
    for (int s = 0; s < SIZE_COUNT; s++)
        if (SIZES[s] > most)
            most = SIZES[s];
    uint8_t *src[RING], *dst[RING];
    for (int b = 0; b < RING; b++) {
        src[b] = shmem_malloc(most);
        dst[b] = shmem_malloc(most);
        CHECK_FATAL(src[b] && dst[b]);
    }
    shmem_team_t evens = SHMEM_TEAM_INVALID, odds = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, (npes + 1) / 2, NULL, 0, &evens) ==
          0);
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 2, npes / 2, NULL, 0, &odds) == 0);
    shmem_barrier_all();
    for (int round = 0; round < rounds; round++) {
        int b = round % RING, b2 = (round + 2) % RING;
        size_t n = SIZES[(round * 7 + round / SIZE_COUNT) % SIZE_COUNT];
        size_t m = SIZES[(round * 3 + 1) % SIZE_COUNT];
        one(SHMEM_TEAM_WORLD, npes, me, world_identity, round, round % 2, n, dst[b], src[b],
            "the world");
        /* The world's block is the pool's first; its lead published the call's
         * algorithm before releasing this PE. */
        uint64_t algorithm = 0;
        CHECK(osoe_test_scoped_metric("team.tree.algorithm", OSOE_MSCOPE_TEAM, 0, &algorithm));
        circulant_calls += algorithm == 1;
        if (me % 2 == 0)
            one(evens, (npes + 1) / 2, me / 2, world_of_even, round, (round / 2) % 2, m, dst[b2],
                src[b2], "the evens");
        else
            one(odds, npes / 2, me / 2, world_of_odd, round, (round / 3) % 2, m, dst[b2],
                src[b2], "the odds");
    }
    shmem_barrier_all();
    int *all = shmem_malloc(2 * sizeof *all), *mine = shmem_malloc(2 * sizeof *mine);
    CHECK_FATAL(all && mine);
    mine[0] = failures;
    mine[1] = circulant_calls;
    CHECK(shmem_int_sum_reduce(SHMEM_TEAM_WORLD, all, mine, 2) == 0);
    CHECK(all[0] == 0);
    CHECK(all[1] > 0);
    if (me == 0 && all[0] == 0)
        printf("SHAPES BACK TO BACK: %d rounds on %d PEs, %d of their world calls through "
               "Träff's\n", rounds, npes, all[1]);
    shmem_team_destroy(me % 2 == 0 ? evens : odds);
    shmem_finalize();
    return CHECK_DONE();
}
