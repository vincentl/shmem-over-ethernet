/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Question 4's first experiment (v0.2, 2026-10-09), as the fast-link kit runs it
 * (docs/fast-link.md, §8): a blocking get against a nonblocking get and a quiet,
 * and a put and a quiet, at sizes from 16 KiB by fours to LARGEST, between two
 * PEs on two nodes. A nonblocking get's pieces go out together, as far as the
 * landing zone and the credits allow, and since 2026-10-10 so do a blocking
 * get's (docs/decisions/2026-10-10-blocking-gets.md): the two should match,
 * and both are held by the zone (OSOE_GET_LANDING_BYTES), which the kit sweeps.
 * Before then a blocking get asked for one piece at a time.
 * usage: get-pieces [REPS [LARGEST]]; prints the median of REPS (default 21)
 * for each size; LARGEST defaults to 4 MiB. */
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
    int repetitions = argc > 1 ? atoi(argv[1]) : 21;
    size_t most = argc > 2 ? (size_t)strtoull(argv[2], NULL, 0) : (size_t)4 << 20;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    char *remote = shmem_malloc(most);
    char *local = malloc(most);
    if (!remote || !local) {
        fprintf(stderr, "get-pieces: PE %d: no room for %zu bytes\n", me, most);
        shmem_global_exit(2);
    }
    memset(remote, me + 1, most);
    memset(local, 0, most);
    double *times = malloc(sizeof *times * (size_t)repetitions);
    size_t sizes[16], count = 0;
    for (size_t n = (size_t)16 << 10; n <= most && count < 16; n *= 4)
        sizes[count++] = n;
    shmem_barrier_all();
    if (me == 0 && npes >= 2) {
        int peer = npes - 1;
        printf("get-pieces pes=%d peer=%d repetitions=%d (median microseconds)\n", npes, peer,
               repetitions);
        for (size_t s = 0; s < count; s++) {
            size_t n = sizes[s];
            double put, get, nbi;
            for (int r = 0; r < repetitions; r++) {
                double t0 = now_us();
                shmem_putmem(remote, local, n, peer);
                shmem_quiet();
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)repetitions, sizeof *times, by_value);
            put = times[repetitions / 2];
            for (int r = 0; r < repetitions; r++) {
                double t0 = now_us();
                shmem_getmem(local, remote, n, peer);
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)repetitions, sizeof *times, by_value);
            get = times[repetitions / 2];
            for (int r = 0; r < repetitions; r++) {
                double t0 = now_us();
                shmem_getmem_nbi(local, remote, n, peer);
                shmem_quiet();
                times[r] = now_us() - t0;
            }
            qsort(times, (size_t)repetitions, sizeof *times, by_value);
            nbi = times[repetitions / 2];
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
