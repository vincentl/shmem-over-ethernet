/*
 * A job that dies, dies everywhere (§3.15; phase 1 step 4).
 *
 *   test-global-exit STATUS         PE 0 calls shmem_global_exit(STATUS)
 *   test-global-exit fatal          PE 0 ends the job with a fatal
 *
 * Every other PE is computing - spinning outside the library, never calling it
 * again - which is the case the abort word alone could not reach: only library
 * waits read it, and a PE that makes no call never waits. Each PE first prints
 * a line to stdout without flushing it; under the launcher stdout is a pipe, so
 * the line stays in a buffer that only exit's flush writes (spec §9.1.6:
 * shmem_global_exit "flushes I/O ... in accordance with C/C++ language
 * requirements for normal program termination").
 *
 * global-exit.sh runs the job and requires every rank to have left with the
 * status - the caller's, or a fatal's failure - and, for a global exit, every
 * line to have been written. A PE still computing after 30 s says so and
 * leaves with a status of its own, which fails the script.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

enum { COMPUTE_SECONDS = 30 };

int main(int argc, char **argv)
{
    const char *how = argc > 1 ? argv[1] : "7";
    shmem_init();
    int me = shmem_my_pe();
    printf("PE %d: computing\n", me);           /* buffered: flushed only by exit */
    shmem_barrier_all();

    if (me == 0) {
        struct timespec settle = { 0, 200 * 1000 * 1000 };
        nanosleep(&settle, NULL);               /* the others are computing by now */
        if (strcmp(how, "fatal") == 0) {
            int private_word = 0;               /* not symmetric: a fatal (§3.15) */
            shmem_putmem(&private_word, &private_word, sizeof private_word, 0);
        }
        shmem_global_exit(atoi(how));
    }

    time_t until = time(NULL) + COMPUTE_SECONDS;
    volatile unsigned long spins = 0;
    while (time(NULL) < until)
        spins++;
    fprintf(stderr, "PE %d: NOT TERMINATED after %d s of computing\n", me, COMPUTE_SECONDS);
    return 3;
}
