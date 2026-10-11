/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * rma - ordering, strides, and many in flight (proposal §4.6;
 * docs/prompts/done/task-stress-phase3.md). Every round, on every PE at once:
 *
 *   the halo      a bulk put of the round's pattern to the next PE, a fence,
 *                 then an update of that PE's flag - an atomic add, a
 *                 shmem_signal_add or a shmem_signal_set in turn, on the
 *                 default context or a private one - after which the next PE
 *                 waits for its flag and checks the whole buffer: a put that
 *                 overtook its fence, on either path, shows. Sizes from 8 B to
 *                 1 MiB in turn; the put itself putmem, putmem_nbi or a typed
 *                 put in turn, the fence ordering the nonblocking one too - or
 *                 putmem_signal or putmem_signal_nbi, which raise the flag
 *                 themselves
 *   a transfer    one case of seven classes over every type of Table 5 and
 *                 every size of the sized forms: put and get; iput and iget
 *                 with strides; ibput and ibget with blocks; put_nbi and
 *                 get_nbi in bursts, completed by quiet or by pe_quiet naming
 *                 the one target; p and g, element by element; put_signal,
 *                 and put_signal_nbi quieted, whose receiver waits for the
 *                 signal and reads every byte then, before any sync - data
 *                 that a signal overtook shows - beside a get. The put side
 *                 writes the next PE's inbox, the get side reads the previous
 *                 PE's source; both read every byte back, the gaps a strided
 *                 transfer must not touch and the bytes past its end among them
 *   shmem_ptr     stores into each node-mate's slot, a fence and a flag, then
 *                 loads from each node-mate's source; the node-mate checks
 *                 the stores when its flags say they are there
 *
 * Every byte moved is (source, target, index, round)-determined. A round's
 * transfer case is round mod the 198 cases, on the default context in even
 * passes over them and a private one in odd; where the type has a name of its
 * own, the put side takes it and the get side the C11 generic, or the other
 * way about, which flips every second pass - so four passes, 792 rounds, run
 * every case in both spellings on both contexts.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { REGION = 64 << 10, HALO_SIZES = 7, PTR_WORDS = 64, CANARY = 0xa5 };
enum transfer_class { CONTIGUOUS, STRIDED, BLOCKS, NONBLOCKING, SINGLE, SIGNAL, SIGNAL_NBI,
                      CLASSES };
enum flag_method { ATOMIC_ADD, SIGNAL_ADD, SIGNAL_SET, FLAG_METHODS };

static const char *const class_names[CLASSES] = {
    "put_get", "iput_iget", "ibput_ibget", "nbi", "p_g", "put_signal_get",
    "put_signal_nbi_get_nbi"
};
static const char *const flag_names[FLAG_METHODS] = { "atomic_add", "signal_add", "signal_set" };
enum { HALO_PUTS = 5 };
static const char *const put_names[HALO_PUTS] = { "putmem", "putmem_nbi", "uint64_put",
                                                  "putmem_signal", "putmem_signal_nbi" };
static const char *const phase_names[] = { "halo", "transfer", "ptr", "check", NULL };

/*
 * An element kind: a type of Table 5 by its routines, or a size of the sized
 * forms. Pointers are void, counts and strides in elements; `generic` picks
 * the C11 generic over the type's name, which a sized kind has not.
 */
struct rma_kind {
    const char *name;
    int size, typed;
    void (*put)(shmem_ctx_t ctx, int generic, void *dest, const void *source, size_t n, int pe);
    void (*get)(shmem_ctx_t ctx, int generic, void *dest, const void *source, size_t n, int pe);
    void (*iput)(shmem_ctx_t ctx, int generic, void *dest, const void *source, ptrdiff_t dst,
                 ptrdiff_t sst, size_t n, int pe);
    void (*iget)(shmem_ctx_t ctx, int generic, void *dest, const void *source, ptrdiff_t dst,
                 ptrdiff_t sst, size_t n, int pe);
    void (*ibput)(shmem_ctx_t ctx, int generic, void *dest, const void *source, ptrdiff_t dst,
                  ptrdiff_t sst, size_t bsize, size_t nblocks, int pe);
    void (*ibget)(shmem_ctx_t ctx, int generic, void *dest, const void *source, ptrdiff_t dst,
                  ptrdiff_t sst, size_t bsize, size_t nblocks, int pe);
    void (*put_nbi)(shmem_ctx_t ctx, int generic, void *dest, const void *source, size_t n,
                    int pe);
    void (*get_nbi)(shmem_ctx_t ctx, int generic, void *dest, const void *source, size_t n,
                    int pe);
    /* p and g, a typed kind's only: values as small whole numbers */
    void (*p)(shmem_ctx_t ctx, int generic, void *dest, uint64_t value, int pe);
    uint64_t (*g)(shmem_ctx_t ctx, int generic, const void *source, int pe);
    uint64_t (*load)(const void *slot);
    void (*store)(void *slot, uint64_t value);
    /* put-with-signal, blocking and not: every kind's */
    void (*put_signal)(shmem_ctx_t ctx, int generic, void *dest, const void *source, size_t n,
                       uint64_t *signal, uint64_t value, int operation, int pe);
    void (*put_signal_nbi)(shmem_ctx_t ctx, int generic, void *dest, const void *source,
                           size_t n, uint64_t *signal, uint64_t value, int operation, int pe);
};

/* A typed routine by one of its spellings, context-free or on a context. */
#define TYPED(ctx, generic, NAME, OP, ...)                                          \
    do {                                                                            \
        if ((ctx) == SHMEM_CTX_DEFAULT) {                                           \
            if (generic)                                                            \
                shmem_##OP(__VA_ARGS__);                                            \
            else                                                                    \
                shmem_##NAME##_##OP(__VA_ARGS__);                                   \
        } else {                                                                    \
            if (generic)                                                            \
                shmem_##OP((ctx), __VA_ARGS__);                                     \
            else                                                                    \
                shmem_ctx_##NAME##_##OP((ctx), __VA_ARGS__);                        \
        }                                                                           \
    } while (0)

#define SIZED(ctx, OP, ...)                                                         \
    do {                                                                            \
        if ((ctx) == SHMEM_CTX_DEFAULT)                                             \
            shmem_##OP(__VA_ARGS__);                                                \
        else                                                                        \
            shmem_ctx_##OP((ctx), __VA_ARGS__);                                     \
    } while (0)

#define TYPED_KIND(T, NAME)                                                         \
    static void put_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                           size_t n, int pe)                                        \
    {                                                                               \
        TYPED(ctx, generic, NAME, put, (T *)dest, (const T *)source, n, pe);        \
    }                                                                               \
    static void get_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                           size_t n, int pe)                                        \
    {                                                                               \
        TYPED(ctx, generic, NAME, get, (T *)dest, (const T *)source, n, pe);        \
    }                                                                               \
    static void iput_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                            ptrdiff_t dst, ptrdiff_t sst, size_t n, int pe)         \
    {                                                                               \
        TYPED(ctx, generic, NAME, iput, (T *)dest, (const T *)source, dst, sst, n, pe); \
    }                                                                               \
    static void iget_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                            ptrdiff_t dst, ptrdiff_t sst, size_t n, int pe)         \
    {                                                                               \
        TYPED(ctx, generic, NAME, iget, (T *)dest, (const T *)source, dst, sst, n, pe); \
    }                                                                               \
    static void ibput_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                             ptrdiff_t dst, ptrdiff_t sst, size_t bsize, size_t nblocks, \
                             int pe)                                                \
    {                                                                               \
        TYPED(ctx, generic, NAME, ibput, (T *)dest, (const T *)source, dst, sst, bsize, \
              nblocks, pe);                                                         \
    }                                                                               \
    static void ibget_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                             ptrdiff_t dst, ptrdiff_t sst, size_t bsize, size_t nblocks, \
                             int pe)                                                \
    {                                                                               \
        TYPED(ctx, generic, NAME, ibget, (T *)dest, (const T *)source, dst, sst, bsize, \
              nblocks, pe);                                                         \
    }                                                                               \
    static void put_nbi_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                               size_t n, int pe)                                    \
    {                                                                               \
        TYPED(ctx, generic, NAME, put_nbi, (T *)dest, (const T *)source, n, pe);    \
    }                                                                               \
    static void get_nbi_##NAME(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                               size_t n, int pe)                                    \
    {                                                                               \
        TYPED(ctx, generic, NAME, get_nbi, (T *)dest, (const T *)source, n, pe);    \
    }                                                                               \
    static void p_##NAME(shmem_ctx_t ctx, int generic, void *dest, uint64_t value, int pe) \
    {                                                                               \
        TYPED(ctx, generic, NAME, p, (T *)dest, (T)value, pe);                      \
    }                                                                               \
    static uint64_t g_##NAME(shmem_ctx_t ctx, int generic, const void *source, int pe) \
    {                                                                               \
        T v;                                                                        \
        if (ctx == SHMEM_CTX_DEFAULT)                                               \
            v = generic ? shmem_g((const T *)source, pe)                            \
                        : shmem_##NAME##_g((const T *)source, pe);                  \
        else                                                                        \
            v = generic ? shmem_g(ctx, (const T *)source, pe)                       \
                        : shmem_ctx_##NAME##_g(ctx, (const T *)source, pe);         \
        return (uint64_t)v;                                                         \
    }                                                                               \
    static uint64_t load_##NAME(const void *slot)                                   \
    {                                                                               \
        return (uint64_t)*(const T *)slot;                                          \
    }                                                                               \
    static void store_##NAME(void *slot, uint64_t value)                            \
    {                                                                               \
        *(T *)slot = (T)value;                                                      \
    }                                                                               \
    static void put_signal_##NAME(shmem_ctx_t ctx, int generic, void *dest,         \
                                  const void *source, size_t n, uint64_t *signal,   \
                                  uint64_t value, int operation, int pe)            \
    {                                                                               \
        TYPED(ctx, generic, NAME, put_signal, (T *)dest, (const T *)source, n, signal, \
              value, operation, pe);                                                \
    }                                                                               \
    static void put_signal_nbi_##NAME(shmem_ctx_t ctx, int generic, void *dest,     \
                                      const void *source, size_t n, uint64_t *signal, \
                                      uint64_t value, int operation, int pe)        \
    {                                                                               \
        TYPED(ctx, generic, NAME, put_signal_nbi, (T *)dest, (const T *)source, n,  \
              signal, value, operation, pe);                                        \
    }                                                                               \
    static const struct rma_kind kind_##NAME = {                                    \
        #NAME, (int)sizeof(T), 1, put_##NAME, get_##NAME, iput_##NAME, iget_##NAME, \
        ibput_##NAME, ibget_##NAME, put_nbi_##NAME, get_nbi_##NAME, p_##NAME, g_##NAME, \
        load_##NAME, store_##NAME, put_signal_##NAME, put_signal_nbi_##NAME };

#define SIZED_KIND(BITS)                                                            \
    static void put_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                           size_t n, int pe)                                        \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, put##BITS, dest, source, n, pe);                                 \
    }                                                                               \
    static void get_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                           size_t n, int pe)                                        \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, get##BITS, dest, source, n, pe);                                 \
    }                                                                               \
    static void iput_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                            ptrdiff_t dst, ptrdiff_t sst, size_t n, int pe)         \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, iput##BITS, dest, source, dst, sst, n, pe);                      \
    }                                                                               \
    static void iget_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                            ptrdiff_t dst, ptrdiff_t sst, size_t n, int pe)         \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, iget##BITS, dest, source, dst, sst, n, pe);                      \
    }                                                                               \
    static void ibput_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                             ptrdiff_t dst, ptrdiff_t sst, size_t bsize, size_t nblocks, \
                             int pe)                                                \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, ibput##BITS, dest, source, dst, sst, bsize, nblocks, pe);        \
    }                                                                               \
    static void ibget_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                             ptrdiff_t dst, ptrdiff_t sst, size_t bsize, size_t nblocks, \
                             int pe)                                                \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, ibget##BITS, dest, source, dst, sst, bsize, nblocks, pe);        \
    }                                                                               \
    static void put_nbi_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                               size_t n, int pe)                                    \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, put##BITS##_nbi, dest, source, n, pe);                           \
    }                                                                               \
    static void get_nbi_##BITS(shmem_ctx_t ctx, int generic, void *dest, const void *source, \
                               size_t n, int pe)                                    \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, get##BITS##_nbi, dest, source, n, pe);                           \
    }                                                                               \
    static void put_signal_##BITS(shmem_ctx_t ctx, int generic, void *dest,         \
                                  const void *source, size_t n, uint64_t *signal,   \
                                  uint64_t value, int operation, int pe)            \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, put##BITS##_signal, dest, source, n, signal, value, operation, pe); \
    }                                                                               \
    static void put_signal_nbi_##BITS(shmem_ctx_t ctx, int generic, void *dest,     \
                                      const void *source, size_t n, uint64_t *signal, \
                                      uint64_t value, int operation, int pe)        \
    {                                                                               \
        (void)generic;                                                              \
        SIZED(ctx, put##BITS##_signal_nbi, dest, source, n, signal, value, operation, pe); \
    }                                                                               \
    static const struct rma_kind kind_##BITS = {                                    \
        "size" #BITS, BITS / 8, 0, put_##BITS, get_##BITS, iput_##BITS, iget_##BITS, \
        ibput_##BITS, ibget_##BITS, put_nbi_##BITS, get_nbi_##BITS, NULL, NULL, NULL, NULL, \
        put_signal_##BITS, put_signal_nbi_##BITS };

/* Table 5, as the specification lists it (§9.6), and the sized forms' sizes. */
#define RMA_TYPES(X)                                                                \
    X(float, float) X(double, double) X(long double, longdouble) X(char, char)       \
    X(signed char, schar) X(short, short) X(int, int) X(long, long)                 \
    X(long long, longlong) X(unsigned char, uchar) X(unsigned short, ushort)        \
    X(unsigned int, uint) X(unsigned long, ulong) X(unsigned long long, ulonglong)  \
    X(int8_t, int8) X(int16_t, int16) X(int32_t, int32) X(int64_t, int64)          \
    X(uint8_t, uint8) X(uint16_t, uint16) X(uint32_t, uint32) X(uint64_t, uint64)   \
    X(size_t, size) X(ptrdiff_t, ptrdiff)
#define RMA_SIZES(X) X(8) X(16) X(32) X(64) X(128)

RMA_TYPES(TYPED_KIND)
RMA_SIZES(SIZED_KIND)

#define TYPED_ENTRY(T, NAME) &kind_##NAME,
#define SIZED_ENTRY(BITS) &kind_##BITS,
static const struct rma_kind *const kinds[] = { RMA_TYPES(TYPED_ENTRY) RMA_SIZES(SIZED_ENTRY) };
enum { KINDS = sizeof kinds / sizeof *kinds, TYPED_KINDS = 24 };

/* The cases: four classes over every kind, p and g over the typed ones, then
 * put_signal and put_signal_nbi over every kind - after the others, so that
 * the first 140 cases keep their rounds. */
enum { SIGNAL_FIRST = 4 * KINDS + TYPED_KINDS, CASE_COUNT = SIGNAL_FIRST + 2 * KINDS };

static struct {
    /* the plan */
    uint64_t seed;
    size_t halo_most;               /* the largest halo, bytes */
    int plant_halo, plant_put, plant_gap, plant_get, plant_get_gap, plant_ptr, plant_flag,
        plant_single, plant_ptr_null, plant_signal;
    /* the team */
    shmem_team_t team;
    int n, me, me_world, next, previous;    /* team ranks */
    int next_world, previous_world;
    int local_rank, local_count;            /* this PE on its node */
    int *mates;                     /* world PEs of this node, by local rank */
    shmem_ctx_t context;            /* a private one, for odd passes */
    /* symmetric */
    unsigned char *halo;            /* [halo_most], written by the previous PE */
    uint64_t *flag;                 /* the halo's, raised by the previous PE */
    uint64_t *signal;               /* a put_signal case's, set by the previous PE */
    unsigned char *inbox;           /* [REGION], written by the previous PE */
    unsigned char *source;          /* [REGION], read by the next PE */
    uint64_t *ptr_slots;            /* [local_most][PTR_WORDS], by the writer's local rank */
    uint64_t *ptr_flags;            /* [local_most] */
    /* private */
    unsigned char *outgoing, *incoming, *halo_out;
    int local_most;
    long round;
    double bytes, transfers, phase[4];
} rma;

static uint64_t rma_memory(int npes, const struct pacer_options *options, char *fields,
                           size_t size)
{
    const struct pacer_layout *layout = pacer_layout();
    int local_most = 1;
    for (int p = 0; p < layout->pes; p++)
        if (layout->local[p] + 1 > local_most)
            local_most = layout->local[p] + 1;
    uint64_t fixed = 2 * REGION + (uint64_t)local_most * (PTR_WORDS + 1) * 8 + 16;
    size_t halo = 1 << 20;
    while (halo > 8 && fixed + halo > options->memory)
        halo /= 8;
    if (fixed + halo > options->memory) {
        snprintf(fields, size, "%d PEs' regions need %llu bytes", npes,
                 (unsigned long long)(fixed + halo));
        return UINT64_MAX;
    }
    rma.halo_most = halo;
    rma.local_most = local_most;
    rma.seed = options->seed;
    rma.plant_halo = options->plant == PACER_PLANT_RMA_HALO;
    rma.plant_put = options->plant == PACER_PLANT_RMA_PUT;
    rma.plant_gap = options->plant == PACER_PLANT_RMA_GAP;
    rma.plant_get = options->plant == PACER_PLANT_RMA_GET;
    rma.plant_get_gap = options->plant == PACER_PLANT_RMA_GET_GAP;
    rma.plant_ptr = options->plant == PACER_PLANT_RMA_PTR;
    rma.plant_flag = options->plant == PACER_PLANT_RMA_FLAG;
    rma.plant_single = options->plant == PACER_PLANT_RMA_SINGLE;
    rma.plant_ptr_null = options->plant == PACER_PLANT_RMA_PTR_NULL;
    rma.plant_signal = options->plant == PACER_PLANT_RMA_SIGNAL;
    char halo_text[32];
    pacer_size(halo_text, sizeof halo_text, halo);
    snprintf(fields, size, "halo=8..%s cases=%d kinds=%d flags=atomic_add,signal_add,signal_set "
             "puts=putmem,putmem_nbi,uint64_put,putmem_signal,putmem_signal_nbi", halo_text,
             CASE_COUNT, KINDS);
    return fixed + halo;
}

/* Onto a team, afresh: its ring, and the node-mates in it - a dense list,
 * which a strided team's local ranks are not. The rounds carry on from the
 * last period, alike on every member of a team, since a team's members run
 * its periods together: so the halo's flag, which holds the rounds done and
 * gains one a round, is set to them; the node-mates' flags are set outright
 * each round, to the round plus one, above whatever an earlier period left.
 * Nothing is in flight, the last period having ended in a quiet and a sync. */
static int rma_rebind(shmem_team_t team, const struct pacer_options *options)
{
    int n = shmem_team_n_pes(team);
    const struct pacer_layout *layout = pacer_layout();
    rma.team = team;
    rma.n = n;
    rma.me = shmem_team_my_pe(team);
    rma.seed = options->seed;
    rma.next = (rma.me + 1) % n;
    rma.previous = (rma.me + n - 1) % n;
    rma.next_world = shmem_team_translate_pe(team, rma.next, SHMEM_TEAM_WORLD);
    rma.previous_world = shmem_team_translate_pe(team, rma.previous, SHMEM_TEAM_WORLD);
    rma.local_count = 0;
    for (int p = 0; p < layout->pes; p++)
        if (layout->node[p] == layout->node[rma.me_world] &&
            shmem_team_translate_pe(SHMEM_TEAM_WORLD, p, team) >= 0)
            rma.mates[rma.local_count++] = p;
    shmem_uint64_atomic_set(rma.flag, (uint64_t)rma.round, rma.me_world);
    shmem_uint64_atomic_set(rma.signal, 0, rma.me_world);
    for (int l = 0; l < rma.local_most; l++)
        rma.ptr_flags[l] = 0;
    rma.bytes = rma.transfers = 0;
    memset(rma.phase, 0, sizeof rma.phase);
    shmem_team_sync(team);
    return 0;
}

static int rma_setup(shmem_team_t team, const struct pacer_options *options)
{
    const struct pacer_layout *layout = pacer_layout();
    rma.me_world = shmem_my_pe();
    rma.local_rank = layout->local[rma.me_world];
    rma.mates = malloc(sizeof *rma.mates * (size_t)rma.local_most);
    if (!rma.mates)
        return -1;
    if (shmem_ctx_create(SHMEM_CTX_PRIVATE, &rma.context) != 0)
        return -1;
    rma.halo = shmem_malloc(rma.halo_most);
    rma.flag = shmem_calloc(1, sizeof *rma.flag);
    rma.signal = shmem_calloc(1, sizeof *rma.signal);
    rma.inbox = shmem_malloc(REGION);
    rma.source = shmem_malloc(REGION);
    rma.ptr_slots = shmem_calloc((size_t)rma.local_most * PTR_WORDS, sizeof *rma.ptr_slots);
    rma.ptr_flags = shmem_calloc((size_t)rma.local_most, sizeof *rma.ptr_flags);
    rma.outgoing = malloc(REGION);
    rma.incoming = malloc(REGION);
    rma.halo_out = malloc(rma.halo_most);
    if (!rma.halo || !rma.flag || !rma.signal || !rma.inbox || !rma.source || !rma.ptr_slots ||
        !rma.ptr_flags || !rma.outgoing || !rma.incoming || !rma.halo_out)
        return -1;
    rma.round = 0;     /* and on through every rebind: mix's periods */
    return rma_rebind(team, options);
}

/* Byte i of what `from` sends `to` in a round, i counted from the start. */
static unsigned char pattern_byte(int from, int to, uint64_t i, long round)
{
    uint64_t word = random_mix(rma.seed ^ random_name("rma"), (uint64_t)from,
                               ((uint64_t)to << 32) | (i >> 3), (uint64_t)round);
    return (unsigned char)(word >> (8 * (i & 7)));
}

static void fill(unsigned char *into, size_t bytes, int from, int to, long round)
{
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t word = random_mix(rma.seed ^ random_name("rma"), (uint64_t)from,
                                   ((uint64_t)to << 32) | (i >> 3), (uint64_t)round);
        size_t k = bytes - i < 8 ? bytes - i : 8;
        memcpy(into + i, &word, k);
    }
}

/* The first byte of `bytes` that is not the pattern's, or -1. */
static long mismatch(const unsigned char *have, size_t bytes, int from, int to, long round)
{
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t word = random_mix(rma.seed ^ random_name("rma"), (uint64_t)from,
                                   ((uint64_t)to << 32) | (i >> 3), (uint64_t)round);
        size_t k = bytes - i < 8 ? bytes - i : 8;
        if (memcmp(have + i, &word, k) != 0)
            for (size_t j = 0; j < k; j++)
                if (have[i + j] != (unsigned char)(word >> (8 * j)))
                    return (long)(i + j);
    }
    return -1;
}

static void quiet_on(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_DEFAULT)
        shmem_quiet();
    else
        shmem_ctx_quiet(ctx);
}

/* ---- the halo ---------------------------------------------------------------- */

static void halo(long round, shmem_ctx_t ctx, const char *context)
{
    static const size_t sizes[HALO_SIZES] = { 8, 64, 512, 4096, 32768, 262144, 1048576 };
    size_t bytes = sizes[round % HALO_SIZES];
    if (bytes > rma.halo_most)
        bytes = rma.halo_most;
    int method = (int)(round % FLAG_METHODS), put = (int)((round / FLAG_METHODS) % HALO_PUTS);
    double started = pacer_now();
    fill(rma.halo_out, bytes, rma.me_world, rma.next_world, round);
    if (rma.plant_halo && rma.me_world == 1) {
        rma.halo_out[bytes / 2] ^= 1;       /* the planted defect: one byte wrong */
        rma.plant_halo = 0;
    }
    if (put == 0)
        ctx == SHMEM_CTX_DEFAULT ? shmem_putmem(rma.halo, rma.halo_out, bytes, rma.next_world)
                                 : shmem_ctx_putmem(ctx, rma.halo, rma.halo_out, bytes,
                                                    rma.next_world);
    else if (put == 1)
        ctx == SHMEM_CTX_DEFAULT ? shmem_putmem_nbi(rma.halo, rma.halo_out, bytes, rma.next_world)
                                 : shmem_ctx_putmem_nbi(ctx, rma.halo, rma.halo_out, bytes,
                                                        rma.next_world);
    else if (put == 2 && ctx == SHMEM_CTX_DEFAULT)
        shmem_uint64_put((uint64_t *)rma.halo, (const uint64_t *)rma.halo_out, bytes / 8,
                         rma.next_world);
    else if (put == 2)
        shmem_ctx_uint64_put(ctx, (uint64_t *)rma.halo, (const uint64_t *)rma.halo_out,
                             bytes / 8, rma.next_world);
    uint64_t raised = (uint64_t)round + 1;
    if (put >= 3) {
        /* The data and the flag in one call: the flag set to the rounds
         * done, or added one to them, by the signal itself. */
        if (method == ATOMIC_ADD)
            method = SIGNAL_ADD;
        int operation = method == SIGNAL_SET ? SHMEM_SIGNAL_SET : SHMEM_SIGNAL_ADD;
        uint64_t value = method == SIGNAL_SET ? raised : 1;
        if (put == 3 && ctx == SHMEM_CTX_DEFAULT)
            shmem_putmem_signal(rma.halo, rma.halo_out, bytes, rma.flag, value, operation,
                                rma.next_world);
        else if (put == 3)
            shmem_ctx_putmem_signal(ctx, rma.halo, rma.halo_out, bytes, rma.flag, value,
                                    operation, rma.next_world);
        else if (ctx == SHMEM_CTX_DEFAULT)
            shmem_putmem_signal_nbi(rma.halo, rma.halo_out, bytes, rma.flag, value, operation,
                                    rma.next_world);
        else
            shmem_ctx_putmem_signal_nbi(ctx, rma.halo, rma.halo_out, bytes, rma.flag, value,
                                        operation, rma.next_world);
        if (put == 4)               /* a nonblocking one's delivery waits for its quiet */
            quiet_on(ctx);
    } else if (ctx == SHMEM_CTX_DEFAULT) {
        shmem_fence();
        if (method == ATOMIC_ADD)
            shmem_uint64_atomic_add(rma.flag, 1, rma.next_world);
        else if (method == SIGNAL_ADD)
            shmem_signal_add(rma.flag, 1, rma.next_world);
        else
            shmem_signal_set(rma.flag, raised, rma.next_world);
    } else {
        shmem_ctx_fence(ctx);
        if (method == ATOMIC_ADD)
            shmem_ctx_uint64_atomic_add(ctx, rma.flag, 1, rma.next_world);
        else if (method == SIGNAL_ADD)
            shmem_ctx_signal_add(ctx, rma.flag, 1, rma.next_world);
        else
            shmem_ctx_signal_set(ctx, rma.flag, raised, rma.next_world);
    }
    if (round % 2)
        shmem_signal_wait_until(rma.flag, SHMEM_CMP_GE, raised);
    else
        shmem_uint64_wait_until(rma.flag, SHMEM_CMP_GE, raised);
    rma.phase[0] += pacer_now() - started;

    started = pacer_now();
    uint64_t flag = shmem_signal_fetch(rma.flag);
    if (rma.plant_flag && rma.me_world == 1) {
        raised++;                   /* the planted defect: the flag misread once */
        rma.plant_flag = 0;
    }
    if (flag != raised)
        pacer_failure("rma round=%ld halo %s %s: the flag reads %llu, expected %llu", round,
                      flag_names[method], context, (unsigned long long)flag,
                      (unsigned long long)raised);
    size_t checked = put == 2 ? bytes / 8 * 8 : bytes;
    long at = mismatch(rma.halo, checked, rma.previous_world, rma.me_world, round);
    if (at >= 0)
        pacer_failure("rma round=%ld halo %s %s %s: byte %ld of %zu from PE %d is %02x, not "
                      "%02x, when the flag said it had landed", round, put_names[put],
                      flag_names[method], context, at, checked, rma.previous_world,
                      rma.halo[at],
                      pattern_byte(rma.previous_world, rma.me_world, (uint64_t)at, round));
    rma.bytes += (double)bytes;
    rma.phase[3] += pacer_now() - started;
}

/* ---- a transfer ----------------------------------------------------------------- */

/* Where a transfer puts element k, and which element of the stream it is: the
 * stream's elements lie contiguous in `outgoing` and in `source`. */
struct transfer_layout {
    size_t count;                   /* elements moved */
    ptrdiff_t dst, sst;             /* strides, in elements; blocks: block strides */
    size_t bsize;                   /* elements a block; 0 but for blocks */
};

static void place(const struct transfer_layout *layout, size_t k, size_t *at, size_t *from)
{
    if (layout->bsize) {
        size_t block = k / layout->bsize, e = k % layout->bsize;
        *at = block * (size_t)layout->dst + e;
        *from = block * (size_t)layout->sst + e;
    } else {
        *at = k * (size_t)layout->dst;
        *from = k * (size_t)layout->sst;
    }
}

/* The first element place a strided or blocked transfer leaves empty: after
 * element 0 when the stride is more than one, after the first block. 0 when
 * there is none. */
static size_t first_gap(enum transfer_class form, const struct transfer_layout *layout)
{
    if (form == STRIDED)
        return layout->dst > 1 ? 1 : 0;
    if (form == BLOCKS)
        return (size_t)layout->dst > layout->bsize ? layout->bsize : 0;
    return 0;
}

static void plan(enum transfer_class form, long round, int size, struct transfer_layout *layout)
{
    memset(layout, 0, sizeof *layout);
    switch (form) {
    case STRIDED:
        layout->count = 24;
        layout->dst = 1 + round % 3;
        layout->sst = 1 + (round / 3) % 3;
        break;
    case BLOCKS:
        layout->bsize = 1 + (size_t)(round % 4);
        layout->count = 6 * layout->bsize;                    /* six blocks */
        layout->dst = (ptrdiff_t)layout->bsize + 1 + round % 2;
        layout->sst = (ptrdiff_t)layout->bsize + (round / 2) % 2;
        break;
    case SINGLE:
        layout->count = 8;
        layout->dst = layout->sst = 1;
        break;
    default:                        /* contiguous and nonblocking */
        layout->count = 32 + (size_t)(round % 64);
        layout->dst = layout->sst = 1;
        break;
    }
    (void)size;
}

/* A value p and g carry: a small whole number every type holds exactly. */
static uint64_t value_of(int from, int to, size_t k, long round, int size)
{
    uint64_t limit = size == 1 ? 100 : size == 2 ? 30000 : UINT64_C(1) << 20;
    return 1 + random_mix(rma.seed ^ random_name("rma values"), (uint64_t)from,
                          ((uint64_t)to << 32) | k, (uint64_t)round) % limit;
}

/* What `into` must hold after a transfer from `from` to `to`: the canary but
 * where an element lands. Returns the first byte past the last element. */
static size_t expect(unsigned char *into, unsigned char *owned, const struct rma_kind *kind,
                     enum transfer_class form, const struct transfer_layout *layout, int from, int to, long round)
{
    size_t w = (size_t)kind->size, end = 0;
    memset(into, CANARY, REGION);
    memset(owned, 0, REGION);
    for (size_t e = 0; e < layout->count; e++) {
        size_t at, source;
        place(layout, e, &at, &source);
        if (form == SINGLE) {
            kind->store(into + at * w, value_of(from, to, e, round, kind->size));
        } else {
            for (size_t b = 0; b < w; b++)
                into[at * w + b] = pattern_byte(from, to, source * w + b, round);
        }
        memset(owned + at * w, 1, w);
        if ((at + 1) * w > end)
            end = (at + 1) * w;
    }
    return end;
}

static unsigned char *expected_bytes, *owned_bytes;

/* The first byte of `have` that is not `want`; which kind of place it is. p
 * and g carry values, compared as values: a long double's padding bytes are
 * nobody's. */
static void compare(const unsigned char *have, long round, const char *what,
                    const struct rma_kind *kind, enum transfer_class form, const char *context, int from)
{
    size_t w = (size_t)kind->size;
    for (size_t i = 0; i < REGION; i++) {
        if (form == SINGLE && owned_bytes[i] && i % w == 0) {
            uint64_t got = kind->load(have + i), want = kind->load(expected_bytes + i);
            if (got != want) {
                pacer_failure("rma round=%ld %s %s %s %s: element %zu from PE %d is %llu, not "
                              "%llu", round, class_names[form], kind->name, context, what, i / w,
                              from, (unsigned long long)got, (unsigned long long)want);
                return;
            }
            i += w - 1;
            continue;
        }
        if (have[i] != expected_bytes[i]) {
            if (owned_bytes[i])
                pacer_failure("rma round=%ld %s %s %s %s: byte %zu, of element %zu from PE %d, "
                              "is %02x, not %02x", round, class_names[form], kind->name, context,
                              what, i, i / (size_t)kind->size, from, have[i], expected_bytes[i]);
            else
                pacer_failure("rma round=%ld %s %s %s %s: byte %zu, where no element lands, is "
                              "%02x, not the canary", round, class_names[form], kind->name, context,
                              what, i, have[i]);
            return;
        }
    }
}

static void transfer_case(long round, enum transfer_class *form, const struct rma_kind **kind)
{
    int index = (int)(round % CASE_COUNT);
    if (index < 4 * KINDS) {
        *form = (enum transfer_class)(index / KINDS);
        *kind = kinds[index % KINDS];
    } else if (index < SIGNAL_FIRST) {
        *form = SINGLE;
        *kind = kinds[index - 4 * KINDS];
    } else {
        *form = (enum transfer_class)(SIGNAL + (index - SIGNAL_FIRST) / KINDS);
        *kind = kinds[(index - SIGNAL_FIRST) % KINDS];
    }
}

/* A put_signal case's signal: set to the round plus one, or added one to the
 * zero its receiver left, by the case and the pass - alike at both ends. */
static void signal_of(long round, int *operation, uint64_t *value)
{
    long pass = round / CASE_COUNT, index = round % CASE_COUNT;
    *operation = (index + pass) % 2 ? SHMEM_SIGNAL_SET : SHMEM_SIGNAL_ADD;
    *value = *operation == SHMEM_SIGNAL_SET ? (uint64_t)round + 1 : 1;
}

/* A put_signal case at its receiver: the signal, then every byte of the
 * inbox, read before any sync - data the signal overtook shows here - and the
 * signal back to zero for the next case, before the round's syncs. */
static void signaled(long round, const struct rma_kind *kind, enum transfer_class form,
                     const struct transfer_layout *layout, const char *context)
{
    int operation;
    uint64_t want;
    signal_of(round, &operation, &want);
    shmem_signal_wait_until(rma.signal, SHMEM_CMP_NE, 0);
    uint64_t have = shmem_signal_fetch(rma.signal);
    if (rma.plant_signal && rma.me_world == 1) {
        have++;                     /* the planted defect: the signal misread once */
        rma.plant_signal = 0;
    }
    if (have != want)
        pacer_failure("rma round=%ld %s %s %s: the signal from PE %d reads %llu, expected %llu",
                      round, class_names[form], kind->name, context, rma.previous_world,
                      (unsigned long long)have, (unsigned long long)want);
    expect(expected_bytes, owned_bytes, kind, form, layout, rma.previous_world, rma.me_world,
           round);
    compare(rma.inbox, round, "put, at its signal,", kind, form, context, rma.previous_world);
    shmem_uint64_atomic_set(rma.signal, 0, rma.me_world);
}

/* The round's sources: this PE's `source`, which the next PE reads, and its
 * `outgoing`, which it puts; its inbox cleared to the canary. */
static void prepare(long round, enum transfer_class form, const struct rma_kind *kind)
{
    size_t w = (size_t)kind->size;
    if (form == SINGLE) {
        memset(rma.source, CANARY, REGION);
        for (size_t e = 0; e < 8; e++)
            kind->store(rma.source + e * w, value_of(rma.me_world, rma.next_world, e, round,
                                                      kind->size));
    } else {
        fill(rma.source, REGION, rma.me_world, rma.next_world, round);
    }
    if (rma.plant_get && rma.me_world == 1) {
        rma.source[0] ^= 1;         /* the planted defect: a byte of what is read */
        rma.plant_get = 0;
    }
    fill(rma.outgoing, REGION, rma.me_world, rma.next_world, round);
    memset(rma.inbox, CANARY, REGION);
}

static void transfer(long round, shmem_ctx_t ctx, const char *context)
{
    enum transfer_class form;
    const struct rma_kind *kind;
    transfer_case(round, &form, &kind);
    /* Half the cases start from the generic, and a case's spelling flips
     * every second pass, its context every pass (rma_step). */
    long pass = round / CASE_COUNT, index = round % CASE_COUNT;
    int generic = kind->typed && (index / 2 + pass / 2) % 2;
    size_t w = (size_t)kind->size;
    struct transfer_layout layout;
    plan(form, round, kind->size, &layout);
    double started = pacer_now();

    /* The put side, into the next PE's inbox. */
    if (rma.plant_put && rma.me_world == 1 && form != SINGLE) {
        rma.outgoing[0] ^= 1;       /* the planted defect: one element sent wrong */
        rma.plant_put = 0;
    }
    int next = rma.next_world, previous = rma.previous_world;
    switch (form) {
    case CONTIGUOUS:
        kind->put(ctx, generic, rma.inbox, rma.outgoing, layout.count, next);
        break;
    case STRIDED:
        kind->iput(ctx, generic, rma.inbox, rma.outgoing, layout.dst, layout.sst, layout.count, next);
        break;
    case BLOCKS:
        kind->ibput(ctx, generic, rma.inbox, rma.outgoing, layout.dst, layout.sst, layout.bsize,
                    layout.count / layout.bsize, next);
        break;
    case NONBLOCKING:
        for (size_t first = 0; first < layout.count; first += layout.count / 4 + 1) {
            size_t chunk = layout.count - first < layout.count / 4 + 1 ? layout.count - first : layout.count / 4 + 1;
            kind->put_nbi(ctx, generic, rma.inbox + first * w, rma.outgoing + first * w, chunk,
                          next);
        }
        break;
    case SINGLE:
        for (size_t e = 0; e < layout.count; e++) {
            uint64_t v = value_of(rma.me_world, next, e, round, kind->size);
            if (rma.plant_single && rma.me_world == 1) {
                v++;                /* the planted defect: one value sent wrong */
                rma.plant_single = 0;
            }
            kind->p(ctx, generic, rma.inbox + e * w, v, next);
        }
        break;
    case SIGNAL:
    case SIGNAL_NBI: {
        int operation;
        uint64_t value;
        signal_of(round, &operation, &value);
        if (form == SIGNAL) {
            kind->put_signal(ctx, generic, rma.inbox, rma.outgoing, layout.count, rma.signal,
                             value, operation, next);
        } else {
            kind->put_signal_nbi(ctx, generic, rma.inbox, rma.outgoing, layout.count,
                                 rma.signal, value, operation, next);
            quiet_on(ctx);          /* a nonblocking one's delivery waits for its quiet */
        }
        break;
    }
    default:
        break;
    }
    if (rma.plant_gap && rma.me_world == 1 && first_gap(form, &layout)) {
        /* The planted defect: a byte into the first gap of the next PE's inbox. */
        unsigned char stray = (unsigned char)~CANARY;
        shmem_putmem(rma.inbox + first_gap(form, &layout) * w, &stray, 1, next);
        rma.plant_gap = 0;
    }

    /* The get side, from the previous PE's source. */
    memset(rma.incoming, CANARY, REGION);
    switch (form) {
    case CONTIGUOUS:
    case SIGNAL:
        kind->get(ctx, !generic && kind->typed, rma.incoming, rma.source, layout.count, previous);
        break;
    case STRIDED:
        kind->iget(ctx, !generic && kind->typed, rma.incoming, rma.source, layout.dst, layout.sst, layout.count,
                   previous);
        break;
    case BLOCKS:
        kind->ibget(ctx, !generic && kind->typed, rma.incoming, rma.source, layout.dst, layout.sst,
                    layout.bsize, layout.count / layout.bsize, previous);
        break;
    case NONBLOCKING:
    case SIGNAL_NBI:
        for (size_t first = 0; first < layout.count; first += 8) {
            size_t chunk = layout.count - first < 8 ? layout.count - first : 8;
            kind->get_nbi(ctx, !generic && kind->typed, rma.incoming + first * w,
                          rma.source + first * w, chunk, previous);
        }
        break;
    case SINGLE:
        for (size_t e = 0; e < layout.count; e++)
            kind->store(rma.incoming + e * w,
                        kind->g(ctx, !generic, rma.source + e * w, previous));
        break;
    default:
        break;
    }
    if (form == SIGNAL || form == SIGNAL_NBI)
        signaled(round, kind, form, &layout, context);
    if (form == NONBLOCKING && round % 2) {
        int both[2] = { next, previous };
        if (ctx == SHMEM_CTX_DEFAULT)
            shmem_pe_quiet(both, next == previous ? 1 : 2);
        else
            shmem_ctx_pe_quiet(ctx, both, next == previous ? 1 : 2);
    } else {
        quiet_on(ctx);
    }
    rma.transfers += 2;
    rma.bytes += 2.0 * (double)(layout.count * w);
    rma.phase[1] += pacer_now() - started;

    started = pacer_now();
    if (rma.plant_get_gap && rma.me_world == 1 && first_gap(form, &layout)) {
        rma.incoming[first_gap(form, &layout) * w] = (unsigned char)~CANARY;   /* the planted defect */
        rma.plant_get_gap = 0;
    }
    expect(expected_bytes, owned_bytes, kind, form, &layout, previous, rma.me_world, round);
    compare(rma.incoming, round, "get", kind, form, context, previous);
    rma.phase[3] += pacer_now() - started;
}

/* After the sync: the inbox, as the previous PE's put side left it. */
static void check_inbox(long round, const char *context)
{
    enum transfer_class form;
    const struct rma_kind *kind;
    transfer_case(round, &form, &kind);
    struct transfer_layout layout;
    plan(form, round, kind->size, &layout);
    double started = pacer_now();
    expect(expected_bytes, owned_bytes, kind, form, &layout, rma.previous_world, rma.me_world, round);
    compare(rma.inbox, round, "put", kind, form, context, rma.previous_world);
    rma.phase[3] += pacer_now() - started;
}

/* ---- shmem_ptr --------------------------------------------------------------------- */

static uint64_t ptr_word(int from, int to, int word, long round)
{
    return random_mix(rma.seed ^ random_name("rma ptr"), (uint64_t)from,
                      ((uint64_t)to << 32) | (uint64_t)word, (uint64_t)round);
}

static void pointers(long round, int source_is_pattern)
{
    if (rma.local_count < 2)
        return;
    double started = pacer_now();
    uint64_t raised = (uint64_t)round + 1;
    for (int l = 0; l < rma.local_count; l++) {
        int mate = rma.mates[l];
        if (mate == rma.me_world)
            continue;
        uint64_t *slot = shmem_ptr(rma.ptr_slots + (size_t)rma.local_rank * PTR_WORDS, mate);
        int null = !slot;
        if (rma.plant_ptr_null && rma.me_world == 1) {
            null = 1;               /* the planted defect: a pointer taken for NULL */
            rma.plant_ptr_null = 0;
        }
        if (null)
            pacer_failure("rma round=%ld ptr: shmem_ptr to node-mate PE %d is NULL", round, mate);
        /* Stores where there is a pointer; the flag always, so the node-mate
         * never waits for what cannot come - its check then says what is missing. */
        if (slot) {
            for (int i = 0; i < PTR_WORDS; i++)
                slot[i] = ptr_word(rma.me_world, mate, i, round);
            if (rma.plant_ptr && rma.me_world == 1) {
                slot[PTR_WORDS / 2] ^= 1;   /* the planted defect: one word stored wrong */
                rma.plant_ptr = 0;
            }
        }
        shmem_fence();
        shmem_uint64_atomic_set(&rma.ptr_flags[rma.local_rank], raised, mate);
    }
    const struct pacer_layout *layout = pacer_layout();
    for (int l = 0; l < rma.local_count; l++) {
        int mate = rma.mates[l], rank = layout->local[mate];
        if (mate == rma.me_world)
            continue;
        shmem_uint64_wait_until(&rma.ptr_flags[rank], SHMEM_CMP_GE, raised);
        const uint64_t *slot = rma.ptr_slots + (size_t)rank * PTR_WORDS;
        for (int i = 0; i < PTR_WORDS; i++)
            if (slot[i] != ptr_word(mate, rma.me_world, i, round)) {
                pacer_failure("rma round=%ld ptr: word %d node-mate PE %d stored is %016llx, "
                              "not %016llx, when its flag said it was there", round, i, mate,
                              (unsigned long long)slot[i],
                              (unsigned long long)ptr_word(mate, rma.me_world, i, round));
                break;
            }
    }
    /* Loads from each node-mate's source, as it filled it this round. */
    if (source_is_pattern)
        for (int l = 0; l < rma.local_count; l++) {
            int mate = rma.mates[l];
            if (mate == rma.me_world)
                continue;
            const unsigned char *there = shmem_ptr(rma.source, mate);
            if (!there)
                continue;           /* reported above */
            int mate_rank = shmem_team_translate_pe(SHMEM_TEAM_WORLD, mate, rma.team);
            int mate_next = shmem_team_translate_pe(rma.team, (mate_rank + 1) % rma.n,
                                                    SHMEM_TEAM_WORLD);
            long at = mismatch(there, 4096, mate, mate_next, round);
            if (at >= 0)
                pacer_failure("rma round=%ld ptr: byte %ld loaded from node-mate PE %d's source "
                              "is %02x, not %02x", round, at, mate, there[at],
                              pattern_byte(mate, mate_next, (uint64_t)at, round));
        }
    rma.phase[2] += pacer_now() - started;
}

/* ---- the exercise ------------------------------------------------------------------ */

static void rma_step(long units, int last)
{
    (void)last;
    for (long u = 0; u < units; u++, rma.round++) {
        long round = rma.round;
        int odd_pass = (int)((round / CASE_COUNT) % 2);
        shmem_ctx_t ctx = odd_pass ? rma.context : SHMEM_CTX_DEFAULT;
        const char *context = odd_pass ? "private" : "default";
        enum transfer_class form;
        const struct rma_kind *kind;
        transfer_case(round, &form, &kind);
        prepare(round, form, kind);
        shmem_team_sync(rma.team);
        halo(round, ctx, context);
        transfer(round, ctx, context);
        shmem_quiet();
        shmem_team_sync(rma.team);
        check_inbox(round, context);
        pointers(round, form != SINGLE);
        shmem_team_sync(rma.team);  /* every check done before the next round writes */
    }
}

static void rma_checkpoint(struct pacer_counters *counters)
{
    counters->work = rma.transfers;
    counters->bytes = rma.bytes;
    for (int k = 0; k < 4; k++)
        counters->phase_seconds[k] = rma.phase[k];
    rma.transfers = rma.bytes = 0;
    memset(rma.phase, 0, sizeof rma.phase);
}

static void rma_teardown(void)
{
    shmem_quiet();
    shmem_team_sync(rma.team);
    shmem_ctx_destroy(rma.context);
    shmem_free(rma.ptr_flags);
    shmem_free(rma.ptr_slots);
    shmem_free(rma.source);
    shmem_free(rma.inbox);
    shmem_free(rma.signal);
    shmem_free(rma.flag);
    shmem_free(rma.halo);
    free(rma.halo_out);
    free(rma.incoming);
    free(rma.outgoing);
    free(rma.mates);
    free(expected_bytes);
    free(owned_bytes);
    expected_bytes = owned_bytes = NULL;
}

static int rma_setup_checks(shmem_team_t team, const struct pacer_options *options)
{
    expected_bytes = malloc(REGION);
    owned_bytes = malloc(REGION);
    if (!expected_bytes || !owned_bytes)
        return -1;
    return rma_setup(team, options);
}

const struct pacer_exercise pacer_rma = {
    .name = "rma",
    .work = "transfers",
    .phases = phase_names,
    .quick_rounds = CASE_COUNT,         /* every transfer case once */
    .memory = rma_memory,
    .setup = rma_setup_checks,
    .step = rma_step,
    .checkpoint = rma_checkpoint,
    .teardown = rma_teardown,
    .rebind = rma_rebind,
};
