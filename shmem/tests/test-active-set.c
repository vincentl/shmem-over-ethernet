/*
 * The deprecated active-set collectives (Annex F; design §7.4) over the whole
 * job, the only active set osoe runs them over: every _to_all operator on each
 * of Table 11's types - the integers at one element and several, the floating
 * and complex types at several - with negative values where the order is
 * signed, and in place; shmem_broadcast32 and shmem_broadcast64
 * from every root - the root's dest left exactly as it was, every other PE's
 * updated - at a size that goes through the internode phase in pieces, and in
 * place; shmem_barrier and shmem_sync, beside the C11 shmem_sync(team) that
 * shares the name. pSync is initialized as the spec requires and must be found
 * so afterwards; one call passes a static pSync, which works because osoe never
 * touches it.
 *
 * With OSOE_TEST_PARTIAL=1 every PE passes an active set one PE short of the
 * job, which must end the job by name (shmem.active-set.partial).
 */
#include <complex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SEVERAL = 5, LARGE = 5000 };  /* LARGE 4-byte elements: two internal-region slots */

static int me, npes;
static long *pSync;
static void *pWrk;
static long static_pSync[SHMEM_REDUCE_SYNC_SIZE];

/* A value per PE and element, positive or - for the orderings - either sign. */
static long long value(int pe, int i, int signs)
{
    long long v = (long long)pe * 31 + i + 1;
    return signs && pe % 2 ? -v : v;
}

/*
 * One integer type's seven operators, at one element and several. Expected
 * values are folded in long long and converted, which is exact for the ranges
 * used at the PE counts the tests run (up to 8).
 */
#define TO_ALL_INTEGER_CHECK(T, NAME)                                                 \
    static void integer_##NAME(void)                                                  \
    {                                                                                 \
        T *src = shmem_malloc(SEVERAL * sizeof *src);                                 \
        T *dst = shmem_malloc(SEVERAL * sizeof *dst);                                 \
        CHECK_FATAL(src && dst);                                                      \
        for (int n = 1; n <= SEVERAL; n += SEVERAL - 1) {                             \
            T want_and[SEVERAL], want_or[SEVERAL], want_xor[SEVERAL];                 \
            T want_max[SEVERAL], want_min[SEVERAL], want_sum[SEVERAL], want_prod[SEVERAL]; \
            for (int i = 0; i < n; i++) {                                             \
                want_and[i] = (T)value(0, i, 0);                                      \
                want_or[i] = want_xor[i] = 0;                                         \
                want_max[i] = want_min[i] = (T)value(0, i, 1);                        \
                want_sum[i] = 0;                                                      \
                want_prod[i] = 1;                                                     \
                for (int p = 0; p < npes; p++) {                                      \
                    T u = (T)value(p, i, 0), s = (T)value(p, i, 1);                   \
                    want_and[i] = (T)(want_and[i] & u);                               \
                    want_or[i] = (T)(want_or[i] | u);                                 \
                    want_xor[i] = (T)(want_xor[i] ^ u);                               \
                    if (s > want_max[i]) want_max[i] = s;                             \
                    if (s < want_min[i]) want_min[i] = s;                             \
                    want_sum[i] = (T)(want_sum[i] + u);                               \
                    want_prod[i] = (T)(want_prod[i] * (T)(i % 2 ? 2 : 1));            \
                }                                                                     \
            }                                                                         \
            for (int i = 0; i < n; i++) src[i] = (T)value(me, i, 0);                  \
            shmem_##NAME##_and_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);         \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_and[i]);                 \
            shmem_##NAME##_or_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);          \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_or[i]);                  \
            shmem_##NAME##_xor_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);         \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_xor[i]);                 \
            shmem_##NAME##_sum_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);         \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_sum[i]);                 \
            for (int i = 0; i < n; i++) src[i] = (T)value(me, i, 1);                  \
            shmem_##NAME##_max_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);         \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_max[i]);                 \
            shmem_##NAME##_min_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);         \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_min[i]);                 \
            for (int i = 0; i < n; i++) src[i] = (T)(i % 2 ? 2 : 1);                  \
            shmem_##NAME##_prod_to_all(dst, src, n, 0, 0, npes, pWrk, pSync);        \
            for (int i = 0; i < n; i++) CHECK(dst[i] == want_prod[i]);                \
        }                                                                             \
        /* In place, with the static pSync. */                                        \
        for (int i = 0; i < SEVERAL; i++) src[i] = (T)value(me, i, 0);                \
        shmem_##NAME##_sum_to_all(src, src, SEVERAL, 0, 0, npes, pWrk, static_pSync); \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            long long want = 0;                                                       \
            for (int p = 0; p < npes; p++) want += value(p, i, 0);                    \
            CHECK(src[i] == (T)want);                                                 \
        }                                                                             \
        shmem_free(src);                                                              \
        shmem_free(dst);                                                              \
    }

TO_ALL_INTEGER_CHECK(short, short)
TO_ALL_INTEGER_CHECK(int, int)
TO_ALL_INTEGER_CHECK(long, long)
TO_ALL_INTEGER_CHECK(long long, longlong)

/* One floating type's four operators, on small integers, which are exact. */
#define TO_ALL_FLOATING_CHECK(T, NAME)                                                \
    static void floating_##NAME(void)                                                 \
    {                                                                                 \
        T *src = shmem_malloc(SEVERAL * sizeof *src);                                 \
        T *dst = shmem_malloc(SEVERAL * sizeof *dst);                                 \
        CHECK_FATAL(src && dst);                                                      \
        for (int i = 0; i < SEVERAL; i++) src[i] = (T)value(me, i, 1);                \
        shmem_##NAME##_max_to_all(dst, src, SEVERAL, 0, 0, npes, pWrk, pSync);       \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = (T)value(0, i, 1);                                               \
            for (int p = 1; p < npes; p++)                                            \
                if ((T)value(p, i, 1) > want) want = (T)value(p, i, 1);               \
            CHECK(dst[i] == want);                                                    \
        }                                                                             \
        shmem_##NAME##_min_to_all(dst, src, SEVERAL, 0, 0, npes, pWrk, pSync);       \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = (T)value(0, i, 1);                                               \
            for (int p = 1; p < npes; p++)                                            \
                if ((T)value(p, i, 1) < want) want = (T)value(p, i, 1);               \
            CHECK(dst[i] == want);                                                    \
        }                                                                             \
        shmem_##NAME##_sum_to_all(dst, src, SEVERAL, 0, 0, npes, pWrk, pSync);       \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = 0;                                                               \
            for (int p = 0; p < npes; p++) want += (T)value(p, i, 1);                 \
            CHECK(dst[i] == want);                                                    \
        }                                                                             \
        for (int i = 0; i < SEVERAL; i++) src[i] = (T)(i % 2 ? 2 : 1);                \
        shmem_##NAME##_prod_to_all(src, src, SEVERAL, 0, 0, npes, pWrk, pSync);      \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = 1;                                                               \
            for (int p = 0; p < npes; p++) want *= (T)(i % 2 ? 2 : 1);                \
            CHECK(src[i] == want);                                                    \
        }                                                                             \
        shmem_free(src);                                                              \
        shmem_free(dst);                                                              \
    }

TO_ALL_FLOATING_CHECK(float, float)
TO_ALL_FLOATING_CHECK(double, double)
TO_ALL_FLOATING_CHECK(long double, longdouble)

/* The complex types' sum and product, on small Gaussian integers. */
#define TO_ALL_COMPLEX_CHECK(T, NAME, REAL)                                           \
    static void complex_##NAME(void)                                                  \
    {                                                                                 \
        T *src = shmem_malloc(SEVERAL * sizeof *src);                                 \
        T *dst = shmem_malloc(SEVERAL * sizeof *dst);                                 \
        CHECK_FATAL(src && dst);                                                      \
        for (int i = 0; i < SEVERAL; i++)                                             \
            src[i] = (REAL)(me + i) + (REAL)(i % 2) * (T)I;                           \
        shmem_##NAME##_sum_to_all(dst, src, SEVERAL, 0, 0, npes, pWrk, pSync);       \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = 0;                                                               \
            for (int p = 0; p < npes; p++) want += (REAL)(p + i) + (REAL)(i % 2) * (T)I; \
            CHECK(dst[i] == want);                                                    \
        }                                                                             \
        for (int i = 0; i < SEVERAL; i++) src[i] = (REAL)1 + (REAL)(i % 2) * (T)I;    \
        shmem_##NAME##_prod_to_all(dst, src, SEVERAL, 0, 0, npes, pWrk, pSync);      \
        for (int i = 0; i < SEVERAL; i++) {                                           \
            T want = 1;                                                               \
            for (int p = 0; p < npes; p++) want *= (REAL)1 + (REAL)(i % 2) * (T)I;    \
            CHECK(dst[i] == want);                                                    \
        }                                                                             \
        shmem_free(src);                                                              \
        shmem_free(dst);                                                              \
    }

TO_ALL_COMPLEX_CHECK(double _Complex, complexd, double)
TO_ALL_COMPLEX_CHECK(float _Complex, complexf, float)

/*
 * shmem_broadcast32 and shmem_broadcast64 from every root: the root's dest keeps
 * the value it had, every other PE's receives the root's source. The 32-bit
 * form at LARGE goes in pieces, so a root that is its node's lead serves every
 * piece from its source.
 */
static void broadcasts(void)
{
    int32_t *src32 = shmem_malloc(LARGE * sizeof *src32);
    int32_t *dst32 = shmem_malloc(LARGE * sizeof *dst32);
    int64_t *buffer = shmem_malloc(SEVERAL * sizeof *buffer);
    CHECK_FATAL(src32 && dst32 && buffer);
    size_t sizes[] = { 1, SEVERAL, LARGE };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        for (int root = 0; root < npes; root++) {
            size_t n = sizes[s];
            for (size_t i = 0; i < n; i++) {
                src32[i] = (int32_t)value(me, (int)i, 0);
                dst32[i] = -1 - me;
            }
            shmem_broadcast32(dst32, src32, n, root, 0, 0, npes, pSync);
            size_t wrong = n;
            for (size_t i = 0; i < n && wrong == n; i++)
                if (dst32[i] != (me == root ? -1 - me : (int32_t)value(root, (int)i, 0)))
                    wrong = i;
            if (wrong < n)
                fprintf(stderr, "PE %d: broadcast32 of %zu from %d, element %zu is %d\n", me,
                        n, root, wrong, dst32[wrong]);
            CHECK(wrong == n);
        }
    }
    /* In place, 64-bit: the root's buffer is its source and stays; the others'
     * are overwritten. */
    for (int root = 0; root < npes; root++) {
        for (int i = 0; i < SEVERAL; i++)
            buffer[i] = (int64_t)value(me, i, 1);
        shmem_broadcast64(buffer, buffer, SEVERAL, root, 0, 0, npes, pSync);
        for (int i = 0; i < SEVERAL; i++)
            CHECK(buffer[i] == (int64_t)value(root, i, 1));
    }
    shmem_free(src32);
    shmem_free(dst32);
    shmem_free(buffer);
}

/*
 * The active-set barrier and sync, each on its own. shmem_barrier completes the
 * PE's puts before it synchronizes - the quiet a sync does not have - so the
 * left neighbour's put is here when this PE's barrier returns; across nodes,
 * where the put and the barrier travel different paths, a barrier without the
 * quiet would be caught. The deprecated sync is checked alone, after the quiet
 * its caller owes it and with nothing after it that synchronizes, so a sync
 * that did nothing would be caught too. The C11 shmem_sync(team) that shares
 * its name, last.
 */
static void barriers(void)
{
    int *flag = shmem_malloc(sizeof *flag), *mark = shmem_malloc(sizeof *mark);
    CHECK_FATAL(flag && mark);
    *flag = *mark = -1;
    shmem_barrier_all();
    int right = (me + 1) % npes;
    for (int round = 0; round < 6; round++) {
        shmem_int_p(flag, round, right);
        shmem_barrier(0, 0, npes, pSync);
        CHECK(*flag == round);
        shmem_int_p(mark, round, right);
        shmem_quiet();
        shmem_sync(0, 0, npes, pSync);
        CHECK(*mark == round);
    }
    CHECK(shmem_sync(SHMEM_TEAM_WORLD) == 0);
    shmem_free(flag);
    shmem_free(mark);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    pSync = shmem_malloc(SHMEM_REDUCE_SYNC_SIZE * sizeof *pSync);
    pWrk = shmem_malloc(SHMEM_REDUCE_MIN_WRKDATA_SIZE * sizeof(long double _Complex));
    CHECK_FATAL(pSync && pWrk);
    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
        pSync[i] = static_pSync[i] = SHMEM_SYNC_VALUE;
    shmem_barrier_all();

    const char *partial = getenv("OSOE_TEST_PARTIAL");
    if (partial && partial[0] == '1') {
        long *x = shmem_malloc(sizeof *x);
        CHECK_FATAL(x);
        shmem_long_sum_to_all(x, x, 1, 0, 0, npes - 1, pWrk, pSync);
        printf("PE %d: a partial active set returned, which it must not\n", me);
        return 1;
    }

    integer_short();
    integer_int();
    integer_long();
    integer_longlong();
    floating_float();
    floating_double();
    floating_longdouble();
    complex_complexd();
    complex_complexf();
    broadcasts();
    barriers();

    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++) {
        CHECK(pSync[i] == SHMEM_SYNC_VALUE);
        CHECK(static_pSync[i] == SHMEM_SYNC_VALUE);
    }

    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
