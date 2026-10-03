/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The store-buffering shape (Dekker, SB) on aarch64, for §3.16's two-sided
 * rule and for quiet (§4.6).
 *
 *     thread 0:  x = 1;  <between>;  r0 = y
 *     thread 1:  y = 1;  <between>;  r1 = x
 *
 * r0 == 0 && r1 == 0 is forbidden by sequential consistency. In a two-sided
 * protocol it is a lost wake (each side misses the other's store); after quiet
 * it is a store that was not "complete and visible to all PEs" (spec §9.12.2).
 * Variants:
 *
 *     plain        plain store, plain load: the control. Must show both-zero.
 *     sc-acquire   seq_cst store, acquire load: §3.16's two-sided rule as it
 *                  stands. Built for Armv8.3 or later, GCC emits stlr; ldapr.
 *     sc-sc        seq_cst store, seq_cst load: stlr; ldar on every target.
 *     quiet-now    plain store, the serializing store (a seq_cst store to a
 *                  thread-local word), plain load: quiet as implemented.
 *     quiet-dmb    plain store, atomic_thread_fence(seq_cst), plain load.
 *
 * Each round uses fresh words, one cache line each, so nothing is reset while
 * the other thread might still read it. The two threads step through the rounds
 * together, meeting at each on a pair of progress words.
 *
 *   gcc -O2 -march=armv8.3-a -pthread store-buffer-litmus.c -o sb
 *   ./sb plain|sc-acquire|sc-sc|quiet-now|quiet-dmb [ROUNDS]
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE = 64 };
enum variant { PLAIN, SC_ACQUIRE, SC_SC, QUIET_NOW, QUIET_DMB, VARIANTS };
static const char *names[VARIANTS] = { "plain", "sc-acquire", "sc-sc", "quiet-now", "quiet-dmb" };

struct word { _Alignas(LINE) _Atomic uint32_t v; };
static struct word *words[2];
static uint8_t *seen[2];
static _Alignas(LINE) _Atomic uint64_t progress[2];
static enum variant variant;
static uint64_t rounds;
static _Thread_local _Atomic uint32_t serial;       /* base/order.c's word */

static void pin(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

/* Kept out of line so each variant's instruction pair is exactly as named. */
__attribute__((noinline)) static uint32_t side(_Atomic uint32_t *mine, _Atomic uint32_t *other)
{
    switch (variant) {
    case PLAIN:
        *(volatile uint32_t *)mine = 1;
        return *(volatile uint32_t *)other;
    case SC_ACQUIRE:
        atomic_store_explicit(mine, 1, memory_order_seq_cst);
        return atomic_load_explicit(other, memory_order_acquire);
    case SC_SC:
        atomic_store_explicit(mine, 1, memory_order_seq_cst);
        return atomic_load_explicit(other, memory_order_seq_cst);
    case QUIET_NOW:
        *(volatile uint32_t *)mine = 1;
        atomic_store_explicit(&serial, 1, memory_order_seq_cst);
        return *(volatile uint32_t *)other;
    case QUIET_DMB:
    default:
        *(volatile uint32_t *)mine = 1;
        atomic_thread_fence(memory_order_seq_cst);
        return *(volatile uint32_t *)other;
    }
}

static void *thread(void *arg)
{
    int t = (int)(intptr_t)arg;
    pin(t);
    for (uint64_t r = 0; r < rounds; r++) {
        atomic_store_explicit(&progress[t], r + 1, memory_order_seq_cst);
        while (atomic_load_explicit(&progress[1 - t], memory_order_seq_cst) < r + 1)
            ;
        seen[t][r] = (uint8_t)side(&words[t][r].v, &words[1 - t][r].v);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    variant = VARIANTS;
    for (int i = 0; i < VARIANTS; i++)
        if (strcmp(argv[1], names[i]) == 0)
            variant = (enum variant)i;
    if (variant == VARIANTS)
        return 2;
    rounds = argc > 2 ? strtoull(argv[2], NULL, 10) : 2000000;
    for (int t = 0; t < 2; t++) {
        words[t] = aligned_alloc(LINE, rounds * sizeof(struct word));
        seen[t] = malloc(rounds);
        memset(words[t], 0, rounds * sizeof(struct word));
    }
    pthread_t th[2];
    for (int t = 0; t < 2; t++)
        pthread_create(&th[t], NULL, thread, (void *)(intptr_t)t);
    for (int t = 0; t < 2; t++)
        pthread_join(th[t], NULL);
    uint64_t both_zero = 0;
    for (uint64_t r = 0; r < rounds; r++)
        both_zero += seen[0][r] == 0 && seen[1][r] == 0;
    printf("%-10s %llu rounds, %llu both-zero (forbidden)\n", names[variant],
           (unsigned long long)rounds, (unsigned long long)both_zero);
    return 0;
}
