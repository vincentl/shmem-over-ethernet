/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The large collectives whose shape the chooser picks (docs/fast-link.md §14;
 * H13 of docs/prompts/v0.2-open-items.md): a sum reduction of longs and a
 * broadcast from PE 0 over the world team, at each size given - by default
 * 64 KiB, 256 KiB, 1 MiB and 4 MiB, from four of the leads' 16 KiB pieces to
 * 256, where a tree's window and fan-out and Träff's rounds part - 20 of
 * warm-up and TRIPS timed (100 by default). PE 0 prints each one's median and
 * fastest in osoe-bench's form, "reduce pes=N bytes=B median_us=M
 * fastest_us=F", which summarize.py reads as osoe-bench's; the shape is
 * whatever OSOE_COLLECTIVE_SHAPE, OSOE_COLLECTIVE_FANOUT and
 * OSOE_COLLECTIVE_PIPELINE make it.
 *
 * usage: shapes [TRIPS [BYTES...]]
 */
#define _GNU_SOURCE
#include <shmem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WARMUP = 20 };

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    static const size_t standard[] = { 65536, 262144, 1048576, 4194304 };
    int trips = argc > 1 ? atoi(argv[1]) : 100;
    size_t count = argc > 2 ? (size_t)(argc - 2) : sizeof standard / sizeof *standard;
    size_t *sizes = malloc(count * sizeof *sizes), largest = 0;
    for (size_t s = 0; s < count; s++) {
        sizes[s] = argc > 2 ? strtoull(argv[2 + s], NULL, 10) : standard[s];
        sizes[s] -= sizes[s] % sizeof(long);
        if (sizes[s] > largest)
            largest = sizes[s];
    }
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    if (trips < 1 || largest == 0) {
        if (me == 0)
            fprintf(stderr, "shapes: TRIPS at least 1, and sizes of a long or more\n");
        shmem_global_exit(2);
    }
    long *src = shmem_malloc(largest), *dst = shmem_malloc(largest);
    uint64_t *times = malloc(sizeof *times * (size_t)trips);
    if (!src || !dst || !times) {
        fprintf(stderr, "shapes: PE %d cannot allocate %zu bytes twice\n", me, largest);
        shmem_global_exit(1);
    }
    memset(src, me + 1, largest);
    for (size_t s = 0; s < count; s++) {
        size_t n = sizes[s] / sizeof *src;
        for (int kind = 0; kind < 2; kind++) {
            shmem_barrier_all();
            for (int i = -WARMUP; i < trips; i++) {
                uint64_t t = now_ns();
                if (kind == 0)
                    shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, n);
                else
                    shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, n, 0);
                if (i >= 0)
                    times[i] = now_ns() - t;
            }
            if (me == 0) {
                qsort(times, (size_t)trips, sizeof *times, ascending);
                printf("%s pes=%d bytes=%zu median_us=%.1f fastest_us=%.1f\n",
                       kind == 0 ? "reduce" : "broadcast", npes, sizes[s],
                       (double)times[trips / 2] / 1e3, (double)times[0] / 1e3);
                fflush(stdout);
            }
        }
    }
    shmem_barrier_all();
    free(times);
    free(sizes);
    shmem_free(dst);
    shmem_free(src);
    shmem_finalize();
    return 0;
}
