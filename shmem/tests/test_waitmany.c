/*
 * The multiple-element waits and tests (spec §9.11.2-§9.11.14), every type, and
 * the two things about them that are easy to get wrong.
 *
 * **The empty set.** Six routines, three different answers, and they do not
 * follow from one another: `test_all` returns 1 where `test_some` returns 0 and
 * `test_any` returns SIZE_MAX, which is why 1.6 change #19 exists at all. A set
 * is empty when `nelems` is 0 *or* when every element is masked out, and both
 * are checked for every routine, with a condition nothing in the array meets -
 * so an implementation that fell through to the normal path would hang rather
 * than quietly return the wrong number.
 *
 * **The watched span.** §4.9 watches the span of the *unmasked* elements rather
 * than the whole array, which is what makes spec Example 44's idiom - mask each
 * element as it completes - stop waking the waiter for elements it has already
 * finished with. That is invisible from inside the waiting PE, so another PE
 * reads the waiter's published watch word out of the control segment and checks
 * it against the span it should be. This is the one place a test looks at the
 * §4.9 encoding directly, which is why it includes watch_range.h; everything
 * else goes through the segment reader by name.
 *
 * Signed comparison gets its own checks. Every value crosses the library's
 * internal boundary as a uint64_t, so `SHMEM_CMP_LT` against 0 is the case that
 * separates a signed type from a bit pattern.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"
#include "transport.h"   /* OSOE_REGION_HEAP, for the watch word below */
#include "watch_range.h"

static int me, npes;

/* ---- every type, generated from the same list the library is ----------- */

#define MANY_COVERAGE(T, NAME)                                                      \
    do {                                                                            \
        enum { N = 8 };                                                             \
        T *v = shmem_malloc(N * sizeof *v);                                         \
        CHECK(v != NULL);                                                           \
        size_t idx[N];                                                              \
        int st[N];                                                                  \
        T want[N];                                                                  \
        for (int i = 0; i < N; i++) {                                               \
            v[i] = (T)1;                                                            \
            st[i] = 0;                                                              \
        }                                                                           \
        shmem_barrier_all();                                                        \
                                                                                    \
        /* Everything satisfied: the waits return without blocking. */              \
        CHECK(shmem_##NAME##_test_all(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 1);        \
        CHECK(shmem_##NAME##_test_any(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 0);        \
        CHECK(shmem_##NAME##_test_some(v, N, idx, NULL, SHMEM_CMP_EQ, (T)1) == N);  \
        shmem_##NAME##_wait_until_all(v, N, NULL, SHMEM_CMP_EQ, (T)1);              \
        CHECK(shmem_##NAME##_wait_until_any(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 0);  \
        CHECK(shmem_##NAME##_wait_until_some(v, N, idx, NULL, SHMEM_CMP_EQ, (T)1)   \
              == N);                                                                \
        for (int i = 0; i < N; i++)                                                 \
            CHECK(idx[i] == (size_t)i);                                             \
                                                                                    \
        /* One element out of line. */                                              \
        v[3] = (T)0;                                                                \
        CHECK(shmem_##NAME##_test_all(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 0);        \
        CHECK(shmem_##NAME##_test_any(v, N, NULL, SHMEM_CMP_EQ, (T)0) == 3);        \
        CHECK(shmem_##NAME##_test_some(v, N, idx, NULL, SHMEM_CMP_EQ, (T)1)         \
              == N - 1);                                                            \
                                                                                    \
        /* Masking it out puts the set back in line, which is the whole point of    \
         * `status`: the excluded element is never even loaded. */                  \
        st[3] = 1;                                                                  \
        CHECK(shmem_##NAME##_test_all(v, N, st, SHMEM_CMP_EQ, (T)1) == 1);          \
        CHECK(shmem_##NAME##_test_any(v, N, st, SHMEM_CMP_EQ, (T)0) == SIZE_MAX);   \
        CHECK(shmem_##NAME##_test_some(v, N, idx, st, SHMEM_CMP_EQ, (T)1) == N - 1); \
        shmem_##NAME##_wait_until_all(v, N, st, SHMEM_CMP_EQ, (T)1);                \
        st[3] = 0;                                                                  \
        v[3] = (T)1;                                                                \
                                                                                    \
        /* The empty set, by nelems, with a condition nothing meets. */             \
        CHECK(shmem_##NAME##_test_all(v, 0, NULL, SHMEM_CMP_EQ, (T)99) == 1);       \
        CHECK(shmem_##NAME##_test_any(v, 0, NULL, SHMEM_CMP_EQ, (T)99) == SIZE_MAX); \
        CHECK(shmem_##NAME##_test_some(v, 0, idx, NULL, SHMEM_CMP_EQ, (T)99) == 0); \
        CHECK(shmem_##NAME##_test_all_vector(v, 0, NULL, SHMEM_CMP_EQ, want) == 1); \
        CHECK(shmem_##NAME##_test_any_vector(v, 0, NULL, SHMEM_CMP_EQ, want)        \
              == SIZE_MAX);                                                         \
        CHECK(shmem_##NAME##_test_some_vector(v, 0, idx, NULL, SHMEM_CMP_EQ, want)  \
              == 0);                                                                \
        shmem_##NAME##_wait_until_all(v, 0, NULL, SHMEM_CMP_EQ, (T)99);             \
        CHECK(shmem_##NAME##_wait_until_any(v, 0, NULL, SHMEM_CMP_EQ, (T)99)        \
              == SIZE_MAX);                                                         \
        CHECK(shmem_##NAME##_wait_until_some(v, 0, idx, NULL, SHMEM_CMP_EQ, (T)99)  \
              == 0);                                                                \
                                                                                    \
        /* The empty set, by the mask. Same answers; a different code path. */      \
        for (int i = 0; i < N; i++)                                                 \
            st[i] = 1;                                                              \
        CHECK(shmem_##NAME##_test_all(v, N, st, SHMEM_CMP_EQ, (T)99) == 1);         \
        CHECK(shmem_##NAME##_test_any(v, N, st, SHMEM_CMP_EQ, (T)99) == SIZE_MAX);  \
        CHECK(shmem_##NAME##_test_some(v, N, idx, st, SHMEM_CMP_EQ, (T)99) == 0);   \
        shmem_##NAME##_wait_until_all(v, N, st, SHMEM_CMP_EQ, (T)99);               \
        CHECK(shmem_##NAME##_wait_until_any(v, N, st, SHMEM_CMP_EQ, (T)99)          \
              == SIZE_MAX);                                                         \
        CHECK(shmem_##NAME##_wait_until_some(v, N, idx, st, SHMEM_CMP_EQ, (T)99)    \
              == 0);                                                                \
        for (int i = 0; i < N; i++)                                                 \
            st[i] = 0;                                                              \
                                                                                    \
        /* The vector forms: one comparison value per element. */                   \
        for (int i = 0; i < N; i++)                                                 \
            want[i] = v[i];                                                         \
        CHECK(shmem_##NAME##_test_all_vector(v, N, NULL, SHMEM_CMP_EQ, want) == 1); \
        want[5] = (T)77;                                                            \
        CHECK(shmem_##NAME##_test_all_vector(v, N, NULL, SHMEM_CMP_EQ, want) == 0); \
        CHECK(shmem_##NAME##_test_any_vector(v, N, NULL, SHMEM_CMP_NE, want) == 5); \
        CHECK(shmem_##NAME##_test_some_vector(v, N, idx, NULL, SHMEM_CMP_EQ, want)  \
              == N - 1);                                                            \
        want[5] = v[5];                                                             \
        shmem_##NAME##_wait_until_all_vector(v, N, NULL, SHMEM_CMP_EQ, want);       \
        CHECK(shmem_##NAME##_wait_until_any_vector(v, N, NULL, SHMEM_CMP_EQ, want)  \
              == 0);                                                                \
        CHECK(shmem_##NAME##_wait_until_some_vector(v, N, idx, NULL, SHMEM_CMP_EQ,  \
                                                    want) == N);                    \
                                                                                    \
        /* The C11 forms, on every type: a missing association is a compile         \
         * error here, which is the only check a selection list admits. */          \
        CHECK(shmem_test_all(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 1);                 \
        CHECK(shmem_test_any(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 0);                 \
        CHECK(shmem_test_some(v, N, idx, NULL, SHMEM_CMP_EQ, (T)1) == N);           \
        CHECK(shmem_test_all_vector(v, N, NULL, SHMEM_CMP_EQ, want) == 1);          \
        CHECK(shmem_test_any_vector(v, N, NULL, SHMEM_CMP_EQ, want) == 0);          \
        CHECK(shmem_test_some_vector(v, N, idx, NULL, SHMEM_CMP_EQ, want) == N);    \
        shmem_wait_until_all(v, N, NULL, SHMEM_CMP_EQ, (T)1);                       \
        CHECK(shmem_wait_until_any(v, N, NULL, SHMEM_CMP_EQ, (T)1) == 0);           \
        CHECK(shmem_wait_until_some(v, N, idx, NULL, SHMEM_CMP_EQ, (T)1) == N);     \
        shmem_wait_until_all_vector(v, N, NULL, SHMEM_CMP_EQ, want);                \
        CHECK(shmem_wait_until_any_vector(v, N, NULL, SHMEM_CMP_EQ, want) == 0);    \
        CHECK(shmem_wait_until_some_vector(v, N, idx, NULL, SHMEM_CMP_EQ, want)     \
              == N);                                                                \
                                                                                    \
        /* The scalar pair, and the C11 spellings of it. */                         \
        CHECK(shmem_##NAME##_test(v, SHMEM_CMP_EQ, (T)1) == 1);                     \
        CHECK(shmem_##NAME##_test(v, SHMEM_CMP_NE, (T)1) == 0);                     \
        shmem_##NAME##_wait_until(v, SHMEM_CMP_EQ, (T)1);                           \
        CHECK(shmem_test(v, SHMEM_CMP_EQ, (T)1) == 1);                              \
        shmem_wait_until(v, SHMEM_CMP_EQ, (T)1);                                    \
        shmem_free(v);                                                              \
    } while (0);

/* The deprecated two-byte types (Table 12) and the deprecated shmem_wait. */
#define SHORT_COVERAGE(T, NAME)                                                     \
    do {                                                                            \
        T *v = shmem_malloc(sizeof *v);                                             \
        CHECK(v != NULL);                                                           \
        *v = (T)5;                                                                  \
        shmem_barrier_all();                                                        \
        CHECK(shmem_##NAME##_test(v, SHMEM_CMP_EQ, (T)5) == 1);                     \
        CHECK(shmem_##NAME##_test(v, SHMEM_CMP_GT, (T)4) == 1);                     \
        CHECK(shmem_##NAME##_test(v, SHMEM_CMP_LT, (T)4) == 0);                     \
        shmem_##NAME##_wait_until(v, SHMEM_CMP_EQ, (T)5);                           \
        CHECK(shmem_test(v, SHMEM_CMP_EQ, (T)5) == 1);                              \
        shmem_wait_until(v, SHMEM_CMP_NE, (T)6);                                    \
        shmem_free(v);                                                              \
    } while (0);

static void signed_comparisons(void)
{
    int *si = shmem_malloc(sizeof *si);
    uint32_t *ui = shmem_malloc(sizeof *ui);
    long *sl = shmem_malloc(sizeof *sl);
    short *ss = shmem_malloc(sizeof *ss);
    CHECK(si && ui && sl && ss);
    *si = -1;
    *ui = 0xffffffffu;
    *sl = -1;
    *ss = (short)-1;
    shmem_barrier_all();

    /* The same bit pattern, two types, opposite answers. Everything crosses
     * the library's internal boundary as a uint64_t, so this is the check that
     * separates a signed type from a bit pattern. */
    CHECK(shmem_int_test(si, SHMEM_CMP_LT, 0) == 1);
    CHECK(shmem_uint32_test(ui, SHMEM_CMP_LT, 0) == 0);
    CHECK(shmem_int_test(si, SHMEM_CMP_GT, 0) == 0);
    CHECK(shmem_uint32_test(ui, SHMEM_CMP_GT, 0) == 1);
    CHECK(shmem_long_test(sl, SHMEM_CMP_LE, -1L) == 1);
    CHECK(shmem_short_test(ss, SHMEM_CMP_LT, (short)0) == 1);

    /* And at the boundary between the two halves of the range. */
    *si = -2;
    CHECK(shmem_int_test(si, SHMEM_CMP_LT, -1) == 1);
    CHECK(shmem_int_test(si, SHMEM_CMP_GE, -2) == 1);
    shmem_int_wait_until(si, SHMEM_CMP_LT, 0);

    /* The deprecated shmem_wait: wait_until with SHMEM_CMP_NE. */
    *sl = 7;
    shmem_long_wait(sl, 8L);
    shmem_wait(sl, 8L);
    (shmem_wait)(sl, 8L);               /* the unprefixed function, not the macro */

    shmem_free(ss);
    shmem_free(sl);
    shmem_free(ui);
    shmem_free(si);
}

/* ---- blocking, and the watched span ----------------------------------- */

enum { SPAN_N = 64, SPAN_FIRST = 40, SPAN_LAST = 47 };

/*
 * PE 1 parks in wait_until_some with only elements [SPAN_FIRST, SPAN_LAST]
 * unmasked; PE 0 reads the watch word it published and checks that the watched
 * range is that span and not the whole array. Then PE 0 releases it, one
 * element at a time, which is spec Example 44's idiom.
 */
static void watched_span(uint64_t *v)
{
    char segment[OSOE_SEG_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_seg s;
    if (osoe_seg_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK(0);
        return;
    }

    /* The array is symmetric, so its offset from the heap base is the same on
     * every PE; the base itself is in the segment (§3.14). */
    uint64_t base = 0;
    CHECK(osoe_seg_read_name(&s, "node.heap_base", 0, &base));
    uint64_t offset = (uint64_t)(uintptr_t)v - base;

    struct osoe_seg_blocked b = { 0 };
    int waited = 0;
    for (; waited < 2000; waited++) {
        if (osoe_seg_blocked(&s, 1, &b) && b.valid && b.blocked &&
            strstr(b.routine, "wait_until_some"))
            break;
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(waited < 2000);

    uint64_t published = 0;
    CHECK(osoe_seg_read_name(&s, "pe.watch_rng", 1, &published));
    uint64_t expect = osoe_watch_pack(OSOE_REGION_HEAP,
                                      offset + SPAN_FIRST * sizeof *v,
                                      (SPAN_LAST - SPAN_FIRST + 1) * sizeof *v);
    uint64_t whole = osoe_watch_pack(OSOE_REGION_HEAP, offset, SPAN_N * sizeof *v);
    CHECK(published == expect);
    CHECK(published != whole);          /* the refinement, not the array */
    osoe_seg_close(&s);

    /* Release it: an AMO on an element inside the span. */
    shmem_uint64_atomic_set(&v[SPAN_FIRST + 2], 1, 1);
}

static void blocking(uint64_t *v)
{
    int st[SPAN_N];
    size_t idx[SPAN_N];

    for (int i = 0; i < SPAN_N; i++) {
        v[i] = 0;
        st[i] = (i >= SPAN_FIRST && i <= SPAN_LAST) ? 0 : 1;
    }
    shmem_barrier_all();

    if (me == 1) {
        size_t n = shmem_uint64_wait_until_some(v, SPAN_N, idx, st, SHMEM_CMP_EQ, 1);
        CHECK(n >= 1);
        for (size_t i = 0; i < n; i++)
            CHECK(idx[i] >= SPAN_FIRST && idx[i] <= SPAN_LAST);
    } else if (me == 0) {
        watched_span(v);
    }
    shmem_barrier_all();

    /* A blocking wait_until_all released by another PE's AMOs, so the wake
     * path is exercised rather than an already-satisfied predicate. */
    for (int i = 0; i < SPAN_N; i++)
        v[i] = 0;
    shmem_barrier_all();
    if (me == 1) {
        shmem_uint64_wait_until_all(v, SPAN_N, NULL, SHMEM_CMP_EQ, 2);
        for (int i = 0; i < SPAN_N; i++)
            CHECK(v[i] == 2);
    } else if (me == 0) {
        for (int i = 0; i < SPAN_N; i++)
            shmem_uint64_atomic_set(&v[i], 2, 1);
    }
    shmem_barrier_all();
}

/*
 * The two-byte types across PEs. A short has no AMO, so the only way to update
 * one remotely is a put - for which notification is best effort by design
 * (§4.10) and a lost wake is charged to invariant.missed_wake. So the waiter
 * here spins on shmem_short_test, which touches no watch slot: the 2-byte
 * coherent load is what is under test, and the blocking form on a put-written
 * variable would make this test's own invariant assertion flaky by design.
 */
static void short_across_pes(void)
{
    short *v = shmem_malloc(sizeof *v);
    CHECK(v != NULL);
    *v = 0;
    shmem_barrier_all();
    int right = (me + 1) % npes;
    short payload = (short)-1234;
    shmem_putmem(v, &payload, sizeof payload, right);
    while (!shmem_short_test(v, SHMEM_CMP_EQ, (short)-1234))
        ;
    CHECK(*v == (short)-1234);
    shmem_barrier_all();
    shmem_free(v);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    OSOE_AMO_STANDARD_TYPES(MANY_COVERAGE)
    OSOE_WAIT_SHORT_TYPES(SHORT_COVERAGE)
    signed_comparisons();
    short_across_pes();

    uint64_t *v = shmem_malloc(SPAN_N * sizeof *v);
    CHECK(v != NULL);
    if (npes >= 2)
        blocking(v);
    shmem_free(v);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
