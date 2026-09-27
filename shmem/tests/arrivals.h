/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Barrier timing checks that do not depend on scheduling.
 *
 * A barrier guarantees that no PE leaves before every PE has arrived. Each PE
 * records its arrival time (CLOCK_MONOTONIC, which all processes on a machine
 * share) in a small shared-memory table that is independent of the library,
 * then enters the collective; on return it checks that it left no earlier than
 * the latest recorded arrival. PE r delays r * STAGGER before arriving, so
 * arrivals are spread out and a collective that did not wait would fail.
 *
 * The table is /dev/shm/osoetest.<job>. Every PE opens it before the first
 * check and removes the name at the end; osoe-launch also removes it, quietly,
 * in case a PE failed first. It is not named osoe.<job>.*, so it never counts
 * as a library leak.
 *
 * **One row per round, and that is a fix rather than a convenience.** With a
 * single row, a PE that has finished its check goes straight into the next
 * arrival and overwrites its entry with a *later* timestamp - and the PE with
 * the smallest stagger overwrites immediately - while a slower PE is still
 * comparing against that row. The slow PE then sees an arrival later than its
 * own departure and reports a barrier that released early, which is a false
 * failure in the check whose whole job is to be trustworthy about that. It was
 * caught as a 2-in-8 failure of the multi-node barrier test under tsan, where
 * the rounds are slow enough to overlap; the step-1 tests of this helper have
 * the same flaw and were passing only because one node makes the window small.
 *
 * Each PE advances its own row in the check, so the rows stay in step without
 * needing any synchronization of their own: every PE runs the same sequence of
 * collectives, which is already required of them.
 */
#ifndef OSOE_TEST_ARRIVALS_H
#define OSOE_TEST_ARRIVALS_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "launcher.h"

enum { ARRIVALS_MAX = 256, ARRIVALS_ROUNDS = 64, ARRIVALS_STAGGER_MS = 20 };

static double *arrivals_table;      /* ARRIVALS_ROUNDS rows of ARRIVALS_MAX */
static int arrivals_round;          /* this PE's row; the check advances it */
static char arrivals_name[64];

#define ARRIVALS_BYTES (sizeof(double) * ARRIVALS_MAX * ARRIVALS_ROUNDS)


/* This round's row. A test that indexes the table directly means this row. */
static inline double *arrivals_row(void)
{
    return arrivals_table + (size_t)arrivals_round * ARRIVALS_MAX;
}

static inline double arrivals_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static inline void arrivals_open(void)
{
    snprintf(arrivals_name, sizeof arrivals_name, "/osoetest.%s", osoe_test_launcher("JOB_ID"));
    int fd = shm_open(arrivals_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)ARRIVALS_BYTES) != 0) {
        perror("arrivals_open");
        exit(2);
    }
    arrivals_table = mmap(NULL, ARRIVALS_BYTES, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    close(fd);
    if (arrivals_table == MAP_FAILED) {
        perror("arrivals_open: mmap");
        exit(2);
    }
}

static inline void arrivals_close(void)
{
    munmap(arrivals_table, ARRIVALS_BYTES);
    shm_unlink(arrivals_name);          /* every PE tries; ENOENT is fine */
}

/* Delay by rank, then record this PE's arrival. Call immediately before the
 * collective. */
static inline void arrivals_arrive(int me)
{
    if (arrivals_round >= ARRIVALS_ROUNDS) {
        fprintf(stderr, "arrivals: more than %d rounds; raise ARRIVALS_ROUNDS\n",
                ARRIVALS_ROUNDS);
        exit(2);
    }
    struct timespec ts = { 0, (long)me * ARRIVALS_STAGGER_MS * 1000 * 1000 };
    nanosleep(&ts, NULL);
    arrivals_row()[me] = arrivals_now();
}

/*
 * Call immediately after the collective: true if this PE left no earlier than
 * any PE arrived.
 *
 * An entry that is still zero means that PE had not arrived when this one left,
 * and that is a failure rather than a vacuous pass. Without the check, a
 * collective that released early compared against zero - earlier than
 * everything - and reported success: the table said nothing precisely in the
 * case it exists to catch. Found when a deliberately broken internode barrier
 * passed this.
 */
static inline int arrivals_left_after_all(int n)
{
    double left = arrivals_now();
    const double *row = arrivals_row();
    /* Before the comparison, so that this PE's next arrival cannot land in the
     * row another PE is still reading. */
    arrivals_round++;
    for (int i = 0; i < n; i++) {
        if (row[i] == 0.0)
            return 0;
        if (left < row[i])
            return 0;
    }
    return 1;
}

#endif /* OSOE_TEST_ARRIVALS_H */
