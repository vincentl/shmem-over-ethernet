/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * P4 on the Pi cluster (docs/prompts/v0.2-open-items.md): on two nodes, a byte
 * sum over the world through whichever shape the run's settings give it - the
 * tree under OSOE_COLLECTIVE_SHAPE=tree, Träff's where the link set makes it
 * the cheaper - at the sizes either side of the multiples of 16 KiB where the
 * cost model's choice flips, 16 KiB k and 16 KiB k + 1, and two below. Each
 * PE's median over its calls after WARMUP, each call between two barriers, as
 * e-band's (docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c); PE 0
 * prints the slowest and the fastest PE's medians, the largest of the PEs'
 * tenth and ninetieth percentiles, and what the last call took, read from this
 * node's lead's record through osoe-stat's reader as test-circulant-reduce
 * reads it - the algorithm (0 the tree, 1 Träff's) and its window, with
 * "differs" where the PEs read different ones, and the most tree children a
 * lead has. The last call's first, middle and last bytes are checked against
 * the sum. Built with the tree's oshcc, from ~/osoe:
 *   oshcc -O2 -I shmem/tests -I tools/stat -I shmem/src -o two-node two-node.c \
 *         tools/stat/segment.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "invariants.h"

enum { WARMUP = 20, PIECE = 16384, MOST = 256 * PIECE + 1 };

static uint8_t *source, *dest;
static long *mine, *most;       /* symmetric: what every PE read, and the max */
static int me, npes, wrong;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* What this node's lead published for the world's last reduction. */
static long published(const char *metric)
{
    uint64_t value = 0;
    if (!osoe_test_scoped_metric(metric, OSOE_MSCOPE_TEAM, 0, &value))
        return -1;
    return (long)value;
}

static uint8_t expected(size_t i)
{
    return (uint8_t)((size_t)npes * i + (size_t)npes * (size_t)(npes - 1) / 2);
}

static void measure(size_t bytes, int iterations)
{
    uint64_t *times = malloc(sizeof *times * (size_t)iterations);
    for (int i = 0; i < WARMUP + iterations; i++) {
        shmem_barrier_all();
        uint64_t t0 = now_ns();
        shmem_uint8_sum_reduce(SHMEM_TEAM_WORLD, dest, source, bytes);
        uint64_t t1 = now_ns();
        if (i >= WARMUP)
            times[i - WARMUP] = t1 - t0;
    }
    size_t checked[] = { 0, bytes / 2, bytes - 1 };
    for (int c = 0; c < 3; c++)
        if (dest[checked[c]] != expected(checked[c])) {
            fprintf(stderr, "PE %d: %zu bytes: byte %zu is %u, not %u\n", me, bytes,
                    checked[c], dest[checked[c]], expected(checked[c]));
            wrong++;
        }
    qsort(times, (size_t)iterations, sizeof *times, compare);
    long algorithm = published("team.tree.algorithm");
    long window = published("team.tree.shape_window");
    long children = published("team.tree.shape_children");
    long median = (long)times[iterations / 2];
    long values[] = { median, -median, (long)times[iterations / 10],
                      (long)times[iterations * 9 / 10], algorithm, -algorithm,
                      window, -window, children };
    enum { VALUES = sizeof values / sizeof values[0] };
    memcpy(mine, values, sizeof values);
    free(times);
    shmem_barrier_all();
    shmem_long_max_reduce(SHMEM_TEAM_WORLD, most, mine, VALUES);
    if (me == 0) {
        printf("two-node pes=%d bytes=%zu slowest_median_us=%.1f fastest_median_us=%.1f "
               "p10_us=%.1f p90_us=%.1f algorithm=%ld%s window=%ld%s children=%ld\n", npes,
               bytes, most[0] / 1000.0, -most[1] / 1000.0, most[2] / 1000.0, most[3] / 1000.0,
               most[4], most[4] != -most[5] ? "(differs)" : "", most[6],
               most[6] != -most[7] ? "(differs)" : "", most[8]);
        fflush(stdout);
    }
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    source = shmem_malloc(MOST);
    dest = shmem_malloc(MOST);
    mine = shmem_malloc(16 * sizeof *mine);
    most = shmem_malloc(16 * sizeof *most);
    if (!source || !dest || !mine || !most)
        return 1;
    for (size_t i = 0; i < MOST; i++)
        source[i] = (uint8_t)(me + (int)i);
    static const size_t below[] = { 4096, 8192 };
    static const size_t k[] = { 1, 2, 3, 4, 6, 8, 16, 32, 64, 128, 256 };
    for (size_t s = 0; s < sizeof below / sizeof below[0]; s++)
        measure(below[s], 300);
    for (size_t s = 0; s < sizeof k / sizeof k[0]; s++)
        for (size_t extra = 0; extra <= 1; extra++) {
            size_t bytes = PIECE * k[s] + extra;
            measure(bytes, bytes <= 4 * PIECE + 1 ? 300 : bytes <= 64 * PIECE + 1 ? 100 : 40);
        }
    long bad = wrong;
    memcpy(mine, &bad, sizeof bad);
    shmem_barrier_all();
    shmem_long_max_reduce(SHMEM_TEAM_WORLD, most, mine, 1);
    bad = most[0];
    if (me == 0)
        printf("two-node done: %s\n", bad == 0 ? "every result right" : "WRONG RESULTS");
    shmem_free(most);
    shmem_free(mine);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return bad != 0;
}
