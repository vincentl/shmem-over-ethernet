/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Question 4's first experiment (v0.2, 2026-10-09): a blocking get against a
 * nonblocking get and a quiet, and a put and a quiet, at sizes from 16 KiB to
 * 4 MiB, between two PEs on two nodes. A blocking get asks for one piece of a
 * chunk's payload at a time; a nonblocking get's pieces go out together, as far
 * as the landing zone and the credits allow - so the nonblocking get's time is
 * about what a blocking get that issued its pieces so would take.
 * usage: get-pieces [REPS]; prints the median of REPS (default 21) for each. */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    int reps = argc > 1 ? atoi(argv[1]) : 21;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    enum { MOST = 4 << 20 };
    char *remote = shmem_malloc(MOST);
    char *local = malloc(MOST);
    memset(remote, me + 1, MOST);
    memset(local, 0, MOST);
    double *times = malloc(sizeof *times * (size_t)reps);
    static const size_t sizes[] = { 16 << 10, 64 << 10, 128 << 10, 256 << 10, 1 << 20, 4 << 20 };
    shmem_barrier_all();
    if (me == 0 && npes >= 2) {
        int peer = npes - 1;
        printf("get-pieces pes=%d peer=%d reps=%d (median microseconds)\n", npes, peer, reps);
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            size_t n = sizes[s];
            double put, get, nbi;
            for (int r = 0; r < reps; r++) {
                double t0 = now_us();
                shmem_putmem(remote, local, n, peer);
                shmem_quiet();
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)reps, sizeof *times, by_value);
            put = times[reps / 2];
            for (int r = 0; r < reps; r++) {
                double t0 = now_us();
                shmem_getmem(local, remote, n, peer);
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)reps, sizeof *times, by_value);
            get = times[reps / 2];
            for (int r = 0; r < reps; r++) {
                double t0 = now_us();
                shmem_getmem_nbi(local, remote, n, peer);
                shmem_quiet();
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)reps, sizeof *times, by_value);
            nbi = times[reps / 2];
            printf("bytes=%zu put_quiet_us=%.1f get_us=%.1f get_nbi_quiet_us=%.1f "
                   "get_over_put=%.2f nbi_over_put=%.2f\n",
                   n, put, get, nbi, get / put, nbi / put);
            fflush(stdout);
        }
    }
    shmem_barrier_all();
    shmem_free(remote);
    free(local);
    free(times);
    shmem_finalize();
    return 0;
}
