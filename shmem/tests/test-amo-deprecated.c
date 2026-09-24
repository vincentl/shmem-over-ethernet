/*
 * The deprecated atomic memory operations (Annex F.2.12): shmem_TYPENAME_fetch,
 * _set, _swap, _cswap, _finc, _inc, _fadd and _add, and their C11 generic
 * spellings shmem_fetch, shmem_set, shmem_swap, shmem_cswap, shmem_finc,
 * shmem_inc, shmem_fadd and shmem_add.
 *
 * Each is exactly its modern shmem_TYPENAME_atomic_* routine under its old
 * name (deprecated.c), so this is a coverage test rather than a semantics
 * one: test-amo.c and test-amo-types.c already establish what a fetch, a
 * swap or a compare-and-swap must do. This only has to show that every old
 * name reaches the same routine, for the subset of types the deprecated
 * names cover - the five of OSOE_AMO_DEPRECATED_EXTENDED_TYPES for
 * fetch/set/swap, the three of OSOE_AMO_DEPRECATED_TYPES for the rest.
 *
 * Every PE operates on a word of its right neighbour's, one routine at a
 * time, and checks the result in its own copy: every PE runs the same
 * sequence, so the barrier after a step makes its left neighbour's identical
 * call into this PE's word visible, and a plain read of the word checks the
 * call. A second barrier ends the step - without it the left neighbour's next
 * call can change the word under a check still running, which a one-PE job
 * never shows and every other size does. A fetching routine's result is the
 * right neighbour's word, which only this PE writes, so it is checked where
 * it returns. The value trail is walked by hand through both the deprecated
 * name and its C11 generic spelling, so each of the sixteen (eight
 * deprecated, eight generic) entry points is exercised at least once per
 * type, with its return value and its effect both checked.
 */
#include <stddef.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static int me, npes, right;

/* The end of one step of the ring: the left neighbour's call into this PE's
 * word is visible after the first barrier, and nobody's next call starts
 * until every check of this step is done. */
#define STEP(checks)                                                                \
    do {                                                                            \
        shmem_barrier_all();                                                        \
        checks;                                                                     \
        shmem_barrier_all();                                                        \
    } while (0)

/* fetch, set and swap: int, long, long long, float and double. */
#define DEPRECATED_FETCH_SET_SWAP(T, NAME)                                          \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK_FATAL(a != NULL);      /* NULL would be dereferenced just below */    \
        a[0] = (T)0;                                                                \
        a[1] = (T)0x5a;                          /* the guard */                    \
        shmem_barrier_all();                                                        \
                                                                                    \
        shmem_##NAME##_set(a, (T)7, right);                                         \
        STEP(CHECK(a[0] == (T)7));                                                  \
        CHECK(shmem_##NAME##_fetch(a, right) == (T)7);                              \
        CHECK(shmem_##NAME##_swap(a, (T)9, right) == (T)7);                         \
        STEP(CHECK(a[0] == (T)9); CHECK(a[1] == (T)0x5a));                          \
                                                                                    \
        /* The C11 generic spellings, at least once each. */                        \
        shmem_set(a, (T)3, right);                                                  \
        STEP(CHECK(a[0] == (T)3));                                                  \
        CHECK(shmem_fetch(a, right) == (T)3);                                       \
        CHECK(shmem_fetch((const T *)a, right) == (T)3);  /* source is const */    \
        CHECK(shmem_swap(a, (T)4, right) == (T)3);                                  \
        STEP(CHECK(a[0] == (T)4); CHECK(a[1] == (T)0x5a));                          \
                                                                                    \
        shmem_free(a);                                                              \
    } while (0);

/* cswap, finc, inc, fadd and add: int, long and long long only. */
#define DEPRECATED_CSWAP_FINC_INC_FADD_ADD(T, NAME)                                 \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK_FATAL(a != NULL);      /* NULL would be dereferenced just below */    \
        a[0] = (T)0;                                                                \
        a[1] = (T)0x5a;                          /* the guard */                    \
        shmem_barrier_all();                                                        \
                                                                                    \
        shmem_##NAME##_inc(a, right);                             /* 0 -> 1 */      \
        STEP(CHECK(a[0] == (T)1));                                                  \
        CHECK(shmem_##NAME##_finc(a, right) == (T)1);             /* 1 -> 2 */      \
        STEP(CHECK(a[0] == (T)2));                                                  \
        shmem_##NAME##_add(a, (T)10, right);                      /* 2 -> 12 */     \
        STEP(CHECK(a[0] == (T)12));                                                 \
        CHECK(shmem_##NAME##_fadd(a, (T)3, right) == (T)12);      /* 12 -> 15 */    \
        STEP(CHECK(a[0] == (T)15));                                                 \
        CHECK(shmem_##NAME##_cswap(a, (T)15, (T)20, right) == (T)15); /* matches */ \
        STEP(CHECK(a[0] == (T)20));                                                 \
        CHECK(shmem_##NAME##_cswap(a, (T)15, (T)99, right) == (T)20); /* stale */   \
        STEP(CHECK(a[0] == (T)20); CHECK(a[1] == (T)0x5a));                         \
                                                                                    \
        /* The C11 generic spellings, at least once each. */                        \
        shmem_inc(a, right);                                      /* 20 -> 21 */    \
        STEP(CHECK(a[0] == (T)21));                                                 \
        CHECK(shmem_finc(a, right) == (T)21);                     /* 21 -> 22 */    \
        STEP(CHECK(a[0] == (T)22));                                                 \
        shmem_add(a, (T)5, right);                                /* 22 -> 27 */    \
        STEP(CHECK(a[0] == (T)27));                                                 \
        CHECK(shmem_fadd(a, (T)1, right) == (T)27);               /* 27 -> 28 */    \
        STEP(CHECK(a[0] == (T)28));                                                 \
        CHECK(shmem_cswap(a, (T)28, (T)0, right) == (T)28);       /* 28 -> 0 */     \
        STEP(CHECK(a[0] == (T)0); CHECK(a[1] == (T)0x5a));                          \
                                                                                    \
        shmem_free(a);                                                              \
    } while (0);

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    right = (me + 1) % npes;

    OSOE_AMO_DEPRECATED_EXTENDED_TYPES(DEPRECATED_FETCH_SET_SWAP)
    OSOE_AMO_DEPRECATED_TYPES(DEPRECATED_CSWAP_FINC_INC_FADD_ADD)

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
