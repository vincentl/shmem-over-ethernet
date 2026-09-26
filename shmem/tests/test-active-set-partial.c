/*
 * The deprecated collectives on active sets that are not the whole job
 * (Annex F; phase 1 step 5 slice 7; active-set.c).
 *
 * Six sets over the job's PEs: all but the last, all but the first, the even
 * PEs and the odd ones - these two at once, each with its own pSync - a set of
 * one, and a pair. On each, every member runs shmem_barrier, shmem_sync,
 * broadcast64 from every root, fcollect64, collect64 with a different count
 * per member, the sum, max and product reductions (a long one past the
 * reduction's chunk, one in place, one of doubles), alltoall64 and alltoalls32
 * with strides - three rounds back to back on the same pSync, with no barrier
 * between, as the spec allows a set to reuse it. Every result is checked by
 * value, and at the end every pSync word is SHMEM_SYNC_VALUE again.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SETS = 6, ROUNDS = 3, BLOCK = 5, LONG_REDUCE = 20000, MAX_PES = 64 };

static int me, npes;

struct set {
    int start, stride_log, size;
};

static int member_pe(const struct set *s, int rank)
{
    return s->start + (rank << s->stride_log);
}

static int rank_in(const struct set *s)
{
    int offset = me - s->start;
    if (offset < 0 || (offset & ((1 << s->stride_log) - 1)))
        return -1;
    int rank = offset >> s->stride_log;
    return rank < s->size ? rank : -1;
}

static long pattern(int a, int b, int c)
{
    return (long)a * 1000000 + (long)b * 1000 + c;
}

static long *psync;             /* [SETS][SHMEM_REDUCE_SYNC_SIZE] */
static long *arrivals;          /* [SETS], at each set's rank 0 */
static long *source, *dest;     /* LONG_REDUCE longs each */
static double *dsource, *ddest;
static int32_t *isource, *idest;
static long *pwrk;              /* SHMEM_REDUCE_MIN_WRKDATA_SIZE, unused by osoe */

static void run(const struct set *s, int index, int round)
{
    int rank = rank_in(s);
    if (rank < 0)
        return;
    long *sync = &psync[index * SHMEM_REDUCE_SYNC_SIZE];
    int n = s->size;

    /* barrier: every member's increment at rank 0 is there after it. */
    shmem_long_atomic_inc(&arrivals[index], member_pe(s, 0));
    shmem_barrier(s->start, s->stride_log, n, sync);
    if (rank == 0)
        CHECK(shmem_long_atomic_fetch(&arrivals[index], me) == (long)n * (round + 1));
    shmem_sync(s->start, s->stride_log, n, sync);

    /* broadcast64 from every root; the root's dest untouched. */
    for (int root = 0; root < n; root++) {
        for (int i = 0; i < BLOCK; i++) {
            source[i] = pattern(root, round, i);
            dest[i] = -7;
        }
        shmem_broadcast64(dest, source, BLOCK, root, s->start, s->stride_log, n, sync);
        for (int i = 0; i < BLOCK; i++)
            CHECK(dest[i] == (rank == root ? -7 : pattern(root, round, i)));
        /* dest is read before the next root's put: a sync first. */
        shmem_sync(s->start, s->stride_log, n, sync);
    }

    /* fcollect64. */
    for (int i = 0; i < BLOCK; i++)
        source[i] = pattern(rank, round, i);
    shmem_fcollect64(dest, source, BLOCK, s->start, s->stride_log, n, sync);
    for (int r = 0; r < n; r++)
        for (int i = 0; i < BLOCK; i++)
            CHECK(dest[r * BLOCK + i] == pattern(r, round, i));
    shmem_sync(s->start, s->stride_log, n, sync);

    /* collect64: rank r gives r % 3 + 1 elements. */
    int mine = rank % 3 + 1;
    for (int i = 0; i < mine; i++)
        source[i] = pattern(rank, round, 100 + i);
    shmem_collect64(dest, source, (size_t)mine, s->start, s->stride_log, n, sync);
    int at = 0;
    for (int r = 0; r < n; r++)
        for (int i = 0; i < r % 3 + 1; i++, at++)
            CHECK(dest[at] == pattern(r, round, 100 + i));
    shmem_sync(s->start, s->stride_log, n, sync);

    /* Reductions: long sum past a chunk, int max in place, double product. */
    for (int i = 0; i < LONG_REDUCE; i++)
        source[i] = (long)rank * 7 + i + round;
    shmem_long_sum_to_all(dest, source, LONG_REDUCE, s->start, s->stride_log, n, pwrk, sync);
    long rank_sum = (long)n * (n - 1) / 2 * 7;
    int bad = 0;
    for (int i = 0; i < LONG_REDUCE; i++)
        bad += dest[i] != rank_sum + (long)n * (i + round);
    CHECK(bad == 0);
    shmem_sync(s->start, s->stride_log, n, sync);
    for (int i = 0; i < BLOCK; i++)
        idest[i] = (int32_t)((rank * 13 + i * 5 + round) % 17);
    shmem_int_max_to_all(idest, idest, BLOCK, s->start, s->stride_log, n, (int *)pwrk, sync);
    for (int i = 0; i < BLOCK; i++) {
        int32_t want = 0;
        for (int r = 0; r < n; r++) {
            int32_t v = (int32_t)((r * 13 + i * 5 + round) % 17);
            want = v > want ? v : want;
        }
        CHECK(idest[i] == want);
    }
    shmem_sync(s->start, s->stride_log, n, sync);
    for (int i = 0; i < BLOCK; i++)
        dsource[i] = 1.0 + (rank + i) % 3;
    shmem_double_prod_to_all(ddest, dsource, BLOCK, s->start, s->stride_log, n,
                             (double *)pwrk, sync);
    for (int i = 0; i < BLOCK; i++) {
        double want = 1.0;
        for (int r = 0; r < n; r++)
            want *= 1.0 + (r + i) % 3;
        CHECK(ddest[i] == want);
    }
    shmem_sync(s->start, s->stride_log, n, sync);

    /* alltoall64: block of 3 from j to i. */
    for (int j = 0; j < n; j++)
        for (int k = 0; k < 3; k++)
            source[j * 3 + k] = pattern(rank, j, k + round);
    shmem_alltoall64(dest, source, 3, s->start, s->stride_log, n, sync);
    for (int j = 0; j < n; j++)
        for (int k = 0; k < 3; k++)
            CHECK(dest[j * 3 + k] == pattern(j, rank, k + round));
    /* Ready before the sync: alltoalls writes into other members' dests with
     * no barrier first, as the spec lets it, so a fast member's blocks may
     * arrive before this one's own call. */
    memset(idest, 0, sizeof(int32_t) * (size_t)(n * 2 * 2));
    shmem_sync(s->start, s->stride_log, n, sync);

    /* alltoalls32: 2 elements a block, dest stride 2, source stride 3. */
    for (int j = 0; j < n; j++)
        for (int k = 0; k < 2; k++)
            isource[(j * 2 + k) * 3] = (int32_t)(rank * 100 + j * 10 + k + round);
    shmem_alltoalls32(idest, isource, 2, 3, 2, s->start, s->stride_log, n, sync);
    for (int j = 0; j < n; j++)
        for (int k = 0; k < 2; k++)
            CHECK(idest[(j * 2 + k) * 2] == (int32_t)(j * 100 + rank * 10 + k + round));
    shmem_sync(s->start, s->stride_log, n, sync);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(npes <= MAX_PES);
    if (npes < 2) {                     /* no set within one PE is not the whole job */
        shmem_finalize();
        return CHECK_DONE();
    }
    struct set sets[SETS] = {
        { 0, 0, npes - 1 },                 /* all but the last */
        { 1, 0, npes - 1 },                 /* all but the first */
        { 0, 1, (npes + 1) / 2 },           /* the even PEs */
        { 1, 1, npes / 2 },                 /* the odd PEs, at once with the even */
        { npes - 1, 0, 1 },                 /* one */
        { 0, 0, 2 },                        /* a pair */
    };
    psync = shmem_malloc(sizeof(long) * SETS * SHMEM_REDUCE_SYNC_SIZE);
    arrivals = shmem_calloc(SETS, sizeof *arrivals);
    source = shmem_malloc(sizeof(long) * LONG_REDUCE);
    dest = shmem_malloc(sizeof(long) * LONG_REDUCE);
    dsource = shmem_malloc(sizeof(double) * BLOCK);
    ddest = shmem_malloc(sizeof(double) * BLOCK);
    isource = shmem_malloc(sizeof(int32_t) * MAX_PES * 2 * 3);
    idest = shmem_malloc(sizeof(int32_t) * MAX_PES * 2 * 2);
    pwrk = shmem_malloc(sizeof(long) * SHMEM_REDUCE_MIN_WRKDATA_SIZE);
    CHECK_FATAL(psync && arrivals && source && dest && dsource && ddest && isource && idest &&
                pwrk);
    for (int i = 0; i < SETS * SHMEM_REDUCE_SYNC_SIZE; i++)
        psync[i] = SHMEM_SYNC_VALUE;
    shmem_barrier_all();

    for (int index = 0; index < SETS; index++) {
        /* The even and odd sets run together, as index 2 and 3 in one step. */
        if (index == 3)
            continue;
        for (int round = 0; round < ROUNDS; round++) {
            run(&sets[index], index, round);
            if (index == 2)
                run(&sets[3], 3, round);
        }
        shmem_barrier_all();
    }

    for (int i = 0; i < SETS * SHMEM_REDUCE_SYNC_SIZE; i++)
        if (psync[i] != SHMEM_SYNC_VALUE) {
            fprintf(stderr, "PE %d: pSync of set %d word %d is %ld after the last call\n", me,
                    i / SHMEM_REDUCE_SYNC_SIZE, i % SHMEM_REDUCE_SYNC_SIZE, psync[i]);
            CHECK(0);
            break;
        }
    shmem_barrier_all();
    shmem_free(pwrk);
    shmem_free(idest);
    shmem_free(isource);
    shmem_free(ddest);
    shmem_free(dsource);
    shmem_free(dest);
    shmem_free(source);
    shmem_free(arrivals);
    shmem_free(psync);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
