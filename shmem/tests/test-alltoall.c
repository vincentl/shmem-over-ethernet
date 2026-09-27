/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * alltoall and alltoalls (spec §9.10.5, §9.10.6; collectives.c's exchange,
 * alltoall.c): PE i's block j lands in PE j's dest block i.
 *
 *   - alltoall of ints, longs and doubles, one element a block and many;
 *   - alltoalls with dest and source strides that differ, and the elements
 *     between the strided ones left as they were;
 *   - the byte forms, the C11 generics, and the deprecated alltoall32/64 and
 *     alltoalls32/64;
 *   - over SHMEM_TEAM_SHARED;
 *   - every block checked, on every PE, on return: the barrier the exchange
 *     ends with is what makes them all there, and its quiet is checked by
 *     this PE's count of the transport's waits.
 *
 * With OSOE_TEST_ALLTOALL_STRIDE=0 a PE calls shmem_long_alltoalls with a dest
 * stride of 0, which must end the job by name (shmem.alltoall.stride).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static int me, npes;
static long *pSync;

/*
 * This PE's count of `name`, a counter of its threads', summed over its
 * counter blocks in its node's control segment (§3.14).
 */
static uint64_t own_count(const char *name)
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
    uint32_t mine = local ? (uint32_t)atoi(local) : 0;
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, mine * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

/* PE i's element e of its block for PE j. */
static long value(int i, int j, size_t e)
{
    return (long)i * 1000000 + (long)j * 1000 + (long)(e % 1000);
}

#define ALLTOALL_CHECK(T, NAME, N)                                                  \
    do {                                                                            \
        size_t n = (N), all = (size_t)npes * n;                                     \
        T *source = shmem_malloc(all * sizeof *source);                             \
        T *dest = shmem_malloc(all * sizeof *dest);                                 \
        CHECK_FATAL(source && dest);                                                \
        for (int j = 0; j < npes; j++)                                              \
            for (size_t e = 0; e < n; e++)                                          \
                source[(size_t)j * n + e] = (T)value(me, j, e);                     \
        shmem_barrier_all();                                                        \
        CHECK(shmem_##NAME##_alltoall(SHMEM_TEAM_WORLD, dest, source, n) == 0);     \
        size_t wrong = 0;                                                           \
        for (int i = 0; i < npes; i++)                                              \
            for (size_t e = 0; e < n; e++)                                          \
                wrong += dest[(size_t)i * n + e] != (T)value(i, me, e);             \
        if (wrong)                                                                  \
            fprintf(stderr, "PE %d: %s alltoall of %zu: %zu wrong\n", me, #NAME, n,  \
                    wrong);                                                         \
        CHECK(wrong == 0);                                                          \
        shmem_barrier_all();                                                        \
        shmem_free(dest);                                                           \
        shmem_free(source);                                                         \
    } while (0)

static void alltoalls_contiguous(void)
{
    ALLTOALL_CHECK(int, int, 1);
    ALLTOALL_CHECK(int, int, 777);
    ALLTOALL_CHECK(long, long, 1);
    ALLTOALL_CHECK(long, long, 2000);
    ALLTOALL_CHECK(long, long, 65536);          /* half a MiB a block */
    ALLTOALL_CHECK(double, double, 3);

    /* The barrier alltoall ends with quiets first, which the transport counts
     * among this PE's waits: half a MiB a block did not catch an alltoall
     * without it (the verification of this slice), the send buffer being
     * bounded - as for shmem_barrier (test-active-set.c). */
    long *qsource = shmem_malloc((size_t)npes * sizeof *qsource);
    long *qdest = shmem_malloc((size_t)npes * sizeof *qdest);
    CHECK_FATAL(qsource && qdest);
    for (int j = 0; j < npes; j++)
        qsource[j] = value(me, j, 9);
    shmem_barrier_all();
    uint64_t waits = own_count("transport.wait.calls");
    CHECK(shmem_long_alltoall(SHMEM_TEAM_WORLD, qdest, qsource, 1) == 0);
    CHECK(own_count("transport.wait.calls") > waits);
    for (int i = 0; i < npes; i++)
        CHECK(qdest[i] == value(i, me, 9));
    shmem_barrier_all();
    shmem_free(qdest);
    shmem_free(qsource);

    /* The byte form and the generic spelling. */
    char *source = shmem_malloc(3 * (size_t)npes), *dest = shmem_malloc(3 * (size_t)npes);
    long *lsource = shmem_malloc((size_t)npes * sizeof *lsource);
    long *ldest = shmem_malloc((size_t)npes * sizeof *ldest);
    CHECK_FATAL(source && dest && lsource && ldest);
    for (int j = 0; j < npes; j++) {
        for (int e = 0; e < 3; e++)
            source[3 * j + e] = (char)(me * 16 + j + e);
        lsource[j] = value(me, j, 0);
    }
    shmem_barrier_all();
    CHECK(shmem_alltoallmem(SHMEM_TEAM_WORLD, dest, source, 3) == 0);
    CHECK(shmem_alltoall(SHMEM_TEAM_WORLD, ldest, lsource, 1) == 0);
    size_t wrong = 0;
    for (int i = 0; i < npes; i++) {
        for (int e = 0; e < 3; e++)
            wrong += dest[3 * i + e] != (char)(i * 16 + me + e);
        wrong += ldest[i] != value(i, me, 0);
    }
    CHECK(wrong == 0);
    shmem_barrier_all();
    shmem_free(ldest);
    shmem_free(lsource);
    shmem_free(dest);
    shmem_free(source);
}

/* alltoalls: source strided by 3, dest by 2 - element (j n + e) sst of
 * source goes to element (i n + e) dst of PE j's dest - and every element in
 * between untouched. */
static void strided(void)
{
    enum { N = 5, SST = 3, DST = 2 };
    size_t span = (size_t)npes * N;
    long *source = shmem_malloc(span * SST * sizeof *source);
    long *dest = shmem_malloc(span * DST * sizeof *dest);
    CHECK_FATAL(source && dest);
    for (size_t k = 0; k < span * SST; k++)
        source[k] = -7;
    for (size_t k = 0; k < span * DST; k++)
        dest[k] = -9;
    for (int j = 0; j < npes; j++)
        for (size_t e = 0; e < N; e++)
            source[((size_t)j * N + e) * SST] = value(me, j, e);
    shmem_barrier_all();
    CHECK(shmem_long_alltoalls(SHMEM_TEAM_WORLD, dest, source, DST, SST, N) == 0);
    size_t wrong = 0;
    for (size_t k = 0; k < span * DST; k++) {
        size_t block = k / DST;
        long want = k % DST ? -9 : value((int)(block / N), me, block % N);
        wrong += dest[k] != want;
    }
    if (wrong)
        fprintf(stderr, "PE %d: alltoalls: %zu wrong\n", me, wrong);
    CHECK(wrong == 0);

    /* The byte form, a long's bytes a block, and the generic spelling, both
     * with strides of 1. */
    shmem_barrier_all();
    for (int j = 0; j < npes; j++)
        source[j] = value(me, j, 4);
    shmem_barrier_all();
    CHECK(shmem_alltoallsmem(SHMEM_TEAM_WORLD, dest, source, 1, 1, sizeof(long)) == 0);
    for (int i = 0; i < npes; i++)
        wrong += dest[i] != value(i, me, 4);
    CHECK(shmem_alltoalls(SHMEM_TEAM_WORLD, dest + npes, source, 1, 1, 1) == 0);
    for (int i = 0; i < npes; i++)
        wrong += dest[npes + i] != value(i, me, 4);
    CHECK(wrong == 0);
    shmem_barrier_all();
    shmem_free(dest);
    shmem_free(source);
}

static void active_set_forms(void)
{
    uint32_t *s32 = shmem_malloc(2 * (size_t)npes * sizeof *s32);
    uint32_t *d32 = shmem_malloc(2 * (size_t)npes * sizeof *d32);
    uint64_t *s64 = shmem_malloc(2 * (size_t)npes * 2 * sizeof *s64);
    uint64_t *d64 = shmem_malloc(2 * (size_t)npes * 2 * sizeof *d64);
    CHECK_FATAL(s32 && d32 && s64 && d64);
    for (int j = 0; j < npes; j++)
        for (int e = 0; e < 2; e++) {
            s32[2 * j + e] = (uint32_t)(me * 100 + j * 10 + e);
            s64[2 * (2 * j + e)] = (uint64_t)me << 32 | (uint64_t)(j * 10 + e);
        }
    shmem_barrier_all();
    shmem_alltoall32(d32, s32, 2, 0, 0, npes, pSync);
    shmem_alltoalls64(d64, s64, 1, 2, 2, 0, 0, npes, pSync);
    size_t wrong = 0;
    for (int i = 0; i < npes; i++)
        for (int e = 0; e < 2; e++) {
            wrong += d32[2 * i + e] != (uint32_t)(i * 100 + me * 10 + e);
            wrong += d64[2 * i + e] != ((uint64_t)i << 32 | (uint64_t)(me * 10 + e));
        }
    CHECK(wrong == 0);
    shmem_alltoall64(d64, d64 + 2 * (size_t)npes, 0, 0, 0, npes, pSync);   /* nelems 0: nothing */
    shmem_alltoalls32(d32, s32, 1, 1, 2, 0, 0, npes, pSync);
    for (int i = 0; i < npes; i++)
        for (int e = 0; e < 2; e++)
            wrong += d32[2 * i + e] != (uint32_t)(i * 100 + me * 10 + e);
    CHECK(wrong == 0);
    for (int i = 0; i < SHMEM_ALLTOALLS_SYNC_SIZE; i++)
        CHECK(pSync[i] == SHMEM_SYNC_VALUE);
    shmem_barrier_all();
    shmem_free(d64);
    shmem_free(s64);
    shmem_free(d32);
    shmem_free(s32);
}

static void shared_team(void)
{
    int n = shmem_team_n_pes(SHMEM_TEAM_SHARED), mine = shmem_team_my_pe(SHMEM_TEAM_SHARED);
    int *source = shmem_malloc((size_t)npes * sizeof *source);
    int *dest = shmem_malloc((size_t)npes * sizeof *dest);
    CHECK_FATAL(source && dest);
    for (int j = 0; j < n; j++)
        source[j] = mine * 100 + j;
    shmem_barrier_all();
    CHECK(shmem_int_alltoall(SHMEM_TEAM_SHARED, dest, source, 1) == 0);
    size_t wrong = 0;
    for (int i = 0; i < n; i++)
        wrong += dest[i] != i * 100 + mine;
    CHECK(wrong == 0);
    shmem_barrier_all();
    shmem_free(dest);
    shmem_free(source);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    pSync = shmem_malloc(SHMEM_ALLTOALLS_SYNC_SIZE * sizeof *pSync);
    CHECK_FATAL(pSync);
    for (int i = 0; i < SHMEM_ALLTOALLS_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;
    shmem_barrier_all();

    const char *stride = getenv("OSOE_TEST_ALLTOALL_STRIDE");
    if (stride && stride[0] == '0') {
        long *x = shmem_malloc((size_t)npes * sizeof *x);
        CHECK_FATAL(x);
        shmem_long_alltoalls(SHMEM_TEAM_WORLD, x, x, 0, 1, 1);
        printf("PE %d: a stride of 0 returned, which it must not\n", me);
        return 1;
    }

    alltoalls_contiguous();
    strided();
    active_set_forms();
    shared_team();

    shmem_free(pSync);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
