/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The one-node agent's wake (tier 2; the v0.2 step 1 review, correctness K3).
 *
 * In a job of one node each PE's progress thread sleeps on its wake word
 * (transport-am.c, osoe_transport_agent_wait) until a stop or the node's abort
 * wakes it, its 100 ms sweep only the backstop. A wake lost there - the word
 * waited in the private scope and woken in the shared - passed the whole
 * suite: every finalize waited out the sweep, a short job 319 ms where it
 * takes 22. So: five sessions, each init, barrier and finalize, and the
 * fastest finalize well under the sweep. The fastest of five, so that a PE
 * descheduled on a loaded machine does not fail a wake that came.
 */
#define _POSIX_C_SOURCE 200809L       /* clock_gettime, under -std=c11 too */
#include <stdio.h>
#include <time.h>

#include <shmem.h>

#include "check.h"

enum { SESSIONS = 5 };

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(void)
{
    double fastest = 1e9;
    int me = -1;
    for (int i = 0; i < SESSIONS; i++) {
        shmem_init();
        me = shmem_my_pe();
        shmem_barrier_all();
        double start = seconds();
        shmem_finalize();
        double took = seconds() - start;
        if (took < fastest)
            fastest = took;
    }
    printf("PE %d: the fastest of %d finalizes took %.1f ms\n", me, SESSIONS, fastest * 1e3);
    /* The sweep is 100 ms; a lost wake makes every finalize wait most of it. */
    CHECK(fastest < 0.050);
    return CHECK_DONE();
}
