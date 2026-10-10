/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* v0.2 step 4 part 0: a steady put stream from PE 0 to the first PE on
 * another node, for profiling. usage: putstream [BYTES [SECONDS]] */
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

int main(int argc, char **argv)
{
    size_t bytes = argc > 1 ? (size_t)strtoull(argv[1], NULL, 0) : (size_t)64 << 10;
    double seconds = argc > 2 ? atof(argv[2]) : 30;
    size_t batch = ((size_t)64 << 20) / bytes ? ((size_t)64 << 20) / bytes : 1;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    unsigned char *window = shmem_malloc(bytes);
    unsigned char *source = malloc(bytes);
    memset(source, 1, bytes);
    int target = -1;
    for (int p = 1; p < npes && target < 0; p++)
        if (shmem_ptr(window, p) == NULL)
            target = p;
    shmem_barrier_all();
    if (me == 0 && target >= 0) {
        double start = now(), mark = start;
        size_t total = 0, since = 0;
        while (now() - start < seconds) {
            for (size_t i = 0; i < batch; i++)
                shmem_putmem(window, source, bytes, target);
            shmem_quiet();
            total += batch * bytes;
            since += batch * bytes;
            double t = now();
            if (t - mark >= 2) {
                printf("t=%.1f mib_per_s=%.0f\n", t - start, (double)since / (1 << 20) / (t - mark));
                fflush(stdout);
                mark = t;
                since = 0;
            }
        }
        double t = now() - start;
        printf("putstream target=%d bytes=%zu seconds=%.1f mib_per_s=%.0f\n", target, bytes, t,
               (double)total / (1 << 20) / t);
        fflush(stdout);
    }
    shmem_barrier_all();
    shmem_free(window);
    free(source);
    shmem_finalize();
    return 0;
}
