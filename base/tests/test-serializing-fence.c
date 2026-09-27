/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The serializing fence orders a store before a later load (§3.16, §4.6
 * rule 2): the store-buffering shape quiet needs, W->R, which is the stronger
 * half. fence's W->W is weaker and follows from it.
 *
 *     thread 0:  x = 1;  osoe_serializing_fence();  r0 = y
 *     thread 1:  y = 1;  osoe_serializing_fence();  r1 = x
 *
 * r0 == 0 && r1 == 0 must never happen. The same pair with nothing between
 * runs first as a control: it must show that outcome, or this machine and load
 * give the harness no way to see a reordering, and the test skips (77) rather
 * than pass on nothing. On the development VM the control shows it in about
 * half the rounds, and so did the old serializing point, a seq_cst store to a
 * thread-local word (an stlr on aarch64): that is the mutation this catches.
 *
 * Plain volatile accesses on purpose: they are what an application's store
 * through shmem_ptr and a put's memcpy are. Not run under ThreadSanitizer, which
 * would report exactly the race this measures.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/order.h"

enum { ROUNDS = 200000, LINE = 64 };

struct word { _Alignas(LINE) volatile uint32_t v; };

static struct word *words[2];
static uint8_t *seen[2];
static _Alignas(LINE) volatile uint64_t progress[2];
static bool fenced;

static void *side(void *arg)
{
    int t = (int)(intptr_t)arg;
    for (uint64_t r = 0; r < ROUNDS; r++) {
        /* Meet the other thread at round r. The meeting is itself fenced, so
         * nothing of round r - 1 leaks into round r. */
        progress[t] = r + 1;
        osoe_serializing_fence();
        while (progress[1 - t] < r + 1)
            ;
        osoe_serializing_fence();
        words[t][r].v = 1;
        if (fenced)
            osoe_serializing_fence();
        seen[t][r] = (uint8_t)words[1 - t][r].v;
    }
    return NULL;
}

static uint64_t run(bool with_fence)
{
    fenced = with_fence;
    for (int t = 0; t < 2; t++) {
        memset(words[t], 0, (size_t)ROUNDS * sizeof(struct word));
        progress[t] = 0;
    }
    pthread_t th[2];
    for (int t = 0; t < 2; t++)
        pthread_create(&th[t], NULL, side, (void *)(intptr_t)t);
    for (int t = 0; t < 2; t++)
        pthread_join(th[t], NULL);
    uint64_t both_zero = 0;
    for (uint64_t r = 0; r < ROUNDS; r++)
        both_zero += seen[0][r] == 0 && seen[1][r] == 0;
    return both_zero;
}

int main(void)
{
    for (int t = 0; t < 2; t++) {
        words[t] = aligned_alloc(LINE, (size_t)ROUNDS * sizeof(struct word));
        seen[t] = malloc(ROUNDS);
        if (!words[t] || !seen[t])
            return 1;
    }
    uint64_t control = run(false);
    uint64_t with_fence = run(true);
    printf("store-buffering, %d rounds: %llu both-zero with nothing between, "
           "%llu with the serializing fence\n", ROUNDS, (unsigned long long)control,
           (unsigned long long)with_fence);
    for (int t = 0; t < 2; t++) {
        free(words[t]);
        free(seen[t]);
    }
    if (with_fence != 0)
        return 1;
    if (control == 0) {
        printf("SKIPPED: the control showed no reordering, so a zero proves nothing\n");
        return 77;
    }
    return 0;
}
