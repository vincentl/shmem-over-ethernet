/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Limits experiment 2: is a get reply starved by local put traffic?
 *
 * Design 3.8's starvation guard (a freed buffer enters DRAINING, where only the
 * progress thread may reserve, capped at half a buffer) is implemented in
 * connection.c (osoe_connection_drain_reserve) but progress.c never calls it:
 * drain() competes with application writers through the ordinary reserve.
 *
 *   osoe-launch -n 4 -m 0,1,1,1 -- limit-reply-starvation <pressure 0|1> <put bytes>
 *
 * PE 0 (node 0) times 8-byte gets from PE 1 (node 1). With pressure, PEs 2 and 3
 * (node 1) stream puts toward PE 0 for the whole run, competing for the node 1 ->
 * node 0 connection buffer that PE 0's replies need.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <shmem.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
int main(int argc, char **argv)
{
    int pressure = argc > 1 ? atoi(argv[1]) : 1;
    size_t bytes = argc > 2 ? (size_t)atol(argv[2]) : 32768;
    shmem_init();
    int me = shmem_my_pe();
    char *buf = shmem_malloc(1 << 20);
    char *priv = malloc(1 << 20);
    memset(priv, 1, 1 << 20);
    memset(buf, 0, 1 << 20);
    static double s[100000];
    int n = 0;
    shmem_barrier_all();
    double t0 = now();
    if (me == 0) {
        while (now() - t0 < 4.0 && n < 100000) {
            double a = now();
            shmem_getmem(priv, buf, 8, 1);
            s[n++] = (now() - a) * 1e6;
        }
        qsort(s, (size_t)n, sizeof *s, cmp);
        printf("pressure %d (%zu-byte puts): %d gets in 4 s; us: min %.0f p10 %.0f med %.0f "
               "p90 %.0f p99 %.0f max %.0f\n", pressure, bytes, n, s[0], s[n / 10], s[n / 2],
               s[n * 9 / 10], s[n * 99 / 100], s[n - 1]);
    } else if (me >= 2 && pressure) {
        long puts = 0;
        while (now() - t0 < 5.0) {
            shmem_putmem(buf, priv, bytes, 0);
            puts++;
        }
        shmem_quiet();
        printf("PE %d: %ld puts of %zu bytes (%.1f MiB/s)\n", me, puts, bytes,
               (double)puts * (double)bytes / 5.0 / (1 << 20));
    }
    shmem_barrier_all();
    shmem_free(buf);
    shmem_finalize();
    return 0;
}
