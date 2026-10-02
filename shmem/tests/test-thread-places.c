/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * What OSOE_MAX_THREADS counts (README, Limits; metrics.c): the live threads of
 * a PE that have called the library. A thread takes its place at its first
 * counted call - any after shmem_init but the queries, shmem_my_pe among them -
 * and gives it back when it exits, not when the call returns, so the limit is
 * not one on threads in the library at once. shmem.multiple.exhaust parks its
 * workers inside waits, which fits either reading; these two modes do not.
 *
 * Both run with OSOE_MAX_THREADS=2 and plain shmem_init, so at the level an
 * OpenMP team of serialized callers would run at, and the main thread has
 * called and holds one of the two places.
 *
 *   held:  a worker makes one call, and lives on outside the library; the next
 *          worker is started only once that call has returned, so no two
 *          threads are ever in the library together. Its first call finds
 *          both places held and ends the job, naming OSOE_MAX_THREADS.
 *   freed: far more workers than there are places, one after another, each
 *          making a call and exiting before the next starts: the one place
 *          left is enough, because an exit gives it back.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"

enum { FREED_WORKERS = 20 };

static sem_t called, may_exit;

static void *call_once_and_exit(void *argument)
{
    (void)argument;
    shmem_quiet();
    return NULL;
}

/* Alive, and out of the library, until the process ends. */
static void *call_once_and_live_on(void *argument)
{
    (void)argument;
    shmem_quiet();
    sem_post(&called);
    sem_wait(&may_exit);
    return NULL;
}

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2 && (strcmp(argv[1], "held") == 0 || strcmp(argv[1], "freed") == 0));
    shmem_init();
    int me = shmem_my_pe();
    shmem_quiet();              /* the main thread's first call: its place */

    if (strcmp(argv[1], "freed") == 0) {
        for (int i = 0; i < FREED_WORKERS; i++) {
            pthread_t worker;
            CHECK_FATAL(pthread_create(&worker, NULL, call_once_and_exit, NULL) == 0);
            CHECK_FATAL(pthread_join(worker, NULL) == 0);
        }
        shmem_finalize();
        fprintf(stderr, "PE %d: %d workers each made one call and exited; one place did\n",
                me, FREED_WORKERS);
        return 0;
    }

    CHECK_FATAL(sem_init(&called, 0, 0) == 0 && sem_init(&may_exit, 0, 0) == 0);
    pthread_t first, second;
    CHECK_FATAL(pthread_create(&first, NULL, call_once_and_live_on, NULL) == 0);
    CHECK_FATAL(sem_wait(&called) == 0);
    /* The first worker's call is over and it is alive: both places are held.
     * The line is what the test's expectation reads the fatal after, so that a
     * job ended earlier, for another reason, is not taken for this one. */
    fprintf(stderr, "PE %d: a worker made one call and lives on\n", me);
    CHECK_FATAL(pthread_create(&second, NULL, call_once_and_live_on, NULL) == 0);
    CHECK_FATAL(sem_wait(&called) == 0);
    fprintf(stderr, "PE %d: the main thread and two live workers ran on two places\n", me);
    return 1;
}
