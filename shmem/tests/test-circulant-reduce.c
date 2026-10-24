/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Träff's circulant reduce-scatter and allgather on the data path (tier 2;
 * v0.2 step 3, Part 2; internode-generic.c's in_rounds).
 *
 *   test-circulant-reduce [threads | lanes | diverge]
 *
 * Run over logical nodes, the link set by OSOE_LINK_* so that the choice is
 * known: the test asks the chooser itself (collective-shape.c, compiled in)
 * what each call takes, from what the data path asks it with (shape_of), and
 * holds what this node's lead published to it. In order:
 *   1. sizes: a byte sum over the world at sizes from the small class's edge
 *      to 1 MiB and an odd tail, either side of the chooser's crossover: each
 *      result right; each call's algorithm the chooser's; a Träff's call's
 *      rounds twice ceil(log2 N), counted as they ran, its tree children none,
 *      and the most pieces its lead had put in a round and not yet held the
 *      credit for at most the window - the credits' pin, where a sender that
 *      ran ahead of them would overwrite a slot only in an unlucky run;
 *   2. types: every typed reduction - every operation on every type the
 *      specification gives it - at the smallest size that takes Träff's, of
 *      values whose result no order of combining can round: small integers,
 *      and for the products 1, -1 and 2;
 *   3. in place: source == dest;
 *   4. staggered: two Träff's reductions in a row, the PEs entering each some
 *      milliseconds apart, in one order and then the other, so that some lead
 *      is ready for its reduce-scatter while its receivers have still to
 *      enter - what its grants hold it back from;
 *   5. back to back: Träff's reductions followed at once, no barrier between,
 *      by small reductions, broadcasts and the trees' large reductions, on the
 *      world and on a copy of it whose leads are the world's - the
 *      reduce-scatter's counters its own, which a small reduction's child that
 *      has left adds to at once;
 *   6. reuse: a copy of the world made, given two Träff's reductions and
 *      destroyed, three times over, each new copy taking the number its last
 *      gave back - whose words every lead must have zeroed for it;
 *   7. few: long double sums of fewer elements than nodes, where some blocks
 *      are empty and some rounds send or take nothing - Träff's only where the
 *      link makes a few hundred bytes worth it, as shmem.circulant.16.tiny's
 *      does.
 * With `threads`, under SHMEM_THREAD_MULTIPLE: two threads a PE, each with a
 * copy of the world of its own, making Träff's reductions on it at once - two
 * teams' reduce-scatters through two lanes of each lead; then both on one copy
 * in turn, each round's reduction led by the other thread, so that the lane a
 * lead lends changes from call to call; at 1 MiB, so that every round credits.
 * With `lanes`, under SHMEM_THREAD_MULTIPLE and one PE a node: on node i, i
 * helper threads claim thread records first and hold them, and then a new
 * thread makes the reductions of phase 1, so that every node's lead lends a
 * lane of its own - which the single-threaded phases cannot arrange, every lead
 * lending its main thread's - and a sender that put into another round's lane,
 * or its own index, would land in slots nobody lent it; the leads' lanes are
 * checked to differ. With `diverge`, PE 1 reduces a byte more than the others:
 * the job must end naming the difference. Under
 * OSOE_COLLECTIVE_SHAPE=tree every call must take a tree: the setting turns
 * Träff's off by name. And where the rounds outnumber the rows a lane of the
 * fan-out ceiling's + 1 would hold - nine nodes and more at a ceiling of 2, a
 * lane of three (P3) - the chooser must still take Träff's at some size: a lane
 * has as many rows as the rounds need, and a test that took none of it would
 * pass on the trees alone.
 *
 * Every result is checked as the call returns; at the end every PE's failures
 * are summed, and the job's last line says how many calls took Träff's.
 */
#define _GNU_SOURCE
#include <complex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "collective-circulant.h"
#include "collective-shape.h"
#include "internal-region.h"
#include "invariants.h"
#include "launcher.h"

static int me, npes, failures, circulant_calls;
static uint64_t nodes;
static struct osoe_link link;
static uint64_t ceiling, capacity, small;

static uint64_t setting(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    return value ? strtoull(value, NULL, 10) : fallback;
}

/* What the data path asks the chooser for a reduction of `bytes` over the
 * world (internode-generic.c, shape_of): Träff's allowed under the shape
 * `link` - not `tree` - where its rows fit the reduce-scatter's words, in a
 * lane of the rows the job's nodes and the ceiling give it. */
static struct osoe_collective_shape chosen(uint64_t bytes)
{
    const char *shape = getenv("OSOE_COLLECTIVE_SHAPE");
    bool allowed = (!shape || strcmp(shape, "link") == 0) &&
                   osoe_circulant_rows((uint32_t)nodes) <=
                       osoe_internal_circulant_rows((uint32_t)nodes);
    return osoe_shape_choose(OSOE_SHAPE_REDUCE, (uint32_t)nodes, bytes, &link,
                             (uint32_t)ceiling,
                             osoe_internal_lane_rows((uint32_t)ceiling, (uint32_t)nodes),
                             (uint32_t)capacity, (uint32_t)small, OSOE_INTERNAL_PIECE_BYTES,
                             allowed);
}

/* The smallest of a ladder of sizes that takes Träff's here, or 0. */
static uint64_t smallest_circulant(void)
{
    for (uint64_t bytes = 2048; bytes <= (UINT64_C(1) << 20); bytes *= 2)
        if (chosen(bytes).algorithm == OSOE_SHAPE_CIRCULANT)
            return bytes;
    return 0;
}

/*
 * What this node's lead published for the world's last reduction (node.h):
 * the algorithm the chooser gives for `bytes`, and for Träff's its rounds,
 * no tree children and at most the window in flight. Every PE reads its own
 * node's lead, so every node is held to it.
 */
static void published(uint64_t bytes, const char *what)
{
    struct osoe_collective_shape want = chosen(bytes);
    uint64_t algorithm = 0, rounds = 0, children = 0, most = 0, window = 0;
    CHECK(osoe_test_scoped_metric("team.tree.algorithm", OSOE_MSCOPE_TEAM, 0, &algorithm));
    CHECK(osoe_test_scoped_metric("team.tree.circulant_rounds", OSOE_MSCOPE_TEAM, 0, &rounds));
    CHECK(osoe_test_scoped_metric("team.tree.shape_children", OSOE_MSCOPE_TEAM, 0, &children));
    CHECK(osoe_test_scoped_metric("team.tree.most_pieces_in_flight", OSOE_MSCOPE_TEAM, 0,
                                  &most));
    CHECK(osoe_test_scoped_metric("team.tree.shape_window", OSOE_MSCOPE_TEAM, 0, &window));
    if (bytes <= small)
        return;                 /* the small class publishes nothing past the intranode phase */
    if (algorithm != (uint64_t)want.algorithm) {
        fprintf(stderr, "PE %d: %s of %llu bytes on %llu nodes took algorithm %llu, the "
                "chooser's %d\n", me, what, (unsigned long long)bytes,
                (unsigned long long)nodes, (unsigned long long)algorithm, (int)want.algorithm);
        failures++;
        return;
    }
    if (want.algorithm != OSOE_SHAPE_CIRCULANT)
        return;
    circulant_calls++;
    uint64_t want_rounds = osoe_circulant_rounds((uint32_t)nodes);
    if (rounds != want_rounds || children != 0 || window != want.window || most < 1 ||
        most > window) {
        fprintf(stderr, "PE %d: %s of %llu bytes on %llu nodes took Träff's with %llu rounds "
                "(%llu wanted), %llu children, a window of %llu (%u wanted) and %llu pieces "
                "in flight at most\n", me, what, (unsigned long long)bytes,
                (unsigned long long)nodes, (unsigned long long)rounds,
                (unsigned long long)want_rounds, (unsigned long long)children,
                (unsigned long long)window, want.window, (unsigned long long)most);
        failures++;
    }
}

/* ---- 1, 3, 4 and 5: byte sums --------------------------------------------- */

static uint8_t byte_value(int pe, size_t i, int round)
{
    return (uint8_t)(pe * 37 + (int)(i % 251) * 11 + round * 5 + (int)(i / 251));
}

static void check_byte_sum(const uint8_t *dest, size_t n, int round, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t want = 0;
        for (int p = 0; p < npes; p++)
            want = (uint8_t)(want + byte_value(p, i, round));
        if (dest[i] != want) {
            fprintf(stderr, "PE %d: %s of %zu bytes, round %d: byte %zu is %u, not %u\n", me,
                    what, n, round, i, dest[i], want);
            failures++;
            return;
        }
    }
}

/* A byte sum of `n` over `team`, checked; `in_place` makes source dest. */
static void byte_sum(shmem_team_t team, uint8_t *dest, uint8_t *source, size_t n, int round,
                     bool in_place, const char *what)
{
    uint8_t *from = in_place ? dest : source;
    for (size_t i = 0; i < n; i++) {
        from[i] = byte_value(me, i, round);
        if (!in_place)
            dest[i] = 0;
    }
    CHECK(shmem_uint8_sum_reduce(team, dest, from, n) == 0);
    check_byte_sum(dest, n, round, what);
}

static const size_t SIZES[] = { 1025, 2048, 4096, 8192, 16384, 16385, 32768, 65536, 131072,
                                262144, 524288, 1048576, 1048589 };
enum { SIZE_COUNT = sizeof SIZES / sizeof SIZES[0] };

static void sizes(uint8_t *dest, uint8_t *source)
{
    for (int s = 0; s < SIZE_COUNT; s++) {
        shmem_barrier_all();
        byte_sum(SHMEM_TEAM_WORLD, dest, source, SIZES[s], s, false, "a byte sum");
        published(SIZES[s], "a byte sum");
    }
}

static void in_place(uint8_t *dest, size_t n)
{
    shmem_barrier_all();
    byte_sum(SHMEM_TEAM_WORLD, dest, NULL, n, 100, true, "a byte sum in place");
    published(n, "a byte sum in place");
}

/* Sleep `ms` milliseconds. */
static void pause_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static void staggered(uint8_t *dest, uint8_t *source, size_t n)
{
    shmem_barrier_all();
    pause_ms(me * 3);
    byte_sum(SHMEM_TEAM_WORLD, dest, source, n, 200, false, "a staggered byte sum");
    pause_ms((npes - 1 - me) * 3);
    byte_sum(SHMEM_TEAM_WORLD, dest, source, n, 201, false, "a second staggered byte sum");
    published(n, "a staggered byte sum");
}

static void back_to_back(shmem_team_t copy, uint8_t *dest[4], uint8_t *source[4], size_t large)
{
    shmem_barrier_all();
    /* The largest size the trees take, if any: a large reduction through them. */
    size_t tree = 0;
    for (int s = 0; s < SIZE_COUNT; s++)
        if (SIZES[s] > small && chosen(SIZES[s]).algorithm == OSOE_SHAPE_TREE)
            tree = SIZES[s];
    for (int round = 0; round < 8; round++) {
        int b = round % 4;
        byte_sum(SHMEM_TEAM_WORLD, dest[b], source[b], large, 300 + round, false,
                 "back to back, Träff's on the world");
        byte_sum(copy, dest[(b + 1) % 4], source[(b + 1) % 4], 40, 400 + round, false,
                 "back to back, a small sum on the copy");
        byte_sum(SHMEM_TEAM_WORLD, dest[(b + 2) % 4], source[(b + 2) % 4], 40, 500 + round,
                 false, "back to back, a small sum on the world");
        int root = round % npes;
        for (size_t i = 0; i < 64; i++)
            source[(b + 3) % 4][i] = byte_value(me, i, 600 + round);
        CHECK(shmem_broadcastmem(SHMEM_TEAM_WORLD, dest[(b + 3) % 4], source[(b + 3) % 4], 64,
                                 root) == 0);
        for (size_t i = 0; i < 64; i++)
            if (dest[(b + 3) % 4][i] != byte_value(root, i, 600 + round)) {
                fprintf(stderr, "PE %d: back to back, round %d: a broadcast's byte %zu is "
                        "wrong\n", me, round, i);
                failures++;
                break;
            }
        byte_sum(copy, dest[b], source[b], large, 700 + round, false,
                 "back to back, Träff's on the copy");
        if (tree)
            byte_sum(SHMEM_TEAM_WORLD, dest[(b + 1) % 4], source[(b + 1) % 4], tree,
                     800 + round, false, "back to back, a tree's large sum on the world");
        byte_sum(SHMEM_TEAM_WORLD, dest[(b + 2) % 4], source[(b + 2) % 4], large, 900 + round,
                 false, "back to back, Träff's on the world again");
    }
}

static void reuse(uint8_t *dest, uint8_t *source, size_t large)
{
    for (int round = 0; round < 3; round++) {
        shmem_team_t team = SHMEM_TEAM_INVALID;
        shmem_barrier_all();
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &team) ==
                    0);
        byte_sum(team, dest, source, large, 1100 + 2 * round, false,
                 "Träff's on a number taken again");
        byte_sum(team, dest, source, large, 1101 + 2 * round, false,
                 "Träff's again on a number taken again");
        shmem_team_destroy(team);
    }
}

/* Long double sums of `n` elements, fewer than the nodes: each element the sum
 * of small integers, exact in any order. */
static void few(long double *dest, long double *source, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        source[i] = (long double)(me * 3 + (int)i);
        dest[i] = 0;
    }
    shmem_barrier_all();
    CHECK(shmem_longdouble_sum_reduce(SHMEM_TEAM_WORLD, dest, source, (size_t)n) == 0);
    for (uint64_t i = 0; i < n; i++) {
        long double want = 0;
        for (int p = 0; p < npes; p++)
            want += (long double)(p * 3 + (int)i);
        if (dest[i] != want) {
            fprintf(stderr, "PE %d: a long double sum of %llu elements: element %llu is "
                    "wrong\n", me, (unsigned long long)n, (unsigned long long)i);
            failures++;
            break;
        }
    }
    published(n * sizeof(long double), "a long double sum of fewer elements than nodes");
}

/* ---- 2: every typed reduction ----------------------------------------------- */

/* Values a reduction of every type takes exactly, whatever order the nodes'
 * partials combine in. */
static int bits_value(int p, size_t i) { return (13 * p + 7 * (int)(i % 4093)) & 0x7f; }
static int order_value(int p, size_t i) { return (7 * p + 3 * (int)(i % 23)) % 23 - 11; }
static int sum_value(int p, size_t i) { return (p + 2 * (int)(i % 5)) % 3 - 1; }
static int sign_value(int p, size_t i) { return (p + (int)(i % 2)) % 2 ? -1 : 1; }
static int product_value(int p, size_t i)
{
    return p % 7 == 0 && i % 3 == 0 ? 2 : sign_value(p, i);
}

static void *type_source, *type_dest;
static size_t type_bytes;

static void type_failed(const char *name, size_t i)
{
    fprintf(stderr, "PE %d: %s over %zu bytes: element %zu is wrong\n", me, name, type_bytes,
            i);
    failures++;
}

#define INTEGER_CASE(T, NAME, OPNAME, VALUE, FOLD)                                \
    static void NAME##_##OPNAME(void)                                             \
    {                                                                             \
        size_t n = type_bytes / sizeof(T);                                        \
        T *source = type_source, *dest = type_dest;                               \
        for (size_t i = 0; i < n; i++)                                            \
            source[i] = (T)VALUE(me, i);                                          \
        CHECK(shmem_##NAME##_##OPNAME##_reduce(SHMEM_TEAM_WORLD, dest, source, n) == 0); \
        for (size_t i = 0; i < n; i++) {                                          \
            T acc = (T)VALUE(0, i);                                               \
            for (int p = 1; p < npes; p++) {                                      \
                T v = (T)VALUE(p, i);                                             \
                acc = (T)(FOLD);                                                  \
            }                                                                     \
            if (dest[i] != acc) {                                                 \
                type_failed("shmem_" #NAME "_" #OPNAME "_reduce", i);             \
                break;                                                            \
            }                                                                     \
        }                                                                         \
    }

#define BITWISE_CASES(T, NAME)                                                    \
    INTEGER_CASE(T, NAME, and, bits_value, acc & v)                               \
    INTEGER_CASE(T, NAME, or, bits_value, acc | v)                                \
    INTEGER_CASE(T, NAME, xor, bits_value, acc ^ v)
#define ORDERED_CASES(T, NAME)                                                    \
    INTEGER_CASE(T, NAME, max, order_value, v > acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, min, order_value, v < acc ? v : acc)
#define INTEGER_ARITHMETIC_CASES(T, NAME)                                         \
    INTEGER_CASE(T, NAME, sum, sum_value, acc + v)                                \
    INTEGER_CASE(T, NAME, prod, sign_value, acc * v)
#define FLOATING_CASES(T, NAME)                                                   \
    INTEGER_CASE(T, NAME, max, order_value, v > acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, min, order_value, v < acc ? v : acc)                    \
    INTEGER_CASE(T, NAME, sum, sum_value, acc + v)                                \
    INTEGER_CASE(T, NAME, prod, product_value, acc * v)

OSOE_REDUCE_BITWISE_TYPES(BITWISE_CASES)
OSOE_REDUCE_INTEGER_TYPES(ORDERED_CASES)
OSOE_REDUCE_INTEGER_TYPES(INTEGER_ARITHMETIC_CASES)
OSOE_REDUCE_FLOATING_TYPES(FLOATING_CASES)

/* The complex values: a sum's of small integers, a product's a power of i. */
static double complex complex_sum_value(int p, size_t i)
{
    return (double)sum_value(p, i) + (double)sum_value(p + 1, i) * I;
}

static double complex complex_product_value(int p, size_t i)
{
    static const double complex power[4] = { 1, I, -1, -I };
    return power[(p + (int)(i % 4)) % 4];
}

#define COMPLEX_CASE(T, NAME, OPNAME, VALUE, FOLD)                                \
    static void NAME##_##OPNAME(void)                                             \
    {                                                                             \
        size_t n = type_bytes / sizeof(T);                                        \
        T *source = type_source, *dest = type_dest;                               \
        for (size_t i = 0; i < n; i++)                                            \
            source[i] = (T)VALUE(me, i);                                          \
        CHECK(shmem_##NAME##_##OPNAME##_reduce(SHMEM_TEAM_WORLD, dest, source, n) == 0); \
        for (size_t i = 0; i < n; i++) {                                          \
            T acc = (T)VALUE(0, i);                                               \
            for (int p = 1; p < npes; p++) {                                      \
                T v = (T)VALUE(p, i);                                             \
                acc = (T)(FOLD);                                                  \
            }                                                                     \
            if (creal(dest[i]) != creal(acc) || cimag(dest[i]) != cimag(acc)) {   \
                type_failed("shmem_" #NAME "_" #OPNAME "_reduce", i);             \
                break;                                                            \
            }                                                                     \
        }                                                                         \
    }
#define COMPLEX_CASES(T, NAME)                                                    \
    COMPLEX_CASE(T, NAME, sum, complex_sum_value, acc + v)                        \
    COMPLEX_CASE(T, NAME, prod, complex_product_value, acc * v)
OSOE_REDUCE_COMPLEX_TYPES(COMPLEX_CASES)

/* Every case, in a table: its function and its element's width. */
struct type_case {
    void (*run)(void);
    size_t width;
    const char *name;
};

#define BITWISE_ROWS(T, NAME)                                                     \
    { NAME##_and, sizeof(T), #NAME " and" }, { NAME##_or, sizeof(T), #NAME " or" }, \
    { NAME##_xor, sizeof(T), #NAME " xor" },
#define INTEGER_ROWS(T, NAME)                                                     \
    { NAME##_max, sizeof(T), #NAME " max" }, { NAME##_min, sizeof(T), #NAME " min" }, \
    { NAME##_sum, sizeof(T), #NAME " sum" }, { NAME##_prod, sizeof(T), #NAME " prod" },
#define COMPLEX_ROWS(T, NAME)                                                     \
    { NAME##_sum, sizeof(T), #NAME " sum" }, { NAME##_prod, sizeof(T), #NAME " prod" },

static const struct type_case TYPE_CASES[] = {
    OSOE_REDUCE_BITWISE_TYPES(BITWISE_ROWS)
    OSOE_REDUCE_INTEGER_TYPES(INTEGER_ROWS)
    OSOE_REDUCE_FLOATING_TYPES(INTEGER_ROWS)
    OSOE_REDUCE_COMPLEX_TYPES(COMPLEX_ROWS)
};
enum { TYPE_CASE_COUNT = sizeof TYPE_CASES / sizeof TYPE_CASES[0] };

/* Every case at `bytes` - which takes Träff's - each of whole elements. */
static void types(size_t bytes)
{
    for (int c = 0; c < TYPE_CASE_COUNT; c++) {
        type_bytes = bytes / TYPE_CASES[c].width * TYPE_CASES[c].width;
        shmem_barrier_all();
        TYPE_CASES[c].run();
        published(type_bytes, TYPE_CASES[c].name);
    }
}

/* ---- threads ----------------------------------------------------------------- */

enum { THREADS = 2, THREAD_ROUNDS = 6 };

struct thread_work {
    shmem_team_t team;
    shmem_team_t shared;                /* the copy the threads take in turn */
    pthread_barrier_t *turns;
    uint8_t *dest, *source;
    size_t bytes;
    int index;
    int failed;
};

/* One byte sum on `team` from a thread, checked; tagged by `tag`. */
static void thread_sum(struct thread_work *w, shmem_team_t team, int tag, int round)
{
    for (size_t i = 0; i < w->bytes; i++) {
        w->source[i] = byte_value(me, i, tag);
        w->dest[i] = 0;
    }
    if (shmem_uint8_sum_reduce(team, w->dest, w->source, w->bytes) != 0)
        w->failed = 1;
    for (size_t i = 0; i < w->bytes && !w->failed; i++) {
        uint8_t want = 0;
        for (int p = 0; p < npes; p++)
            want = (uint8_t)(want + byte_value(p, i, tag));
        if (w->dest[i] != want) {
            fprintf(stderr, "PE %d, thread %d, round %d: byte %zu of %zu is %u, not %u\n", me,
                    w->index, round, i, w->bytes, w->dest[i], want);
            w->failed = 1;
        }
    }
}

static void *thread_main(void *argument)
{
    struct thread_work *w = argument;
    for (int round = 0; round < THREAD_ROUNDS && !w->failed; round++)
        thread_sum(w, w->team, 1000 + 10 * w->index + round, round);
    /* In turn on the shared copy: round r's reduction is thread r mod THREADS's
     * on every PE, the others waiting at the barrier, so one call at a time
     * on the team, as the specification asks, each from another thread. */
    for (int round = 0; round < 2 * THREADS * THREAD_ROUNDS; round++) {
        if (round % THREADS == w->index && !w->failed)
            thread_sum(w, w->shared, 2000 + round, round);
        pthread_barrier_wait(w->turns);
    }
    return NULL;
}

/* The lanes mode: helpers that claim a thread record each and hold it until
 * released, and the thread that then reduces. */
static pthread_barrier_t claimed, released;

static void *helper_main(void *argument)
{
    (void)argument;
    shmem_quiet();                      /* a counted call: claims this thread's record */
    pthread_barrier_wait(&claimed);
    pthread_barrier_wait(&released);
    return NULL;
}

static uint64_t reducer_lane = UINT64_MAX;

static void *reducer_main(void *argument)
{
    uint8_t **buffers = argument;
    sizes(buffers[0], buffers[1]);
    CHECK(osoe_test_scoped_metric("team.tree.lane", OSOE_MSCOPE_TEAM, 0, &reducer_lane));
    return NULL;
}

static void lanes(void)
{
    int provided = 0;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL((uint64_t)npes == nodes);           /* one PE a node: its node is its rank */
    size_t most = SIZES[SIZE_COUNT - 1];
    uint8_t *buffers[2] = { shmem_malloc(most), shmem_malloc(most) };
    CHECK_FATAL(buffers[0] && buffers[1]);
    pthread_t helper[64], reducer;
    CHECK_FATAL(me < 64);
    CHECK_FATAL(pthread_barrier_init(&claimed, NULL, (unsigned)me + 1) == 0);
    CHECK_FATAL(pthread_barrier_init(&released, NULL, (unsigned)me + 1) == 0);
    for (int h = 0; h < me; h++)
        CHECK_FATAL(pthread_create(&helper[h], NULL, helper_main, NULL) == 0);
    pthread_barrier_wait(&claimed);
    shmem_barrier_all();
    CHECK_FATAL(pthread_create(&reducer, NULL, reducer_main, buffers) == 0);
    CHECK(pthread_join(reducer, NULL) == 0);
    pthread_barrier_wait(&released);
    for (int h = 0; h < me; h++)
        CHECK(pthread_join(helper[h], NULL) == 0);
    /* Every node's lead lent a lane of its own, or the test proved nothing. */
    uint64_t *seen = shmem_calloc((size_t)npes, sizeof *seen);
    CHECK_FATAL(seen != NULL);
    for (int p = 0; p < npes; p++)
        shmem_uint64_p(&seen[me], reducer_lane, p);
    shmem_barrier_all();
    for (int p = 0; p < npes; p++)
        for (int q = p + 1; q < npes; q++)
            if (seen[p] == seen[q]) {
                fprintf(stderr, "PE %d: the leads of nodes %d and %d lent the same lane, %llu\n",
                        me, p, q, (unsigned long long)seen[p]);
                failures++;
            }
    shmem_barrier_all();
    shmem_free(seen);
}

/* The diverge mode: PE 1 reduces a byte more than the others, at a size every
 * node takes Träff's for. */
static void diverge(uint64_t smallest)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(smallest != 0);
    size_t n = (size_t)smallest * 4 + (me == 1);
    /* Room for the larger on every PE: allocations of different sizes end the
     * job by name before the reduction is reached (P23). */
    size_t room = (size_t)smallest * 4 + 2;
    uint8_t *source = shmem_malloc(room), *dest = shmem_malloc(room);
    CHECK_FATAL(source && dest);
    memset(source, 1, n);
    shmem_uint8_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n);
    fprintf(stderr, "PE %d: a divergent reduction returned\n", me);
    CHECK(0);
}

static int threads(size_t bytes)
{
    int provided = 0;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    struct thread_work work[THREADS];
    pthread_t thread[THREADS];
    pthread_barrier_t turns;
    CHECK_FATAL(pthread_barrier_init(&turns, NULL, THREADS) == 0);
    shmem_team_t shared = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &shared) == 0);
    for (int t = 0; t < THREADS; t++) {
        work[t] = (struct thread_work){ SHMEM_TEAM_INVALID, shared, &turns, shmem_malloc(bytes),
                                        shmem_malloc(bytes), bytes, t, 0 };
        CHECK_FATAL(work[t].dest && work[t].source);
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0,
                                             &work[t].team) == 0);
    }
    shmem_barrier_all();
    for (int t = 0; t < THREADS; t++)
        CHECK_FATAL(pthread_create(&thread[t], NULL, thread_main, &work[t]) == 0);
    for (int t = 0; t < THREADS; t++) {
        CHECK(pthread_join(thread[t], NULL) == 0);
        failures += work[t].failed;
    }
    shmem_barrier_all();
    for (int t = 0; t < THREADS; t++)
        shmem_team_destroy(work[t].team);
    shmem_team_destroy(shared);
    pthread_barrier_destroy(&turns);
    return 0;
}

int main(int argc, char **argv)
{
    bool threaded = argc > 1 && strcmp(argv[1], "threads") == 0;
    const char *step_nodes = osoe_test_launcher("STEP_NUM_NODES");
    nodes = strtoull(step_nodes ? step_nodes : "1", NULL, 10);
    link = (struct osoe_link){ setting("OSOE_LINK_HOP_NS", 0), setting("OSOE_LINK_MESSAGE_NS", 0),
                               setting("OSOE_LINK_BYTES_PER_SECOND", 0) };
    ceiling = setting("OSOE_COLLECTIVE_FANOUT", 8);
    capacity = setting("OSOE_COLLECTIVE_PIPELINE", 4);
    small = setting("OSOE_SMALL_COLLECTIVE_BYTES", 1024);
    CHECK_FATAL(link.bytes_per_second != 0);    /* the link is set, so the choice is known */
    uint64_t smallest = smallest_circulant();
    size_t large = smallest ? (size_t)smallest : (size_t)1 << 20;
    const char *shape = getenv("OSOE_COLLECTIVE_SHAPE");
    CHECK_FATAL(smallest != 0 || (shape && strcmp(shape, "link") != 0) ||
                osoe_circulant_rows((uint32_t)nodes) <= ceiling + 1);

    if (argc > 1 && strcmp(argv[1], "diverge") == 0) {
        diverge(smallest);
    } else if (argc > 1 && strcmp(argv[1], "lanes") == 0) {
        lanes();
    } else if (threaded) {
        /* At 1 MiB every round has more pieces than its window, so credits
         * flow. */
        threads((size_t)1 << 20);
    } else {
        shmem_init();
        me = shmem_my_pe();
        npes = shmem_n_pes();
        size_t most = SIZES[SIZE_COUNT - 1];
        uint8_t *dest[4], *source[4];
        for (int b = 0; b < 4; b++) {
            dest[b] = shmem_malloc(most);
            source[b] = shmem_malloc(most);
            CHECK_FATAL(dest[b] && source[b]);
        }
        type_source = source[0];
        type_dest = dest[0];
        shmem_team_t copy = SHMEM_TEAM_INVALID;
        CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &copy) == 0);
        sizes(dest[0], source[0]);
        types(large);
        in_place(dest[1], large + 3);
        staggered(dest[2], source[2], large);
        back_to_back(copy, dest, source, large);
        /* The copy gives its number back first, which the reuse takes, so
         * that it runs at OSOE_MAX_TEAMS=4 too. */
        shmem_barrier_all();
        shmem_team_destroy(copy);
        reuse(dest[3], source[3], large);
        long double *ld_source = (long double *)(void *)source[0];
        long double *ld_dest = (long double *)(void *)dest[0];
        if (nodes > 2) {
            few(ld_dest, ld_source, nodes - 1);
            few(ld_dest, ld_source, nodes / 2 + 1);
        }
    }

    int *mine = shmem_malloc(2 * sizeof *mine), *all = shmem_malloc(2 * sizeof *all);
    CHECK_FATAL(mine && all);
    mine[0] = failures;
    mine[1] = circulant_calls;
    CHECK(shmem_int_sum_reduce(SHMEM_TEAM_WORLD, all, mine, 2) == 0);
    CHECK(all[0] == 0);
    /* Träff's must have run wherever the chooser takes it at some size. */
    CHECK(threaded || smallest == 0 || all[1] > 0);
    const char *mode = threaded ? "two threads" : argc > 1 ? "lanes of their own" : "every phase";
    if (me == 0 && all[0] == 0)
        printf("CIRCULANT REDUCE: %s on %llu nodes, %d PEs, Träff's from %llu bytes, %d calls "
               "checked as Träff's\n", mode,
               (unsigned long long)nodes, npes, (unsigned long long)smallest, all[1]);
    shmem_finalize();
    return CHECK_DONE();
}
