/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The nonblocking fetching AMOs (spec §9.7.2): after the call and a
 * shmem_quiet, *fetch holds the target's value from before the operation and
 * the target holds the new one - the same check test-amo-types.c makes for
 * the blocking forms, run here for every spelling in turn: typed plain, typed
 * ctx, C11 generic plain, C11 generic ctx. That last pair is also the only
 * place compare_swap_nbi's two forms - five and six arguments, OSOE_PICK6 -
 * are exercised at all.
 *
 * A second phase has every PE hammer one shared word with a mix of
 * fetch_inc_nbi and fetch_add_nbi(+1) - both add exactly one, so the two are
 * interchangeable here - issuing every call before a single shmem_quiet, as a
 * real user of _nbi would, and checks the fetched values are the exact
 * permutation 0 .. ROUNDS * npes - 1: one per operation, none lost, none
 * duplicated, as test-amo.c's contended_fetch_add checks for the blocking
 * form.
 *
 * The first phase runs twice: on each PE's own word, then on its right
 * neighbour's, which crosses a node on the map tests - the path a nonblocking
 * AMO that really waited for the next quiet would change. Each PE's copy is
 * written by one PE only, and every check reads the target's word with a
 * blocking fetch. `fetch` is set to a value no step expects before every call,
 * so a routine that never stores is seen; and with OSOE_TEST_AMO_NBI_MISUSE
 * set, one call on a word outside the symmetric heap must end the job naming
 * the nonblocking routine rather than the blocking one beneath it.
 *
 * shmem_barrier_all separates every phase from the next, so no PE reads a
 * word a neighbour is still changing (an earlier atomics test raced exactly
 * that way).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 500 };

/* The target's copy of a[i], read as the checks need it: with a blocking
 * fetch, since on the second pass the word is another PE's. */
#define TARGET_WORD(a, i) shmem_atomic_fetch(&(a)[i], target)

/* ---- Table 6 and the integer part of Table 7 --------------------------- */

/*
 * fetch_nbi doesn't change a[0]; swap_nbi, compare_swap_nbi, fetch_inc_nbi
 * and fetch_add_nbi each do, once per spelling, so the expected value after
 * every step is worked out by hand below rather than re-derived at runtime.
 * Every op targets `target`'s copy of `a`, which every PE seeds alike before
 * the barrier, and which only one PE ever changes.
 */
#define STANDARD_AND_EXTENDED_NBI(T, NAME)                                         \
    do {                                                                          \
        shmem_ctx_t ctx = SHMEM_CTX_DEFAULT;                                      \
        T *a = shmem_malloc(2 * sizeof *a);                                       \
        T fetch;                                                                  \
        CHECK_FATAL(a != NULL);      /* NULL would be dereferenced just below */  \
        a[0] = (T)1;                                                              \
        a[1] = (T)0x5a;                          /* the guard */                  \
        shmem_barrier_all();                                                      \
                                                                                    \
        /* fetch_nbi: typed plain, typed ctx, generic plain, generic ctx. */      \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_nbi(&fetch, a, target);                           \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)1);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_nbi(ctx, &fetch, a, target);                  \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)1);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_nbi(&fetch, a, target);                                    \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)1);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_nbi(ctx, &fetch, a, target);                               \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)1);                                                     \
        CHECK(TARGET_WORD(a, 0) == (T)1);                                                      \
                                                                                    \
        /* swap_nbi: 1 -> 2 -> 3 -> 4 -> 5. */                                    \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_swap_nbi(&fetch, a, (T)2, target);                      \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)1);                                                     \
        CHECK(TARGET_WORD(a, 0) == (T)2);                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_swap_nbi(ctx, &fetch, a, (T)3, target);             \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)2);                                                     \
        CHECK(TARGET_WORD(a, 0) == (T)3);                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_swap_nbi(&fetch, a, (T)4, target);                               \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)3);                                                     \
        CHECK(TARGET_WORD(a, 0) == (T)4);                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_swap_nbi(ctx, &fetch, a, (T)5, target);                          \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)4);                                                     \
        CHECK(TARGET_WORD(a, 0) == (T)5);                                                      \
                                                                                    \
        /* compare_swap_nbi: each spelling once where the condition holds and       \
         * once where it does not, 5 -> 6 -> 7 -> 8 -> 11: a routine that swapped   \
         * cond and value, or ignored cond, fails where it holds. */                \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_compare_swap_nbi(&fetch, a, (T)5, (T)6, target);      \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)5);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)6);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_compare_swap_nbi(&fetch, a, (T)100, (T)999, target);  \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)6);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)6);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_compare_swap_nbi(ctx, &fetch, a, (T)6, (T)7, target); \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)6);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)7);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_compare_swap_nbi(ctx, &fetch, a, (T)100, (T)999, target); \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)7);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)7);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_compare_swap_nbi(&fetch, a, (T)7, (T)8, target);               \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)7);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)8);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_compare_swap_nbi(&fetch, a, (T)100, (T)999, target);           \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)8);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)8);                                                        \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_compare_swap_nbi(ctx, &fetch, a, (T)8, (T)11, target);         \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)8);                                                       \
        CHECK(TARGET_WORD(a, 0) == (T)11);                                                       \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_compare_swap_nbi(ctx, &fetch, a, (T)100, (T)999, target);      \
        shmem_quiet();                                                              \
        CHECK(fetch == (T)11);                                                      \
        CHECK(TARGET_WORD(a, 0) == (T)11);                                                       \
                                                                                    \
        /* fetch_inc_nbi: +1 four times, 11 -> 15. */                             \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_inc_nbi(&fetch, a, target);                       \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)11);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)12);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_inc_nbi(ctx, &fetch, a, target);              \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)12);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)13);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_inc_nbi(&fetch, a, target);                                \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)13);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)14);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_inc_nbi(ctx, &fetch, a, target);                           \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)14);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)15);                                                     \
                                                                                    \
        /* fetch_add_nbi: +5, +3, +2, +1, 15 -> 26. */                            \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_add_nbi(&fetch, a, (T)5, target);                 \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)15);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)20);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_add_nbi(ctx, &fetch, a, (T)3, target);        \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)20);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)23);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_add_nbi(&fetch, a, (T)2, target);                          \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)23);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)25);                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_add_nbi(ctx, &fetch, a, (T)1, target);                     \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)25);                                                    \
        CHECK(TARGET_WORD(a, 0) == (T)26);                                                     \
                                                                                    \
        CHECK(TARGET_WORD(a, 1) == (T)0x5a);                   /* nothing spilled */          \
        shmem_free(a);                                                            \
    } while (0)

/* ---- Table 8 ------------------------------------------------------------ */

/* fetch_or_nbi, fetch_and_nbi, fetch_xor_nbi, each once per spelling; the bit
 * pattern after every step is again worked out by hand. At every step the
 * operator under test gives a result the other two do not, so a routine that
 * applied the wrong one fails at every spelling: an or that set only clear
 * bits would pass as an xor, and an and of a word with itself as an or. */
#define BITWISE_NBI(T, NAME)                                                       \
    do {                                                                          \
        shmem_ctx_t ctx = SHMEM_CTX_DEFAULT;                                      \
        T *a = shmem_malloc(2 * sizeof *a);                                       \
        T fetch;                                                                  \
        CHECK_FATAL(a != NULL);      /* NULL would be dereferenced just below */  \
        a[0] = (T)0x01;                                                           \
        a[1] = (T)0x5a;                                                           \
        shmem_barrier_all();                                                      \
                                                                                    \
        /* fetch_or_nbi: 0x01 -> 0x0f -> 0x1f -> 0x3f -> 0x7f. */                 \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_or_nbi(&fetch, a, (T)0x0f, target);               \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x01);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x0f);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_or_nbi(ctx, &fetch, a, (T)0x18, target);      \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x0f);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x1f);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_or_nbi(&fetch, a, (T)0x30, target);                        \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x1f);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x3f);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_or_nbi(ctx, &fetch, a, (T)0x7f, target);                   \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x3f);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x7f);                                                   \
                                                                                    \
        /* fetch_and_nbi: 0x7f -> 0x55 -> 0x11 -> 0x11 -> 0x01. */                \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_and_nbi(&fetch, a, (T)0x55, target);              \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x7f);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x55);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_and_nbi(ctx, &fetch, a, (T)0x33, target);     \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x55);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x11);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_and_nbi(&fetch, a, (T)0x13, target);                       \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x11);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x11);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_and_nbi(ctx, &fetch, a, (T)0x01, target);                  \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x11);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x01);                                                   \
                                                                                    \
        /* fetch_xor_nbi: 0x01 -> 0xfe -> 0xf1 -> 0x01 -> 0x00. */                \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_xor_nbi(&fetch, a, (T)0xff, target);              \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x01);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0xfe);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_xor_nbi(ctx, &fetch, a, (T)0x0f, target);     \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0xfe);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0xf1);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_xor_nbi(&fetch, a, (T)0xf0, target);                       \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0xf1);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x01);                                                   \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_xor_nbi(ctx, &fetch, a, (T)0x01, target);                  \
        shmem_quiet();                                                            \
        CHECK(fetch == (T)0x01);                                                  \
        CHECK(TARGET_WORD(a, 0) == (T)0x00);                                                   \
                                                                                    \
        CHECK(TARGET_WORD(a, 1) == (T)0x5a);                                                   \
        shmem_free(a);                                                            \
    } while (0)

/* ---- Table 7's float and double ------------------------------------------ */

/* fetch_nbi and swap_nbi only (Table 7's other _nbi routine, set_nbi, does
 * not exist - §9.7.2 gives set no nonblocking form). Fractional, signed
 * values: nothing but a bit-pattern move carries them through an integer
 * read-modify-write intact, as test-amo-types.c's FLOATING macro checks for
 * the blocking forms. */
#define FLOATING_NBI(T, NAME, V0, V1, V2, V3, V4)                                  \
    do {                                                                          \
        shmem_ctx_t ctx = SHMEM_CTX_DEFAULT;                                      \
        T *a = shmem_malloc(2 * sizeof *a);                                       \
        T fetch;                                                                  \
        CHECK_FATAL(a != NULL);      /* NULL would be dereferenced just below */  \
        a[0] = (V0);                                                              \
        a[1] = (T)-3.25;                                                          \
        shmem_barrier_all();                                                      \
                                                                                    \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_fetch_nbi(&fetch, a, target);                           \
        shmem_quiet();                                                            \
        CHECK(fetch == (V0));                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_fetch_nbi(ctx, &fetch, a, target);                  \
        shmem_quiet();                                                            \
        CHECK(fetch == (V0));                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_nbi(&fetch, a, target);                                    \
        shmem_quiet();                                                            \
        CHECK(fetch == (V0));                                                     \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_fetch_nbi(ctx, &fetch, a, target);                               \
        shmem_quiet();                                                            \
        CHECK(fetch == (V0));                                                     \
        CHECK(TARGET_WORD(a, 0) == (V0));                                                      \
                                                                                    \
        fetch = (T)0x7e;                                                            \
        shmem_##NAME##_atomic_swap_nbi(&fetch, a, (V1), target);                      \
        shmem_quiet();                                                            \
        CHECK(fetch == (V0));                                                     \
        CHECK(TARGET_WORD(a, 0) == (V1));                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_ctx_##NAME##_atomic_swap_nbi(ctx, &fetch, a, (V2), target);             \
        shmem_quiet();                                                            \
        CHECK(fetch == (V1));                                                     \
        CHECK(TARGET_WORD(a, 0) == (V2));                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_swap_nbi(&fetch, a, (V3), target);                               \
        shmem_quiet();                                                            \
        CHECK(fetch == (V2));                                                     \
        CHECK(TARGET_WORD(a, 0) == (V3));                                                      \
        fetch = (T)0x7e;                                                            \
        shmem_atomic_swap_nbi(ctx, &fetch, a, (V4), target);                          \
        shmem_quiet();                                                            \
        CHECK(fetch == (V3));                                                     \
        CHECK(TARGET_WORD(a, 0) == (V4));                                                      \
                                                                                    \
        CHECK(TARGET_WORD(a, 1) == (T)-3.25);                                                  \
        shmem_free(a);                                                            \
    } while (0)

static void semantics(int target)
{
    STANDARD_AND_EXTENDED_NBI(int, int);
    STANDARD_AND_EXTENDED_NBI(long, long);
    STANDARD_AND_EXTENDED_NBI(unsigned long long, ulonglong);

    BITWISE_NBI(int64_t, int64);

    FLOATING_NBI(float, float, -0.125f, 2.5f, -7.5f, 100.25f, 0.0f);
    FLOATING_NBI(double, double, -0.125, 2.5, -7.5, 100.25, 0.0);
}

/*
 * Every PE hammers one shared word with a mix of fetch_inc_nbi and
 * fetch_add_nbi(+1), batching every call before a single shmem_quiet - unlike
 * test-amo.c's contended_fetch_add, which quiets (returns) once per call,
 * because a blocking fetching AMO has no other option. The fetched values,
 * gathered from every PE, must be exactly 0 .. ROUNDS * npes - 1.
 */
static void contended_nbi(int me, int n)
{
    uint64_t *counter = shmem_malloc(sizeof *counter);
    uint64_t *seen = shmem_malloc(sizeof *seen * ROUNDS * (size_t)n);
    CHECK(counter && seen);
    *counter = 0;
    memset(seen, 0xff, sizeof *seen * ROUNDS * (size_t)n);
    shmem_barrier_all();

    uint64_t mine[ROUNDS];
    for (int i = 0; i < ROUNDS; i++) {
        if (i % 2 == 0)
            shmem_uint64_atomic_fetch_inc_nbi(&mine[i], counter, 0);
        else
            shmem_uint64_atomic_fetch_add_nbi(&mine[i], counter, 1, 0);
    }
    shmem_quiet();
    shmem_putmem(seen + (size_t)me * ROUNDS, mine, sizeof mine, 0);
    shmem_barrier_all();

    if (me == 0) {
        CHECK(*counter == (uint64_t)ROUNDS * (uint64_t)n);
        char *hit = calloc(ROUNDS * (size_t)n, 1);
        CHECK_FATAL(hit != NULL);   /* NULL would be indexed just below */
        int bad = 0;
        for (size_t i = 0; i < ROUNDS * (size_t)n; i++) {
            if (seen[i] >= ROUNDS * (uint64_t)n || hit[seen[i]])
                bad++;
            else
                hit[seen[i]] = 1;
        }
        CHECK(bad == 0);
        free(hit);
    }
    shmem_barrier_all();
    shmem_free(seen);
    shmem_free(counter);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    if (getenv("OSOE_TEST_AMO_NBI_MISUSE")) {
        long outside = 0, fetch;
        shmem_long_atomic_fetch_add_nbi(&fetch, &outside, 1, (me + 1) % n);
    }
    semantics(me);
    semantics((me + 1) % n);
    contended_nbi(me, n);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
