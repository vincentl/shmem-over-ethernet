/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Splits whose parent PEs have different thread levels (spec §9.2 lets each PE
 * ask for its own; the team redesign's step D, D7). PE 0 - world rank 0, by the
 * launcher's rank variable (launcher.h) - asks for SHMEM_THREAD_MULTIPLE, every
 * other PE calls shmem_init and has SHMEM_THREAD_SERIALIZED. A split under
 * SHMEM_THREAD_MULTIPLE takes its numbers pending between two reductions; every
 * parent PE must run the same rounds, so the first reduction's marks say
 * whether any parent PE has the level, and all take the second reduction if
 * one does. The adversarial read of the rebuilt part found the first rebuild
 * deciding it on each PE's own level: PE 0 ran a second reduction that the
 * others' sync met, and the split ended with "no block is mapped" or as a
 * deadlock.
 *
 * Several splits of the world, one of two numbers (split_2d), and one making a
 * team PE 0 is not in, each team checked by a sum over it and destroyed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"
#include "launcher.h"

enum { ROUNDS = 8 };

static long *source, *dest;     /* symmetric: shmem_malloc'ed */

/* The sum of world rank + 1 over the PEs of team `t`, against a reduction. */
static void check_team(shmem_team_t t, const char *what)
{
    long want = 0;
    for (int r = 0; r < shmem_team_n_pes(t); r++)
        want += shmem_team_translate_pe(t, r, SHMEM_TEAM_WORLD) + 1;
    *source = shmem_my_pe() + 1;
    *dest = 0;
    CHECK(shmem_long_sum_reduce(t, dest, source, 1) == 0);
    if (*dest != want)
        fprintf(stderr, "PE %d: %s: sum %ld, not %ld\n", shmem_my_pe(), what, *dest, want);
    CHECK(*dest == want);
}

int main(void)
{
    const char *procid = osoe_test_launcher("PROCID");
    int provided = -1;
    if (procid && atoi(procid) == 0) {
        CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
        CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    } else {
        shmem_init();
    }
    int me = shmem_my_pe(), npes = shmem_n_pes();
    source = shmem_malloc(sizeof *source);
    dest = shmem_malloc(sizeof *dest);
    CHECK_FATAL(source && dest);
    shmem_query_thread(&provided);
    CHECK(provided == (me == 0 ? SHMEM_THREAD_MULTIPLE : SHMEM_THREAD_SERIALIZED));

    for (int round = 0; round < ROUNDS; round++) {
        shmem_team_t whole = SHMEM_TEAM_INVALID;
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &whole) == 0);
        check_team(whole, "the world again");

        shmem_team_t x = SHMEM_TEAM_INVALID, y = SHMEM_TEAM_INVALID;
        CHECK_FATAL(shmem_team_split_2d(whole, 2, NULL, 0, &x, NULL, 0, &y) == 0);
        check_team(x, "a row");
        check_team(y, "a column");

        /* A team PE 0 is not in, split from one it is in. */
        shmem_team_t rest = SHMEM_TEAM_INVALID;
        if (npes > 1) {
            CHECK(shmem_team_split_strided(whole, 1, 1, npes - 1, NULL, 0, &rest) == 0);
            CHECK((rest != SHMEM_TEAM_INVALID) == (me != 0));
            if (rest != SHMEM_TEAM_INVALID) {
                check_team(rest, "every PE but PE 0");
                shmem_team_destroy(rest);
            }
        }
        shmem_team_destroy(y);
        shmem_team_destroy(x);
        shmem_team_destroy(whole);
    }
    shmem_free(dest);
    shmem_free(source);
    shmem_barrier_all();
    if (me == 0 && CHECK_DONE() == 0)
        fprintf(stderr, "PE 0: %d rounds of splits at mixed thread levels\n", ROUNDS);
    shmem_finalize();
    return CHECK_DONE();
}
