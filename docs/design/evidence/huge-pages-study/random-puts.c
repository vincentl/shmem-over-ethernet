/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * random-puts: the huge-pages study's measurement (docs/design/huge-pages-proposal.md).
 * Every PE puts 8-byte values to its partner's symmetric array - at random
 * places, the GUPS pattern, where every access is a new page in 4 KiB pages and
 * a TLB miss; and in sequence, where it is not - and times each, then reports
 * updates a second, its own VmPTE from /proc/self/status, and the node's
 * ShmemHugePages. The partner is the PE whose rank differs in the lowest bit:
 * on one node the puts are memcpy into a node-mate's heap; on two logical nodes
 * of one PE each they go over TCP. Arguments: the array's MiB (default 192)
 * and the updates (default 16 M).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static long status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, strlen(key)) == 0) { v = atol(line + strlen(key)); break; }
    if (f) fclose(f);
    return v;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, strlen(key)) == 0) { v = atol(line + strlen(key)); break; }
    if (f) fclose(f);
    return v;
}

static uint64_t next(uint64_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
    return *s;
}

int main(int argc, char **argv)
{
    size_t mib = argc > 1 ? (size_t)atol(argv[1]) : 192;
    uint64_t updates = argc > 2 ? (uint64_t)atoll(argv[2]) : UINT64_C(16) << 20;
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    int partner = me ^ 1;
    if (partner >= n) partner = me;
    size_t words = mib << 17;                       /* 8-byte words in mib MiB */
    long *array = shmem_malloc(words * sizeof *array);
    if (!array) { fprintf(stderr, "PE %d: no heap for %zu MiB\n", me, mib); shmem_global_exit(2); }
    shmem_barrier_all();

    uint64_t seed = 0x9E3779B97F4A7C15ull ^ (uint64_t)(me + 1);
    double t0 = now();
    for (uint64_t i = 0; i < updates; i++)
        shmem_long_p(&array[next(&seed) % words], (long)i, partner);
    shmem_quiet();
    double random_seconds = now() - t0;
    shmem_barrier_all();

    t0 = now();
    for (uint64_t i = 0; i < updates; i++)
        shmem_long_p(&array[i % words], (long)i, partner);
    shmem_quiet();
    double sequential_seconds = now() - t0;
    shmem_barrier_all();

    printf("random-puts pe=%d partner=%d array_mib=%zu updates=%" PRIu64
           " random_s=%.3f random_mups=%.2f sequential_s=%.3f sequential_mups=%.2f"
           " vmpte_kb=%ld shmem_huge_kb=%ld\n",
           me, partner, mib, updates, random_seconds, updates / random_seconds / 1e6,
           sequential_seconds, updates / sequential_seconds / 1e6, status_kb("VmPTE:"),
           meminfo_kb("ShmemHugePages:"));
    fflush(stdout);
    shmem_barrier_all();
    shmem_finalize();
    return 0;
}
