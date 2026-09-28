/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The deadlock detector (§3.15, deadlock.h): a job whose every PE waits in the
 * library, with nothing in flight, ends naming where each waits; a job that
 * only looks like one - a PE computing, sleeping, or with a thread the library
 * cannot see - runs to its end.
 *
 * Run with OSOE_DEADLOCK_SECONDS=1, so a deadlock ends in about three seconds:
 * a second of the root's own node looking quiet, a second between the two
 * collections, and the samples.
 *
 *   waits       every PE waits in shmem_int_wait_until for a flag nobody sets
 *   reinit      the same, in a second session after a finalize and an init
 *   mismatch    the even PEs in shmem_barrier_all, the odd ones in a wait: a
 *               collective that does not match, whose arrivals cross nodes
 *   computing   the last PE computes for four seconds while the rest wait in
 *               a barrier, then joins them
 *   sleeping    the last PE sleeps outside the library for four seconds
 *   helper      the last PE waits for a flag that a thread of its own, outside
 *               the library, sets after four seconds; the rest wait in a
 *               barrier
 *   busy        every PE waits for a flag that a thread of its own sets after
 *               computing for four seconds: every node looks quiet to a count
 *               of waits alone, and a round is begun and found busy each time
 *
 * The last PE, so that across nodes the root's node is the one that waits:
 * it begins rounds, and the last node's reports end each.
 *
 * The deadlocked modes print nothing of their own past the point where they
 * wait; the others print "DEADLOCK TEST OK" from PE 0 at the end.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

enum { LOOKS_LIKE_ONE_SECONDS = 4 };

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void *compute_then_set(void *flag)
{
    volatile double sink = 0;
    for (double end = now() + LOOKS_LIKE_ONE_SECONDS; now() < end;)
        for (int i = 0; i < 100000; i++)
            sink += i;
    (void)sink;
    atomic_store_explicit((_Atomic int *)flag, 1, memory_order_release);
    return NULL;
}

static void *set_later(void *flag)
{
    struct timespec t = { LOOKS_LIKE_ONE_SECONDS, 0 };
    nanosleep(&t, NULL);
    /* A store the library does not see: the waiter finds it at its next sweep. */
    atomic_store_explicit((_Atomic int *)flag, 1, memory_order_release);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "waits";
    if (strcmp(mode, "reinit") == 0) {
        shmem_init();
        shmem_barrier_all();
        shmem_finalize();
        mode = "waits";
    }
    shmem_init();
    int me = shmem_my_pe(), last = shmem_n_pes() - 1;
    int *flag = shmem_malloc(sizeof *flag);
    *flag = 0;
    shmem_barrier_all();

    if (strcmp(mode, "waits") == 0) {
        shmem_int_wait_until(flag, SHMEM_CMP_NE, 0);
        printf("DEADLOCK TEST REACHED: PE %d left a wait nobody satisfies\n", me);
    } else if (strcmp(mode, "mismatch") == 0) {
        if (me % 2 == 0)
            shmem_barrier_all();
        else
            shmem_int_wait_until(flag, SHMEM_CMP_NE, 0);
        printf("DEADLOCK TEST REACHED: PE %d left a mismatched collective\n", me);
    } else if (strcmp(mode, "computing") == 0) {
        if (me == last) {
            volatile double sink = 0;
            for (double end = now() + LOOKS_LIKE_ONE_SECONDS; now() < end;)
                for (int i = 0; i < 100000; i++)
                    sink += i;
            (void)sink;
        }
        shmem_barrier_all();
    } else if (strcmp(mode, "sleeping") == 0) {
        if (me == last) {
            struct timespec t = { LOOKS_LIKE_ONE_SECONDS, 0 };
            nanosleep(&t, NULL);
        }
        shmem_barrier_all();
    } else if (strcmp(mode, "busy") == 0) {
        pthread_t helper;
        if (pthread_create(&helper, NULL, compute_then_set, flag) != 0) {
            fprintf(stderr, "CHECK(pthread_create) failed\n");
            return 1;
        }
        shmem_int_wait_until(flag, SHMEM_CMP_NE, 0);
        pthread_join(helper, NULL);
        shmem_barrier_all();
    } else if (strcmp(mode, "helper") == 0) {
        if (me == last) {
            pthread_t helper;
            if (pthread_create(&helper, NULL, set_later, flag) != 0) {
                fprintf(stderr, "CHECK(pthread_create) failed\n");
                return 1;
            }
            shmem_int_wait_until(flag, SHMEM_CMP_NE, 0);
            pthread_join(helper, NULL);
        }
        shmem_barrier_all();
    } else {
        fprintf(stderr, "CHECK(mode) failed: %s\n", mode);
        return 1;
    }

    if (me == 0)
        printf("DEADLOCK TEST OK: %s\n", mode);
    shmem_free(flag);
    shmem_finalize();
    return 0;
}
