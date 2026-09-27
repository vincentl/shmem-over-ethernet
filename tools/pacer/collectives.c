/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * collectives - every collective, every type, at every size (proposal §4.8;
 * docs/prompts/task-stress-phase3.md). Each round, on every PE at once:
 *
 *   three typed cases   every typed routine of the team-based collectives in
 *                       turn - the reductions of Table 10 by operator and
 *                       type, the two sum scans, and broadcast, collect,
 *                       fcollect, alltoall and alltoalls over Table 5's types -
 *                       314 cases at a few elements each, on the world or the
 *                       strided team, the C11 generic or the type's own name
 *   one timing case     a collective's byte or long form at one size from 8 B
 *                       to 1 MiB (32 KiB under --quick), on the world or on the
 *                       strided team of the even PEs, called five times, each
 *                       result checked; the median and the fastest, the slowest
 *                       PE's, go into the summary lines
 *
 * Every input is (PE, element, round)-determined and every result has a
 * closed form a PE computes from the team's member list alone: a sum is the
 * members' count times the base plus their offsets; a product has one factor
 * of 3 among ones; a maximum and minimum are the base plus the largest and
 * smallest offset; a bitwise result is the members' bits rotated; a scan's
 * prefix is a prefix of the offsets; the data collectives move bytes of a
 * pattern and are compared byte by byte, the gaps of alltoalls' strided dest
 * against a canary. A plant must not be absorbed - a maximum ignores an input
 * below it - so each forces a change its result cannot hide (the lesson of
 * the teams exercise, docs/decisions/2026-09-27.md item 11).
 */
#define _GNU_SOURCE
#include <complex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { TYPED_A_ROUND = 3, REPEATS = 5, TIMING_SIZES = 7, CANARY = 0x5a, MOST_ELEMENTS = 8 };
enum operation { AND, OR, XOR, MAX, MIN, SUM, PROD, INSCAN, EXSCAN, BROADCAST, COLLECT,
                 FCOLLECT, ALLTOALL, ALLTOALLS, OPERATIONS };
enum timing_family { TIME_REDUCE, TIME_BROADCAST, TIME_COLLECT, TIME_FCOLLECT, TIME_ALLTOALL,
                     TIME_SCAN, TIME_SYNC, TIME_BARRIER, TIME_FAMILIES };

static const char *const operation_names[OPERATIONS] = {
    "and_reduce", "or_reduce", "xor_reduce", "max_reduce", "min_reduce", "sum_reduce",
    "prod_reduce", "sum_inscan", "sum_exscan", "broadcast", "collect", "fcollect", "alltoall",
    "alltoalls"
};
static const char *const timing_names[TIME_FAMILIES] = {
    "reduce", "broadcast", "collect", "fcollect", "alltoall", "scan", "sync", "barrier"
};
static const char *const phase_names[] = { "typed", "timed", "check", NULL };

/* A value class: how a type's values are made and compared. */
enum value_class { INTEGER, FLOATING, COMPLEX, BYTES };

/*
 * A typed case: an operator on a type, by its routines. Arithmetic crosses as
 * uint64_t (an integer's bits, a float's small whole number, a complex's real
 * part - its imaginary part is 0 in and out); the data collectives move bytes.
 */
struct typed_case {
    enum operation operation;
    const char *type;
    int size;
    enum value_class values;
    int (*call)(shmem_team_t team, void *dest, const void *source, size_t n, int generic,
                int root, ptrdiff_t dst, ptrdiff_t sst);
    uint64_t (*load)(const void *slot);
    void (*store)(void *slot, uint64_t value);
};

/* ---- the typed routines, generated from Table 10's and Table 5's lists ---------- */

#define REDUCE_ONE(T, NAME, OP, SUFFIX)                                               \
    static int OP##_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                           int generic, int root, ptrdiff_t dst, ptrdiff_t sst)       \
    {                                                                                 \
        (void)root, (void)dst, (void)sst;                                             \
        return generic ? shmem_##SUFFIX(team, (T *)dest, (const T *)source, n)        \
                       : shmem_##NAME##_##SUFFIX(team, (T *)dest, (const T *)source, n); \
    }

#define ACCESS(T, NAME)                                                               \
    static uint64_t load_##NAME(const void *slot)                                     \
    {                                                                                 \
        return (uint64_t)*(const T *)slot;                                            \
    }                                                                                 \
    static void store_##NAME(void *slot, uint64_t value)                              \
    {                                                                                 \
        *(T *)slot = (T)value;                                                        \
    }
#define ACCESS_COMPLEX(T, NAME)                                                       \
    static uint64_t load_##NAME(const void *slot)                                     \
    {                                                                                 \
        T v = *(const T *)slot;                                                       \
        return cimag(v) != 0 ? UINT64_MAX : (uint64_t)creal(v);                       \
    }                                                                                 \
    static void store_##NAME(void *slot, uint64_t value)                              \
    {                                                                                 \
        *(T *)slot = (T)(double)value;                                                \
    }

#define BITWISE_ROUTINES(T, NAME)                                                     \
    REDUCE_ONE(T, NAME, and, and_reduce) REDUCE_ONE(T, NAME, or, or_reduce)           \
    REDUCE_ONE(T, NAME, xor, xor_reduce)
#define ORDERED_ROUTINES(T, NAME)                                                     \
    REDUCE_ONE(T, NAME, max, max_reduce) REDUCE_ONE(T, NAME, min, min_reduce)
#define ARITHMETIC_ROUTINES(T, NAME)                                                  \
    REDUCE_ONE(T, NAME, sum, sum_reduce) REDUCE_ONE(T, NAME, prod, prod_reduce)       \
    REDUCE_ONE(T, NAME, inscan, sum_inscan) REDUCE_ONE(T, NAME, exscan, sum_exscan)

#define DATA_ROUTINES(T, NAME)                                                        \
    static int broadcast_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                                int generic, int root, ptrdiff_t dst, ptrdiff_t sst)  \
    {                                                                                 \
        (void)dst, (void)sst;                                                         \
        return generic ? shmem_broadcast(team, (T *)dest, (const T *)source, n, root)  \
                       : shmem_##NAME##_broadcast(team, (T *)dest, (const T *)source, n, root); \
    }                                                                                 \
    static int collect_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                              int generic, int root, ptrdiff_t dst, ptrdiff_t sst)    \
    {                                                                                 \
        (void)root, (void)dst, (void)sst;                                             \
        return generic ? shmem_collect(team, (T *)dest, (const T *)source, n)         \
                       : shmem_##NAME##_collect(team, (T *)dest, (const T *)source, n); \
    }                                                                                 \
    static int fcollect_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                               int generic, int root, ptrdiff_t dst, ptrdiff_t sst)   \
    {                                                                                 \
        (void)root, (void)dst, (void)sst;                                             \
        return generic ? shmem_fcollect(team, (T *)dest, (const T *)source, n)        \
                       : shmem_##NAME##_fcollect(team, (T *)dest, (const T *)source, n); \
    }                                                                                 \
    static int alltoall_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                               int generic, int root, ptrdiff_t dst, ptrdiff_t sst)   \
    {                                                                                 \
        (void)root, (void)dst, (void)sst;                                             \
        return generic ? shmem_alltoall(team, (T *)dest, (const T *)source, n)        \
                       : shmem_##NAME##_alltoall(team, (T *)dest, (const T *)source, n); \
    }                                                                                 \
    static int alltoalls_##NAME(shmem_team_t team, void *dest, const void *source, size_t n, \
                                int generic, int root, ptrdiff_t dst, ptrdiff_t sst)  \
    {                                                                                 \
        (void)root;                                                                   \
        return generic ? shmem_alltoalls(team, (T *)dest, (const T *)source, dst, sst, n) \
                       : shmem_##NAME##_alltoalls(team, (T *)dest, (const T *)source, dst, \
                                                  sst, n);                            \
    }

/* Table 10 (as shmem.h lists it) and Table 5. */
#define BITWISE_TYPES(X)                                                              \
    X(unsigned char, uchar) X(unsigned short, ushort) X(unsigned int, uint)           \
    X(unsigned long, ulong) X(unsigned long long, ulonglong) X(int8_t, int8)          \
    X(int16_t, int16) X(int32_t, int32) X(int64_t, int64) X(uint8_t, uint8)           \
    X(uint16_t, uint16) X(uint32_t, uint32) X(uint64_t, uint64) X(size_t, size)
#define INTEGER_TYPES(X)                                                              \
    X(char, char) X(signed char, schar) X(short, short) X(int, int) X(long, long)     \
    X(long long, longlong) X(ptrdiff_t, ptrdiff) BITWISE_TYPES(X)
#define FLOATING_TYPES(X) X(float, float) X(double, double) X(long double, longdouble)
#define COMPLEX_TYPES(X) X(double _Complex, complexd) X(float _Complex, complexf)
#define DATA_TYPES(X)                                                                 \
    X(float, float) X(double, double) X(long double, longdouble) X(char, char)         \
    X(signed char, schar) X(short, short) X(int, int) X(long, long)                   \
    X(long long, longlong) X(unsigned char, uchar) X(unsigned short, ushort)          \
    X(unsigned int, uint) X(unsigned long, ulong) X(unsigned long long, ulonglong)    \
    X(int8_t, int8) X(int16_t, int16) X(int32_t, int32) X(int64_t, int64)            \
    X(uint8_t, uint8) X(uint16_t, uint16) X(uint32_t, uint32) X(uint64_t, uint64)     \
    X(size_t, size) X(ptrdiff_t, ptrdiff)

INTEGER_TYPES(ACCESS)
FLOATING_TYPES(ACCESS)
COMPLEX_TYPES(ACCESS_COMPLEX)
BITWISE_TYPES(BITWISE_ROUTINES)
INTEGER_TYPES(ORDERED_ROUTINES)
FLOATING_TYPES(ORDERED_ROUTINES)
INTEGER_TYPES(ARITHMETIC_ROUTINES)
FLOATING_TYPES(ARITHMETIC_ROUTINES)
COMPLEX_TYPES(ARITHMETIC_ROUTINES)
DATA_TYPES(DATA_ROUTINES)

#define AND_CASE(T, NAME) { AND, #NAME, (int)sizeof(T), INTEGER, and_##NAME, load_##NAME, store_##NAME },
#define OR_CASE(T, NAME) { OR, #NAME, (int)sizeof(T), INTEGER, or_##NAME, load_##NAME, store_##NAME },
#define XOR_CASE(T, NAME) { XOR, #NAME, (int)sizeof(T), INTEGER, xor_##NAME, load_##NAME, store_##NAME },
#define MAX_CASE(T, NAME) { MAX, #NAME, (int)sizeof(T), INTEGER, max_##NAME, load_##NAME, store_##NAME },
#define MIN_CASE(T, NAME) { MIN, #NAME, (int)sizeof(T), INTEGER, min_##NAME, load_##NAME, store_##NAME },
#define MAX_FLOAT_CASE(T, NAME) { MAX, #NAME, (int)sizeof(T), FLOATING, max_##NAME, load_##NAME, store_##NAME },
#define MIN_FLOAT_CASE(T, NAME) { MIN, #NAME, (int)sizeof(T), FLOATING, min_##NAME, load_##NAME, store_##NAME },
#define ARITHMETIC_CASES(T, NAME, VALUES)                                             \
    { SUM, #NAME, (int)sizeof(T), VALUES, sum_##NAME, load_##NAME, store_##NAME },     \
    { PROD, #NAME, (int)sizeof(T), VALUES, prod_##NAME, load_##NAME, store_##NAME },   \
    { INSCAN, #NAME, (int)sizeof(T), VALUES, inscan_##NAME, load_##NAME, store_##NAME }, \
    { EXSCAN, #NAME, (int)sizeof(T), VALUES, exscan_##NAME, load_##NAME, store_##NAME },
#define INTEGER_ARITHMETIC(T, NAME) ARITHMETIC_CASES(T, NAME, INTEGER)
#define FLOATING_ARITHMETIC(T, NAME) ARITHMETIC_CASES(T, NAME, FLOATING)
#define COMPLEX_ARITHMETIC(T, NAME) ARITHMETIC_CASES(T, NAME, COMPLEX)
#define DATA_CASES(T, NAME)                                                           \
    { BROADCAST, #NAME, (int)sizeof(T), BYTES, broadcast_##NAME, NULL, NULL },         \
    { COLLECT, #NAME, (int)sizeof(T), BYTES, collect_##NAME, NULL, NULL },             \
    { FCOLLECT, #NAME, (int)sizeof(T), BYTES, fcollect_##NAME, NULL, NULL },           \
    { ALLTOALL, #NAME, (int)sizeof(T), BYTES, alltoall_##NAME, NULL, NULL },           \
    { ALLTOALLS, #NAME, (int)sizeof(T), BYTES, alltoalls_##NAME, NULL, NULL },

static const struct typed_case typed_cases[] = {
    BITWISE_TYPES(AND_CASE) BITWISE_TYPES(OR_CASE) BITWISE_TYPES(XOR_CASE)
    INTEGER_TYPES(MAX_CASE) INTEGER_TYPES(MIN_CASE)
    FLOATING_TYPES(MAX_FLOAT_CASE) FLOATING_TYPES(MIN_FLOAT_CASE)
    INTEGER_TYPES(INTEGER_ARITHMETIC) FLOATING_TYPES(FLOATING_ARITHMETIC)
    COMPLEX_TYPES(COMPLEX_ARITHMETIC)
    DATA_TYPES(DATA_CASES)
};
enum { TYPED_CASES = sizeof typed_cases / sizeof *typed_cases };

/* ---- the timing cases ---------------------------------------------------------- */

struct timing_case {
    enum timing_family family;
    int bytes;                      /* a member's contribution */
    int strided;                    /* on the even PEs' team, not the world */
};

static const int timing_sizes[TIMING_SIZES] = { 8, 64, 512, 4096, 32768, 262144, 1048576 };
enum { DATA_FAMILIES = TIME_SYNC, TIMING_CASES = DATA_FAMILIES * TIMING_SIZES * 2 + 3 };

static struct timing_case timing_case_of(int index)
{
    struct timing_case c;
    if (index < DATA_FAMILIES * TIMING_SIZES * 2) {
        c.family = (enum timing_family)(index % DATA_FAMILIES);
        c.bytes = timing_sizes[(index / DATA_FAMILIES) % TIMING_SIZES];
        c.strided = index / (DATA_FAMILIES * TIMING_SIZES);
    } else {
        int k = index - DATA_FAMILIES * TIMING_SIZES * 2;
        c.family = k < 2 ? TIME_SYNC : TIME_BARRIER;
        c.bytes = 0;
        c.strided = k == 1;
    }
    return c;
}

/* ---- the state ------------------------------------------------------------------ */

struct team_view {
    shmem_team_t handle;
    const char *name;
    int size, rank;                 /* rank -1: not a member */
    int *world;                     /* [size]: members' world PEs, by team rank */
};

static struct {
    uint64_t seed;
    int quick, last;
    size_t most_bytes;              /* the timing sweep's largest contribution */
    int plant_reduce, plant_scan, plant_data, plant_gap;
    shmem_team_t team;
    int me_world;
    struct team_view teams[2];      /* the exercise's team, and the even PEs of it */
    /* symmetric */
    unsigned char *source, *dest;
    size_t buffer_bytes;
    double *times, *times_result;   /* [2 * TIMING_CASES]: medians, then fastest */
    /* private */
    unsigned char *expected, *owned;
    double samples[TIMING_CASES][64];
    int sample_count[TIMING_CASES];
    double fastest[TIMING_CASES];
    long round;
    double calls, bytes, phase[3];
} coll;

static uint64_t collectives_memory(int npes, const struct pacer_options *options, char *fields,
                                   size_t size)
{
    coll.seed = options->seed;
    coll.quick = options->quick;
    coll.plant_reduce = options->plant == PACER_PLANT_COLLECTIVES_REDUCE;
    coll.plant_scan = options->plant == PACER_PLANT_COLLECTIVES_SCAN;
    coll.plant_data = options->plant == PACER_PLANT_COLLECTIVES_DATA;
    coll.plant_gap = options->plant == PACER_PLANT_COLLECTIVES_GAP;
    /* Two buffers of a contribution from every member, and the typed cases'
     * strided blocks, within the budget. */
    size_t typed = (size_t)npes * MOST_ELEMENTS * 16 * 4;
    size_t most = coll.quick ? 32768 : 1048576;
    while (most > 8 && 2 * ((uint64_t)npes * most + typed) + 4 * TIMING_CASES * 8 > options->memory)
        most /= 8;
    uint64_t symmetric = 2 * ((uint64_t)npes * most + typed) + 4 * TIMING_CASES * 8;
    if (symmetric > options->memory) {
        snprintf(fields, size, "%d PEs' buffers need %llu bytes", npes,
                 (unsigned long long)symmetric);
        return UINT64_MAX;
    }
    coll.most_bytes = most;
    coll.buffer_bytes = (size_t)npes * most + typed;
    char most_text[32];
    pacer_size(most_text, sizeof most_text, most);
    snprintf(fields, size, "typed=%d timed=%d sizes=8..%s teams=world,even", TYPED_CASES,
             TIMING_CASES, most_text);
    return symmetric;
}

static int view(struct team_view *v, shmem_team_t handle, const char *name)
{
    v->handle = handle;
    v->name = name;
    v->size = handle == SHMEM_TEAM_INVALID ? 0 : shmem_team_n_pes(handle);
    v->rank = handle == SHMEM_TEAM_INVALID ? -1 : shmem_team_my_pe(handle);
    v->world = malloc(sizeof *v->world * (size_t)(v->size ? v->size : 1));
    if (!v->world)
        return -1;
    for (int k = 0; k < v->size; k++)
        v->world[k] = shmem_team_translate_pe(handle, k, SHMEM_TEAM_WORLD);
    return 0;
}

static int collectives_setup(shmem_team_t team, const struct pacer_options *options)
{
    (void)options;
    coll.team = team;
    coll.me_world = shmem_my_pe();
    int n = shmem_team_n_pes(team);
    shmem_team_t even = SHMEM_TEAM_INVALID;
    if (shmem_team_split_strided(team, 0, 2, (n + 1) / 2, NULL, 0, &even) != 0)
        return -1;
    if (view(&coll.teams[0], team, "world") || view(&coll.teams[1], even, "even"))
        return -1;
    coll.source = shmem_malloc(coll.buffer_bytes);
    coll.dest = shmem_malloc(coll.buffer_bytes);
    coll.times = shmem_calloc(2 * TIMING_CASES, sizeof *coll.times);
    coll.times_result = shmem_calloc(2 * TIMING_CASES, sizeof *coll.times_result);
    coll.expected = malloc(coll.buffer_bytes);
    coll.owned = malloc(coll.buffer_bytes);
    if (!coll.source || !coll.dest || !coll.times || !coll.times_result || !coll.expected ||
        !coll.owned)
        return -1;
    memset(coll.sample_count, 0, sizeof coll.sample_count);
    for (int i = 0; i < TIMING_CASES; i++)
        coll.fastest[i] = 1e30;
    coll.round = 0;
    coll.calls = coll.bytes = 0;
    memset(coll.phase, 0, sizeof coll.phase);
    shmem_team_sync(team);
    return 0;
}

/* ---- inputs and their closed forms ---------------------------------------------- */

static uint64_t base_of(size_t e, long round)
{
    return ((uint64_t)e + (uint64_t)round) % 20;
}

/* What member k (world PE p) of a team of m gives, element e, for an operator. */
static uint64_t input_of(enum operation op, int p, int k, int m, size_t e, long round, int bits)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t bit = UINT64_C(1) << ((uint64_t)(p + (int)e) % (uint64_t)bits);
    switch (op) {
    case AND: return ~bit & mask;
    case OR: case XOR: return bit;
    case PROD: return k == (int)((e + (size_t)round) % (size_t)m) ? 3 : 1;
    default: return base_of(e, round) + (uint64_t)(p % 50) + 1;     /* max, min, sums */
    }
}

/* What this PE's dest element e must hold. */
static uint64_t result_of(enum operation op, const struct team_view *v, size_t e, long round,
                          int bits)
{
    uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
    uint64_t r = op == AND ? mask : op == PROD ? 1 : op == MIN ? UINT64_MAX : 0;
    int last = op == INSCAN ? v->rank : op == EXSCAN ? v->rank - 1 : v->size - 1;
    for (int k = 0; k <= last; k++) {
        uint64_t x = input_of(op, v->world[k], k, v->size, e, round, bits);
        switch (op) {
        case AND: r &= x; break;
        case OR: r |= x; break;
        case XOR: r ^= x; break;
        case MAX: r = x > r ? x : r; break;
        case MIN: r = x < r ? x : r; break;
        case PROD: r *= x; break;
        default: r += x; break;
        }
    }
    return r & mask;
}

/* Byte b of what world PE p offers in a data collective. */
static unsigned char data_byte(int p, uint64_t b, long round)
{
    uint64_t w = random_mix(coll.seed ^ random_name("collectives"), (uint64_t)p, b >> 3,
                            (uint64_t)round);
    return (unsigned char)(w >> (8 * (b & 7)));
}

/* ---- one typed case -------------------------------------------------------------- */

static void failure_line(long round, const struct typed_case *c, const struct team_view *v,
                         const char *what)
{
    pacer_failure("collectives round=%ld %s %s %s: %s", round, operation_names[c->operation],
                  c->type, v->name, what);
}

static void run_typed(const struct typed_case *c, struct team_view *v, long round, int generic)
{
    if (v->rank < 0)
        return;
    size_t w = (size_t)c->size, m = (size_t)v->size, n = 1 + (size_t)(round % MOST_ELEMENTS);
    int bits = 8 * c->size > 64 ? 64 : 8 * c->size;
    int root = (int)(round % (long)m);
    ptrdiff_t dst = 1 + round % 3, sst = 1 + (round / 3) % 3;
    double started = pacer_now();
    enum operation op = c->operation;
    /* What a typed case can reach: m blocks of the most elements, strided. */
    size_t reach = m * MOST_ELEMENTS * 16 * 3 + 64;
    if (reach > coll.buffer_bytes)
        reach = coll.buffer_bytes;
    memset(coll.dest, CANARY, reach);
    size_t n_here = n;                      /* collect: this member's own count */
    if (c->values != BYTES) {
        for (size_t e = 0; e < n; e++) {
            uint64_t x = input_of(op, coll.me_world, v->rank, (int)m, e, round, bits);
            if (e == 0 && v->rank >= 0 && coll.me_world == 1) {
                if (coll.plant_reduce && op <= PROD) {
                    /* The planted defect, forced past what the result absorbs. */
                    x = op == AND ? 0 : op == OR ? UINT64_MAX : op == XOR ? x ^ 1
                        : op == MAX ? 100 : op == MIN ? 0 : op == PROD ? 2 : x + 1;
                    coll.plant_reduce = 0;
                } else if (coll.plant_scan && (op == INSCAN || op == EXSCAN)) {
                    x++;
                    coll.plant_scan = 0;
                }
            }
            c->store(coll.source + e * w, x);
        }
    } else {
        if (op == COLLECT)
            n_here = 1 + (size_t)((v->rank + round) % 3);
        size_t bytes = m * MOST_ELEMENTS * 3 * w;  /* the most any case reads */
        for (size_t b = 0; b < bytes; b++)
            coll.source[b] = data_byte(coll.me_world, b, round);
        if (coll.plant_data && coll.me_world == 1 && op != BROADCAST) {
            coll.source[0] ^= 1;    /* the planted defect: a byte every member reads */
            coll.plant_data = 0;
        }
    }
    shmem_team_sync(v->handle);     /* every dest ready before anyone calls */
    int status = c->call(v->handle, coll.dest, coll.source, op == COLLECT ? n_here : n, generic,
                         root, dst, sst);
    coll.calls++;
    coll.phase[0] += pacer_now() - started;
    started = pacer_now();
    if (status != 0) {
        char what[64];
        snprintf(what, sizeof what, "returned %d", status);
        failure_line(round, c, v, what);
        coll.phase[2] += pacer_now() - started;
        return;
    }

    /* The closed forms. */
    char what[192];
    if (c->values != BYTES) {
        size_t count = op == INSCAN || op == EXSCAN || op <= PROD ? n : 0;
        for (size_t e = 0; e < count; e++) {
            uint64_t want = result_of(op, v, e, round, bits);
            uint64_t got = c->load(coll.dest + e * w);
            if (c->values == INTEGER) {
                uint64_t mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
                got &= mask;
            }
            if (got != want) {
                snprintf(what, sizeof what, "element %zu is %llu, expected %llu", e,
                         (unsigned long long)got, (unsigned long long)want);
                failure_line(round, c, v, what);
                break;
            }
        }
        coll.phase[2] += pacer_now() - started;
        return;
    }
    /* The data collectives: where each byte comes from. */
    memset(coll.expected, CANARY, reach);
    memset(coll.owned, 0, reach);
    size_t extent = 0;
    for (size_t k = 0; k < m; k++) {
        int p = v->world[k];
        size_t count = op == COLLECT ? 1 + (size_t)(((long)k + round) % 3) : n;
        size_t before = 0;
        if (op == COLLECT)
            for (size_t j = 0; j < k; j++)
                before += 1 + (size_t)(((long)j + round) % 3);
        for (size_t e = 0; e < count; e++)
            for (size_t b = 0; b < w; b++) {
                size_t at, from;
                int owner = p;
                switch (op) {
                case BROADCAST:
                    if (k != 0)
                        goto next_member;
                    owner = v->world[root];
                    at = e * w + b;
                    from = e * w + b;
                    break;
                case COLLECT:
                    at = (before + e) * w + b;
                    from = e * w + b;
                    break;
                case FCOLLECT:
                    at = (k * n + e) * w + b;
                    from = e * w + b;
                    break;
                case ALLTOALL:
                    at = (k * n + e) * w + b;
                    from = ((size_t)v->rank * n + e) * w + b;
                    break;
                default:            /* alltoalls */
                    at = (k * n + e) * (size_t)dst * w + b;
                    from = ((size_t)v->rank * n + e) * (size_t)sst * w + b;
                    break;
                }
                coll.expected[at] = data_byte(owner, from, round);
                coll.owned[at] = 1;
                if (at + 1 > extent)
                    extent = at + 1;
            }
    next_member:;
    }
    if (op == ALLTOALLS && coll.plant_gap && coll.me_world == 1 && dst > 1) {
        coll.dest[w] = (unsigned char)~CANARY;  /* the planted defect: a byte in a gap */
        coll.plant_gap = 0;
    }
    size_t checked = extent + 64 < reach ? extent + 64 : reach;
    for (size_t i = 0; i < checked; i++)
        if (coll.dest[i] != coll.expected[i]) {
            snprintf(what, sizeof what, "byte %zu is %02x, %s", i, coll.dest[i],
                     coll.owned[i] ? "not what was sent" : "where nothing lands");
            failure_line(round, c, v, what);
            break;
        }
    coll.bytes += (double)(extent);
    coll.phase[2] += pacer_now() - started;
}

/* ---- one timing case -------------------------------------------------------------- */

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The byte form's result, or the long form's, against its closed form. */
static void check_timed(const struct timing_case *c, const struct team_view *v, size_t bytes,
                        int root, long round)
{
    size_t m = (size_t)v->size, n = bytes / 8;
    char what[160];
    const char *team = v->name;
    if (c->family == TIME_REDUCE || c->family == TIME_SCAN) {
        enum operation op = c->family == TIME_REDUCE ? SUM : INSCAN;
        const long *dest = (const long *)coll.dest;
        for (size_t e = 0; e < n; e++) {
            uint64_t want = result_of(op, v, e, round, 64);
            if ((uint64_t)dest[e] != want) {
                snprintf(what, sizeof what, "element %zu of %zu is %ld, expected %llu", e, n,
                         dest[e], (unsigned long long)want);
                pacer_failure("collectives round=%ld timed %s %s bytes=%zu: %s", round,
                              timing_names[c->family], team, bytes, what);
                return;
            }
        }
        return;
    }
    size_t blocks = c->family == TIME_BROADCAST ? 1 : m;
    for (size_t k = 0; k < blocks; k++) {
        int owner = c->family == TIME_BROADCAST ? v->world[root] : v->world[k];
        size_t from = c->family == TIME_ALLTOALL ? (size_t)v->rank * bytes : 0;
        const unsigned char *at = coll.dest + k * bytes;
        for (size_t b = 0; b < bytes; b++)
            if (at[b] != data_byte(owner, from + b, round)) {
                pacer_failure("collectives round=%ld timed %s %s bytes=%zu: byte %zu of block %zu "
                              "is %02x, not what PE %d sent", round, timing_names[c->family],
                              team, bytes, b, k, at[b], owner);
                return;
            }
    }
}

static void run_timed(int index, long round)
{
    struct timing_case c = timing_case_of(index);
    struct team_view *v = &coll.teams[c.strided];
    if (v->rank < 0 || (size_t)c.bytes > coll.most_bytes)
        return;
    size_t bytes = (size_t)c.bytes, m = (size_t)v->size, n = bytes / 8;
    int root = (int)(round % (long)m);
    /* The source, once: this member's contribution, or its block for each. */
    if (c.family == TIME_REDUCE || c.family == TIME_SCAN) {
        long *source = (long *)coll.source;
        for (size_t e = 0; e < n; e++)
            source[e] = (long)input_of(SUM, coll.me_world, v->rank, (int)m, e, round, 64);
    } else if (c.family != TIME_SYNC && c.family != TIME_BARRIER) {
        size_t total = (c.family == TIME_ALLTOALL ? m : 1) * bytes;
        for (size_t b = 0; b < total; b++)
            coll.source[b] = data_byte(coll.me_world, b, round);
    }
    double times[REPEATS];
    for (int rep = 0; rep < REPEATS; rep++) {
        shmem_team_sync(v->handle);         /* every dest ready, and a common start */
        double started = pacer_now();
        switch (c.family) {
        case TIME_REDUCE:
            shmem_long_sum_reduce(v->handle, (long *)coll.dest, (const long *)coll.source, n);
            break;
        case TIME_BROADCAST:
            shmem_broadcastmem(v->handle, coll.dest, coll.source, bytes, root);
            break;
        case TIME_COLLECT:
            shmem_collectmem(v->handle, coll.dest, coll.source, bytes);
            break;
        case TIME_FCOLLECT:
            shmem_fcollectmem(v->handle, coll.dest, coll.source, bytes);
            break;
        case TIME_ALLTOALL:
            shmem_alltoallmem(v->handle, coll.dest, coll.source, bytes);
            break;
        case TIME_SCAN:
            shmem_long_sum_inscan(v->handle, (long *)coll.dest, (const long *)coll.source, n);
            break;
        case TIME_SYNC:
            shmem_team_sync(v->handle);
            break;
        default:
            shmem_barrier_all();
            break;
        }
        times[rep] = pacer_now() - started;
        coll.calls++;
        coll.phase[1] += times[rep];
        double checking = pacer_now();
        if (c.family != TIME_SYNC && c.family != TIME_BARRIER)
            check_timed(&c, v, bytes, root, round);
        coll.phase[2] += pacer_now() - checking;
        coll.bytes += (double)(bytes * (c.family == TIME_BROADCAST ? 1 : m));
    }
    qsort(times, REPEATS, sizeof *times, by_value);
    int k = coll.sample_count[index] % 64;
    coll.samples[index][k] = times[REPEATS / 2];
    coll.sample_count[index]++;
    if (times[0] < coll.fastest[index])
        coll.fastest[index] = times[0];
}

/* ---- the exercise ------------------------------------------------------------------ */

static void collectives_step(long units, int last)
{
    coll.last = last;
    for (long u = 0; u < units; u++, coll.round++) {
        long round = coll.round;
        for (int t = 0; t < TYPED_A_ROUND; t++) {
            int index = (int)((round * TYPED_A_ROUND + t) % TYPED_CASES);
            run_typed(&typed_cases[index], &coll.teams[(round + t) % 2], round,
                      (int)((round / 2) % 2));
        }
        run_timed((int)(round % TIMING_CASES), round);
        shmem_team_sync(coll.team);         /* every buffer free for the next round */
    }
}

/* At the last checkpoint: each timing case's median, the slowest PE's, and its
 * fastest call anywhere, as summary lines. */
static void summarize(void)
{
    for (int i = 0; i < TIMING_CASES; i++) {
        int count = coll.sample_count[i] < 64 ? coll.sample_count[i] : 64;
        double median = 0;
        if (count) {
            double sorted[64];
            memcpy(sorted, coll.samples[i], sizeof(double) * (size_t)count);
            qsort(sorted, (size_t)count, sizeof *sorted, by_value);
            median = sorted[count / 2];
        }
        coll.times[i] = median;
        coll.times[TIMING_CASES + i] = -coll.fastest[i];    /* the largest of the negated */
    }
    shmem_double_max_reduce(coll.team, coll.times_result, coll.times, 2 * TIMING_CASES);
    if (shmem_team_my_pe(coll.team) != 0)
        return;
    for (int strided = 0; strided < 2; strided++) {
        for (int s = 0; s < TIMING_SIZES && (size_t)timing_sizes[s] <= coll.most_bytes; s++) {
            char median[256], fastest[256];
            size_t used = 0, used_fastest = 0;
            for (int f = 0; f < DATA_FAMILIES; f++) {
                int i = (strided * TIMING_SIZES + s) * DATA_FAMILIES + f;
                used += (size_t)snprintf(median + used, sizeof median - used, " %s_us=%.1f",
                                         timing_names[f], coll.times_result[i] * 1e6);
                used_fastest += (size_t)snprintf(fastest + used_fastest,
                                                 sizeof fastest - used_fastest, " %s_us=%.1f",
                                                 timing_names[f],
                                                 -coll.times_result[TIMING_CASES + i] * 1e6);
            }
            pacer_summary("summary collectives team=%s bytes=%d median%s",
                          coll.teams[strided].name, timing_sizes[s], median);
            pacer_summary("summary collectives team=%s bytes=%d fastest%s",
                          coll.teams[strided].name, timing_sizes[s], fastest);
        }
    }
    int base = DATA_FAMILIES * TIMING_SIZES * 2;
    pacer_summary("summary collectives sync_us=%.1f even_sync_us=%.1f barrier_all_us=%.1f",
                  coll.times_result[base] * 1e6, coll.times_result[base + 1] * 1e6,
                  coll.times_result[base + 2] * 1e6);
}

static void collectives_checkpoint(struct pacer_counters *counters)
{
    if (coll.last)
        summarize();
    counters->work = coll.calls;
    counters->bytes = coll.bytes;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = coll.phase[k];
    coll.calls = coll.bytes = 0;
    memset(coll.phase, 0, sizeof coll.phase);
}

static void collectives_teardown(void)
{
    shmem_team_sync(coll.team);
    if (coll.teams[1].handle != SHMEM_TEAM_INVALID)
        shmem_team_destroy(coll.teams[1].handle);
    shmem_free(coll.times_result);
    shmem_free(coll.times);
    shmem_free(coll.dest);
    shmem_free(coll.source);
    free(coll.owned);
    free(coll.expected);
    free(coll.teams[1].world);
    free(coll.teams[0].world);
}

const struct pacer_exercise pacer_collectives = {
    .name = "collectives",
    .work = "calls",
    .phases = phase_names,
    .quick_rounds = (TYPED_CASES + TYPED_A_ROUND - 1) / TYPED_A_ROUND,     /* every typed case */
    .memory = collectives_memory,
    .setup = collectives_setup,
    .step = collectives_step,
    .checkpoint = collectives_checkpoint,
    .teardown = collectives_teardown,
};
