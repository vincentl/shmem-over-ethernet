/* f1probe BLOCK... : time shmem_fcollectmem on the world team for each block
 * size, 200 calls after 20 warm-up, printing the median, the mean, and how
 * many calls took over 20 ms (a retransmission-timeout-sized stall). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <shmem.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
int main(int argc, char **argv) {
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    for (int a = 1; a < argc; a++) {
        size_t block = strtoul(argv[a], NULL, 0);
        char *src = shmem_malloc(block), *dst = shmem_malloc(block * n);
        memset(src, me, block);
        enum { WARM = 20, CALLS = 200 };
        double t[CALLS];
        for (int i = 0; i < WARM + CALLS; i++) {
            shmem_barrier_all();
            double s = now();
            shmem_fcollectmem(SHMEM_TEAM_WORLD, dst, src, block);
            if (i >= WARM) t[i - WARM] = now() - s;
        }
        double sum = 0; int slow = 0;
        for (int i = 0; i < CALLS; i++) { sum += t[i]; slow += t[i] > 0.020; }
        qsort(t, CALLS, sizeof *t, cmp);
        if (me == 0)
            printf("block %zu median_us %.1f mean_us %.1f slow %d max_ms %.1f\n", block,
                   t[CALLS / 2] * 1e6, sum / CALLS * 1e6, slow, t[CALLS - 1] * 1e3), fflush(stdout);
        shmem_free(dst); shmem_free(src);
    }
    shmem_finalize();
    return 0;
}
