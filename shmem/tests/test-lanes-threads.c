/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A lane for each thread, under SHMEM_THREAD_MULTIPLE (design §5.8; the team
 * redesign's step E, E9 of docs/design/team-resources-proposal.md;
 * internode-generic.c's own_lane). Two threads of a PE may make large
 * collectives on two teams at once, each through the lane its own thread
 * lends; for a scan that is every member of a team, not only a node's leads,
 * so any SHMEM_THREAD_MULTIPLE program whose threads scan teams of their own
 * depends on it.
 *
 * Round six's finding H1 (docs/reviews/archive/phase1-step7-slice4-
 * correctness.md): the scans' one-lane-per-thread rule had no test. A one-line
 * regression - osoe_internode_generic_scan binding lane 0 where it binds its
 * thread's own, so that every large scan of a PE goes through one lane - passed
 * all 115 of the suite's lane, scan, pipeline and team tests and returned wrong
 * prefix sums with status 0, since a scan's pieces carry no header that could
 * name the difference. The same regression for the reductions ends the job by
 * name (shmem.lanes.two-threads, which leads reductions only); test-scan.c has
 * no threads. This is the stress the review's correctness investigator wrote to
 * find that, kept as the test that fails without the rule.
 *
 * THREADS threads a PE, each with its own copy of the world (made before the
 * threads start), each round splitting that copy into a team of its own shape -
 * all PEs, all but the first, the even ones, the world reversed - so that
 * numbers are given back and taken again across threads and parents; then on
 * that team small and large sums, broadcasts from a turning root and inclusive
 * and exclusive scans, with a large sum on the parent between them, so that each
 * thread's one lane serves reductions, broadcasts and scans of two teams in
 * turn while the PE's other threads use theirs. Every result is checked, and a
 * failure ends the PE at once, naming the thread, the round, the operation and
 * the element.
 *
 * Under the review's two mutations it fails within its first rounds, on both of
 * its maps: scan_one_lane (the scan's lane bound to 0) by a wrong prefix sum,
 * which this test names; and one_lane (the reductions' lane, and the lane the
 * grant names, bound to 0) by the library's own message, which names what was
 * expected in a lane and what is there instead.
 *
 * Usage: test-lanes-threads [ROUNDS [THREADS]], by default 300 rounds of 4
 * threads: a second or two a map, ten under tsan.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"

enum {
    MOST_THREADS = 8,
    MOST_ELEMENTS = 6000,               /* longs in a buffer: the byte buffers are as large */
    DEFAULT_ROUNDS = 300,
    DEFAULT_THREADS = 4,
};

static int me, npes, threads = DEFAULT_THREADS, rounds = DEFAULT_ROUNDS;
/* Symmetric, each thread its own, so that the threads' collectives run at once. */
static long *src[MOST_THREADS], *dst[MOST_THREADS];
static unsigned char *byte_source[MOST_THREADS], *byte_destination[MOST_THREADS];
static shmem_team_t parent[MOST_THREADS];       /* each thread's copy of the world */

/*
 * A failed check, named by PE, then by thread and round, which every use puts
 * first in its format, and out of the process at once with a nonzero status
 * (check.h's CHECK_FATAL): the PE's other threads and the other PEs are in
 * collectives that this thread will not come back to, and a recorded failure
 * would turn into a wait for the launcher's timeout. One fprintf, so that two
 * threads failing together do not interleave their lines.
 */
#define FAIL(format, ...)                                                                      \
    do {                                                                                       \
        fprintf(stderr, "PE %d: " format "\n", me, __VA_ARGS__);                               \
        CHECK_FATAL(false);                                                                    \
    } while (0)

static long value(int pe, int round, int thread, size_t element)
{
    return (long)(pe + 1) * 1000003L + (long)round * 7919L + (long)thread * 104729L +
           (long)element;
}

/* Sizes either side of the default 1 KiB line, in longs: 8, 128 (1,024 B,
 * small), 129 (large, one piece), 2048 (16 KiB, two pieces), 5000 (40 KB). */
static const size_t sizes[] = { 8, 128, 129, 2048, 5000 };

/* A sum of `count` longs on `team`, thread `thread`'s buffers, every element
 * checked against the sum of what each member of the team put in. */
static void check_sum(shmem_team_t team, int thread, int round, size_t count, const char *what)
{
    int size = shmem_team_n_pes(team);
    for (size_t i = 0; i < count; i++)
        src[thread][i] = value(me, round, thread, i);
    if (shmem_long_sum_reduce(team, dst[thread], src[thread], count) != 0)
        FAIL("thread %d round %d: %s sum of %zu returned nonzero", thread, round, what, count);
    for (size_t i = 0; i < count; i++) {
        long want = 0;
        for (int r = 0; r < size; r++)
            want += value(shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD), round, thread, i);
        if (dst[thread][i] != want)
            FAIL("thread %d round %d: %s sum of %zu longs, element %zu is %ld, want %ld", thread,
                 round, what, count, i, dst[thread][i], want);
    }
}

/* A broadcast of `bytes` bytes on `team` from the rank the round turns to,
 * every byte checked against what the root put in. */
static void check_broadcast(shmem_team_t team, int thread, int round, size_t bytes)
{
    int size = shmem_team_n_pes(team), root = round % size;
    int root_pe = shmem_team_translate_pe(team, root, SHMEM_TEAM_WORLD);
    for (size_t i = 0; i < bytes; i++) {
        byte_source[thread][i] = (unsigned char)(value(me, round, thread, i) & 0xff);
        byte_destination[thread][i] = 0xee;
    }
    if (shmem_broadcastmem(team, byte_destination[thread], byte_source[thread], bytes, root) != 0)
        FAIL("thread %d round %d: broadcast of %zu bytes returned nonzero", thread, round, bytes);
    for (size_t i = 0; i < bytes; i++) {
        unsigned char want = (unsigned char)(value(root_pe, round, thread, i) & 0xff);
        if (byte_destination[thread][i] != want)
            FAIL("thread %d round %d: broadcast of %zu bytes from rank %d, byte %zu is %u, "
                 "want %u", thread, round, bytes, root, i, byte_destination[thread][i], want);
    }
}

/* An inclusive or exclusive sum scan of `count` longs on `team`, every element
 * checked against the sum of what the members before this one (and this one,
 * if inclusive) put in. */
static void check_scan(shmem_team_t team, int thread, int round, size_t count, bool inclusive)
{
    int rank = shmem_team_my_pe(team);
    for (size_t i = 0; i < count; i++)
        src[thread][i] = value(me, round, thread, i);
    int status = inclusive ? shmem_long_sum_inscan(team, dst[thread], src[thread], count)
                           : shmem_long_sum_exscan(team, dst[thread], src[thread], count);
    if (status != 0)
        FAIL("thread %d round %d: %sclusive scan of %zu returned nonzero", thread, round,
             inclusive ? "in" : "ex", count);
    for (size_t i = 0; i < count; i++) {
        long want = 0;
        for (int r = 0; r < (inclusive ? rank + 1 : rank); r++)
            want += value(shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD), round, thread, i);
        if (dst[thread][i] != want)
            FAIL("thread %d round %d: %sclusive scan of %zu longs at rank %d, element %zu is "
                 "%ld, want %ld", thread, round, inclusive ? "in" : "ex", count, rank, i,
                 dst[thread][i], want);
    }
}

static void *worker(void *argument)
{
    int thread = (int)(intptr_t)argument;
    size_t size_kinds = sizeof sizes / sizeof sizes[0];
    for (int round = 0; round < rounds; round++) {
        /* The team's shape: all PEs; all but the first; the even ones; the
         * world reversed. Every PE of the parent calls the split. */
        int shape = (thread + round) % 4, start = 0, stride = 1, size = npes;
        if (shape == 1) { start = 1; size = npes - 1; }
        if (shape == 2) { stride = 2; size = (npes + 1) / 2; }
        if (shape == 3) { start = npes - 1; stride = -1; }
        shmem_team_t team = SHMEM_TEAM_INVALID;
        if (shmem_team_split_strided(parent[thread], start, stride, size, NULL, 0, &team) != 0)
            FAIL("thread %d round %d: split (%d, %d, %d) failed", thread, round, start, stride,
                 size);
        size_t first_count = sizes[(round + thread) % size_kinds];
        size_t second_count = sizes[(round + thread + 2) % size_kinds];
        if (team != SHMEM_TEAM_INVALID) {
            check_sum(team, thread, round, first_count, "the team's");
            check_scan(team, thread, round, second_count, true);
            check_broadcast(team, thread, round, second_count * 8 - 3);
            check_scan(team, thread, round, first_count, false);
        }
        /* The parent's large sum, between the team's collectives and its destroy. */
        check_sum(parent[thread], thread, round, 2048 + (size_t)(round % 3), "the parent's");
        if (team != SHMEM_TEAM_INVALID) {
            check_sum(team, thread, round, second_count, "the team's second");
            shmem_team_destroy(team);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        rounds = atoi(argv[1]);
    if (argc > 2)
        threads = atoi(argv[2]);
    CHECK_FATAL(rounds >= 1);
    CHECK_FATAL(threads >= 1 && threads <= MOST_THREADS);
    int provided = -1;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    for (int t = 0; t < threads; t++) {
        src[t] = shmem_malloc(MOST_ELEMENTS * sizeof(long));
        dst[t] = shmem_malloc(MOST_ELEMENTS * sizeof(long));
        byte_source[t] = shmem_malloc(MOST_ELEMENTS * sizeof(long));
        byte_destination[t] = shmem_malloc(MOST_ELEMENTS * sizeof(long));
        CHECK_FATAL(src[t] && dst[t] && byte_source[t] && byte_destination[t]);
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0,
                                             &parent[t]) == 0);
    }
    pthread_t workers[MOST_THREADS];
    for (int t = 0; t < threads; t++)
        CHECK_FATAL(pthread_create(&workers[t], NULL, worker, (void *)(intptr_t)t) == 0);
    for (int t = 0; t < threads; t++)
        CHECK(pthread_join(workers[t], NULL) == 0);
    shmem_barrier_all();
    for (int t = 0; t < threads; t++) {
        shmem_team_destroy(parent[t]);
        shmem_free(byte_destination[t]);
        shmem_free(byte_source[t]);
        shmem_free(dst[t]);
        shmem_free(src[t]);
    }
    shmem_barrier_all();
    if (me == 0 && !CHECK_DONE())
        fprintf(stderr, "PE 0: %d PEs, %d threads a PE, %d rounds passed\n", npes, threads,
                rounds);
    shmem_finalize();
    return CHECK_DONE();
}
