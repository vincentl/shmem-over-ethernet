/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The one-node quiet's fence (T35; the v0.2 step 1 review, correctness K2): a
 * store-buffering litmus through shmem_quiet. PE 0 stores X = 1, quiets and
 * loads Y; PE 1 stores Y = 1, quiets and loads X; both words on PE 0's heap,
 * through shmem_ptr, a round at a time from a spin barrier so that the two
 * race within a line's transfer of each other. Both loads seeing 0 is
 * forbidden: quiet orders this PE's stores before what follows (spec §9.12.2),
 * and in a job of one node that is all osoe_serializing_fence does, in
 * osoe_transport_wait (transport-am.c).
 *
 * Only some builds can show the fence's absence. On aarch64 at GCC's default
 * -march the counters quiet bumps around the fence are ldar and stlr, which
 * keep a store before a later load in the hardware: the review's litmus found
 * no forbidden outcome without the fence in 500,000 rounds there. Built with
 * -march=armv8.3-a, whose acquire loads are ldapr, it found 109; and on x86-64
 * the fence's mfence is all that orders the two. So the test runs where the
 * library's acquire loads are ldapr - the build's own flags say so
 * (__ARM_FEATURE_RCPC) and the CPU has them (HWCAP_LRCPC) - or on x86-64, and
 * elsewhere skips (77) saying why. No preset builds it so (the author,
 * 2026-10-20): configure by hand, as the README's "How osoe is tested" says.
 *
 *   test-quiet-litmus [ROUNDS]     20 million by default: built for Armv8.3 in
 *                                  the VM, without the fence, 500,000 rounds
 *                                  found 0, 2 and 3 forbidden outcomes in three
 *                                  runs, and a round takes about 80 ns
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#endif

#include <shmem.h>

#include "check.h"

/* The words both PEs share, on PE 0's heap: the litmus's two, PE 1's result,
 * and the barrier that starts and ends each round. */
struct shared {
    _Atomic int x, y, result;
    _Atomic unsigned long start, end;
};

/* Whether this build on this CPU can show the fence's absence, and if not why. */
static const char *cannot_show(void)
{
#if defined(__x86_64__)
    return NULL;
#elif defined(__aarch64__) && defined(__ARM_FEATURE_RCPC)
    return (getauxval(AT_HWCAP) & HWCAP_LRCPC) ? NULL
                                                : "the CPU has no ldapr (HWCAP_LRCPC)";
#elif defined(__aarch64__)
    return "built without ldapr: configure with -DCMAKE_C_FLAGS=-march=armv8.3-a";
#else
    return "neither x86-64 nor aarch64";
#endif
}

/* Every PE once at `word`, the round's count of arrivals: a spin, as the
 * review's litmus started its rounds, so that both leave within a line's
 * transfer. */
static void meet(_Atomic unsigned long *word, unsigned long round)
{
    atomic_fetch_add_explicit(word, 1, memory_order_seq_cst);
    while (atomic_load_explicit(word, memory_order_acquire) < 2 * (round + 1))
        ;
}

int main(int argc, char **argv)
{
    unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000000;
    shmem_init();
    int me = shmem_my_pe();
    CHECK_FATAL(shmem_n_pes() == 2);
    struct shared *heap = shmem_calloc(1, sizeof *heap);
    CHECK_FATAL(heap != NULL);
    struct shared *s = shmem_ptr(heap, 0);
    CHECK_FATAL(s != NULL);             /* two PEs on one node */
    const char *why = cannot_show();
    if (why) {
        if (me == 0)
            printf("skipped: %s\n", why);
        shmem_finalize();
        return 77;
    }

    unsigned long forbidden = 0;
    for (unsigned long r = 0; r < rounds; r++) {
        meet(&s->start, r);
        if (me == 0) {
            atomic_store_explicit(&s->x, 1, memory_order_relaxed);
            shmem_quiet();
            int r0 = atomic_load_explicit(&s->y, memory_order_relaxed);
            meet(&s->end, r);
            if (r0 == 0 && atomic_load_explicit(&s->result, memory_order_relaxed) == 0)
                forbidden++;
            atomic_store_explicit(&s->x, 0, memory_order_relaxed);
            atomic_store_explicit(&s->y, 0, memory_order_relaxed);
        } else {
            atomic_store_explicit(&s->y, 1, memory_order_relaxed);
            shmem_quiet();
            atomic_store_explicit(&s->result,
                                  atomic_load_explicit(&s->x, memory_order_relaxed),
                                  memory_order_relaxed);
            meet(&s->end, r);
        }
    }
    shmem_barrier_all();
    if (me == 0) {
        printf("%lu forbidden outcomes in %lu rounds\n", forbidden, rounds);
        CHECK(forbidden == 0);
    }
    shmem_free(heap);
    shmem_finalize();
    return CHECK_DONE();
}
