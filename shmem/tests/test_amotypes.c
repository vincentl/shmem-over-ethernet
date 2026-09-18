/*
 * Every AMO type of Tables 6, 7 and 8, and the C11 type-generic interfaces
 * (spec §9.7; §7.4).
 *
 * The routines are macro-generated from three type lists, so the risk is not
 * that one of them computes the wrong answer - they all run the same three
 * lines - but that a type is missing from a list, is generated at the wrong
 * width, or is absent from the C11 selection. So this test is generated from
 * the *same* lists shmem.h declares from, which makes it a completeness check:
 * a type added to a table and forgotten anywhere else fails to compile here.
 *
 * Two things a generated test can still get wrong and this one does not:
 *
 *   - **Width.** Every object is allocated as a two-element array and the
 *     second element is a guard, checked after every operation. A routine
 *     generated at eight bytes for a four-byte type passes every value check
 *     and destroys its neighbour.
 *   - **Signedness.** The value crosses the boundary as a uint64_t, so a
 *     negative value and a negative fetched result are the interesting cases,
 *     and the signed types are exercised with both.
 *
 * The C11 forms are called for every type in each table. That part is mostly a
 * compile-time check - a missing `_Generic` association is an error at the call
 * - which is the only kind of check a selection list admits.
 */
#include <stddef.h>
#include <stdint.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static int me, npes, right;

/* ---- Table 6 and the integer part of Table 7 -------------------------- */

#define STANDARD_AND_EXTENDED(T, NAME)                                              \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK(a != NULL);                                                           \
        a[0] = (T)0;                                                                \
        a[1] = (T)0x5a;                         /* the guard */                     \
        shmem_barrier_all();                                                        \
                                                                                    \
        /* Reaching another PE, then reading one's own. */                          \
        shmem_##NAME##_atomic_set(a, (T)7, right);                                  \
        shmem_barrier_all();                                                        \
        CHECK(a[0] == (T)7);                                                        \
        CHECK(shmem_##NAME##_atomic_fetch(a, me) == (T)7);                          \
        CHECK(a[1] == (T)0x5a);                                                     \
                                                                                    \
        CHECK(shmem_##NAME##_atomic_swap(a, (T)9, me) == (T)7);                     \
        CHECK(shmem_##NAME##_atomic_compare_swap(a, (T)9, (T)11, me) == (T)9);      \
        CHECK(a[0] == (T)11);                                                       \
        CHECK(shmem_##NAME##_atomic_compare_swap(a, (T)9, (T)13, me) == (T)11);     \
        CHECK(a[0] == (T)11);                   /* the condition did not hold */    \
                                                                                    \
        shmem_##NAME##_atomic_inc(a, me);                                           \
        CHECK(a[0] == (T)12);                                                       \
        CHECK(shmem_##NAME##_atomic_fetch_inc(a, me) == (T)12);                     \
        CHECK(a[0] == (T)13);                                                       \
        shmem_##NAME##_atomic_add(a, (T)10, me);                                    \
        CHECK(a[0] == (T)23);                                                       \
        CHECK(shmem_##NAME##_atomic_fetch_add(a, (T)2, me) == (T)23);               \
        CHECK(a[0] == (T)25);                                                       \
        CHECK(a[1] == (T)0x5a);                 /* nothing spilled */               \
                                                                                    \
        /* The C11 forms, on every type. */                                         \
        shmem_atomic_set(a, (T)1, me);                                              \
        CHECK(shmem_atomic_fetch(a, me) == (T)1);                                   \
        CHECK(shmem_atomic_swap(a, (T)2, me) == (T)1);                              \
        CHECK(shmem_atomic_compare_swap(a, (T)2, (T)3, me) == (T)2);                \
        shmem_atomic_inc(a, me);                                                    \
        CHECK(shmem_atomic_fetch_inc(a, me) == (T)4);                               \
        shmem_atomic_add(a, (T)5, me);                                              \
        CHECK(shmem_atomic_fetch_add(a, (T)1, me) == (T)10);                        \
        CHECK(a[0] == (T)11);                                                       \
        CHECK(a[1] == (T)0x5a);                                                     \
        shmem_free(a);                                                              \
    } while (0);

/* Negative values, for the types that have them. The value crosses the
 * boundary as a uint64_t, so this is where a width or conversion mistake in
 * either direction shows. */
#define SIGNED_ROUNDTRIP(T, NAME)                                                   \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK(a != NULL);                                                           \
        a[0] = (T)0;                                                                \
        a[1] = (T)0x5a;                                                             \
        shmem_barrier_all();                                                        \
        shmem_##NAME##_atomic_set(a, (T)-1, me);                                    \
        CHECK(shmem_##NAME##_atomic_fetch(a, me) == (T)-1);                         \
        CHECK(a[0] == (T)-1);                                                       \
        CHECK(shmem_##NAME##_atomic_fetch_add(a, (T)1, me) == (T)-1);               \
        CHECK(a[0] == (T)0);                                                        \
        shmem_##NAME##_atomic_add(a, (T)-5, me);                                    \
        CHECK(a[0] == (T)-5);                                                       \
        CHECK(shmem_##NAME##_atomic_compare_swap(a, (T)-5, (T)-6, me) == (T)-5);    \
        CHECK(a[0] == (T)-6);                                                       \
        CHECK(a[1] == (T)0x5a);                                                     \
        shmem_free(a);                                                              \
    } while (0);

#define SIGNED_TYPES(X)                                                             \
    X(int, int) X(long, long) X(long long, longlong)                                \
    X(int32_t, int32) X(int64_t, int64) X(ptrdiff_t, ptrdiff)

/* ---- Table 8 ----------------------------------------------------------- */

#define BITWISE(T, NAME)                                                            \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK(a != NULL);                                                           \
        a[1] = (T)0x5a;                                                             \
        shmem_barrier_all();                                                        \
        shmem_##NAME##_atomic_set(a, (T)0, me);                                     \
        shmem_##NAME##_atomic_or(a, (T)0x0f, me);                                   \
        CHECK(a[0] == (T)0x0f);                                                     \
        shmem_##NAME##_atomic_and(a, (T)0x0c, me);                                  \
        CHECK(a[0] == (T)0x0c);                                                     \
        shmem_##NAME##_atomic_xor(a, (T)0x0a, me);                                  \
        CHECK(a[0] == (T)0x06);                                                     \
        CHECK(shmem_##NAME##_atomic_fetch_or(a, (T)0x10, me) == (T)0x06);           \
        CHECK(a[0] == (T)0x16);                                                     \
        CHECK(shmem_##NAME##_atomic_fetch_and(a, (T)0x12, me) == (T)0x16);          \
        CHECK(a[0] == (T)0x12);                                                     \
        CHECK(shmem_##NAME##_atomic_fetch_xor(a, (T)0x02, me) == (T)0x12);          \
        CHECK(a[0] == (T)0x10);                                                     \
        CHECK(a[1] == (T)0x5a);                                                     \
                                                                                    \
        shmem_atomic_and(a, (T)0, me);                                              \
        shmem_atomic_or(a, (T)0x21, me);                                            \
        shmem_atomic_xor(a, (T)0x01, me);                                           \
        CHECK(a[0] == (T)0x20);                                                     \
        CHECK(shmem_atomic_fetch_or(a, (T)0x01, me) == (T)0x20);                    \
        CHECK(shmem_atomic_fetch_and(a, (T)0x20, me) == (T)0x21);                   \
        CHECK(shmem_atomic_fetch_xor(a, (T)0x20, me) == (T)0x20);                   \
        CHECK(a[0] == (T)0);                                                        \
        CHECK(a[1] == (T)0x5a);                                                     \
        shmem_free(a);                                                              \
    } while (0);

/* ---- Table 7's float and double ---------------------------------------- */

#define FLOATING(T, NAME, NOTINT, OTHER)                                            \
    do {                                                                            \
        T *a = shmem_malloc(2 * sizeof *a);                                         \
        CHECK(a != NULL);                                                           \
        a[0] = (T)0;                                                                \
        a[1] = (T)-3.25;                                                            \
        shmem_barrier_all();                                                        \
        /* A value with a fraction and a sign: nothing but a bit-pattern move       \
         * carries it through an integer read-modify-write intact. */               \
        shmem_##NAME##_atomic_set(a, NOTINT, right);                                \
        shmem_barrier_all();                                                        \
        CHECK(a[0] == NOTINT);                                                      \
        CHECK(shmem_##NAME##_atomic_fetch(a, me) == NOTINT);                        \
        CHECK(shmem_##NAME##_atomic_swap(a, OTHER, me) == NOTINT);                  \
        CHECK(a[0] == OTHER);                                                       \
        CHECK(a[1] == (T)-3.25);                                                    \
        shmem_atomic_set(a, NOTINT, me);                                            \
        CHECK(shmem_atomic_fetch(a, me) == NOTINT);                                 \
        CHECK(shmem_atomic_swap(a, OTHER, me) == NOTINT);                           \
        CHECK(a[0] == OTHER);                                                       \
        CHECK(a[1] == (T)-3.25);                                                    \
        shmem_free(a);                                                              \
    } while (0);

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    right = (me + 1) % npes;

    OSOE_AMO_STANDARD_TYPES(STANDARD_AND_EXTENDED)
    SIGNED_TYPES(SIGNED_ROUNDTRIP)
    OSOE_AMO_BITWISE_TYPES(BITWISE)

    FLOATING(float, float, -0.125f, 2.5f)
    FLOATING(double, double, -0.125, 2.5)

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
