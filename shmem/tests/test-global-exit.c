/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A job that dies, dies everywhere (§3.15; phase 1 step 4).
 *
 *   test-global-exit STATUS [CALLER [MODE]]    PE CALLER (0) calls
 *                                              shmem_global_exit(STATUS)
 *   test-global-exit fatal [CALLER [MODE]]     PE CALLER ends the job with a fatal
 *
 * Every other PE is computing - spinning outside the library, never calling it
 * again - which is the case the abort word alone could not reach: only library
 * waits read it, and a PE that makes no call never waits. Each PE first prints
 * a line to stdout without flushing it; under the launcher stdout is a pipe, so
 * the line stays in a buffer that only exit's flush writes (spec §9.1.6:
 * shmem_global_exit "flushes I/O ... in accordance with C/C++ language
 * requirements for normal program termination").
 *
 * MODE says when and beside what the caller ends the job:
 *
 *   compute    after a barrier and 200 ms, with every other PE computing
 *   immediate  straight after shmem_init, before any barrier: a lead may then
 *              still be connecting to the other nodes, and must not be left
 *              unable to tell them (init waits for the lead's agent). Other
 *              PEs may be ended inside their own init, before their line, so
 *              only the statuses are checked
 *   stream     after 300 ms of putting 1 MiB at a time, with no quiet, to a PE
 *              on another node, while every other PE does the same: the
 *              abortive goodbye is then queued behind a stream, with a stream
 *              still arriving, so the caller's end finds input unread - a
 *              reset, which drops what is queued unless the goodbye has been
 *              acknowledged first. With a quiet each time the senders stall
 *              and the receiver drains, and the end is a clean FIN
 *   start-pes  every PE starts with start_pes, and every other PE returns from
 *              main after a barrier, so its main thread is inside exit, in
 *              start_pes's finalize - a barrier the caller never reaches -
 *              when the caller ends the job 300 ms later
 *   returned   every other PE returns from main after a barrier, without
 *              finalizing, and its main thread is inside exit, in a handler of
 *              the program's that takes 1.5 s, when the caller ends the job
 *              200 ms later: its agent's exit is then waiting on the main
 *              thread before the library's own handler runs
 *
 *   handlers   compute, with an atexit handler on every PE that prints a line:
 *              only the caller's PE ends with exit, which runs it; every other
 *              ends with fflush and _exit, running none beside a main thread
 *              still computing
 *   slow-handler
 *              compute, with an atexit handler on the caller's PE alone that
 *              takes a second and then prints a line: under a launcher that
 *              ends the step at the first bad exit, the caller must still
 *              finish it - every other PE leaves with 0, so the caller's own
 *              exit is the first bad one
 *
 * In start-pes and returned, exit's lock (glibc 2.41) is held by the main
 * thread, which a non-caller's _exit does not wait on.
 *
 * global-exit.sh runs the job and requires, for a global exit, the caller to
 * have left with the status and every other PE with 0, and every line to have
 * been written; for a fatal, every PE to have left with the failure status. A PE still computing after 30 s says so and
 * leaves with a status of its own, which fails the script. The caller says
 * when it ends the job, for the script to time the end.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

enum { COMPUTE_SECONDS = 30, STREAM_BYTES = 1 << 20 };

static void end_the_job(const char *how)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    fprintf(stderr, "ending the job at %lld.%09ld\n", (long long)now.tv_sec, now.tv_nsec);
    if (strcmp(how, "fatal") == 0) {
        int private_word = 0;               /* not symmetric: a fatal (§3.15) */
        shmem_putmem(&private_word, &private_word, sizeof private_word, 0);
    }
    shmem_global_exit(atoi(how));
}

static int my_pe = -1;

static void handler_ran(void)
{
    printf("PE %d: handler ran\n", my_pe);
}

static void slow_handler(void)
{
    struct timespec slow = { 1, 500 * 1000 * 1000 };
    nanosleep(&slow, NULL);
}

static void slow_handler_ran(void)
{
    struct timespec second = { 1, 0 };
    nanosleep(&second, NULL);
    printf("PE %d: handler ran after a second\n", my_pe);
}

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *how = argc > 1 ? argv[1] : "7";
    int caller = argc > 2 ? atoi(argv[2]) : 0;
    const char *mode = argc > 3 ? argv[3] : "compute";
    if (strcmp(mode, "start-pes") == 0)
        start_pes(0);
    else
        shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    my_pe = me;
    if (strcmp(mode, "handlers") == 0)
        atexit(handler_ran);
    if (strcmp(mode, "slow-handler") == 0 && me == caller)
        atexit(slow_handler_ran);
    printf("PE %d: computing\n", me);           /* buffered: flushed only by exit */
    if (me == caller && strcmp(mode, "immediate") == 0)
        end_the_job(how);

    if (strcmp(mode, "start-pes") == 0 || strcmp(mode, "returned") == 0) {
        if (me != caller && strcmp(mode, "returned") == 0)
            atexit(slow_handler);
        shmem_barrier_all();
        if (me != caller)
            return 0;                           /* into exit, holding its lock */
        struct timespec settle = { 0, (strcmp(mode, "start-pes") == 0 ? 300 : 200) * 1000 * 1000 };
        nanosleep(&settle, NULL);
        end_the_job(how);
    }

    /* The stream: 1 MiB at a time to the PE half the job away, which is on
     * another node in every map this runs on. */
    unsigned char *window = shmem_malloc(STREAM_BYTES);
    unsigned char *data = malloc(STREAM_BYTES);
    if (!window || !data) {
        fprintf(stderr, "PE %d: out of memory for the stream\n", me);
        return 4;
    }
    memset(data, me, STREAM_BYTES);
    shmem_barrier_all();

    double start = seconds();
    if (strcmp(mode, "stream") == 0) {
        int target = (me + npes / 2) % npes;
        while (seconds() < start + COMPUTE_SECONDS) {
            shmem_putmem(window, data, STREAM_BYTES, target);
            if (me == caller && seconds() > start + 0.3)
                end_the_job(how);
        }
    } else {
        if (me == caller) {
            struct timespec settle = { 0, 200 * 1000 * 1000 };
            nanosleep(&settle, NULL);           /* the others are computing by now */
            end_the_job(how);
        }
        volatile unsigned long spins = 0;
        while (seconds() < start + COMPUTE_SECONDS)
            spins++;
    }
    fprintf(stderr, "PE %d: NOT TERMINATED after %d s of computing\n", me, COMPUTE_SECONDS);
    return 3;
}
