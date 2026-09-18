/*
 * Point-to-point synchronization: shmem_wait_until, shmem_test and their
 * multiple-element forms (spec §9.11; design §4.9).
 *
 * A wait is always on the calling PE's own variable, updated by an AMO from any
 * PE or thread (spec §9.11.1). The loop follows §4.9 exactly: publish the watch
 * before the first check, load the sequence word before the variables, and wait
 * on the sequence word with a safety-sweep deadline, so a lost wake costs
 * latency rather than the job.
 *
 * **One watch covers the span of the unmasked elements** (§4.9). The multiple-
 * element forms take a `status` mask and the wait must walk it anyway, so the
 * first and last unmasked element cost nothing to find. Spec Example 44 masks
 * each element as it completes, so across a series of calls the watched span
 * shrinks to the elements still outstanding and the waiter stops being woken by
 * ones it has already finished with. Nothing narrower is attempted: §4.9
 * records the futex bitmask and futex_waitv as options and rejects both, on the
 * evidence that these routines have no public users and that the idiom the spec
 * demonstrates is the non-blocking `shmem_test_some`, which touches no watch
 * slot at all.
 *
 * The test forms never touch the slot: they are loads and compares.
 *
 * Each routine's answer for an **empty set** is its own, and they do not agree
 * with each other - `test_all` returns 1 where `test_some` returns 0 and
 * `test_any` returns SIZE_MAX (1.6 change #19 exists because `test_all` was
 * unclear). They are written out one by one below rather than derived, because
 * there is no rule to derive them from.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <shmem.h>

#include "base/deadline.h"
#include "base/sweep.h"
#include "blocked.h"
#include "export.h"
#include "fatal.h"
#include "metrics.h"
#include "sym.h"
#include "transport.h"
#include "wait.h"

/* shmem.h makes these two names C11 generic macros; the deprecated functions of
 * the same name are defined below and must not be expanded (spec §9.11.1). */
#undef shmem_wait
#undef shmem_wait_until

/* ---- comparison ------------------------------------------------------- */

/* A width-byte value, as the 64-bit number its type says it is. */
static uint64_t extend(uint64_t v, unsigned width, bool sgn)
{
    switch (width) {
    case 8: return v;
    case 4: return sgn ? (uint64_t)(int64_t)(int32_t)(uint32_t)v : (uint64_t)(uint32_t)v;
    default: return sgn ? (uint64_t)(int64_t)(int16_t)(uint16_t)v : (uint64_t)(uint16_t)v;
    }
}

static bool satisfied(int cmp, uint64_t have, uint64_t want, bool sgn, const char *routine)
{
    if (sgn) {
        int64_t h = (int64_t)have, w = (int64_t)want;
        switch (cmp) {
        case SHMEM_CMP_EQ: return h == w;
        case SHMEM_CMP_NE: return h != w;
        case SHMEM_CMP_GT: return h > w;
        case SHMEM_CMP_GE: return h >= w;
        case SHMEM_CMP_LT: return h < w;
        case SHMEM_CMP_LE: return h <= w;
        }
    } else {
        switch (cmp) {
        case SHMEM_CMP_EQ: return have == want;
        case SHMEM_CMP_NE: return have != want;
        case SHMEM_CMP_GT: return have > want;
        case SHMEM_CMP_GE: return have >= want;
        case SHMEM_CMP_LT: return have < want;
        case SHMEM_CMP_LE: return have <= want;
        }
    }
    osoe_fatal("%s: comparison %d is not one of SHMEM_CMP_EQ, NE, GT, GE, LT, LE",
               routine, cmp);
}

/* ---- the scalar forms -------------------------------------------------- */

uint64_t osoe_wait_until(const char *routine, const void *ivar, unsigned width, bool sgn,
                         int cmp, uint64_t want)
{
    osoe_count(OSOE_C_SHMEM_WAIT_UNTIL_CALLS, 1);
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    want = extend(want, width, sgn);
    struct osoe_tp_watch *w = osoe_tp_watch(OSOE_REGION_HEAP, offset, width);
    uint64_t have, slept_since = 0;
    bool slept = false, woken = false, timed_out = false;

    for (;;) {
        uint32_t seq = osoe_tp_watch_seq(w);
        have = osoe_tp_load(OSOE_REGION_HEAP, offset, width);
        if (satisfied(cmp, extend(have, width, sgn), want, sgn, routine)) {
            /* The condition was already true when the sweep expired, so the
             * update landed while this PE slept and no wake arrived: a missed
             * wake (§4.9). Zero at quiescence for a conforming program, and
             * legitimately nonzero for an ivar written with a put, where notify
             * is best effort by design (§4.10). */
            if (timed_out)
                osoe_count(OSOE_C_INVARIANT_MISSED_WAKE, 1);
            break;
        }
        if (woken)
            osoe_count(OSOE_C_TP_WATCH_SPURIOUS, 1);

        if (!slept) {
            /* About to block. Everything on this path is paid only by a wait
             * that really sleeps (§3.14 cost rules). */
            slept = true;
            slept_since = osoe_now_ns();
            osoe_blocked_enter(routine, offset);
            osoe_count(OSOE_C_TP_WATCH_BLOCKED, 1);
        }

        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        woken = osoe_tp_watch_wait(w, seq, &sweep);
        timed_out = !woken;
        if (woken) {
            osoe_count(OSOE_C_TP_WATCH_WAKES, 1);
        } else {
            osoe_count(OSOE_C_TP_WATCH_SWEEPS, 1);
            osoe_sweep_expired(routine);        /* only here, never on a wake */
        }
    }

    osoe_tp_unwatch(w);
    if (slept) {
        osoe_count(OSOE_C_TP_WATCH_BLOCKED_NS, osoe_now_ns() - slept_since);
        osoe_blocked_leave();
    }
    return have;
}

int osoe_test(const char *routine, const void *ivar, unsigned width, bool sgn, int cmp,
              uint64_t want)
{
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    osoe_count(OSOE_C_SHMEM_TEST_CALLS, 1);
    uint64_t have = extend(osoe_tp_load(OSOE_REGION_HEAP, offset, width), width, sgn);
    return satisfied(cmp, have, extend(want, width, sgn), sgn, routine);
}

/* ---- the multiple-element forms ---------------------------------------- */

/* Element i of a *local* array of width-byte values, read as the number its
 * type says it is. Reading the object's own width and then extending keeps this
 * free of any assumption about byte order. */
static uint64_t local_element(const void *base, size_t i, unsigned width, bool sgn)
{
    const unsigned char *p = (const unsigned char *)base + i * width;
    uint64_t v;
    switch (width) {
    case 8: {
        uint64_t x;
        memcpy(&x, p, sizeof x);
        v = x;
        break;
    }
    case 4: {
        uint32_t x;
        memcpy(&x, p, sizeof x);
        v = x;
        break;
    }
    default: {
        uint16_t x;
        memcpy(&x, p, sizeof x);
        v = x;
        break;
    }
    }
    return extend(v, width, sgn);
}

static bool in_set(const struct osoe_waitmany *w, size_t i)
{
    return !w->status || w->status[i] == 0;
}

static uint64_t want_of(const struct osoe_waitmany *w, size_t i)
{
    return w->values ? local_element(w->values, i, w->width, w->sgn)
                     : extend(w->value, w->width, w->sgn);
}

/* The heap offset of ivars[0], with the array's bounds and alignment checked
 * once for the whole call. */
static uint64_t ivars_offset(const struct osoe_waitmany *w)
{
    if (w->nelems > UINT64_MAX / w->width)
        osoe_fatal("%s: %zu elements of %u bytes overflows", w->routine, w->nelems,
                   w->width);
    uint64_t offset = osoe_sym_offset(w->routine, w->ivars, (uint64_t)w->nelems * w->width);
    if (offset % w->width)
        osoe_fatal("%s: %p is not %u-byte aligned", w->routine, w->ivars, w->width);
    return offset;
}

/* The first and last unmasked element, or false when the set is empty. */
static bool unmasked_span(const struct osoe_waitmany *w, size_t *first, size_t *last)
{
    bool any = false;
    for (size_t i = 0; i < w->nelems; i++) {
        if (!in_set(w, i))
            continue;
        if (!any) {
            *first = i;
            any = true;
        }
        *last = i;
    }
    return any;
}

static bool element_satisfied(const struct osoe_waitmany *w, uint64_t base, size_t i)
{
    uint64_t have = osoe_tp_load(OSOE_REGION_HEAP, base + (uint64_t)i * w->width, w->width);
    return satisfied(w->cmp, extend(have, w->width, w->sgn), want_of(w, i), w->sgn,
                     w->routine);
}

enum many_mode { MANY_ALL, MANY_ANY, MANY_SOME };

/*
 * One pass over the set. ALL answers 1 or 0, ANY an index or SIZE_MAX, SOME the
 * number of satisfied elements with their indices written out. SOME tests every
 * element of the set at least once, which spec §9.11.4 requires and which ALL
 * and ANY are free to short-circuit.
 */
static size_t scan(const struct osoe_waitmany *w, uint64_t base, enum many_mode mode,
                   size_t *indices)
{
    size_t found = 0;
    for (size_t i = 0; i < w->nelems; i++) {
        if (!in_set(w, i))
            continue;
        bool ok = element_satisfied(w, base, i);
        switch (mode) {
        case MANY_ALL:
            if (!ok)
                return 0;
            break;
        case MANY_ANY:
            if (ok)
                return i;
            break;
        case MANY_SOME:
            if (ok)
                indices[found++] = i;
            break;
        }
    }
    switch (mode) {
    case MANY_ALL:  return 1;
    case MANY_ANY:  return SIZE_MAX;
    default:        return found;
    }
}

static bool scan_done(enum many_mode mode, size_t r)
{
    switch (mode) {
    case MANY_ALL:  return r != 0;
    case MANY_ANY:  return r != SIZE_MAX;
    default:        return r != 0;
    }
}

/* The empty-set answer, which differs per routine (§9.11, 1.6 change #19). */
static size_t empty_answer(enum many_mode mode)
{
    switch (mode) {
    case MANY_ALL:  return 1;
    case MANY_ANY:  return SIZE_MAX;
    default:        return 0;
    }
}

static size_t wait_many(const struct osoe_waitmany *w, enum many_mode mode, size_t *indices)
{
    osoe_count(OSOE_C_SHMEM_WAIT_UNTIL_CALLS, 1);
    size_t first = 0, last = 0;
    if (!unmasked_span(w, &first, &last))
        return empty_answer(mode);

    uint64_t base = ivars_offset(w);
    /* §4.9: one watch, over the span of the unmasked elements. */
    uint64_t span_offset = base + (uint64_t)first * w->width;
    uint64_t span_len = (uint64_t)(last - first + 1) * w->width;
    struct osoe_tp_watch *watch = osoe_tp_watch(OSOE_REGION_HEAP, span_offset,
                                                (size_t)span_len);
    uint64_t slept_since = 0;
    bool slept = false, woken = false, timed_out = false;
    size_t result;

    for (;;) {
        uint32_t seq = osoe_tp_watch_seq(watch);
        result = scan(w, base, mode, indices);
        if (scan_done(mode, result)) {
            if (timed_out)
                osoe_count(OSOE_C_INVARIANT_MISSED_WAKE, 1);
            break;
        }
        if (woken)
            osoe_count(OSOE_C_TP_WATCH_SPURIOUS, 1);

        if (!slept) {
            slept = true;
            slept_since = osoe_now_ns();
            osoe_blocked_enter(w->routine, span_offset);
            osoe_count(OSOE_C_TP_WATCH_BLOCKED, 1);
        }

        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        woken = osoe_tp_watch_wait(watch, seq, &sweep);
        timed_out = !woken;
        if (woken) {
            osoe_count(OSOE_C_TP_WATCH_WAKES, 1);
        } else {
            osoe_count(OSOE_C_TP_WATCH_SWEEPS, 1);
            osoe_sweep_expired(w->routine);
        }
    }

    osoe_tp_unwatch(watch);
    if (slept) {
        osoe_count(OSOE_C_TP_WATCH_BLOCKED_NS, osoe_now_ns() - slept_since);
        osoe_blocked_leave();
    }
    return result;
}

static size_t test_many(const struct osoe_waitmany *w, enum many_mode mode, size_t *indices)
{
    osoe_count(OSOE_C_SHMEM_TEST_CALLS, 1);
    size_t first = 0, last = 0;
    if (!unmasked_span(w, &first, &last))
        return empty_answer(mode);
    return scan(w, ivars_offset(w), mode, indices);
}

void   osoe_wait_all(const struct osoe_waitmany *w)  { wait_many(w, MANY_ALL, NULL); }
size_t osoe_wait_any(const struct osoe_waitmany *w)  { return wait_many(w, MANY_ANY, NULL); }
size_t osoe_wait_some(const struct osoe_waitmany *w, size_t *indices)
{
    return wait_many(w, MANY_SOME, indices);
}
int    osoe_test_all(const struct osoe_waitmany *w)  { return (int)test_many(w, MANY_ALL, NULL); }
size_t osoe_test_any(const struct osoe_waitmany *w)  { return test_many(w, MANY_ANY, NULL); }
size_t osoe_test_some(const struct osoe_waitmany *w, size_t *indices)
{
    return test_many(w, MANY_SOME, indices);
}

/* ---- the API surface, generated from the type lists -------------------- */

#define OSOE_WAIT_SET(T, NAME, IVARS, NELEMS, STATUS, CMP, VALUE, VALUES, ROUTINE)     \
    (struct osoe_waitmany){                                                            \
        .routine = ROUTINE, .ivars = (IVARS), .nelems = (NELEMS),                      \
        .width = (unsigned)sizeof(T), .sgn = ((T)-1 < (T)0),                           \
        .status = (STATUS), .cmp = (CMP), .value = (VALUE), .values = (VALUES) }

/* wait_until and test, for every type in Table 6 and for the deprecated
 * two-byte ones of Table 12. */
#define OSOE_WAIT_SCALAR(T, NAME)                                                      \
    OSOE_API void shmem_##NAME##_wait_until(T *ivar, int cmp, T cmp_value)              \
    {                                                                                   \
        osoe_wait_until("shmem_" #NAME "_wait_until", ivar, (unsigned)sizeof(T),        \
                        (T)-1 < (T)0, cmp, (uint64_t)cmp_value);                        \
    }                                                                                   \
    OSOE_API int shmem_##NAME##_test(T *ivar, int cmp, T cmp_value)                     \
    {                                                                                   \
        return osoe_test("shmem_" #NAME "_test", ivar, (unsigned)sizeof(T),             \
                         (T)-1 < (T)0, cmp, (uint64_t)cmp_value);                       \
    }

/* The multiple-element forms, Table 6 only. */
#define OSOE_WAIT_MANY(T, NAME)                                                        \
    OSOE_API void shmem_##NAME##_wait_until_all(T *ivars, size_t nelems,                \
                                                const int *status, int cmp, T cmp_value) \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_wait_until_all");       \
        osoe_wait_all(&w);                                                              \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_wait_until_any(T *ivars, size_t nelems,              \
                                                  const int *status, int cmp, T cmp_value) \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_wait_until_any");       \
        return osoe_wait_any(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_wait_until_some(T *ivars, size_t nelems,             \
                                                   size_t *indices, const int *status,  \
                                                   int cmp, T cmp_value)                \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_wait_until_some");      \
        return osoe_wait_some(&w, indices);                                             \
    }                                                                                   \
    OSOE_API void shmem_##NAME##_wait_until_all_vector(T *ivars, size_t nelems,         \
                                                       const int *status, int cmp,      \
                                                       const T *cmp_values)             \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_wait_until_all_vector"); \
        osoe_wait_all(&w);                                                              \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_wait_until_any_vector(T *ivars, size_t nelems,       \
                                                         const int *status, int cmp,    \
                                                         const T *cmp_values)           \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_wait_until_any_vector"); \
        return osoe_wait_any(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_wait_until_some_vector(T *ivars, size_t nelems,      \
                                                          size_t *indices,              \
                                                          const int *status, int cmp,   \
                                                          const T *cmp_values)          \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_wait_until_some_vector"); \
        return osoe_wait_some(&w, indices);                                             \
    }                                                                                   \
    OSOE_API int shmem_##NAME##_test_all(T *ivars, size_t nelems, const int *status,    \
                                         int cmp, T cmp_value)                          \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_test_all");             \
        return osoe_test_all(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_test_any(T *ivars, size_t nelems, const int *status, \
                                            int cmp, T cmp_value)                       \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_test_any");             \
        return osoe_test_any(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_test_some(T *ivars, size_t nelems, size_t *indices,  \
                                             const int *status, int cmp, T cmp_value)   \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp,     \
                                               (uint64_t)cmp_value, NULL,               \
                                               "shmem_" #NAME "_test_some");            \
        return osoe_test_some(&w, indices);                                             \
    }                                                                                   \
    OSOE_API int shmem_##NAME##_test_all_vector(T *ivars, size_t nelems,                \
                                                const int *status, int cmp,             \
                                                const T *cmp_values)                    \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_test_all_vector");      \
        return osoe_test_all(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_test_any_vector(T *ivars, size_t nelems,             \
                                                   const int *status, int cmp,          \
                                                   const T *cmp_values)                 \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_test_any_vector");      \
        return osoe_test_any(&w);                                                       \
    }                                                                                   \
    OSOE_API size_t shmem_##NAME##_test_some_vector(T *ivars, size_t nelems,            \
                                                    size_t *indices, const int *status, \
                                                    int cmp, const T *cmp_values)       \
    {                                                                                   \
        struct osoe_waitmany w = OSOE_WAIT_SET(T, NAME, ivars, nelems, status, cmp, 0,  \
                                               cmp_values,                              \
                                               "shmem_" #NAME "_test_some_vector");     \
        return osoe_test_some(&w, indices);                                             \
    }

/* The deprecated shmem_TYPENAME_wait, which is wait_until with SHMEM_CMP_NE
 * (spec §9.11.1 Notes). */
#define OSOE_WAIT_DEPRECATED(T, NAME)                                                  \
    OSOE_API void shmem_##NAME##_wait(T *ivar, T cmp_value)                             \
    {                                                                                   \
        osoe_wait_until("shmem_" #NAME "_wait", ivar, (unsigned)sizeof(T),              \
                        (T)-1 < (T)0, SHMEM_CMP_NE, (uint64_t)cmp_value);               \
    }

OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_SCALAR)
OSOE_WAIT_SHORT_TYPES(OSOE_WAIT_SCALAR)
OSOE_AMO_STANDARD_TYPES(OSOE_WAIT_MANY)
OSOE_WAIT_DEPRECATED_TYPES(OSOE_WAIT_DEPRECATED)

/* The unprefixed deprecated spellings, which the spec fixes at `long`. */
OSOE_API void shmem_wait(long *ivar, long cmp_value)
{
    osoe_wait_until("shmem_wait", ivar, (unsigned)sizeof(long), true, SHMEM_CMP_NE,
                    (uint64_t)cmp_value);
}

OSOE_API void shmem_wait_until(long *ivar, int cmp, long cmp_value)
{
    osoe_wait_until("shmem_wait_until", ivar, (unsigned)sizeof(long), true, cmp,
                    (uint64_t)cmp_value);
}
