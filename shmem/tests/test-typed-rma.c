/*
 * The typed and sized RMA routines of spec §9.6 and the typed put-with-signal
 * of §9.8.3-§9.8.4: shmem_TYPENAME_put/get/p/g/iput/iget and their nonblocking
 * forms, put8..128/get8..128 and their strided and nonblocking forms,
 * shmem_TYPENAME_put_signal[_nbi], and the C11 type-generic macros.
 *
 * Every routine is a thin wrapper over shmem_putmem/getmem/putmem_signal
 * (rma-typed.c, signal.c), so the risk this test is written against is not
 * that one of them computes the wrong answer, but that a type or size is
 * generated at the wrong width, that a stride is scaled wrong, or that a C11
 * `_Generic` association reaches the wrong routine - as test-amo-types.c says
 * of the same risk on the AMO side.
 *
 * The pattern throughout is the ring test-signal.c and test-amo-types.c use:
 * every PE puts into its right neighbour's copy and, after a barrier, checks
 * its own copy against what its left neighbour must have sent - which
 * exercises a real cross-PE transfer regardless of whether "right" is a
 * same-node or a remote PE, a distinction this file never has to name because
 * the logical node map that chooses it is the ctest registration
 * (CMakeLists.txt), not this program. A round trip through the same neighbour
 * (put, then get back) checks put and get together against the value this PE
 * itself sent, which needs no knowledge of any other PE's rank.
 *
 * A PE checks its own copy after a barrier, and its left neighbour must not
 * write that copy again until the check is done: every write into the right
 * neighbour's copy that follows a check of the one before is behind a barrier
 * of its own. Without it the test races itself - the neighbour's next put can
 * land between this PE's barrier and its check.
 *
 * Every symmetric object is allocated with a guard element past what the test
 * writes, checked after every operation: a routine generated at the wrong
 * width passes every value check and corrupts its neighbour instead, exactly
 * the failure mode test-amo-types.c's own guard catches on the AMO side.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

static int me, n, right, left;

/* ---- Block put/get (spec §9.6.1.1, §9.6.1.5) --------------------------- */

#define BLOCK_RMA(T, NAME)                                                        \
    do {                                                                          \
        enum { N = 6 };                                                          \
        T *remote = shmem_malloc((N + 1) * sizeof *remote); /* +1 guard */       \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */  \
        remote[N] = (T)0x5a;                                                     \
        T mine[N], back[N];                                                      \
        for (int i = 0; i < N; i++)                                              \
            mine[i] = (T)(me * 10 + i + 1);                                      \
        shmem_barrier_all();                                                     \
                                                                                    \
        shmem_##NAME##_put(remote, mine, N, right);                              \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
        CHECK(remote[N] == (T)0x5a);                                             \
                                                                                    \
        shmem_##NAME##_get(back, remote, N, right);                              \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        /* Zero nelems touches nothing. */                                       \
        shmem_##NAME##_put(remote, mine, 0, right);                              \
        shmem_##NAME##_get(back, remote, 0, right);                              \
        CHECK(remote[N] == (T)0x5a);                                             \
                                                                                    \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- p/g, single element (spec §9.6.1.2, §9.6.1.6) --------------------- */

#define SCALAR_RMA(T, NAME)                                                       \
    do {                                                                          \
        T *remote = shmem_malloc(sizeof *remote);                                \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */  \
        shmem_barrier_all();                                                     \
        shmem_##NAME##_p(remote, (T)(me + 42), right);                           \
        shmem_barrier_all();                                                     \
        CHECK(*remote == (T)(left + 42));                                        \
        CHECK(shmem_##NAME##_g(remote, me) == (T)(left + 42));                   \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- iput/iget, strided (spec §9.6.1.3, §9.6.1.7) ----------------------
 *
 * A fixed, PE-independent payload: every PE writes the same two values, so
 * checking this PE's own copy needs no left/right arithmetic, only the
 * untouched elements between the strided ones to confirm the stride is really
 * skipping rather than landing contiguous by accident. */

#define STRIDED_RMA(T, NAME)                                                      \
    do {                                                                          \
        enum { N = 6 };                                                         \
        T *remote = shmem_malloc(N * sizeof *remote);                           \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */ \
        for (int i = 0; i < N; i++)                                              \
            remote[i] = (T)0;                                                    \
        T src[3] = { (T)11, (T)22, (T)33 };                                      \
        shmem_barrier_all();                                                     \
                                                                                    \
        shmem_##NAME##_iput(remote, src, 2, 1, 3, right); /* dst stride 2 */     \
        shmem_barrier_all();                                                     \
        CHECK(remote[0] == (T)11 && remote[2] == (T)22 && remote[4] == (T)33);   \
        CHECK(remote[1] == (T)0 && remote[3] == (T)0 && remote[5] == (T)0);      \
                                                                                    \
        T dst[3] = { 0, 0, 0 };                                                  \
        shmem_##NAME##_iget(dst, remote, 1, 2, 3, right); /* src stride 2 */     \
        CHECK(dst[0] == (T)11 && dst[1] == (T)22 && dst[2] == (T)33);            \
                                                                                    \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- put_nbi/get_nbi (spec §9.6.2.1-§9.6.2.2) --------------------------- */

#define NBI_RMA(T, NAME)                                                          \
    do {                                                                          \
        enum { N = 6 };                                                          \
        T *remote = shmem_malloc(N * sizeof *remote);                            \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */  \
        T mine[N], back[N];                                                      \
        for (int i = 0; i < N; i++)                                              \
            mine[i] = (T)(me * 10 + i + 1);                                      \
        shmem_barrier_all();                                                     \
                                                                                    \
        shmem_##NAME##_put_nbi(remote, mine, N, right);                          \
        shmem_quiet();                                                           \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
                                                                                    \
        shmem_##NAME##_get_nbi(back, remote, N, right);                          \
        shmem_quiet();                                                           \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- The C11 generic macros, plain and ctx-qualified -------------------
 *
 * Plain and ctx forms differ only in arity, and `_Generic` dispatch is
 * selected on argument count before type (§7.3), so both spellings are called
 * here: SHMEM_CTX_DEFAULT is the only context phase 1 has, so the two reach
 * the same routine, but only calling the ctx form too proves the wider
 * OSOE_PICK7/OSOE_PICK8 selectors (iput/iget/put_signal) route the 7- and
 * 8-argument calls the same way the existing OSOE_PICK3/4/5 ones already did
 * for the atomics.
 */
#define GENERIC_RMA(T, NAME)                                                      \
    do {                                                                          \
        enum { N = 4 };                                                         \
        T *remote = shmem_malloc(N * sizeof *remote);                           \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */ \
        T mine[N], back[N];                                                      \
        for (int i = 0; i < N; i++)                                              \
            mine[i] = (T)(me * 10 + i + 1);                                      \
        shmem_barrier_all();                                                     \
                                                                                    \
        shmem_put(remote, mine, N, right);                                       \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
        shmem_get(back, remote, N, right);                                       \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        shmem_barrier_all();                                                     \
        shmem_p(remote, (T)55, right);                                           \
        shmem_barrier_all();                                                     \
        CHECK(shmem_g(remote, me) == (T)55);                                     \
                                                                                    \
        T strided_src[2] = { (T)61, (T)62 };                                     \
        T strided_back[2] = { 0, 0 };                                            \
        shmem_barrier_all();                                                     \
        shmem_iput(remote, strided_src, 2, 1, 2, right);                         \
        shmem_barrier_all();                                                     \
        CHECK(remote[0] == (T)61 && remote[2] == (T)62);                         \
        shmem_iget(strided_back, remote, 1, 2, 2, right);                        \
        CHECK(strided_back[0] == (T)61 && strided_back[1] == (T)62);             \
                                                                                    \
        shmem_barrier_all();                                                     \
        shmem_put_nbi(remote, mine, N, right);                                   \
        shmem_quiet();                                                           \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
        shmem_get_nbi(back, remote, N, right);                                   \
        shmem_quiet();                                                           \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        /* The ctx-qualified spellings (design notes §7.3). */                   \
        shmem_barrier_all();                                                     \
        shmem_put(SHMEM_CTX_DEFAULT, remote, mine, N, right);                    \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
        shmem_get(SHMEM_CTX_DEFAULT, back, remote, N, right);                    \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
        shmem_barrier_all();                                                     \
        shmem_iput(SHMEM_CTX_DEFAULT, remote, strided_src, 2, 1, 2, right);      \
        shmem_barrier_all();                                                     \
        CHECK(remote[0] == (T)61 && remote[2] == (T)62);                         \
        shmem_iget(SHMEM_CTX_DEFAULT, strided_back, remote, 1, 2, 2, right);     \
        CHECK(strided_back[0] == (T)61 && strided_back[1] == (T)62);             \
        shmem_barrier_all();                                                     \
        shmem_put_nbi(SHMEM_CTX_DEFAULT, remote, mine, N, right);                \
        shmem_quiet();                                                           \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < N; i++)                                              \
            CHECK(remote[i] == (T)(left * 10 + i + 1));                          \
        shmem_get_nbi(SHMEM_CTX_DEFAULT, back, remote, N, right);                \
        shmem_quiet();                                                           \
        for (int i = 0; i < N; i++)                                              \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- The sized forms, putSIZE..128/getSIZE..128 and their strided and
 * nonblocking forms (spec §9.6.1.1, §9.6.1.3, §9.6.1.5, §9.6.1.7,
 * §9.6.2.1-§9.6.2.2) --------------------------------------------------- */

#define SIZED_RMA(SIZE)                                                           \
    do {                                                                          \
        enum { NELEMS = 4, WIDTH = (SIZE) / 8 };                                 \
        unsigned char *remote = shmem_malloc((size_t)NELEMS * WIDTH + 1); /* +1 guard */ \
        CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */  \
        remote[NELEMS * WIDTH] = 0x5a;                                           \
        unsigned char mine[NELEMS * WIDTH], back[NELEMS * WIDTH];                \
        for (int i = 0; i < NELEMS * WIDTH; i++)                                 \
            mine[i] = (unsigned char)(me * 7 + i + 1);                           \
        shmem_barrier_all();                                                     \
                                                                                    \
        shmem_put##SIZE(remote, mine, NELEMS, right);                            \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < NELEMS * WIDTH; i++)                                 \
            CHECK(remote[i] == (unsigned char)(left * 7 + i + 1));               \
        CHECK(remote[NELEMS * WIDTH] == 0x5a);                                   \
                                                                                    \
        shmem_get##SIZE(back, remote, NELEMS, right);                            \
        for (int i = 0; i < NELEMS * WIDTH; i++)                                 \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        /* Zero nelems touches nothing. */                                       \
        shmem_put##SIZE(remote, mine, 0, right);                                 \
        shmem_get##SIZE(back, remote, 0, right);                                 \
        CHECK(remote[NELEMS * WIDTH] == 0x5a);                                   \
                                                                                    \
        /* Strided: two elements into a four-element target, stride 2. */        \
        unsigned char strided_src[2 * WIDTH], strided_back[2 * WIDTH];           \
        for (int i = 0; i < 2 * WIDTH; i++)                                      \
            strided_src[i] = (unsigned char)(0x90 + i);                          \
        shmem_barrier_all();                                                     \
        shmem_iput##SIZE(remote, strided_src, 2, 1, 2, right);                   \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < WIDTH; i++) {                                        \
            CHECK(remote[i] == strided_src[i]);                                  \
            CHECK(remote[2 * WIDTH + i] == strided_src[WIDTH + i]);              \
        }                                                                        \
        shmem_iget##SIZE(strided_back, remote, 1, 2, 2, right);                  \
        for (int i = 0; i < 2 * WIDTH; i++)                                      \
            CHECK(strided_back[i] == strided_src[i]);                            \
                                                                                    \
        shmem_barrier_all();                                                     \
        shmem_put##SIZE##_nbi(remote, mine, NELEMS, right);                      \
        shmem_quiet();                                                           \
        shmem_barrier_all();                                                     \
        for (int i = 0; i < NELEMS * WIDTH; i++)                                 \
            CHECK(remote[i] == (unsigned char)(left * 7 + i + 1));               \
        shmem_get##SIZE##_nbi(back, remote, NELEMS, right);                      \
        shmem_quiet();                                                           \
        for (int i = 0; i < NELEMS * WIDTH; i++)                                 \
            CHECK(back[i] == mine[i]);                                           \
                                                                                    \
        shmem_free(remote);                                                      \
    } while (0);

/* ---- A strided put is batched: one wake of the lead, not one per element --
 *
 * Every element of an iput but the last carries OSOE_TRANSPORT_MORE
 * (rma-typed.c), whose one effect on the AM path is to defer the wake asking
 * the lead to run - no effect on results by design, so the only way to see it
 * is to count it, as test-internode.c counts put_signal's (§4.10). Only a put
 * that crosses a node reaches the AM path; to a node-mate the flag changes
 * nothing, and none is deferred. */

/* This PE's total for a thread-scope counter, read as osoe-stat reads it: the
 * counter blocks are per local PE, so indexed by local id (test-internode.c). */
static uint64_t counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    const char *local = osoe_test_launcher("LOCALID");
    uint32_t local_id = (uint32_t)atoi(local ? local : "0");
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, local_id * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

static void strided_batching_check(void)
{
    enum { N = 5 };
    long *remote = shmem_malloc(2 * N * sizeof *remote);
    CHECK_FATAL(remote != NULL); /* NULL would be dereferenced just below */
    long src[N];
    for (int i = 0; i < N; i++)
        src[i] = me * 100 + i;
    shmem_barrier_all();

    bool crosses = shmem_ptr(remote, right) == NULL;   /* the network carries it */
    uint64_t before = counter("am.wakes_deferred");
    shmem_long_iput(remote, src, 2, 1, N, right);
    uint64_t deferred = counter("am.wakes_deferred") - before;
    if (deferred != (crosses ? N - 1 : 0))
        fprintf(stderr, "PE %d: an iput of %d elements %s deferred %llu wakes\n", me, N,
                crosses ? "across nodes" : "to a node-mate", (unsigned long long)deferred);
    CHECK(deferred == (crosses ? N - 1 : 0));

    shmem_barrier_all();
    for (int i = 0; i < N; i++)
        CHECK(remote[2 * i] == left * 100 + i);
    shmem_free(remote);
}

/* ---- Typed put-with-signal (spec §9.8.3-§9.8.4) ------------------------- */

static void put_signal_check(void)
{
    uint64_t *sig = shmem_malloc(sizeof *sig);
    int *payload = shmem_malloc(4 * sizeof *payload);
    CHECK_FATAL(sig != NULL && payload != NULL); /* NULL would be dereferenced below */
    int mine[4];
    for (int i = 0; i < 4; i++)
        mine[i] = me * 10 + i;

    *sig = 0;
    shmem_barrier_all();
    shmem_int_put_signal(payload, mine, 4, sig, 1, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 1) == 1);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    /* nelems 0 still signals (as putmem_signal's own; §9.8.3). */
    *sig = 0;
    shmem_barrier_all();
    shmem_int_put_signal(payload, mine, 0, sig, 2, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 2) == 2);

    /* The nonblocking form. */
    *sig = 0;
    shmem_barrier_all();
    shmem_int_put_signal_nbi(payload, mine, 4, sig, 3, SHMEM_SIGNAL_SET, right);
    shmem_quiet();
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 3) == 3);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    /* The C11 generic, plain and ctx-qualified (OSOE_PICK8). */
    *sig = 0;
    shmem_barrier_all();
    shmem_put_signal(payload, mine, 4, sig, 4, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 4) == 4);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    *sig = 0;
    shmem_barrier_all();
    shmem_put_signal(SHMEM_CTX_DEFAULT, payload, mine, 4, sig, 5, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 5) == 5);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    /* put_signal_nbi's C11 generic, plain and ctx-qualified. */
    *sig = 0;
    shmem_barrier_all();
    shmem_put_signal_nbi(payload, mine, 4, sig, 6, SHMEM_SIGNAL_SET, right);
    shmem_quiet();
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 6) == 6);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    *sig = 0;
    shmem_barrier_all();
    shmem_put_signal_nbi(SHMEM_CTX_DEFAULT, payload, mine, 4, sig, 7, SHMEM_SIGNAL_SET, right);
    shmem_quiet();
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 7) == 7);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    /* The sized forms scale nelems by their width: four 32-bit elements and
     * two 64-bit ones are the same sixteen bytes. The second round's payload
     * differs from the first's, so each round's check sees its own bytes. */
    *sig = 0;
    shmem_barrier_all();
    shmem_put32_signal(payload, mine, 4, sig, 8, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 8) == 8);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i);

    for (int i = 0; i < 4; i++)
        mine[i] = me * 10 + i + 100;
    *sig = 0;
    shmem_barrier_all();
    shmem_put64_signal_nbi(payload, mine, 2, sig, 9, SHMEM_SIGNAL_ADD, right);
    shmem_quiet();
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 9) == 9);
    for (int i = 0; i < 4; i++)
        CHECK(payload[i] == left * 10 + i + 100);

    shmem_free(payload);
    shmem_free(sig);
}

/* A representative subset of Table 5: both integer widths either side of a
 * byte, both floating types, an exact-width alias, size_t, and char - not
 * every one of the 24, but every family the table's own shape distinguishes. */
#define TEST_TYPES(X)                                                             \
    X(int, int) X(long, long) X(double, double) X(long double, longdouble)        \
    X(uint8_t, uint8) X(uint64_t, uint64) X(size_t, size)                         \
    X(char, char) X(float, float)

/* The generic macros are a dispatch check more than a per-type one (a wrong
 * association is a compile error), so a handful of types is enough: one
 * integer, one floating, one exact-width alias. */
#define GENERIC_TEST_TYPES(X) X(int, int) X(double, double) X(uint64_t, uint64)

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    n = shmem_n_pes();
    right = (me + 1) % n;
    left = (me + n - 1) % n;

    TEST_TYPES(BLOCK_RMA)
    TEST_TYPES(SCALAR_RMA)
    TEST_TYPES(STRIDED_RMA)
    TEST_TYPES(NBI_RMA)
    GENERIC_TEST_TYPES(GENERIC_RMA)

    OSOE_RMA_SIZES(SIZED_RMA)

    strided_batching_check();
    put_signal_check();

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
