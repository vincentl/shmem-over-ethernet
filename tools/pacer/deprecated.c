/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * deprecated - Annex F's routines, for coverage (proposal §4.10;
 * docs/prompts/task-stress-phase4.md item 5). Not run unless named, and on
 * the world alone. A round is one case:
 *
 *   to_all        a reduction of Table 11 - every operator on the four signed
 *                 integer types, the ordered and arithmetic ones on the three
 *                 floating types, sum and product on the two complex ones -
 *                 checked against a fold of the members' inputs in order
 *   broadcast, collect, fcollect, alltoall, alltoalls
 *                 of 32 and 64 bits, their words (source, target, index,
 *                 round)-determined; a broadcast's root keeps its dest, and
 *                 alltoalls leaves its dest's gaps
 *   barrier, sync each member adds one to a count at the set's first PE -
 *                 quieted before the sync, which completes nothing - and the
 *                 count is whole once the call returns
 *   the atomics   the pre-§9.7 names, by name and by the deprecated generics
 *                 in turn: set and fetch, swap, cswap, finc, inc, fadd, add,
 *                 each on the next PE's word; what is returned and what the
 *                 word holds after
 *   the waits     shmem_TYPE_wait, shmem_wait and the unprefixed
 *                 shmem_wait_until, and wait_until and test on short and
 *                 unsigned short, their words written by the previous PE - by
 *                 an atomic set where the type has one, and for the two-byte
 *                 types, which have none, by a put, as the contract they were
 *                 deprecated with allowed
 *   the cache     the six SHMEM_CACHE routines, which have nothing to check
 *
 * The collectives run on an active set: the whole job, the even PEs, the odd
 * ones, or the upper half, turning with the case and the pass. A PE outside
 * the set does not call, and its dest must be as it was. A world barrier
 * before and after each case keeps every pSync out of use between cases.
 *
 * Case r is r mod the 92 cases; even passes spell the atomics and the
 * two-byte waits by name, odd passes by the deprecated generics.
 */
#define _GNU_SOURCE
#include <complex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { CANARY = 0xa5, MOST_REDUCE = 8, ELEMENT_MOST = 32, SETS = 4 };
enum operation { AND, OR, XOR, MAX, MIN, SUM, PROD };

/* ---- the reductions -------------------------------------------------------------- */

struct to_all_case {
    const char *name;               /* "short_and" */
    enum operation op;
    int complex_valued;
    size_t size;
    void (*call)(void *dest, const void *source, int nreduce, int start, int log_stride,
                 int size, void *work, long *sync);
    void (*store)(void *array, size_t e, long long re, long long im);
    void (*load)(const void *array, size_t e, long long *re, long long *im);
};

#define TO_ALL_CALL(T, NAME, OP)                                                    \
    static void to_all_##NAME##_##OP(void *dest, const void *source, int nreduce,   \
                                     int start, int log_stride, int size, void *work, \
                                     long *sync)                                    \
    {                                                                               \
        shmem_##NAME##_##OP##_to_all((T *)dest, (const T *)source, nreduce, start,  \
                                     log_stride, size, (T *)work, sync);            \
    }
#define TO_ALL_REAL(T, NAME)                                                        \
    static void store_##NAME(void *array, size_t e, long long re, long long im)     \
    {                                                                               \
        (void)im;                                                                   \
        ((T *)array)[e] = (T)re;                                                    \
    }                                                                               \
    static void load_##NAME(const void *array, size_t e, long long *re, long long *im) \
    {                                                                               \
        *re = (long long)((const T *)array)[e];                                     \
        *im = 0;                                                                    \
    }
#define TO_ALL_COMPLEX(T, NAME)                                                     \
    static void store_##NAME(void *array, size_t e, long long re, long long im)     \
    {                                                                               \
        ((T *)array)[e] = (T)re + (T)im * (T)I;                                     \
    }                                                                               \
    static void load_##NAME(const void *array, size_t e, long long *re, long long *im) \
    {                                                                               \
        double _Complex v = (double _Complex)((const T *)array)[e];                 \
        *re = (long long)creal(v);                                                  \
        *im = (long long)cimag(v);                                                  \
    }

#define TO_ALL_INTEGER_TYPES(X)                                                     \
    X(short, short) X(int, int) X(long, long) X(long long, longlong)
#define TO_ALL_FLOATING_TYPES(X) X(float, float) X(double, double) X(long double, longdouble)
#define TO_ALL_COMPLEX_TYPES(X) X(double _Complex, complexd) X(float _Complex, complexf)

#define TO_ALL_INTEGER(T, NAME)                                                     \
    TO_ALL_REAL(T, NAME)                                                            \
    TO_ALL_CALL(T, NAME, and) TO_ALL_CALL(T, NAME, or) TO_ALL_CALL(T, NAME, xor)     \
    TO_ALL_CALL(T, NAME, max) TO_ALL_CALL(T, NAME, min) TO_ALL_CALL(T, NAME, sum)    \
    TO_ALL_CALL(T, NAME, prod)
#define TO_ALL_FLOATING(T, NAME)                                                    \
    TO_ALL_REAL(T, NAME)                                                            \
    TO_ALL_CALL(T, NAME, max) TO_ALL_CALL(T, NAME, min) TO_ALL_CALL(T, NAME, sum)    \
    TO_ALL_CALL(T, NAME, prod)
#define TO_ALL_COMPLEX_KIND(T, NAME)                                                \
    TO_ALL_COMPLEX(T, NAME) TO_ALL_CALL(T, NAME, sum) TO_ALL_CALL(T, NAME, prod)

TO_ALL_INTEGER_TYPES(TO_ALL_INTEGER)
TO_ALL_FLOATING_TYPES(TO_ALL_FLOATING)
TO_ALL_COMPLEX_TYPES(TO_ALL_COMPLEX_KIND)

#define TO_ALL_ENTRY(T, NAME, OP, CODE, COMPLEX)                                    \
    { #NAME "_" #OP, CODE, COMPLEX, sizeof(T), to_all_##NAME##_##OP, store_##NAME,  \
      load_##NAME },
#define TO_ALL_INTEGER_ENTRIES(T, NAME)                                             \
    TO_ALL_ENTRY(T, NAME, and, AND, 0) TO_ALL_ENTRY(T, NAME, or, OR, 0)              \
    TO_ALL_ENTRY(T, NAME, xor, XOR, 0) TO_ALL_ENTRY(T, NAME, max, MAX, 0)            \
    TO_ALL_ENTRY(T, NAME, min, MIN, 0) TO_ALL_ENTRY(T, NAME, sum, SUM, 0)            \
    TO_ALL_ENTRY(T, NAME, prod, PROD, 0)
#define TO_ALL_FLOATING_ENTRIES(T, NAME)                                            \
    TO_ALL_ENTRY(T, NAME, max, MAX, 0) TO_ALL_ENTRY(T, NAME, min, MIN, 0)            \
    TO_ALL_ENTRY(T, NAME, sum, SUM, 0) TO_ALL_ENTRY(T, NAME, prod, PROD, 0)
#define TO_ALL_COMPLEX_ENTRIES(T, NAME)                                             \
    TO_ALL_ENTRY(T, NAME, sum, SUM, 1) TO_ALL_ENTRY(T, NAME, prod, PROD, 1)

static const struct to_all_case to_all_cases[] = {
    TO_ALL_INTEGER_TYPES(TO_ALL_INTEGER_ENTRIES) TO_ALL_FLOATING_TYPES(TO_ALL_FLOATING_ENTRIES)
    TO_ALL_COMPLEX_TYPES(TO_ALL_COMPLEX_ENTRIES)
};
enum { TO_ALL_CASES = sizeof to_all_cases / sizeof *to_all_cases };

/* ---- the atomics ------------------------------------------------------------------ */

enum amo_op { SET_FETCH, SWAP, CSWAP, FINC, INC, FADD, ADD };
static const char *const amo_names[] = { "set_fetch", "swap", "cswap", "finc", "inc", "fadd",
                                         "add" };

struct amo_case {
    const char *type;
    enum amo_op op;
    /* the case's own operation on `dest` at `pe`: what it returned, or 0 */
    uint64_t (*apply)(void *dest, uint64_t value, uint64_t cond, int pe, int generic);
    uint64_t (*fetch)(const void *source, int pe, int generic);     /* SET_FETCH's second half */
    void (*start)(void *word, uint64_t value, int pe);              /* the modern atomic set */
    uint64_t (*load)(const void *word);
};

#define AMO_COMMON(T, NAME)                                                         \
    static void start_##NAME(void *word, uint64_t value, int pe)                    \
    {                                                                               \
        shmem_##NAME##_atomic_set((T *)word, (T)value, pe);                         \
    }                                                                               \
    static uint64_t load_amo_##NAME(const void *word)                               \
    {                                                                               \
        return (uint64_t)*(const T *)word;                                          \
    }                                                                               \
    static uint64_t set_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)cond;                                                                 \
        if (generic)                                                                \
            shmem_set((T *)dest, (T)value, pe);                                     \
        else                                                                        \
            shmem_##NAME##_set((T *)dest, (T)value, pe);                            \
        return 0;                                                                   \
    }                                                                               \
    static uint64_t fetch_##NAME(const void *source, int pe, int generic)           \
    {                                                                               \
        return (uint64_t)(generic ? shmem_fetch((const T *)source, pe)              \
                                  : shmem_##NAME##_fetch((const T *)source, pe));   \
    }                                                                               \
    static uint64_t swap_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)cond;                                                                 \
        return (uint64_t)(generic ? shmem_swap((T *)dest, (T)value, pe)             \
                                  : shmem_##NAME##_swap((T *)dest, (T)value, pe));  \
    }
#define AMO_INTEGER(T, NAME)                                                        \
    static uint64_t cswap_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        return (uint64_t)(generic ? shmem_cswap((T *)dest, (T)cond, (T)value, pe)   \
                                  : shmem_##NAME##_cswap((T *)dest, (T)cond, (T)value, pe)); \
    }                                                                               \
    static uint64_t finc_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)value;                                                                \
        (void)cond;                                                                 \
        return (uint64_t)(generic ? shmem_finc((T *)dest, pe)                       \
                                  : shmem_##NAME##_finc((T *)dest, pe));            \
    }                                                                               \
    static uint64_t inc_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)value;                                                                \
        (void)cond;                                                                 \
        if (generic)                                                                \
            shmem_inc((T *)dest, pe);                                               \
        else                                                                        \
            shmem_##NAME##_inc((T *)dest, pe);                                      \
        return 0;                                                                   \
    }                                                                               \
    static uint64_t fadd_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)cond;                                                                 \
        return (uint64_t)(generic ? shmem_fadd((T *)dest, (T)value, pe)             \
                                  : shmem_##NAME##_fadd((T *)dest, (T)value, pe));  \
    }                                                                               \
    static uint64_t add_##NAME(void *dest, uint64_t value, uint64_t cond, int pe, int generic) \
    {                                                                               \
        (void)cond;                                                                 \
        if (generic)                                                                \
            shmem_add((T *)dest, (T)value, pe);                                     \
        else                                                                        \
            shmem_##NAME##_add((T *)dest, (T)value, pe);                            \
        return 0;                                                                   \
    }

#define AMO_EXTENDED_TYPES(X)                                                       \
    X(int, int) X(long, long) X(long long, longlong) X(float, float) X(double, double)
#define AMO_INTEGER_TYPES(X) X(int, int) X(long, long) X(long long, longlong)

AMO_EXTENDED_TYPES(AMO_COMMON)
AMO_INTEGER_TYPES(AMO_INTEGER)

#define AMO_EXTENDED_ENTRIES(T, NAME)                                               \
    { #NAME, SET_FETCH, set_##NAME, fetch_##NAME, start_##NAME, load_amo_##NAME },   \
    { #NAME, SWAP, swap_##NAME, NULL, start_##NAME, load_amo_##NAME },
#define AMO_INTEGER_ENTRIES(T, NAME)                                                \
    { #NAME, CSWAP, cswap_##NAME, NULL, start_##NAME, load_amo_##NAME },             \
    { #NAME, FINC, finc_##NAME, NULL, start_##NAME, load_amo_##NAME },               \
    { #NAME, INC, inc_##NAME, NULL, start_##NAME, load_amo_##NAME },                 \
    { #NAME, FADD, fadd_##NAME, NULL, start_##NAME, load_amo_##NAME },               \
    { #NAME, ADD, add_##NAME, NULL, start_##NAME, load_amo_##NAME },

static const struct amo_case amo_cases[] = {
    AMO_EXTENDED_TYPES(AMO_EXTENDED_ENTRIES) AMO_INTEGER_TYPES(AMO_INTEGER_ENTRIES)
};
enum { AMO_CASES = sizeof amo_cases / sizeof *amo_cases };

/* ---- the waits -------------------------------------------------------------------- */

enum wait_case {
    WAIT_SHORT, WAIT_INT, WAIT_LONG, WAIT_LONGLONG, WAIT_PLAIN, WAIT_UNTIL_PLAIN,
    WAIT_UNTIL_SHORT, WAIT_UNTIL_USHORT, TEST_SHORT, TEST_USHORT, WAIT_CASES
};
static const char *const wait_names[WAIT_CASES] = {
    "shmem_short_wait", "shmem_int_wait", "shmem_long_wait", "shmem_longlong_wait",
    "shmem_wait", "shmem_wait_until (long)", "short wait_until", "ushort wait_until",
    "short test", "ushort test"
};

/* ---- the cases -------------------------------------------------------------------- */

enum family { TO_ALL, BROADCAST, COLLECT, FCOLLECT, ALLTOALL, ALLTOALLS, BARRIER, SYNC, AMO,
              WAIT, CACHE };
static const char *const family_names[] = { "to_all", "broadcast", "collect", "fcollect",
                                            "alltoall", "alltoalls", "barrier", "sync",
                                            "amo", "wait", "cache" };
/* The families' first cases: to_all's 44, five data families of two widths,
 * barrier and sync, the atomics, the waits, the cache. */
enum {
    DATA_FIRST = TO_ALL_CASES, BARRIER_FIRST = DATA_FIRST + 10, AMO_FIRST = BARRIER_FIRST + 2,
    WAIT_FIRST = AMO_FIRST + AMO_CASES, CACHE_FIRST = WAIT_FIRST + WAIT_CASES,
    CASES = CACHE_FIRST + 1
};

static const char *const phase_names[] = { "call", "check", NULL };

static struct {
    uint64_t seed;
    int n, me;
    unsigned char *dest, *source;   /* symmetric, [bytes] */
    size_t bytes;
    unsigned char *work;            /* symmetric: pWrk */
    long *sync;                     /* symmetric: pSync */
    long *arrivals;                 /* symmetric: barrier and sync's count, at a set's first */
    unsigned char *word;            /* symmetric, 16 bytes: the atomics' and waits' word */
    unsigned char *expected;        /* [bytes] */
    long round;
    int plant_reduce, plant_data, plant_outside, plant_barrier, plant_amo, plant_wait,
        plant_word;
    double cases, phase[2];
} old;

/* An active set: (start, log stride, size), and this PE's place in it. */
struct active_set {
    int start, log_stride, size;
    int rank;                       /* -1 outside */
};

static struct active_set set_of(int which)
{
    int n = old.n;
    struct active_set s = { 0, 0, n, -1 };
    if (which == 1 && n >= 2) {
        s.log_stride = 1;
        s.size = (n + 1) / 2;
    } else if (which == 2 && n >= 2) {
        s.start = 1;
        s.log_stride = 1;
        s.size = n / 2;
    } else if (which == 3 && n >= 2) {
        s.start = n / 2;
        s.size = n - n / 2;
    }
    int step = 1 << s.log_stride, offset = old.me - s.start;
    if (offset >= 0 && offset % step == 0 && offset / step < s.size)
        s.rank = offset / step;
    return s;
}

static int member_pe(const struct active_set *s, int rank)
{
    return s->start + (rank << s->log_stride);
}

/* A 32- or 64-bit word of a data case: from, to, index and round. */
static uint64_t word_of(int from, int to, size_t index, long round, int bits)
{
    uint64_t v = random_mix(old.seed ^ random_name("deprecated"), (uint64_t)from,
                            ((uint64_t)(uint32_t)to << 32) | index, (uint64_t)round);
    return bits == 32 ? (uint32_t)v : v;
}

static void put_word(unsigned char *at, size_t index, uint64_t v, int bits)
{
    if (bits == 32) {
        uint32_t w = (uint32_t)v;
        memcpy(at + index * 4, &w, 4);
    } else {
        memcpy(at + index * 8, &v, 8);
    }
}

static uint64_t memory_needed(int npes)
{
    /* The largest dest: alltoalls' two elements to each PE, three apart, of
     * eight bytes; the rest need less. */
    return (uint64_t)npes * 64 + 512;
}

static uint64_t deprecated_memory(int npes, const struct pacer_options *options, char *fields,
                                  size_t size)
{
    old.seed = options->seed;
    old.plant_reduce = options->plant == PACER_PLANT_DEPRECATED_REDUCE;
    old.plant_data = options->plant == PACER_PLANT_DEPRECATED_DATA;
    old.plant_outside = options->plant == PACER_PLANT_DEPRECATED_OUTSIDE;
    old.plant_barrier = options->plant == PACER_PLANT_DEPRECATED_BARRIER;
    old.plant_amo = options->plant == PACER_PLANT_DEPRECATED_AMO;
    old.plant_wait = options->plant == PACER_PLANT_DEPRECATED_WAIT;
    old.plant_word = options->plant == PACER_PLANT_DEPRECATED_WORD;
    snprintf(fields, size, "cases=%d to_all=%d amo=%d waits=%d sets=whole,even,odd,upper",
             CASES, TO_ALL_CASES, AMO_CASES, WAIT_CASES);
    uint64_t bytes = memory_needed(npes);
    return 3 * bytes + (uint64_t)SHMEM_REDUCE_MIN_WRKDATA_SIZE * ELEMENT_MOST +
           sizeof(long) * (SHMEM_SYNC_SIZE + 1) + 16;
}

static int deprecated_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    if (team != SHMEM_TEAM_WORLD)
        return -1;
    old.n = shmem_n_pes();
    old.me = shmem_my_pe();
    old.bytes = (size_t)memory_needed(old.n);
    old.dest = shmem_malloc(old.bytes);
    old.source = shmem_malloc(old.bytes);
    old.work = shmem_malloc((size_t)SHMEM_REDUCE_MIN_WRKDATA_SIZE * ELEMENT_MOST);
    old.sync = shmem_malloc(sizeof *old.sync * SHMEM_SYNC_SIZE);
    old.arrivals = shmem_calloc(1, sizeof *old.arrivals);
    old.word = shmem_calloc(16, 1);
    old.expected = malloc(old.bytes);
    if (!old.dest || !old.source || !old.work || !old.sync || !old.arrivals || !old.word ||
        !old.expected)
        return -1;
    for (int k = 0; k < SHMEM_SYNC_SIZE; k++)
        old.sync[k] = SHMEM_SYNC_VALUE;
    old.round = 0;
    shmem_barrier_all();
    return 0;
}

/* ---- to_all ----------------------------------------------------------------------- */

/* Member r's input for element e: small whole numbers, every result exact. */
static void input_of(enum operation op, int r, int size, size_t e, long round, long long *re,
                     long long *im)
{
    *im = 0;
    switch (op) {
    case AND: *re = 0xff & ~(1LL << ((r + (long)e + round) % 8)); break;
    case OR: *re = 1LL << ((r + (long)e + round) % 8); break;
    case XOR: *re = (r * 5 + (long)e + round) % 16; break;
    case MAX:
    case MIN: *re = (r * 7 + (long)e + round) % 13 - 6; break;
    case SUM:
        *re = (r + (long)e + round) % 5 + 1;
        *im = (r + 2 * (long)e) % 3;
        break;
    default:                        /* PROD: ones, and one member's 2 - or 1 + i */
        *re = r == (int)(((long)e + round) % size) ? 2 : 1;
        *im = 0;
        break;
    }
}

static void fold(enum operation op, int complex_valued, long long *re, long long *im,
                 long long re2, long long im2)
{
    switch (op) {
    case AND: *re &= re2; break;
    case OR: *re |= re2; break;
    case XOR: *re ^= re2; break;
    case MAX: *re = re2 > *re ? re2 : *re; break;
    case MIN: *re = re2 < *re ? re2 : *re; break;
    case SUM: *re += re2; *im += im2; break;
    default:
        if (complex_valued) {
            long long r = *re * re2 - *im * im2, i = *re * im2 + *im * re2;
            *re = r;
            *im = i;
        } else {
            *re *= re2;
        }
        break;
    }
}

static void run_to_all(long round, const struct to_all_case *c, const struct active_set *s)
{
    int nreduce = 1 + (int)(round % MOST_REDUCE);
    if (s->rank >= 0) {
        for (int e = 0; e < nreduce; e++) {
            long long re, im;
            input_of(c->op, s->rank, s->size, (size_t)e, round, &re, &im);
            if (c->complex_valued && c->op == PROD)
                im = re == 2 ? 1 : 0, re = 1;
            if (!c->complex_valued)
                im = 0;
            if (old.plant_reduce && old.me == 1 && c->op == SUM && e == 0) {
                re++;               /* the planted defect: an input one too many */
                old.plant_reduce = 0;
            }
            c->store(old.source, (size_t)e, re, im);
        }
        c->call(old.dest, old.source, nreduce, s->start, s->log_stride, s->size, old.work,
                old.sync);
    }
}

static void check_to_all(long round, const struct to_all_case *c, const struct active_set *s)
{
    if (s->rank < 0)
        return;
    int nreduce = 1 + (int)(round % MOST_REDUCE);
    for (int e = 0; e < nreduce; e++) {
        long long want_re = 0, want_im = 0;
        for (int r = 0; r < s->size; r++) {
            long long re, im;
            input_of(c->op, r, s->size, (size_t)e, round, &re, &im);
            if (c->complex_valued && c->op == PROD)
                im = re == 2 ? 1 : 0, re = 1;
            if (!c->complex_valued)
                im = 0;
            if (r == 0) {
                want_re = re;
                want_im = im;
            } else {
                fold(c->op, c->complex_valued, &want_re, &want_im, re, im);
            }
        }
        long long have_re, have_im;
        c->load(old.dest, (size_t)e, &have_re, &have_im);
        if (have_re != want_re || have_im != want_im) {
            pacer_failure("deprecated round=%ld %s_to_all set=%d:%d:%d: element %d is %lld%+lldi, "
                          "expected %lld%+lldi", round, c->name, s->start, s->log_stride,
                          s->size, e, have_re, have_im, want_re, want_im);
            return;
        }
    }
}

/* ---- the data collectives ---------------------------------------------------------- */

struct data_shape {
    size_t nelems;
    ptrdiff_t dst, sst;
    int root;                       /* broadcast's, a rank of the set */
};

static struct data_shape shape_of(enum family f, long round, const struct active_set *s)
{
    struct data_shape d = { 1, 1, 1, 0 };
    switch (f) {
    case BROADCAST: d.nelems = 1 + (size_t)(round % 16); d.root = (int)(round % s->size); break;
    case COLLECT: d.nelems = 3; break;          /* the most; a member's own: 1 + (r + round) % 3 */
    case FCOLLECT: d.nelems = 1 + (size_t)(round % 4); break;
    case ALLTOALL: d.nelems = 1 + (size_t)(round % 3); break;
    default:                                    /* ALLTOALLS */
        d.nelems = 1 + (size_t)(round % 2);
        d.dst = 1 + round % 3;
        d.sst = 1 + (round / 3) % 3;
        break;
    }
    return d;
}

static size_t collect_count(int r, long round)
{
    return 1 + (size_t)((r + round) % 3);
}

static void run_data(long round, enum family f, int bits, const struct active_set *s)
{
    struct data_shape d = shape_of(f, round, s);
    if (s->rank < 0)
        return;
    int me = old.me;
    switch (f) {
    case BROADCAST:
        for (size_t k = 0; k < d.nelems; k++)
            put_word(old.source, k, word_of(member_pe(s, d.root), 0, k, round, bits), bits);
        break;
    case COLLECT:
        for (size_t k = 0; k < collect_count(s->rank, round); k++)
            put_word(old.source, k, word_of(me, 0, k, round, bits), bits);
        break;
    case FCOLLECT:
        for (size_t k = 0; k < d.nelems; k++)
            put_word(old.source, k, word_of(me, 0, k, round, bits), bits);
        break;
    case ALLTOALL:
        for (int l = 0; l < s->size; l++)
            for (size_t k = 0; k < d.nelems; k++)
                put_word(old.source, (size_t)l * d.nelems + k,
                         word_of(me, member_pe(s, l), k, round, bits), bits);
        break;
    default:
        for (int l = 0; l < s->size; l++)
            for (size_t k = 0; k < d.nelems; k++)
                put_word(old.source, (size_t)d.sst * ((size_t)l * d.nelems + k),
                         word_of(me, member_pe(s, l), k, round, bits), bits);
        break;
    }
    if (old.plant_data && old.me == 1 && (f != BROADCAST || s->rank == d.root)) {
        old.source[0] ^= 1;         /* the planted defect: a byte offered wrong */
        old.plant_data = 0;
    }
    int start = s->start, log = s->log_stride, size = s->size;
    if (bits == 32) {
        switch (f) {
        case BROADCAST: shmem_broadcast32(old.dest, old.source, d.nelems, d.root, start, log, size, old.sync); break;
        case COLLECT: shmem_collect32(old.dest, old.source, collect_count(s->rank, round), start, log, size, old.sync); break;
        case FCOLLECT: shmem_fcollect32(old.dest, old.source, d.nelems, start, log, size, old.sync); break;
        case ALLTOALL: shmem_alltoall32(old.dest, old.source, d.nelems, start, log, size, old.sync); break;
        default: shmem_alltoalls32(old.dest, old.source, d.dst, d.sst, d.nelems, start, log, size, old.sync); break;
        }
    } else {
        switch (f) {
        case BROADCAST: shmem_broadcast64(old.dest, old.source, d.nelems, d.root, start, log, size, old.sync); break;
        case COLLECT: shmem_collect64(old.dest, old.source, collect_count(s->rank, round), start, log, size, old.sync); break;
        case FCOLLECT: shmem_fcollect64(old.dest, old.source, d.nelems, start, log, size, old.sync); break;
        case ALLTOALL: shmem_alltoall64(old.dest, old.source, d.nelems, start, log, size, old.sync); break;
        default: shmem_alltoalls64(old.dest, old.source, d.dst, d.sst, d.nelems, start, log, size, old.sync); break;
        }
    }
}

/* What this member's dest must hold, into `expected`, the canary where
 * nothing lands; the bytes to compare. */
static size_t expect_data(long round, enum family f, int bits, const struct active_set *s)
{
    struct data_shape d = shape_of(f, round, s);
    size_t w = (size_t)bits / 8, end = 0;
    memset(old.expected, CANARY, old.bytes);
    switch (f) {
    case BROADCAST:
        if (s->rank == d.root)
            return d.nelems * w;    /* the root's dest is left as it was: the canary */
        for (size_t k = 0; k < d.nelems; k++)
            put_word(old.expected, k, word_of(member_pe(s, d.root), 0, k, round, bits), bits);
        return d.nelems * w;
    case COLLECT:
        for (int r = 0; r < s->size; r++)
            for (size_t k = 0; k < collect_count(r, round); k++)
                put_word(old.expected, end++, word_of(member_pe(s, r), 0, k, round, bits), bits);
        return end * w;
    case FCOLLECT:
        for (int r = 0; r < s->size; r++)
            for (size_t k = 0; k < d.nelems; k++)
                put_word(old.expected, (size_t)r * d.nelems + k,
                         word_of(member_pe(s, r), 0, k, round, bits), bits);
        return (size_t)s->size * d.nelems * w;
    case ALLTOALL:
        for (int r = 0; r < s->size; r++)
            for (size_t k = 0; k < d.nelems; k++)
                put_word(old.expected, (size_t)r * d.nelems + k,
                         word_of(member_pe(s, r), old.me, k, round, bits), bits);
        return (size_t)s->size * d.nelems * w;
    default:
        for (int r = 0; r < s->size; r++)
            for (size_t k = 0; k < d.nelems; k++)
                put_word(old.expected, (size_t)d.dst * ((size_t)r * d.nelems + k),
                         word_of(member_pe(s, r), old.me, k, round, bits), bits);
        return (size_t)d.dst * (size_t)s->size * d.nelems * w;
    }
}

static void check_data(long round, enum family f, int bits, const struct active_set *s)
{
    size_t bytes;
    if (s->rank < 0) {
        /* Outside the set: the dest as it was, every byte. */
        memset(old.expected, CANARY, old.bytes);
        bytes = old.bytes;
        if (old.plant_outside && old.me == 1) {
            old.dest[0] ^= 1;       /* the planted defect: a byte where no call wrote */
            old.plant_outside = 0;
        }
    } else {
        bytes = expect_data(round, f, bits, s);
    }
    for (size_t b = 0; b < bytes; b++)
        if (old.dest[b] != old.expected[b]) {
            pacer_failure("deprecated round=%ld %s%d set=%d:%d:%d: byte %zu is %02x, not %02x%s",
                          round, family_names[f], bits, s->start, s->log_stride, s->size, b,
                          old.dest[b], old.expected[b],
                          s->rank < 0 ? ", on a PE outside the set" : "");
            return;
        }
}

/* ---- barrier and sync -------------------------------------------------------------- */

static void run_barrier(long round, enum family f, const struct active_set *s)
{
    (void)round;
    if (s->rank < 0)
        return;
    int first = member_pe(s, 0);
    if (old.plant_barrier && old.me == 1)
        old.plant_barrier = 0;      /* the planted defect: an arrival not counted */
    else
        shmem_long_atomic_inc(old.arrivals, first);
    if (f == BARRIER) {
        shmem_barrier(s->start, s->log_stride, s->size, old.sync);
    } else {
        shmem_quiet();              /* a sync completes nothing */
        shmem_sync(s->start, s->log_stride, s->size, old.sync);
    }
    if (s->rank == 0) {
        long have = shmem_long_atomic_fetch(old.arrivals, old.me);
        if (have != s->size)
            pacer_failure("deprecated round=%ld %s set=%d:%d:%d: the first member counted %ld "
                          "arrivals when it returned, expected %d", round, family_names[f],
                          s->start, s->log_stride, s->size, have, s->size);
    }
}

/* ---- the atomics ------------------------------------------------------------------ */

static uint64_t amo_value(int pe, long round)
{
    return 1 + (uint64_t)((pe * 7 + round) % 50);
}

enum { AMO_START = 3 };

static void run_amo(long round, const struct amo_case *c, int generic)
{
    int next = (old.me + 1) % old.n, previous = (old.me + old.n - 1) % old.n;
    uint64_t mine = amo_value(old.me, round), theirs = amo_value(previous, round);
    uint64_t sent = mine;
    if (old.plant_word && old.me == 1 && c->op != FINC && c->op != INC && c->op != SET_FETCH) {
        sent++;                     /* the planted defect: one value sent wrong */
        old.plant_word = 0;
    }
    uint64_t returned = c->apply(old.word, sent, AMO_START, next, generic);
    uint64_t want_returned = AMO_START;
    int returns = c->op == SWAP || c->op == CSWAP || c->op == FINC || c->op == FADD;
    if (c->op == SET_FETCH) {
        shmem_quiet();
        shmem_barrier_all();        /* every set landed before any fetch */
        returned = c->fetch(old.word, next, generic);
        want_returned = mine;
        returns = 1;
    }
    shmem_quiet();
    shmem_barrier_all();            /* every operation landed */
    if (old.plant_amo && old.me == 1 && returns) {
        returned++;                 /* the planted defect: a return misread once */
        old.plant_amo = 0;
    }
    if (returns && returned != want_returned)
        pacer_failure("deprecated round=%ld %s_%s: returned %llu, expected %llu", round, c->type,
                      amo_names[c->op], (unsigned long long)returned,
                      (unsigned long long)want_returned);
    uint64_t want = c->op == FINC || c->op == INC      ? AMO_START + 1
                    : c->op == FADD || c->op == ADD    ? AMO_START + theirs
                                                       : theirs;
    uint64_t have = c->load(old.word);
    if (have != want)
        pacer_failure("deprecated round=%ld %s_%s: the word holds %llu after PE %d's, expected "
                      "%llu", round, c->type, amo_names[c->op], (unsigned long long)have,
                      previous, (unsigned long long)want);
}

/* ---- the waits -------------------------------------------------------------------- */

static void run_wait(long round, enum wait_case w, int generic)
{
    int next = (old.me + 1) % old.n, previous = (old.me + old.n - 1) % old.n;
    uint64_t value = 1 + (uint64_t)((old.me * 3 + round) % 100);
    if (old.plant_wait && old.me == 1) {
        value++;                    /* the planted defect: a word written wrong, once */
        old.plant_wait = 0;
    }
    uint64_t want = 1 + (uint64_t)((previous * 3 + round) % 100);
    /* The write to the next PE's word, then this PE's wait for its own. */
    switch (w) {
    case WAIT_SHORT:
    case WAIT_UNTIL_SHORT:
    case TEST_SHORT:
        shmem_short_p((short *)old.word, (short)value, next);
        break;
    case WAIT_UNTIL_USHORT:
    case TEST_USHORT:
        shmem_ushort_p((unsigned short *)old.word, (unsigned short)value, next);
        break;
    case WAIT_INT: shmem_int_atomic_set((int *)old.word, (int)value, next); break;
    case WAIT_LONGLONG:
        shmem_longlong_atomic_set((long long *)old.word, (long long)value, next);
        break;
    default: shmem_long_atomic_set((long *)old.word, (long)value, next); break;
    }
    shmem_quiet();
    uint64_t have = 0;
    switch (w) {
    case WAIT_SHORT:
        shmem_short_wait((short *)old.word, 0);
        have = (uint64_t)*(short *)old.word;
        break;
    case WAIT_INT:
        shmem_int_wait((int *)old.word, 0);
        have = (uint64_t)*(int *)old.word;
        break;
    case WAIT_LONG:
        shmem_long_wait((long *)old.word, 0);
        have = (uint64_t)*(long *)old.word;
        break;
    case WAIT_LONGLONG:
        shmem_longlong_wait((long long *)old.word, 0);
        have = (uint64_t)*(long long *)old.word;
        break;
    case WAIT_PLAIN:
        (shmem_wait)((long *)old.word, 0);  /* the function, not the deprecated generic */
        have = (uint64_t)*(long *)old.word;
        break;
    case WAIT_UNTIL_PLAIN:
        (shmem_wait_until)((long *)old.word, SHMEM_CMP_NE, 0);   /* the function, not the generic */
        have = (uint64_t)*(long *)old.word;
        break;
    case WAIT_UNTIL_SHORT:
        if (generic)
            shmem_wait_until((short *)old.word, SHMEM_CMP_NE, (short)0);
        else
            shmem_short_wait_until((short *)old.word, SHMEM_CMP_NE, 0);
        have = (uint64_t)*(short *)old.word;
        break;
    case WAIT_UNTIL_USHORT:
        if (generic)
            shmem_wait_until((unsigned short *)old.word, SHMEM_CMP_NE, (unsigned short)0);
        else
            shmem_ushort_wait_until((unsigned short *)old.word, SHMEM_CMP_NE, 0);
        have = (uint64_t)*(unsigned short *)old.word;
        break;
    case TEST_SHORT:
        while (!(generic ? shmem_test((short *)old.word, SHMEM_CMP_NE, (short)0)
                         : shmem_short_test((short *)old.word, SHMEM_CMP_NE, 0)))
            ;
        have = (uint64_t)*(short *)old.word;
        break;
    default:
        while (!(generic ? shmem_test((unsigned short *)old.word, SHMEM_CMP_NE, (unsigned short)0)
                         : shmem_ushort_test((unsigned short *)old.word, SHMEM_CMP_NE, 0)))
            ;
        have = (uint64_t)*(unsigned short *)old.word;
        break;
    }
    if (have != want)
        pacer_failure("deprecated round=%ld %s: the word from PE %d holds %llu, expected %llu",
                      round, wait_names[w], previous, (unsigned long long)have,
                      (unsigned long long)want);
}

/* ---- the step --------------------------------------------------------------------- */

static void deprecated_step(long units, int last)
{
    (void)last;
    for (long u = 0; u < units; u++, old.round++) {
        long round = old.round, pass = round / CASES;
        int index = (int)(round % CASES), generic = (int)(pass % 2);
        struct active_set s = set_of((int)((index + pass) % SETS));

        /* Every case from a clean start: buffers to the canary, the words to
         * zero, the count at the set's first member to zero, pSync ready. */
        memset(old.dest, CANARY, old.bytes);
        memset(old.source, CANARY, old.bytes);
        shmem_long_atomic_set(old.arrivals, 0, old.me);
        shmem_long_atomic_set((long *)old.word, 0, old.me);
        shmem_long_atomic_set((long *)old.word + 1, 0, old.me);
        for (int k = 0; k < SHMEM_SYNC_SIZE; k++)
            old.sync[k] = SHMEM_SYNC_VALUE;
        shmem_quiet();
        shmem_barrier_all();

        double started = pacer_now();
        if (index < DATA_FIRST) {
            run_to_all(round, &to_all_cases[index], &s);
        } else if (index < BARRIER_FIRST) {
            int k = index - DATA_FIRST;
            run_data(round, (enum family)(BROADCAST + k / 2), k % 2 ? 64 : 32, &s);
        } else if (index < AMO_FIRST) {
            run_barrier(round, index == BARRIER_FIRST ? BARRIER : SYNC, &s);
        } else if (index < WAIT_FIRST) {
            const struct amo_case *c = &amo_cases[index - AMO_FIRST];
            if (c->op == CSWAP || c->op == SWAP || c->op == FINC || c->op == INC ||
                c->op == FADD || c->op == ADD) {
                c->start(old.word, AMO_START, old.me);
                shmem_quiet();
                shmem_barrier_all();    /* every word at its start before anyone's operation */
            }
            run_amo(round, c, generic);
        } else if (index < CACHE_FIRST) {
            run_wait(round, (enum wait_case)(index - WAIT_FIRST), generic);
        } else {
            shmem_clear_cache_inv();
            shmem_set_cache_inv();
            shmem_set_cache_line_inv(old.dest);
            shmem_clear_cache_line_inv(old.dest);
            shmem_udcflush();
            shmem_udcflush_line(old.dest);
        }
        shmem_quiet();
        shmem_barrier_all();
        old.phase[0] += pacer_now() - started;

        started = pacer_now();
        if (index < DATA_FIRST) {
            check_to_all(round, &to_all_cases[index], &s);
        } else if (index < BARRIER_FIRST) {
            int k = index - DATA_FIRST;
            check_data(round, (enum family)(BROADCAST + k / 2), k % 2 ? 64 : 32, &s);
        }
        old.cases++;
        old.phase[1] += pacer_now() - started;
        shmem_barrier_all();        /* every check done before the next case writes */
    }
}

static void deprecated_checkpoint(struct pacer_counters *counters)
{
    counters->work = old.cases;
    counters->phase_seconds[0] = old.phase[0];
    counters->phase_seconds[1] = old.phase[1];
    old.cases = 0;
    memset(old.phase, 0, sizeof old.phase);
}

static void deprecated_teardown(void)
{
    shmem_barrier_all();
    free(old.expected);
    shmem_free(old.word);
    shmem_free(old.arrivals);
    shmem_free(old.sync);
    shmem_free(old.work);
    shmem_free(old.source);
    shmem_free(old.dest);
}

const struct pacer_exercise pacer_deprecated = {
    .name = "deprecated",
    .work = "cases",
    .phases = phase_names,
    .quick_rounds = CASES,          /* every case once, on one set each */
    .memory = deprecated_memory,
    .setup = deprecated_setup,
    .step = deprecated_step,
    .checkpoint = deprecated_checkpoint,
    .teardown = deprecated_teardown,
};
