/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Review micro-benchmarks (phase1-step2-slice1, performance-and-scale).
 *
 * Public API only. Run under osoe-launch; the node map decides whether PE 1 is
 * a node-mate of PE 0 (map 0,0) or on another logical node (map 0,1). PE 0
 * measures, everyone else idles in the closing barrier.
 *
 * Everything this prints is "measured under contention, VM, shared-memory
 * wire - not a socket": order of magnitude only.
 *
 *   bench local      intranode put/p/get/atomics/quiet/fence vs raw floors
 *   bench remote     the same operations toward PE npes-1
 *   bench barrier    barrier_all latency, all PEs
 *   bench idle       sleep 3 s and report each process's CPU time
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

static uint64_t now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* samples are ns per operation */
static void report(const char *what, double *s, int n, double bytes)
{
    qsort(s, (size_t)n, sizeof *s, cmp);
    double med = s[n / 2];
    printf("%-44s min %10.1f  p10 %10.1f  med %10.1f  p90 %10.1f  max %12.1f ns/op",
           what, s[0], s[n / 10], med, s[n * 9 / 10], s[n - 1]);
    if (bytes > 0)
        printf("  (%8.1f MiB/s at med)", bytes / med * 1e9 / (1 << 20));
    printf("\n");
}

#define SAMPLES 41
static double samples[20000];

/* Time `batch` repetitions of BODY, SAMPLES times. */
#define BATCHED(what, batch, bytes, BODY)                                   \
    do {                                                                    \
        for (int s_ = 0; s_ < SAMPLES; s_++) {                              \
            uint64_t t0_ = now();                                           \
            for (int i = 0; i < (batch); i++) { BODY; }                     \
            samples[s_] = (double)(now() - t0_) / (batch);                  \
        }                                                                   \
        report(what, samples, SAMPLES, bytes);                              \
    } while (0)

/* Time every single BODY, n times. */
#define EACH(what, n, bytes, BODY)                                          \
    do {                                                                    \
        for (int s_ = 0; s_ < (n); s_++) {                                  \
            uint64_t t0_ = now();                                           \
            BODY;                                                           \
            samples[s_] = (double)(now() - t0_);                            \
        }                                                                   \
        report(what, samples, (n), bytes);                                  \
    } while (0)

static void *volatile sink;

static void bench_local(int target)
{
    size_t big = 1 << 20;
    char *dst = shmem_malloc(big);
    long *word = shmem_malloc(sizeof *word);
    char *src = malloc(big), *priv = malloc(big);
    memset(src, 1, big);
    memset(priv, 2, big);
    memset(dst, 0, big);
    *word = 0;
    shmem_barrier_all();
    if (shmem_my_pe() != 0)
        goto out;

    _Atomic long raw = 0;
    printf("# intranode, PE 0 -> PE %d\n", target);
    BATCHED("raw memcpy 8 B", 100000, 8, { memcpy(priv, src, 8); sink = priv; });
    BATCHED("shmem_putmem 8 B", 100000, 8, shmem_putmem(dst, src, 8, target));
    /* shmem_p / shmem_g are not in this build (typed RMA is deferred, design 7.5),
     * so an 8-byte putmem / getmem stands in for them. */
    BATCHED("shmem_getmem 8 B", 100000, 8, shmem_getmem(priv, dst, 8, target));
    BATCHED("raw memcpy 4 KiB", 20000, 4096, { memcpy(priv, src, 4096); sink = priv; });
    BATCHED("shmem_putmem 4 KiB", 20000, 4096, shmem_putmem(dst, src, 4096, target));
    BATCHED("shmem_getmem 4 KiB", 20000, 4096, shmem_getmem(priv, dst, 4096, target));
    BATCHED("raw memcpy 1 MiB", 200, 1 << 20, { memcpy(priv, src, big); sink = priv; });
    BATCHED("shmem_putmem 1 MiB", 200, 1 << 20, shmem_putmem(dst, src, big, target));
    BATCHED("raw atomic_fetch_add (seq_cst)", 100000, 0, atomic_fetch_add(&raw, 1));
    BATCHED("shmem_long_atomic_fetch_inc", 100000, 0,
            sink = (void *)shmem_long_atomic_fetch_inc(word, target));
    BATCHED("shmem_long_atomic_inc", 100000, 0, shmem_long_atomic_inc(word, target));
    BATCHED("shmem_fence (nothing outstanding)", 100000, 0, shmem_fence());
    BATCHED("shmem_quiet (nothing outstanding)", 100000, 0, shmem_quiet());
    BATCHED("shmem_putmem 8 B + shmem_quiet", 100000, 8,
            { shmem_putmem(dst, src, 8, target); shmem_quiet(); });
out:
    shmem_barrier_all();
    shmem_free(word);
    shmem_free(dst);
    free(src);
    free(priv);
}

static void bench_remote(int target, int n)
{
    size_t big = 1 << 20;
    char *dst = shmem_malloc(big);
    long *word = shmem_malloc(sizeof *word);
    char *src = malloc(big), *priv = malloc(big);
    memset(src, 1, big);
    memset(dst, 0, big);
    *word = 0;
    shmem_barrier_all();
    if (shmem_my_pe() != 0)
        goto out;

    printf("# internode (logical), PE 0 -> PE %d, shared-memory wire\n", target);
    EACH("put 8 B + quiet (round trip)", n, 0,
         { shmem_putmem(dst, src, 8, target); shmem_quiet(); });
    EACH("put 8 B alone (local completion)", n, 0, shmem_putmem(dst, src, 8, target));
    shmem_quiet();
    /* message rate: 1000 puts, one quiet */
    for (int s = 0; s < SAMPLES; s++) {
        uint64_t t0 = now();
        for (int i = 0; i < 1000; i++)
            shmem_putmem(dst + 8 * i, src, 8, target);
        shmem_quiet();
        samples[s] = (double)(now() - t0) / 1000;
    }
    report("1000 x put 8 B, then one quiet (per put)", samples, SAMPLES, 0);
    for (int s = 0; s < 11; s++) {
        uint64_t t0 = now();
        for (int i = 0; i < 200; i++) {
            shmem_putmem(dst + 8 * i, src, 8, target);
            shmem_quiet();
        }
        samples[s] = (double)(now() - t0) / 200;
    }
    report("200 x (put 8 B + quiet) (per put)", samples, 11, 0);
    EACH("get 8 B", n, 0, shmem_getmem(priv, dst, 8, target));
    EACH("get 4 KiB", n, 4096, shmem_getmem(priv, dst, 4096, target));
    EACH("put 4 KiB + quiet", n, 4096,
         { shmem_putmem(dst, src, 4096, target); shmem_quiet(); });
    EACH("atomic_fetch_inc", n, 0,
         sink = (void *)shmem_long_atomic_fetch_inc(word, target));
    EACH("atomic_inc (no reply) + quiet", n, 0,
         { shmem_long_atomic_inc(word, target); shmem_quiet(); });
    EACH("put 1 MiB + quiet", 30, 1 << 20,
         { shmem_putmem(dst, src, big, target); shmem_quiet(); });
    EACH("get 1 MiB", 30, 1 << 20, shmem_getmem(priv, dst, big, target));
out:
    shmem_barrier_all();
    shmem_free(word);
    shmem_free(dst);
    free(src);
    free(priv);
}

static void bench_barrier(int n)
{
    shmem_barrier_all();
    for (int s = 0; s < n; s++) {
        uint64_t t0 = now();
        shmem_barrier_all();
        samples[s] = (double)(now() - t0);
    }
    if (shmem_my_pe() == 0) {
        char what[64];
        snprintf(what, sizeof what, "barrier_all, %d PEs, seen by PE 0", shmem_n_pes());
        report(what, samples, n, 0);
    }
}

static void bench_idle(void)
{
    shmem_barrier_all();
    struct rusage a, b;
    getrusage(RUSAGE_SELF, &a);
    uint64_t t0 = now();
    sleep(3);
    uint64_t wall = now() - t0;
    getrusage(RUSAGE_SELF, &b);
    double cpu = (double)(b.ru_utime.tv_sec - a.ru_utime.tv_sec) +
                 (double)(b.ru_utime.tv_usec - a.ru_utime.tv_usec) / 1e6 +
                 (double)(b.ru_stime.tv_sec - a.ru_stime.tv_sec) +
                 (double)(b.ru_stime.tv_usec - a.ru_stime.tv_usec) / 1e6;
    printf("PE %d: idle %0.2f s wall, %0.4f s CPU (%0.2f%% of a core), %ld voluntary "
           "context switches\n", shmem_my_pe(), (double)wall / 1e9, cpu,
           cpu / ((double)wall / 1e9) * 100, b.ru_nvcsw - a.ru_nvcsw);
    shmem_barrier_all();
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "local";
    int n = argc > 2 ? atoi(argv[2]) : 2000;
    shmem_init();
    int target = shmem_n_pes() - 1;
    if (!strcmp(mode, "local"))
        bench_local(target);
    else if (!strcmp(mode, "remote"))
        bench_remote(target, n);
    else if (!strcmp(mode, "barrier"))
        bench_barrier(n);
    else if (!strcmp(mode, "idle"))
        bench_idle();
    shmem_finalize();
    return 0;
}
