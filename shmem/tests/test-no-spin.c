/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * No PE's wait spins (T14; v0.2 step 1's scale review, Q3-1). PE 0 sleeps
 * three seconds outside the library while every other PE waits for it in a
 * barrier, and each of those reads the CPU its process - its waiting thread and
 * its agent - used meanwhile. A wait that sleeps between its sweeps uses about
 * a third of a millisecond a second; one that spins before it sleeps - the
 * review's mutant spun up to 200 us on the word before each futex call - used
 * six, and made a 32-PE barrier on four cores 24 to 35 times slower, while
 * every other test of both builds passed: every pin is a latency or a rate,
 * and a spin buys latency with a core. So the job fails when its waiters'
 * average passes 2 ms a second: CPU, not time, so any machine and any load.
 *
 * Run on one node, in both builds - the futex file is where they differ - at
 * more PEs than the development VM's four cores.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>

#include <shmem.h>

enum { ASLEEP_SECONDS = 3 };
#define MOST_MS_A_SECOND 2.0

static double wall_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* The CPU this process has used, user and system, its every thread's. */
static double process_seconds(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + (double)u.ru_utime.tv_usec / 1e6 +
           (double)u.ru_stime.tv_sec + (double)u.ru_stime.tv_usec / 1e6;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    double *rate = shmem_malloc(2 * sizeof *rate);
    if (!rate)
        return 1;
    shmem_barrier_all();

    double wall = wall_seconds(), cpu = process_seconds();
    if (me == 0) {
        struct timespec asleep = { ASLEEP_SECONDS, 0 };
        nanosleep(&asleep, NULL);
    }
    shmem_barrier_all();
    wall = wall_seconds() - wall;
    cpu = process_seconds() - cpu;

    /* Milliseconds of CPU a second of the wait; PE 0, which slept, adds none. */
    rate[0] = me == 0 ? 0 : 1e3 * cpu / wall;
    if (me != 0 && rate[0] >= MOST_MS_A_SECOND)
        fprintf(stderr, "PE %d used %.1f ms of CPU in a wait of %.2f s: %.2f ms a second\n", me,
                1e3 * cpu, wall, rate[0]);
    shmem_double_sum_reduce(SHMEM_TEAM_WORLD, &rate[1], &rate[0], 1);
    double average = npes > 1 ? rate[1] / (npes - 1) : 0;
    int status = 0;
    if (me == 0) {
        if (wall < ASLEEP_SECONDS - 0.5) {
            printf("CHECK(the wait lasted) failed: %.2f s\n", wall);
            status = 1;
        } else if (average >= MOST_MS_A_SECOND) {
            printf("SPINNING: the %d waiters used %.2f ms of CPU a second on average, at most "
                   "%.1f allowed\n", npes - 1, average, MOST_MS_A_SECOND);
            status = 1;
        } else {
            printf("NO SPIN OK: the %d waiters used %.2f ms of CPU a second on average\n",
                   npes - 1, average);
        }
    }
    shmem_free(rate);
    shmem_finalize();
    return status;
}
