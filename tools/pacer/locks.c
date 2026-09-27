/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * locks - the distributed locks (proposal §4.8; docs/prompts/task-stress-phase3.md).
 *
 * LOCKS locks, and for each a count at its home PE, lock mod N. Every PE, a
 * round at a time, takes random locks - by shmem_set_lock, or by spinning on
 * shmem_test_lock, in turn - and inside each critical section gets the count
 * from its home, adds one and puts it back: deliberately not atomic, since the
 * lock is what protects it. It then releases the lock with shmem_clear_lock
 * and no quiet of its own: the specification makes clear_lock complete the
 * section's stores (§9.13), and that is part of what is checked. Inside the
 * section it also puts its own number into the lock's holder word and reads it
 * back; another PE in the same section would overwrite it.
 *
 * At each checkpoint every PE has counted what it took; a sum reduction gives
 * each lock's acquisitions over the team, and the lock's home compares them
 * with its count. It reports acquisitions a second and the longest any PE
 * waited for a lock.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { LOCKS = 8, ACQUISITIONS = 4 };

static const char *const phase_names[] = { "wait", "held", NULL };

static struct {
    uint64_t seed;
    int plant_count, plant_holder;
    shmem_team_t team;
    int n, me, me_world;
    int *world;                     /* team rank to world PE */
    /* symmetric */
    long *locks;                    /* [LOCKS] */
    long *counts;                   /* [LOCKS]: the home's */
    long *holders;                  /* [LOCKS]: the home's */
    long *taken, *taken_total;      /* [LOCKS]: this PE's, and the team's */
    double *longest, *longest_team; /* [1] */
    /* private */
    long round;
    double acquisitions, phase[2], longest_wait;
} locks;

static uint64_t locks_memory(int npes, const struct pacer_options *options, char *fields,
                             size_t size)
{
    (void)npes;
    locks.seed = options->seed;
    locks.plant_count = options->plant == PACER_PLANT_LOCKS_COUNT;
    locks.plant_holder = options->plant == PACER_PLANT_LOCKS_HOLDER;
    snprintf(fields, size, "locks=%d acquisitions=%d", LOCKS, ACQUISITIONS);
    return LOCKS * sizeof(long) * 5 + 2 * sizeof(double);
}

static int locks_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    int n = shmem_team_n_pes(team);
    locks.team = team;
    locks.n = n;
    locks.me = shmem_team_my_pe(team);
    locks.me_world = shmem_my_pe();
    locks.world = malloc(sizeof *locks.world * (size_t)n);
    if (!locks.world)
        return -1;
    for (int j = 0; j < n; j++)
        locks.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
    /* A lock must be zero before first use (§9.13), on every PE. */
    locks.locks = shmem_calloc(LOCKS, sizeof *locks.locks);
    locks.counts = shmem_calloc(LOCKS, sizeof *locks.counts);
    locks.holders = shmem_calloc(LOCKS, sizeof *locks.holders);
    locks.taken = shmem_calloc(LOCKS, sizeof *locks.taken);
    locks.taken_total = shmem_calloc(LOCKS, sizeof *locks.taken_total);
    locks.longest = shmem_calloc(1, sizeof *locks.longest);
    locks.longest_team = shmem_calloc(1, sizeof *locks.longest_team);
    if (!locks.locks || !locks.counts || !locks.holders || !locks.taken || !locks.taken_total ||
        !locks.longest || !locks.longest_team)
        return -1;
    locks.round = 0;
    locks.acquisitions = locks.longest_wait = 0;
    memset(locks.phase, 0, sizeof locks.phase);
    shmem_team_sync(team);
    return 0;
}

static void locks_step(long units, int last)
{
    (void)last;
    for (long u = 0; u < units; u++, locks.round++) {
        uint64_t state = random_mix(locks.seed ^ random_name("locks"), (uint64_t)locks.me_world,
                                    (uint64_t)locks.round, 0);
        for (int a = 0; a < ACQUISITIONS; a++) {
            uint64_t r = random_next(&state);
            int l = (int)(r % LOCKS);
            int home = locks.world[l % locks.n];
            double asked = pacer_now();
            if ((a + locks.round) % 2) {
                shmem_set_lock(&locks.locks[l]);
            } else {
                while (shmem_test_lock(&locks.locks[l]))
                    ;
            }
            double held = pacer_now();
            double waited = held - asked;
            locks.phase[0] += waited;
            if (waited > locks.longest_wait)
                locks.longest_wait = waited;

            long me = locks.me_world, holder;
            if (locks.plant_holder && locks.me_world == 1) {
                me = -1;            /* the planted defect: a holder word not this PE's */
                locks.plant_holder = 0;
            }
            shmem_long_p(&locks.holders[l], me, home);
            shmem_quiet();          /* the holder word there before the count is read */
            long count = shmem_long_g(&locks.counts[l], home);
            shmem_long_p(&locks.counts[l], count + 1, home);
            holder = shmem_long_g(&locks.holders[l], home);
            if (holder != locks.me_world)
                pacer_failure("locks round=%ld lock %d: PE %ld holds its home's holder word "
                              "while this PE is in the section", locks.round, l, holder);
            locks.taken[l]++;
            if (locks.plant_count && locks.me_world == 1) {
                locks.taken[l]++;   /* the planted defect: one acquisition counted twice */
                locks.plant_count = 0;
            }
            shmem_clear_lock(&locks.locks[l]);
            locks.phase[1] += pacer_now() - held;
            locks.acquisitions++;
        }
    }
}

static void locks_checkpoint(struct pacer_counters *counters)
{
    /* Every section is over: each PE ended its step before joining this
     * reduction, and each clear_lock completed its section's puts. */
    shmem_long_sum_reduce(locks.team, locks.taken_total, locks.taken, LOCKS);
    for (int l = 0; l < LOCKS; l++)
        if (l % locks.n == locks.me && locks.counts[l] != locks.taken_total[l])
            pacer_failure("locks round=%ld lock %d: its count is %ld, but it was taken %ld "
                          "times", locks.round - 1, l, locks.counts[l], locks.taken_total[l]);
    locks.longest[0] = locks.longest_wait;
    shmem_double_max_reduce(locks.team, locks.longest_team, locks.longest, 1);
    if (locks.me == 0) {
        printf("locks longest_wait_us=%.1f\n", locks.longest_team[0] * 1e6);
        fflush(stdout);
    }
    locks.longest_wait = 0;
    counters->work = locks.acquisitions;
    counters->phase_seconds[0] = locks.phase[0];
    counters->phase_seconds[1] = locks.phase[1];
    locks.acquisitions = 0;
    memset(locks.phase, 0, sizeof locks.phase);
}

static void locks_teardown(void)
{
    shmem_team_sync(locks.team);
    shmem_free(locks.longest_team);
    shmem_free(locks.longest);
    shmem_free(locks.taken_total);
    shmem_free(locks.taken);
    shmem_free(locks.holders);
    shmem_free(locks.counts);
    shmem_free(locks.locks);
    free(locks.world);
}

const struct pacer_exercise pacer_locks = {
    .name = "locks",
    .work = "acquisitions",
    .phases = phase_names,
    .quick_rounds = 16,
    .memory = locks_memory,
    .setup = locks_setup,
    .step = locks_step,
    .checkpoint = locks_checkpoint,
    .teardown = locks_teardown,
};
