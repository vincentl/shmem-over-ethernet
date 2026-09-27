/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * atomics - every atomic memory operation of Tables 6 to 8, on every type, on
 * three contexts, checked by oracles that do not depend on the order in which
 * the operations landed (proposal §4.5; docs/prompts/task-stress-phase3.md).
 *
 * **A round is a case.** Six families of operation, each over its table's
 * types - 71 cases - and a round runs one of them on every PE at once:
 *
 *   add          add and inc (Table 6) at random slots of random PEs; each
 *                slot must then hold its start plus what every PE sent it,
 *                which every PE recomputes from every PE's seeded stream, so
 *                no exchange is needed to know what a slot should hold
 *   fetch_add    fetch_add of one and fetch_inc: the values a slot handed out
 *                are then a permutation of its start to its start plus its
 *                count, whatever the order. Each issuer adds what it fetched,
 *                less the start, and a hash of it, into two 64-bit words at
 *                the slot's PE, which must then hold the closed forms
 *   claim        compare_swap as bale's randperm uses it: each PE claims 16
 *                empty slots of a table twice that size, spread over every
 *                PE; afterwards each claim still holds its token, and the
 *                table holds exactly N x 16 claims
 *   swap         swap (Table 7) passing tokens between each PE's hands and
 *                the table: the multiset of values is conserved, so their
 *                sum and hash sum over the team are the start's
 *   set_fetch    set, one writer a slot, then fetch it back from the far end
 *                and read it at the near one
 *   bitwise      and, or and xor (Table 8): each PE owns bit (PE mod width);
 *                it ors its bit into a set of slots, xors it, then ands it
 *                away, and after each pass every slot holds what the passes
 *                imply. Where a bit has one owner - N no wider than the type -
 *                what a fetching form returns has that bit predicted too
 *
 * Round r runs case r mod 71, on context r mod 3 - the default, a private
 * one, a serialized one - with the blocking fetching forms on even rounds and
 * the nonblocking ones (completed by a quiet) on odd, so 426 rounds run every
 * combination once and the quick run's 71 every case once. Within a round the
 * operations alternate between the C11 generic and the type's own name: a
 * generic cannot reach the routine of a type that is another's typedef -
 * int32_t is int on LP64, size_t unsigned long - so the names are what call
 * every typed routine, and the generics what check the selection.
 *
 * It reports updates a second, and in a line of its own how many of them went
 * to this node - the PE itself or a node-mate - and how many across nodes.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "exercise.h"
#include "random.h"

enum { SLOTS = 16, OPERATIONS = 64, CLAIMS = 16, CLAIM_SLOTS = 2 * CLAIMS, HANDS = 8,
       CONTEXTS = 3, ATTEMPTS = 8 * CLAIMS };
enum family { ADD, FETCH_ADD, CLAIM, SWAP, SET_FETCH, BITWISE, FAMILIES };
enum bitwise_operation { AND, OR, XOR };

static const char *const context_names[CONTEXTS] = { "default", "private", "serialized" };
static const char *const phase_names[] = { "issue", "sync", "check", NULL };

/*
 * A type's operations, generated below from the three tables' lists. Values
 * cross this interface as uint64_t: an integer as its bits masked to its
 * width, a floating type as the small whole number it holds. `generic` picks
 * the C11 generic over the type's own name; SHMEM_CTX_DEFAULT the context-free
 * routines, any other context the ctx_ ones.
 */
struct amo_type {
    const char *name;
    int size, floating;
    uint64_t (*load)(const void *slot);
    void (*store)(void *slot, uint64_t value);
    /* Table 6 */
    void (*add)(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic);
    void (*inc)(shmem_ctx_t ctx, void *dest, int pe, int generic);
    uint64_t (*fetch_add)(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic);
    uint64_t (*fetch_inc)(shmem_ctx_t ctx, void *dest, int pe, int generic);
    uint64_t (*compare_swap)(shmem_ctx_t ctx, void *dest, uint64_t cond, uint64_t value, int pe,
                             int generic);
    void (*fetch_add_nbi)(shmem_ctx_t ctx, void *fetch, void *dest, uint64_t value, int pe,
                          int generic);
    void (*fetch_inc_nbi)(shmem_ctx_t ctx, void *fetch, void *dest, int pe, int generic);
    void (*compare_swap_nbi)(shmem_ctx_t ctx, void *fetch, void *dest, uint64_t cond,
                             uint64_t value, int pe, int generic);
    /* Table 7 */
    uint64_t (*fetch)(shmem_ctx_t ctx, const void *source, int pe, int generic);
    void (*set)(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic);
    uint64_t (*swap)(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic);
    void (*fetch_nbi)(shmem_ctx_t ctx, void *fetch, const void *source, int pe, int generic);
    void (*swap_nbi)(shmem_ctx_t ctx, void *fetch, void *dest, uint64_t value, int pe,
                     int generic);
    /* Table 8 */
    void (*bitwise)(shmem_ctx_t ctx, int operation, void *dest, uint64_t value, int pe,
                    int generic);
    uint64_t (*fetch_bitwise)(shmem_ctx_t ctx, int operation, void *dest, uint64_t value, int pe,
                              int generic);
    void (*fetch_bitwise_nbi)(shmem_ctx_t ctx, int operation, void *fetch, void *dest,
                              uint64_t value, int pe, int generic);
};

static uint64_t width_mask(int size)
{
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (8 * size)) - 1;
}

/* The routine by one of its two spellings, context-free or on a context. */
#define CALL(ctx, generic, NAME, OP, ...)                                           \
    do {                                                                            \
        if ((ctx) == SHMEM_CTX_DEFAULT) {                                           \
            if (generic)                                                            \
                shmem_atomic_##OP(__VA_ARGS__);                                     \
            else                                                                    \
                shmem_##NAME##_atomic_##OP(__VA_ARGS__);                            \
        } else {                                                                    \
            if (generic)                                                            \
                shmem_atomic_##OP((ctx), __VA_ARGS__);                              \
            else                                                                    \
                shmem_ctx_##NAME##_atomic_##OP((ctx), __VA_ARGS__);                 \
        }                                                                           \
    } while (0)

#define RETURN(ctx, generic, NAME, OP, ...)                                         \
    ((ctx) == SHMEM_CTX_DEFAULT                                                     \
         ? ((generic) ? shmem_atomic_##OP(__VA_ARGS__)                              \
                      : shmem_##NAME##_atomic_##OP(__VA_ARGS__))                    \
         : ((generic) ? shmem_atomic_##OP((ctx), __VA_ARGS__)                       \
                      : shmem_ctx_##NAME##_atomic_##OP((ctx), __VA_ARGS__)))

/* Loads and stores: an integer masked to its width, a float as the whole
 * number it holds. */
#define ACCESS(T, NAME, FLOATING)                                                   \
    static uint64_t load_##NAME(const void *slot)                                   \
    {                                                                               \
        T v = *(const T *)slot;                                                     \
        return FLOATING ? (uint64_t)v : (uint64_t)v & width_mask(sizeof(T));        \
    }                                                                               \
    static void store_##NAME(void *slot, uint64_t value)                            \
    {                                                                               \
        *(T *)slot = (T)value;                                                      \
    }

/* Table 7: fetch, set, swap and their nonblocking forms. */
#define EXTENDED(T, NAME)                                                           \
    static uint64_t fetch_##NAME(shmem_ctx_t ctx, const void *source, int pe, int generic) \
    {                                                                               \
        T v = RETURN(ctx, generic, NAME, fetch, (const T *)source, pe);             \
        return load_##NAME(&v);                                                     \
    }                                                                               \
    static void set_##NAME(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic) \
    {                                                                               \
        CALL(ctx, generic, NAME, set, (T *)dest, (T)value, pe);                     \
    }                                                                               \
    static uint64_t swap_##NAME(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, \
                                int generic)                                        \
    {                                                                               \
        T v = RETURN(ctx, generic, NAME, swap, (T *)dest, (T)value, pe);            \
        return load_##NAME(&v);                                                     \
    }                                                                               \
    static void fetch_nbi_##NAME(shmem_ctx_t ctx, void *fetch, const void *source, int pe, \
                                 int generic)                                       \
    {                                                                               \
        CALL(ctx, generic, NAME, fetch_nbi, (T *)fetch, (const T *)source, pe);     \
    }                                                                               \
    static void swap_nbi_##NAME(shmem_ctx_t ctx, void *fetch, void *dest, uint64_t value, \
                                int pe, int generic)                                \
    {                                                                               \
        CALL(ctx, generic, NAME, swap_nbi, (T *)fetch, (T *)dest, (T)value, pe);    \
    }

/* Table 6's own operations. */
#define STANDARD(T, NAME)                                                           \
    ACCESS(T, NAME, 0)                                                              \
    EXTENDED(T, NAME)                                                               \
    static void add_##NAME(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, int generic) \
    {                                                                               \
        CALL(ctx, generic, NAME, add, (T *)dest, (T)value, pe);                     \
    }                                                                               \
    static void inc_##NAME(shmem_ctx_t ctx, void *dest, int pe, int generic)        \
    {                                                                               \
        CALL(ctx, generic, NAME, inc, (T *)dest, pe);                               \
    }                                                                               \
    static uint64_t fetch_add_##NAME(shmem_ctx_t ctx, void *dest, uint64_t value, int pe, \
                                     int generic)                                   \
    {                                                                               \
        T v = RETURN(ctx, generic, NAME, fetch_add, (T *)dest, (T)value, pe);       \
        return load_##NAME(&v);                                                     \
    }                                                                               \
    static uint64_t fetch_inc_##NAME(shmem_ctx_t ctx, void *dest, int pe, int generic) \
    {                                                                               \
        T v = RETURN(ctx, generic, NAME, fetch_inc, (T *)dest, pe);                 \
        return load_##NAME(&v);                                                     \
    }                                                                               \
    static uint64_t compare_swap_##NAME(shmem_ctx_t ctx, void *dest, uint64_t cond,  \
                                        uint64_t value, int pe, int generic)        \
    {                                                                               \
        T v = RETURN(ctx, generic, NAME, compare_swap, (T *)dest, (T)cond, (T)value, pe); \
        return load_##NAME(&v);                                                     \
    }                                                                               \
    static void fetch_add_nbi_##NAME(shmem_ctx_t ctx, void *fetch, void *dest, uint64_t value, \
                                     int pe, int generic)                           \
    {                                                                               \
        CALL(ctx, generic, NAME, fetch_add_nbi, (T *)fetch, (T *)dest, (T)value, pe); \
    }                                                                               \
    static void fetch_inc_nbi_##NAME(shmem_ctx_t ctx, void *fetch, void *dest, int pe,     \
                                     int generic)                                   \
    {                                                                               \
        CALL(ctx, generic, NAME, fetch_inc_nbi, (T *)fetch, (T *)dest, pe);         \
    }                                                                               \
    static void compare_swap_nbi_##NAME(shmem_ctx_t ctx, void *fetch, void *dest,   \
                                        uint64_t cond, uint64_t value, int pe, int generic) \
    {                                                                               \
        CALL(ctx, generic, NAME, compare_swap_nbi, (T *)fetch, (T *)dest, (T)cond,  \
             (T)value, pe);                                                         \
    }

#define FLOATING(T, NAME)                                                           \
    ACCESS(T, NAME, 1)                                                              \
    EXTENDED(T, NAME)

/* Table 8. */
#define BITWISE_OPERATIONS(T, NAME)                                                 \
    static void bitwise_##NAME(shmem_ctx_t ctx, int operation, void *dest, uint64_t value, \
                               int pe, int generic)                                 \
    {                                                                               \
        if (operation == AND)                                                       \
            CALL(ctx, generic, NAME, and, (T *)dest, (T)value, pe);                 \
        else if (operation == OR)                                                   \
            CALL(ctx, generic, NAME, or, (T *)dest, (T)value, pe);                  \
        else                                                                        \
            CALL(ctx, generic, NAME, xor, (T *)dest, (T)value, pe);                 \
    }                                                                               \
    static uint64_t fetch_bitwise_##NAME(shmem_ctx_t ctx, int operation, void *dest, \
                                         uint64_t value, int pe, int generic)       \
    {                                                                               \
        T v = operation == AND  ? RETURN(ctx, generic, NAME, fetch_and, (T *)dest, (T)value, pe) \
              : operation == OR ? RETURN(ctx, generic, NAME, fetch_or, (T *)dest, (T)value, pe) \
                                : RETURN(ctx, generic, NAME, fetch_xor, (T *)dest, (T)value, pe); \
        return (uint64_t)v & width_mask(sizeof(T));                                 \
    }                                                                               \
    static void fetch_bitwise_nbi_##NAME(shmem_ctx_t ctx, int operation, void *fetch, \
                                         void *dest, uint64_t value, int pe, int generic) \
    {                                                                               \
        if (operation == AND)                                                       \
            CALL(ctx, generic, NAME, fetch_and_nbi, (T *)fetch, (T *)dest, (T)value, pe); \
        else if (operation == OR)                                                   \
            CALL(ctx, generic, NAME, fetch_or_nbi, (T *)fetch, (T *)dest, (T)value, pe); \
        else                                                                        \
            CALL(ctx, generic, NAME, fetch_xor_nbi, (T *)fetch, (T *)dest, (T)value, pe); \
    }

/* The tables, as the specification lists them (§9.7, Tables 6 to 8). */
#define STANDARD_TYPES(X)                                                           \
    X(int, int) X(long, long) X(long long, longlong) X(unsigned int, uint)          \
    X(unsigned long, ulong) X(unsigned long long, ulonglong) X(int32_t, int32)      \
    X(int64_t, int64) X(uint32_t, uint32) X(uint64_t, uint64) X(size_t, size)       \
    X(ptrdiff_t, ptrdiff)
#define FLOATING_TYPES(X) X(float, float) X(double, double)
#define BITWISE_TYPES(X)                                                            \
    X(unsigned int, uint) X(unsigned long, ulong) X(unsigned long long, ulonglong)  \
    X(int32_t, int32) X(int64_t, int64) X(uint32_t, uint32) X(uint64_t, uint64)

STANDARD_TYPES(STANDARD)
FLOATING_TYPES(FLOATING)
BITWISE_TYPES(BITWISE_OPERATIONS)

/* Every type's entry; a bitwise type's entry carries Table 6's operations
 * too, since every Table 8 type is a Table 6 type. */
#define STANDARD_ENTRY(T, NAME)                                                     \
    static const struct amo_type type_##NAME = {                                    \
        #NAME, (int)sizeof(T), 0, load_##NAME, store_##NAME, add_##NAME, inc_##NAME, \
        fetch_add_##NAME, fetch_inc_##NAME, compare_swap_##NAME, fetch_add_nbi_##NAME, \
        fetch_inc_nbi_##NAME, compare_swap_nbi_##NAME, fetch_##NAME, set_##NAME,    \
        swap_##NAME, fetch_nbi_##NAME, swap_nbi_##NAME, NULL, NULL, NULL };
#define FLOATING_ENTRY(T, NAME)                                                     \
    static const struct amo_type type_##NAME = {                                    \
        #NAME, (int)sizeof(T), 1, load_##NAME, store_##NAME, NULL, NULL, NULL, NULL, \
        NULL, NULL, NULL, NULL, fetch_##NAME, set_##NAME, swap_##NAME, fetch_nbi_##NAME, \
        swap_nbi_##NAME, NULL, NULL, NULL };
#define BITWISE_ENTRY(T, NAME)                                                      \
    static const struct amo_type bitwise_type_##NAME = {                            \
        #NAME, (int)sizeof(T), 0, load_##NAME, store_##NAME, NULL, NULL, NULL, NULL, \
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, bitwise_##NAME,        \
        fetch_bitwise_##NAME, fetch_bitwise_nbi_##NAME };
STANDARD_TYPES(STANDARD_ENTRY)
FLOATING_TYPES(FLOATING_ENTRY)
BITWISE_TYPES(BITWISE_ENTRY)

struct amo_case {
    enum family family;
    const struct amo_type *type;
};

#define STANDARD_CASE(T, NAME) { FAMILY_NOW, &type_##NAME },
#define BITWISE_CASE(T, NAME) { BITWISE, &bitwise_type_##NAME },

static const struct amo_case cases[] = {
#define FAMILY_NOW ADD
    STANDARD_TYPES(STANDARD_CASE)
#undef FAMILY_NOW
#define FAMILY_NOW FETCH_ADD
    STANDARD_TYPES(STANDARD_CASE)
#undef FAMILY_NOW
#define FAMILY_NOW CLAIM
    STANDARD_TYPES(STANDARD_CASE)
#undef FAMILY_NOW
#define FAMILY_NOW SWAP
    STANDARD_TYPES(STANDARD_CASE) FLOATING_TYPES(STANDARD_CASE)
#undef FAMILY_NOW
#define FAMILY_NOW SET_FETCH
    STANDARD_TYPES(STANDARD_CASE) FLOATING_TYPES(STANDARD_CASE)
#undef FAMILY_NOW
    BITWISE_TYPES(BITWISE_CASE)
};
enum { CASE_COUNT = sizeof cases / sizeof *cases };

static struct {
    /* the plan */
    uint64_t seed;
    int plant_add, plant_fetch, plant_claim, plant_swap, plant_set, plant_bitwise,
        plant_fetched_bit, plant_fetch_count, plant_attempts;
    /* the team */
    shmem_team_t team;
    int n, me, me_world;
    int *world;                     /* team rank to world PE */
    unsigned char *here;            /* team rank: this node */
    shmem_ctx_t contexts[CONTEXTS];
    /* symmetric */
    unsigned char *table;           /* SLOTS of 8 bytes */
    unsigned char *claims;          /* CLAIM_SLOTS of 8 bytes */
    uint64_t *sums, *hashes;        /* [SLOTS]: fetch_add's closed forms */
    uint64_t *reduce_source, *reduce_result;    /* [4] */
    /* private */
    unsigned char fetched[OPERATIONS * 8];
    uint64_t expected[SLOTS], expected_or[SLOTS], expected_xor[SLOTS];
    long round;
    double updates, local_updates, remote_updates, phase[3], since;
} atomics;

static uint64_t atomics_memory(int npes, const struct pacer_options *options, char *fields,
                               size_t size)
{
    uint64_t symmetric = SLOTS * 8 + CLAIM_SLOTS * 8 + 2 * SLOTS * 8 + 8 * 8;
    atomics.seed = options->seed;
    atomics.plant_add = options->plant == PACER_PLANT_ATOMICS_ADD;
    atomics.plant_fetch = options->plant == PACER_PLANT_ATOMICS_FETCH;
    atomics.plant_claim = options->plant == PACER_PLANT_ATOMICS_CLAIM;
    atomics.plant_swap = options->plant == PACER_PLANT_ATOMICS_SWAP;
    atomics.plant_set = options->plant == PACER_PLANT_ATOMICS_SET;
    atomics.plant_bitwise = options->plant == PACER_PLANT_ATOMICS_BITWISE;
    atomics.plant_fetched_bit = options->plant == PACER_PLANT_ATOMICS_FETCHED_BIT;
    atomics.plant_fetch_count = options->plant == PACER_PLANT_ATOMICS_FETCH_COUNT;
    atomics.plant_attempts = options->plant == PACER_PLANT_ATOMICS_ATTEMPTS;
    (void)npes;
    snprintf(fields, size, "cases=%d contexts=default,private,serialized slots=%d "
             "operations=%d claims=%d", CASE_COUNT, SLOTS, OPERATIONS, CLAIMS);
    return symmetric;
}

/* Onto a team, afresh: its members and which of them share this node; the
 * table's slots are reset by each round before use. */
static int atomics_rebind(shmem_team_t team, const struct pacer_options *options)
{
    int n = shmem_team_n_pes(team);
    const struct pacer_layout *layout = pacer_layout();
    atomics.team = team;
    atomics.n = n;
    atomics.me = shmem_team_my_pe(team);
    atomics.seed = options->seed;
    for (int j = 0; j < n; j++) {
        atomics.world[j] = shmem_team_translate_pe(team, j, SHMEM_TEAM_WORLD);
        atomics.here[j] = layout->node[atomics.world[j]] == layout->node[atomics.me_world];
    }
    atomics.updates = atomics.local_updates = atomics.remote_updates = 0;
    memset(atomics.phase, 0, sizeof atomics.phase);
    atomics.since = pacer_now();
    shmem_team_sync(team);
    return 0;
}

static int atomics_setup(shmem_team_t team, const struct pacer_options *options)
{
    int n = shmem_team_n_pes(team);
    atomics.me_world = shmem_my_pe();
    atomics.world = malloc(sizeof *atomics.world * (size_t)n);
    atomics.here = malloc((size_t)n);
    if (!atomics.world || !atomics.here)
        return -1;
    atomics.contexts[0] = SHMEM_CTX_DEFAULT;
    if (shmem_ctx_create(SHMEM_CTX_PRIVATE, &atomics.contexts[1]) != 0 ||
        shmem_ctx_create(SHMEM_CTX_SERIALIZED, &atomics.contexts[2]) != 0)
        return -1;
    atomics.table = shmem_calloc(SLOTS, 8);
    atomics.claims = shmem_calloc(CLAIM_SLOTS, 8);
    atomics.sums = shmem_calloc(SLOTS, sizeof *atomics.sums);
    atomics.hashes = shmem_calloc(SLOTS, sizeof *atomics.hashes);
    atomics.reduce_source = shmem_calloc(4, sizeof *atomics.reduce_source);
    atomics.reduce_result = shmem_calloc(4, sizeof *atomics.reduce_result);
    if (!atomics.table || !atomics.claims || !atomics.sums || !atomics.hashes ||
        !atomics.reduce_source || !atomics.reduce_result)
        return -1;
    atomics.round = 0;     /* and on through every rebind: mix's periods */
    return atomics_rebind(team, options);
}

/* ---- what every round shares ------------------------------------------------ */

static void *slot(int s)
{
    return atomics.table + 8 * s;
}

static void *claim_slot(int s)
{
    return atomics.claims + 8 * s;
}

/* A PE's stream for a family in a round: every PE can replay any PE's. */
static uint64_t stream(enum family family, int pe, long round)
{
    return random_mix(atomics.seed ^ random_name("atomics"), (uint64_t)pe, (uint64_t)round,
                      (uint64_t)family);
}

static void draw(uint64_t *state, int *pe, int *s, uint64_t *extra)
{
    uint64_t r = random_next(state);
    *pe = (int)(r % (uint64_t)atomics.n);
    *s = (int)((r >> 24) % SLOTS);
    *extra = r >> 40;
}

static void counted(int pe)
{
    atomics.updates++;
    if (atomics.here[pe])
        atomics.local_updates++;
    else
        atomics.remote_updates++;
}

/* The round's operations complete - on its context and on the default one,
 * which carries fetch_add's sums - and then every PE waits for every other. */
static void complete_and_sync(shmem_ctx_t ctx)
{
    double started = pacer_now();
    if (ctx != SHMEM_CTX_DEFAULT)
        shmem_ctx_quiet(ctx);
    shmem_quiet();
    shmem_team_sync(atomics.team);
    atomics.phase[1] += pacer_now() - started;
}

static uint64_t hash_of(uint64_t v)
{
    return random_finalize(v ^ UINT64_C(0x5bd1e9955bd1e995));
}

/* A small whole number every type holds exactly, float's 24 bits included. */
static uint64_t small(uint64_t v)
{
    return 1 + v % (UINT64_C(1) << 20);
}

/* ---- add ---------------------------------------------------------------------- */

static uint64_t add_start(int pe, int s, long round)
{
    return small((uint64_t)pe * 131 + (uint64_t)s * 7 + (uint64_t)round);
}

static void round_add(const struct amo_type *type, shmem_ctx_t ctx, int context, long round)
{
    int me = atomics.me;
    for (int s = 0; s < SLOTS; s++)
        type->store(slot(s), add_start(me, s, round));
    shmem_team_sync(atomics.team);
    double started = pacer_now();
    uint64_t state = stream(ADD, me, round);
    for (int i = 0; i < OPERATIONS; i++) {
        int pe, s;
        uint64_t extra;
        draw(&state, &pe, &s, &extra);
        uint64_t amount = 1 + extra % 7;
        if (extra & 8) {
            type->inc(ctx, slot(s), atomics.world[pe], i & 1);
        } else {
            if (atomics.plant_add && atomics.me_world == 1) {
                amount++;           /* the planted defect: one add more than drawn */
                atomics.plant_add = 0;
            }
            type->add(ctx, slot(s), amount, atomics.world[pe], i & 1);
        }
        counted(pe);
    }
    atomics.phase[0] += pacer_now() - started;
    complete_and_sync(ctx);

    started = pacer_now();
    uint64_t mask = width_mask(type->size);
    for (int s = 0; s < SLOTS; s++)
        atomics.expected[s] = add_start(me, s, round);
    for (int p = 0; p < atomics.n; p++) {
        uint64_t replay = stream(ADD, p, round);
        for (int i = 0; i < OPERATIONS; i++) {
            int pe, s;
            uint64_t extra;
            draw(&replay, &pe, &s, &extra);
            if (pe == me)
                atomics.expected[s] += extra & 8 ? 1 : 1 + extra % 7;
        }
    }
    for (int s = 0; s < SLOTS; s++) {
        uint64_t held = type->load(slot(s));
        if (held != (atomics.expected[s] & mask)) {
            pacer_failure("atomics round=%ld add %s %s: slot %d holds %llu, expected %llu", round,
                          type->name, context_names[context], s, (unsigned long long)held,
                          (unsigned long long)(atomics.expected[s] & mask));
            break;
        }
    }
    atomics.phase[2] += pacer_now() - started;
}

/* ---- fetch_add ---------------------------------------------------------------- */

static uint64_t fetch_start(int pe, int s, long round)
{
    return small((uint64_t)pe * 17 + (uint64_t)s * 3 + (uint64_t)round * 5);
}

/* What an issuer learnt, less the slot's start, accounted at the slot's PE. */
static void account(int pe, int s, uint64_t fetched, uint64_t start, uint64_t mask)
{
    uint64_t offset = (fetched - start) & mask;
    if (atomics.plant_fetch && atomics.me_world == 1) {
        offset++;                   /* the planted defect: one value misread */
        atomics.plant_fetch = 0;
    }
    shmem_uint64_atomic_add(&atomics.sums[s], offset, atomics.world[pe]);
    shmem_uint64_atomic_add(&atomics.hashes[s], hash_of(offset), atomics.world[pe]);
}

static void round_fetch_add(const struct amo_type *type, shmem_ctx_t ctx, int context, long round,
                            int nbi)
{
    int me = atomics.me;
    uint64_t mask = width_mask(type->size);
    for (int s = 0; s < SLOTS; s++) {
        type->store(slot(s), fetch_start(me, s, round));
        atomics.sums[s] = atomics.hashes[s] = 0;
    }
    shmem_team_sync(atomics.team);
    double started = pacer_now();
    uint64_t state = stream(FETCH_ADD, me, round);
    int pes[OPERATIONS], slots_drawn[OPERATIONS];
    memset(atomics.fetched, 0xa5, sizeof atomics.fetched);
    for (int i = 0; i < OPERATIONS; i++) {
        int pe, s;
        uint64_t extra;
        draw(&state, &pe, &s, &extra);
        pes[i] = pe;
        slots_drawn[i] = s;
        int world = atomics.world[pe];
        if (nbi) {
            void *into = atomics.fetched + 8 * i;
            if (extra & 1)
                type->fetch_inc_nbi(ctx, into, slot(s), world, i & 1);
            else
                type->fetch_add_nbi(ctx, into, slot(s), 1, world, i & 1);
        } else {
            uint64_t v = extra & 1 ? type->fetch_inc(ctx, slot(s), world, i & 1)
                                   : type->fetch_add(ctx, slot(s), 1, world, i & 1);
            account(pe, s, v, fetch_start(pe, s, round), mask);
        }
        counted(pe);
    }
    if (nbi) {
        if (ctx != SHMEM_CTX_DEFAULT)
            shmem_ctx_quiet(ctx);
        shmem_quiet();
        for (int i = 0; i < OPERATIONS; i++)
            account(pes[i], slots_drawn[i], type->load(atomics.fetched + 8 * i),
                    fetch_start(pes[i], slots_drawn[i], round), mask);
    }
    if (atomics.plant_fetch_count && atomics.me_world == 1) {
        /* The planted defect: one increment more than the stream holds. */
        uint64_t v = type->fetch_inc(SHMEM_CTX_DEFAULT, slot(slots_drawn[0]),
                                  atomics.world[pes[0]], 0);
        account(pes[0], slots_drawn[0], v, fetch_start(pes[0], slots_drawn[0], round), mask);
        atomics.plant_fetch_count = 0;
    }
    atomics.phase[0] += pacer_now() - started;
    complete_and_sync(ctx);

    started = pacer_now();
    uint64_t count[SLOTS] = { 0 };
    for (int p = 0; p < atomics.n; p++) {
        uint64_t replay = stream(FETCH_ADD, p, round);
        for (int i = 0; i < OPERATIONS; i++) {
            int pe, s;
            uint64_t extra;
            draw(&replay, &pe, &s, &extra);
            if (pe == me)
                count[s]++;
        }
    }
    for (int s = 0; s < SLOTS; s++) {
        uint64_t k = count[s], held = type->load(slot(s));
        uint64_t want = (fetch_start(me, s, round) + k) & mask, hash = 0;
        for (uint64_t u = 0; u < k; u++)
            hash += hash_of(u);
        if (held != want)
            pacer_failure("atomics round=%ld fetch_add %s %s: slot %d holds %llu after %llu "
                          "increments, expected %llu", round, type->name, context_names[context], s,
                          (unsigned long long)held, (unsigned long long)k,
                          (unsigned long long)want);
        uint64_t sum = k ? k * (k - 1) / 2 : 0;
        if (atomics.sums[s] != sum)
            pacer_failure("atomics round=%ld fetch_add %s %s: slot %d handed out values "
                          "summing to %llu, not the %llu of its %llu increments", round,
                          type->name, context_names[context], s, (unsigned long long)atomics.sums[s],
                          (unsigned long long)sum, (unsigned long long)k);
        if (atomics.hashes[s] != hash)
            pacer_failure("atomics round=%ld fetch_add %s %s: slot %d handed out values "
                          "hashing to %016llx, not %016llx", round, type->name, context_names[context], s,
                          (unsigned long long)atomics.hashes[s], (unsigned long long)hash);
    }
    atomics.phase[2] += pacer_now() - started;
}

/* ---- claim ---------------------------------------------------------------------- */

/* A claim's token: 1 + the attempt's number among all PEs' attempts, which
 * fits every Table 6 type for any job this tool runs. */
static uint64_t token(int pe, int attempt)
{
    return 1 + (uint64_t)pe * ATTEMPTS + (uint64_t)attempt;
}

static void round_claim(const struct amo_type *type, shmem_ctx_t ctx, int context, long round, int nbi)
{
    int me = atomics.me;
    for (int s = 0; s < CLAIM_SLOTS; s++)
        type->store(claim_slot(s), 0);
    shmem_team_sync(atomics.team);
    double started = pacer_now();
    uint64_t state = stream(CLAIM, me, round);
    int held = 0, attempt = 0;
    int held_pe[CLAIMS], held_slot[CLAIMS];
    uint64_t held_token[CLAIMS];
    while (held < CLAIMS && attempt < ATTEMPTS) {
        /* A batch of as many attempts as claims are missing: one at a time
         * when blocking, all in flight at once when not. */
        int batch = nbi ? CLAIMS - held : 1, pes[CLAIMS], slots_tried[CLAIMS];
        uint64_t tokens[CLAIMS];
        if (batch > ATTEMPTS - attempt)
            batch = ATTEMPTS - attempt;
        for (int j = 0; j < batch; j++, attempt++) {
            uint64_t r = random_next(&state);
            pes[j] = (int)(r % (uint64_t)atomics.n);
            slots_tried[j] = (int)((r >> 24) % CLAIM_SLOTS);
            tokens[j] = token(me, attempt);
            void *target = claim_slot(slots_tried[j]);
            if (atomics.plant_claim && atomics.me_world == 1 && held + j == CLAIMS - 1) {
                /* The planted defect: the last claim believed, never made. */
                atomics.plant_claim = 0;
                type->store(atomics.fetched + 8 * j, 0);
                if (!nbi) {
                    held_pe[held] = pes[j], held_slot[held] = slots_tried[j];
                    held_token[held++] = tokens[j];
                }
                continue;
            }
            if (nbi) {
                type->store(atomics.fetched + 8 * j, UINT64_MAX);
                type->compare_swap_nbi(ctx, atomics.fetched + 8 * j, target, 0, tokens[j],
                                    atomics.world[pes[j]], j & 1);
            } else if (type->compare_swap(ctx, target, 0, tokens[j], atomics.world[pes[j]],
                                       attempt & 1) == 0) {
                held_pe[held] = pes[j], held_slot[held] = slots_tried[j];
                held_token[held++] = tokens[j];
            }
            counted(pes[j]);
        }
        if (nbi) {
            if (ctx != SHMEM_CTX_DEFAULT)
                shmem_ctx_quiet(ctx);
            shmem_quiet();
            for (int j = 0; j < batch && held < CLAIMS; j++)
                if (type->load(atomics.fetched + 8 * j) == 0) {
                    held_pe[held] = pes[j], held_slot[held] = slots_tried[j];
                    held_token[held++] = tokens[j];
                }
        }
        if (atomics.plant_attempts && atomics.me_world == 1 && held > 0) {
            atomics.plant_attempts = 0;     /* the planted defect: giving up early */
            break;
        }
    }
    atomics.phase[0] += pacer_now() - started;
    complete_and_sync(ctx);

    started = pacer_now();
    if (held < CLAIMS)
        pacer_failure("atomics round=%ld claim %s %s: %d of %d claims after %d attempts, "
                      "in a table half empty", round, type->name, context_names[context], held, CLAIMS,
                      ATTEMPTS);
    for (int k = 0; k < held; k++) {
        uint64_t v = type->fetch(SHMEM_CTX_DEFAULT, claim_slot(held_slot[k]),
                              atomics.world[held_pe[k]], k & 1);
        if (v != held_token[k]) {
            pacer_failure("atomics round=%ld claim %s %s: slot %d of PE %d holds %llu, not "
                          "this PE's claim %llu", round, type->name, context_names[context], held_slot[k],
                          atomics.world[held_pe[k]], (unsigned long long)v,
                          (unsigned long long)held_token[k]);
            break;
        }
    }
    uint64_t mine = 0;
    for (int s = 0; s < CLAIM_SLOTS; s++)
        mine += type->load(claim_slot(s)) != 0;
    atomics.reduce_source[0] = mine;
    shmem_uint64_sum_reduce(atomics.team, atomics.reduce_result, atomics.reduce_source, 1);
    uint64_t total = atomics.reduce_result[0], want = (uint64_t)atomics.n * CLAIMS;
    if (total != want)
        pacer_failure("atomics round=%ld claim %s %s: the table holds %llu claims, not the "
                      "%llu made", round, type->name, context_names[context], (unsigned long long)total,
                      (unsigned long long)want);
    atomics.phase[2] += pacer_now() - started;
}

/* ---- swap ----------------------------------------------------------------------- */

/* Value i of the round's multiset: the table's first, then the hands. */
static uint64_t swap_value(uint64_t i, long round)
{
    return small(i * 3 + (uint64_t)round);
}

static void round_swap(const struct amo_type *type, shmem_ctx_t ctx, int context, long round, int nbi)
{
    int me = atomics.me, n = atomics.n;
    uint64_t hands[HANDS];
    for (int s = 0; s < SLOTS; s++)
        type->store(slot(s), swap_value((uint64_t)me * SLOTS + (uint64_t)s, round));
    for (int j = 0; j < HANDS; j++)
        hands[j] = swap_value((uint64_t)n * SLOTS + (uint64_t)me * HANDS + (uint64_t)j, round);
    shmem_team_sync(atomics.team);
    double started = pacer_now();
    uint64_t state = stream(SWAP, me, round);
    for (int i = 0; i < OPERATIONS; i += HANDS) {
        int pes[HANDS], slots_drawn[HANDS];
        for (int j = 0; j < HANDS; j++) {
            uint64_t extra;
            draw(&state, &pes[j], &slots_drawn[j], &extra);
            int world = atomics.world[pes[j]];
            if (nbi)
                type->swap_nbi(ctx, atomics.fetched + 8 * j, slot(slots_drawn[j]), hands[j], world,
                            j & 1);
            else
                hands[j] = type->swap(ctx, slot(slots_drawn[j]), hands[j], world, j & 1);
            counted(pes[j]);
        }
        if (nbi) {
            if (ctx != SHMEM_CTX_DEFAULT)
                shmem_ctx_quiet(ctx);
            shmem_quiet();
            for (int j = 0; j < HANDS; j++)
                hands[j] = type->load(atomics.fetched + 8 * j);
        }
        if (atomics.plant_swap && atomics.me_world == 1) {
            hands[0]++;             /* the planted defect: a token changed in hand */
            atomics.plant_swap = 0;
        }
    }
    atomics.phase[0] += pacer_now() - started;
    complete_and_sync(ctx);

    started = pacer_now();
    uint64_t sum = 0, hash = 0;
    for (int j = 0; j < HANDS; j++)
        sum += hands[j], hash += hash_of(hands[j]);
    for (int s = 0; s < SLOTS; s++) {
        uint64_t v = type->load(slot(s));
        sum += v, hash += hash_of(v);
    }
    atomics.reduce_source[0] = sum;
    atomics.reduce_source[1] = hash;
    shmem_uint64_sum_reduce(atomics.team, atomics.reduce_result, atomics.reduce_source, 2);
    uint64_t want_sum = 0, want_hash = 0;
    for (uint64_t i = 0; i < (uint64_t)n * (SLOTS + HANDS); i++) {
        uint64_t v = swap_value(i, round);
        want_sum += v, want_hash += hash_of(v);
    }
    if (atomics.reduce_result[0] != want_sum)
        pacer_failure("atomics round=%ld swap %s %s: the tokens sum to %llu, not the %llu "
                      "they started at", round, type->name, context_names[context],
                      (unsigned long long)atomics.reduce_result[0], (unsigned long long)want_sum);
    if (atomics.reduce_result[1] != want_hash)
        pacer_failure("atomics round=%ld swap %s %s: the tokens hash to %016llx, not the "
                      "%016llx they started at", round, type->name, context_names[context],
                      (unsigned long long)atomics.reduce_result[1],
                      (unsigned long long)want_hash);
    atomics.phase[2] += pacer_now() - started;
}

/* ---- set_fetch ------------------------------------------------------------------ */

/* Slot s of PE q is written by q - 1 - s alone, so every slot has one writer. */
static uint64_t set_value(int writer, int s, long round)
{
    return small(((uint64_t)writer * SLOTS + (uint64_t)s) * 31 + (uint64_t)round);
}

static void round_set_fetch(const struct amo_type *type, shmem_ctx_t ctx, int context, long round,
                            int nbi)
{
    int me = atomics.me, n = atomics.n;
    double started = pacer_now();
    for (int s = 0; s < SLOTS; s++) {
        int q = (me + 1 + s) % n;
        uint64_t value = set_value(me, s, round);
        if (atomics.plant_set && atomics.me_world == 1) {
            value++;                /* the planted defect: one value set wrong */
            atomics.plant_set = 0;
        }
        type->set(ctx, slot(s), value, atomics.world[q], s & 1);
        counted(q);
    }
    atomics.phase[0] += pacer_now() - started;
    complete_and_sync(ctx);

    started = pacer_now();
    for (int s = 0; s < SLOTS; s++) {
        int writer = ((me - 1 - s) % n + n) % n;
        uint64_t held = type->load(slot(s)), want = set_value(writer, s, round);
        if (held != want) {
            pacer_failure("atomics round=%ld set_fetch %s %s: slot %d holds %llu, not the %llu "
                          "PE %d set", round, type->name, context_names[context], s,
                          (unsigned long long)held, (unsigned long long)want,
                          atomics.world[writer]);
            break;
        }
    }
    memset(atomics.fetched, 0xa5, sizeof atomics.fetched);
    for (int s = 0; s < SLOTS; s++) {
        int q = (me + 1 + s) % n;
        if (nbi)
            type->fetch_nbi(ctx, atomics.fetched + 8 * s, slot(s), atomics.world[q], s & 1);
        else
            type->store(atomics.fetched + 8 * s, type->fetch(ctx, slot(s), atomics.world[q], s & 1));
    }
    if (nbi) {
        if (ctx != SHMEM_CTX_DEFAULT)
            shmem_ctx_quiet(ctx);
        shmem_quiet();
    }
    for (int s = 0; s < SLOTS; s++) {
        uint64_t got = type->load(atomics.fetched + 8 * s), want = set_value(me, s, round);
        if (got != want) {
            pacer_failure("atomics round=%ld set_fetch %s %s: fetched %llu from slot %d of PE "
                          "%d, which this PE set to %llu", round, type->name, context_names[context],
                          (unsigned long long)got, s, atomics.world[(me + 1 + s) % n],
                          (unsigned long long)want);
            break;
        }
    }
    atomics.phase[2] += pacer_now() - started;
    shmem_team_sync(atomics.team);  /* every fetch done before the next round sets */
}

/* ---- bitwise -------------------------------------------------------------------- */

/* A PE's targets in a bitwise round: distinct slots, as many as half the
 * table can give, found by a hash set stamped with the PE so that nothing is
 * cleared between the PEs a replay goes through. */
enum { BITWISE_TARGETS = OPERATIONS, TARGET_SET = 4 * BITWISE_TARGETS };
static struct {
    uint64_t key[TARGET_SET];
    uint64_t stamp[TARGET_SET];
    uint64_t now;
} targets_seen;

static int bitwise_targets(int pe, long round, int *pes, int *slots_drawn)
{
    int count = atomics.n * SLOTS / 2;
    if (count > BITWISE_TARGETS)
        count = BITWISE_TARGETS;
    targets_seen.now++;
    uint64_t state = stream(BITWISE, pe, round);
    for (int k = 0; k < count;) {
        int q, s;
        uint64_t extra;
        draw(&state, &q, &s, &extra);
        uint64_t key = (uint64_t)q * SLOTS + (uint64_t)s;
        size_t h = (size_t)(random_finalize(key) % TARGET_SET);
        int fresh = 1;
        while (targets_seen.stamp[h] == targets_seen.now) {
            if (targets_seen.key[h] == key) {
                fresh = 0;
                break;
            }
            h = (h + 1) % TARGET_SET;
        }
        if (!fresh)
            continue;
        targets_seen.key[h] = key;
        targets_seen.stamp[h] = targets_seen.now;
        pes[k] = q;
        slots_drawn[k++] = s;
    }
    return count;
}

static const char *const operation_names[] = { "and", "or", "xor" };

/* One pass: every target once, with the pass's operation and bit, fetching on
 * odd targets. A fetched value's own bit is predicted where this PE alone
 * owns it: clear before its or, set before its xor, clear before its and. */
static void bitwise_pass(const struct amo_type *type, shmem_ctx_t ctx, int context, long round, int nbi,
                         int operation, int count, const int *pes, const int *slots_drawn)
{
    int width = 8 * type->size, me = atomics.me;
    uint64_t bit = UINT64_C(1) << (me % width);
    uint64_t value = operation == AND ? ~bit & width_mask(type->size) : bit;
    int exclusive = atomics.n <= width;
    uint64_t predicted = operation == XOR ? bit : 0;
    double started = pacer_now();
    memset(atomics.fetched, 0xa5, sizeof atomics.fetched);
    for (int k = 0; k < count; k++) {
        int world = atomics.world[pes[k]];
        if (operation == XOR && k == 0 && atomics.plant_bitwise && atomics.me_world == 1) {
            atomics.plant_bitwise = 0;  /* the planted defect: one xor left out */
            continue;
        }
        if (k & 1) {
            if (nbi)
                type->fetch_bitwise_nbi(ctx, operation, atomics.fetched + 8 * k,
                                     slot(slots_drawn[k]), value, world, (k >> 1) & 1);
            else
                type->store(atomics.fetched + 8 * k,
                         type->fetch_bitwise(ctx, operation, slot(slots_drawn[k]), value, world,
                                          (k >> 1) & 1));
        } else {
            type->bitwise(ctx, operation, slot(slots_drawn[k]), value, world, (k >> 1) & 1);
        }
        counted(pes[k]);
    }
    if (nbi) {
        if (ctx != SHMEM_CTX_DEFAULT)
            shmem_ctx_quiet(ctx);
        shmem_quiet();
    }
    atomics.phase[0] += pacer_now() - started;
    if (!exclusive)
        return;
    for (int k = 1; k < count; k += 2) {
        uint64_t got = type->load(atomics.fetched + 8 * k) & bit;
        uint64_t want = predicted;
        if (k == 1 && operation == OR && atomics.plant_fetched_bit && atomics.me_world == 1) {
            want ^= bit;            /* the planted defect: one fetched bit misjudged */
            atomics.plant_fetched_bit = 0;
        }
        if (got != want) {
            pacer_failure("atomics round=%ld bitwise %s %s: fetch_%s from slot %d of PE %d "
                          "returned bit %d %s, expected %s", round, type->name, context_names[context],
                          operation_names[operation], slots_drawn[k], atomics.world[pes[k]],
                          me % width, got ? "set" : "clear", want ? "set" : "clear");
            break;
        }
    }
}

static void bitwise_check(const struct amo_type *type, int context, long round, const char *after,
                          const uint64_t *want)
{
    for (int s = 0; s < SLOTS; s++) {
        uint64_t held = type->load(slot(s));
        if (held != want[s]) {
            pacer_failure("atomics round=%ld bitwise %s %s: after the %s pass slot %d holds "
                          "%llx, expected %llx", round, type->name, context_names[context], after, s,
                          (unsigned long long)held, (unsigned long long)want[s]);
            break;
        }
    }
}

static void round_bitwise(const struct amo_type *type, shmem_ctx_t ctx, int context, long round, int nbi)
{
    int me = atomics.me, width = 8 * type->size;
    int pes[BITWISE_TARGETS], slots_drawn[BITWISE_TARGETS];
    for (int s = 0; s < SLOTS; s++)
        type->store(slot(s), 0);
    shmem_team_sync(atomics.team);

    /* What the passes must leave: the or of the bits of every PE that
     * targeted a slot, and then that xor their xor. */
    double started = pacer_now();
    memset(atomics.expected_or, 0, sizeof atomics.expected_or);
    memset(atomics.expected_xor, 0, sizeof atomics.expected_xor);
    for (int p = 0; p < atomics.n; p++) {
        int count = bitwise_targets(p, round, pes, slots_drawn);
        uint64_t bit = UINT64_C(1) << (p % width);
        for (int k = 0; k < count; k++)
            if (pes[k] == me) {
                atomics.expected_or[slots_drawn[k]] |= bit;
                atomics.expected_xor[slots_drawn[k]] ^= bit;
            }
    }
    uint64_t after_xor[SLOTS], after_and[SLOTS] = { 0 };
    for (int s = 0; s < SLOTS; s++)
        after_xor[s] = atomics.expected_or[s] ^ atomics.expected_xor[s];
    int count = bitwise_targets(me, round, pes, slots_drawn);
    atomics.phase[2] += pacer_now() - started;

    bitwise_pass(type, ctx, context, round, nbi, OR, count, pes, slots_drawn);
    complete_and_sync(ctx);
    bitwise_check(type, context, round, "or", atomics.expected_or);
    shmem_team_sync(atomics.team);
    bitwise_pass(type, ctx, context, round, nbi, XOR, count, pes, slots_drawn);
    complete_and_sync(ctx);
    bitwise_check(type, context, round, "xor", after_xor);
    shmem_team_sync(atomics.team);
    bitwise_pass(type, ctx, context, round, nbi, AND, count, pes, slots_drawn);
    complete_and_sync(ctx);
    bitwise_check(type, context, round, "and", after_and);
}

/* ---- the exercise ---------------------------------------------------------------- */

static void atomics_step(long units, int last)
{
    (void)last;
    for (long u = 0; u < units; u++, atomics.round++) {
        long round = atomics.round;
        const struct amo_case *k = &cases[round % CASE_COUNT];
        int context = (int)(round % CONTEXTS), nbi = (int)(round % 2);
        shmem_ctx_t ctx = atomics.contexts[context];
        switch (k->family) {
        case ADD: round_add(k->type, ctx, context, round); break;
        case FETCH_ADD: round_fetch_add(k->type, ctx, context, round, nbi); break;
        case CLAIM: round_claim(k->type, ctx, context, round, nbi); break;
        case SWAP: round_swap(k->type, ctx, context, round, nbi); break;
        case SET_FETCH: round_set_fetch(k->type, ctx, context, round, nbi); break;
        case BITWISE: round_bitwise(k->type, ctx, context, round, nbi); break;
        default: break;
        }
    }
}

static void atomics_checkpoint(struct pacer_counters *counters)
{
    /* Updates to this node and across nodes, a second, over the team. */
    double now = pacer_now(), seconds = now - atomics.since;
    atomics.reduce_source[0] = (uint64_t)atomics.local_updates;
    atomics.reduce_source[1] = (uint64_t)atomics.remote_updates;
    shmem_uint64_sum_reduce(atomics.team, atomics.reduce_result, atomics.reduce_source, 2);
    if (atomics.me == 0 && seconds > 0) {
        printf("%satomics local_per_s=%.0f remote_per_s=%.0f\n", pacer_prefix(),
               (double)atomics.reduce_result[0] / seconds,
               (double)atomics.reduce_result[1] / seconds);
        fflush(stdout);
    }
    atomics.since = now;
    counters->work = atomics.updates;
    for (int k = 0; k < 3; k++)
        counters->phase_seconds[k] = atomics.phase[k];
    atomics.updates = atomics.local_updates = atomics.remote_updates = 0;
    memset(atomics.phase, 0, sizeof atomics.phase);
}

static void atomics_teardown(void)
{
    shmem_quiet();
    shmem_team_sync(atomics.team);
    shmem_ctx_destroy(atomics.contexts[2]);
    shmem_ctx_destroy(atomics.contexts[1]);
    shmem_free(atomics.reduce_result);
    shmem_free(atomics.reduce_source);
    shmem_free(atomics.hashes);
    shmem_free(atomics.sums);
    shmem_free(atomics.claims);
    shmem_free(atomics.table);
    free(atomics.here);
    free(atomics.world);
}

const struct pacer_exercise pacer_atomics = {
    .name = "atomics",
    .work = "updates",
    .phases = phase_names,
    .quick_rounds = CASE_COUNT,         /* every case once */
    .memory = atomics_memory,
    .setup = atomics_setup,
    .step = atomics_step,
    .checkpoint = atomics_checkpoint,
    .teardown = atomics_teardown,
    .rebind = atomics_rebind,
};
