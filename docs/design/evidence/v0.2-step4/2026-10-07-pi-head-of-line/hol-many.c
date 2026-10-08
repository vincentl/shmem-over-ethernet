/* v0.2 step 4 part 0: several writers of small messages beside one stream, on
 * one connection. PEs in blocks of k a node over two nodes: PE 0 streams 1 MiB
 * puts to PE k; PEs 1 to k-1 each time fetch-adds to their counterpart on node
 * 1, whose requests share node 0's connection with the stream. Each reports.
 * usage: hol-many [SECONDS] */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

enum { BYTES = 1 << 20, MOST = 200000 };

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 10;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes(), k = npes / 2;
    static long cell, done;
    unsigned char *window = shmem_malloc(BYTES);
    unsigned char *source = malloc(BYTES);
    double *us = malloc(MOST * sizeof *us);
    memset(source, 7, BYTES);
    shmem_barrier_all();
    if (me == 0) {
        double start = now();
        size_t total = 0;
        while (now() - start < seconds) {
            for (int i = 0; i < 64; i++)
                shmem_putmem(window, source, BYTES, k);
            shmem_quiet();
            total += (size_t)64 * BYTES;
        }
        printf("hol-many stream_mib_per_s=%.0f\n", (double)total / (1 << 20) / (now() - start));
        fflush(stdout);
        for (int p = 1; p < k; p++)
            shmem_long_atomic_set(&done, 1, p);
    } else if (me < k) {
        size_t n = 0;
        double warm = now() + 0.5;
        while (now() < warm)
            (void)shmem_long_atomic_fetch_add(&cell, 1, me + k);
        while (shmem_long_atomic_fetch(&done, me) == 0 && n < MOST) {
            double t = now();
            (void)shmem_long_atomic_fetch_add(&cell, 1, me + k);
            us[n++] = (now() - t) * 1e6;
        }
        qsort(us, n, sizeof *us, compare);
        printf("hol-many pe=%d fetch_adds=%zu median_us=%.0f p90_us=%.0f p99_us=%.0f max_us=%.0f\n",
               me, n, us[n / 2], us[n * 9 / 10], us[n * 99 / 100], us[n - 1]);
        fflush(stdout);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(source);
    free(us);
    shmem_finalize();
    return 0;
}
