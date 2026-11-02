/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * combine-timing: a node's reduction combine, serial against sliced
 * (docs/design/intranode-combine-proposal.md; the large-cluster playbook's C5).
 *
 * L processes share L source vectors of n doubles and one dest vector. In the
 * serial round process 0 sums every source into dest, as osoe's lead does
 * (collectives.c): O(L n) on one thread. In the sliced round process i sums
 * slice i of every source into slice i of dest: O(n) a process, L at once. A
 * flat counter in shared memory is the barrier between phases; the time is
 * from the barrier before a round to the last process's arrival at the
 * barrier after it, as process 0 measures it, the median of R rounds.
 *
 * Standalone, no osoe: it measures the memory system, not the library, and
 * sizes the gain the proposal's arithmetic claims.
 *
 *   combine-timing L n_bytes [rounds]
 *   prints: L bytes serial_us sliced_us ratio
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shared {
    _Alignas(128) _Atomic uint32_t barrier;
    _Alignas(128) _Atomic uint32_t generation;
    _Alignas(128) uint64_t start_ns;
    _Alignas(128) double sink;
};

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* A flat barrier: the counter counts arrivals across the run and everyone
 * waits for the next multiple of L, yielding the CPU on every look, so that
 * with more processes than cores a waiter does not hold the core the process
 * it waits for needs (the library's waits sleep on a futex; a spin here
 * measured the scheduler's slice, 8 ms at 8 processes on 4 cores). */
static void barrier_wait(struct shared *s, unsigned L, uint32_t *epoch)
{
    *epoch += 1;
    atomic_fetch_add_explicit(&s->barrier, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&s->barrier, memory_order_acquire) < *epoch * L)
        sched_yield();
}

static void combine(double *restrict acc, const double *restrict in, size_t count)
{
    for (size_t i = 0; i < count; i++)
        acc[i] += in[i];
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s L n_bytes [rounds]\n", argv[0]);
        return 2;
    }
    unsigned L = (unsigned)atoi(argv[1]);
    size_t bytes = (size_t)atoll(argv[2]);
    unsigned rounds = argc > 3 ? (unsigned)atoi(argv[3]) : 21;
    size_t count = bytes / sizeof(double);
    if (L < 1 || count < L) {
        fprintf(stderr, "L must be 1 or more and n_bytes at least 8 L\n");
        return 2;
    }
    struct shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                            -1, 0);
    double *sources = mmap(NULL, L * bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                           -1, 0);
    double *dest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint64_t *serial_ns = mmap(NULL, rounds * sizeof *serial_ns, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint64_t *sliced_ns = mmap(NULL, rounds * sizeof *sliced_ns, PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED || sources == MAP_FAILED || dest == MAP_FAILED ||
        serial_ns == MAP_FAILED || sliced_ns == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(s, 0, sizeof *s);

    unsigned me = 0;
    for (unsigned p = 1; p < L; p++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            me = p;
            break;
        }
    }

    /* Each process writes its own source and touches dest, so that the pages
     * exist and are placed before anything is timed. */
    double *mine = sources + (size_t)me * count;
    for (size_t i = 0; i < count; i++)
        mine[i] = (double)(me + 1) + (double)i * 1e-9;
    size_t slice = count / L, from = (size_t)me * slice, to = me + 1 == L ? count : from + slice;
    for (size_t i = from; i < to; i++)
        dest[i] = 0.0;
    uint32_t epoch = 0;
    barrier_wait(s, L, &epoch);

    for (unsigned r = 0; r < rounds; r++) {
        /* Serial: process 0 combines every source into dest. */
        barrier_wait(s, L, &epoch);
        if (me == 0) {
            uint64_t t0 = now_ns();
            memcpy(dest, sources, bytes);
            for (unsigned m = 1; m < L; m++)
                combine(dest, sources + (size_t)m * count, count);
            serial_ns[r] = now_ns() - t0;
        }
        barrier_wait(s, L, &epoch);

        /* Sliced: process i combines slice i of every source into dest's. */
        barrier_wait(s, L, &epoch);
        uint64_t t0 = now_ns();
        memcpy(dest + from, sources + from, (to - from) * sizeof(double));
        for (unsigned m = 1; m < L; m++)
            combine(dest + from, sources + (size_t)m * count + from, to - from);
        barrier_wait(s, L, &epoch);
        if (me == 0)
            sliced_ns[r] = now_ns() - t0;
    }
    s->sink = dest[count - 1];

    if (me != 0)
        _exit(0);
    for (unsigned p = 1; p < L; p++)
        wait(NULL);
    qsort(serial_ns, rounds, sizeof *serial_ns, compare_u64);
    qsort(sliced_ns, rounds, sizeof *sliced_ns, compare_u64);
    double serial_us = (double)serial_ns[rounds / 2] / 1000.0;
    double sliced_us = (double)sliced_ns[rounds / 2] / 1000.0;
    printf("%u %zu %.1f %.1f %.2f\n", L, bytes, serial_us, sliced_us, serial_us / sliced_us);
    return 0;
}
