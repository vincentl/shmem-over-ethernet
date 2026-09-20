/*
 * hang.c - deliberately hang an osoe job in one of several ways, so that an
 * operator holding only osoe-stat can be asked what went wrong.
 *
 *   hang MODE [CULPRIT]
 *
 *   barrier   every PE but CULPRIT calls shmem_barrier_all; CULPRIT computes
 *             forever (a sleep loop) and never arrives.
 *   wait      CULPRIT blocks in shmem_long_wait_until on a variable nobody
 *             updates; the others go on to a barrier.
 *   traffic   every PE but CULPRIT loops put+quiet to CULPRIT (and get from
 *             it); CULPRIT prints its pid and sleeps, to be SIGSTOPped or
 *             SIGKILLed from outside. Each writer prints a line per 1000 rounds.
 *   peers     like traffic, but the targets are every PE *except* CULPRIT on
 *             other nodes and CULPRIT itself takes no part: used with CULPRIT =
 *             a node lead, to stall its agent thread while non-leads communicate.
 *   barriers  thirty barriers, timed, then every PE sleeps so the segment can be
 *             read: run against a library with a wake removed (lost-wake.patch).
 *   clean     init, barrier, finalize (checks the next job works).
 */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "clean";
    int culprit = argc > 2 ? atoi(argv[2]) : 0;

    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    long *cell = shmem_calloc(64, sizeof *cell);
    fprintf(stderr, "hang: PE %d of %d pid %d mode %s culprit %d\n", me, n, (int)getpid(),
            mode, culprit);
    shmem_barrier_all();

    if (!strcmp(mode, "barrier")) {
        if (me == culprit)
            for (;;)
                sleep(1);
        shmem_barrier_all();
    } else if (!strcmp(mode, "wait")) {
        if (me == culprit)
            shmem_long_wait_until(cell, SHMEM_CMP_NE, 0);
        shmem_barrier_all();
    } else if (!strcmp(mode, "barriers")) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 30; i++)
            shmem_barrier_all();
        clock_gettime(CLOCK_MONOTONIC, &b);
        fprintf(stderr, "hang: PE %d: 30 barriers in %.3f s\n", me,
                (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9);
        for (;;)
            sleep(1);
    } else if (!strcmp(mode, "traffic")) {
        if (me == culprit)
            for (;;)
                sleep(1);
        long v[64];
        for (long round = 0;; round++) {
            for (int i = 0; i < 64; i++)
                v[i] = round;
            shmem_putmem(cell, v, sizeof v, culprit);
            shmem_quiet();
            if (round % 1000 == 0)
                fprintf(stderr, "hang: PE %d round %ld\n", me, round);
            if (round % 7 == 0)
                shmem_getmem(v, cell, sizeof v, culprit);
        }
    } else if (!strcmp(mode, "peers")) {
        if (me == culprit)
            for (;;)
                sleep(1);
        long v[64];
        for (long round = 0;; round++) {
            for (int pe = 0; pe < n; pe++) {
                if (pe == me || pe == culprit)
                    continue;
                for (int i = 0; i < 64; i++)
                    v[i] = round;
                shmem_putmem(cell, v, sizeof v, pe);
                shmem_quiet();
                shmem_getmem(v, cell, sizeof v, pe);
            }
            if (round % 1000 == 0)
                fprintf(stderr, "hang: PE %d round %ld\n", me, round);
        }
    }

    shmem_free(cell);
    shmem_finalize();
    return 0;
}
