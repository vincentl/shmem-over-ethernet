/*
 * collect and fcollect (spec §9.10.8; collectives.c's gather, collect.c).
 *
 *   - fcollect of ints, longs, doubles and bytes, one element each and enough
 *     that the whole result crosses the internal region's pieces, every
 *     block checked at its place;
 *   - collect with blocks that differ from PE to PE, some of them empty, and
 *     with every block but one empty;
 *   - the C11 generics, and the deprecated fcollect32/64 and collect32/64;
 *   - fcollect over SHMEM_TEAM_SHARED, the node's own team; a collect over
 *     one node's alone, then an allocation over the job and a put across
 *     nodes into it, which a room for collect's lengths not given back as it
 *     was taken would send astray; and the next allocation after two collects
 *     where the one before them was;
 *   - back to back on one dest with no barrier between - an fcollect, a
 *     reduction into the same array, another fcollect - which a node writing
 *     another node's dest for the next collective, while that node's members
 *     still copy the last one out, would corrupt;
 *   - pSync, a static, as it was at the end.
 *
 * With OSOE_TEST_PARTIAL=1 the last PE calls shmem_fcollect64 naming an active
 * set of the PEs before it, which it is not in, and that must end the job by
 * name (shmem.collect.partial); a set within the job but not all of it is
 * test-active-set-partial.c's.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { MANY = 3000 };

static int me, npes;
static long pSync[SHMEM_COLLECT_SYNC_SIZE];     /* a static, as the corpus declares it */

#define FCOLLECT_CHECK(T, NAME, N)                                                  \
    do {                                                                            \
        size_t n = (N);                                                             \
        T *source = shmem_malloc(n * sizeof *source);                               \
        T *dest = shmem_malloc((size_t)npes * n * sizeof *dest);                    \
        CHECK_FATAL(source && dest);                                                \
        for (size_t e = 0; e < n; e++)                                              \
            source[e] = (T)(me * 7 + (int)(e % 101));                               \
        for (size_t i = 0; i < (size_t)npes * n; i++)                               \
            dest[i] = (T)-1;                                                        \
        shmem_barrier_all();                                                        \
        CHECK(shmem_##NAME##_fcollect(SHMEM_TEAM_WORLD, dest, source, n) == 0);     \
        size_t wrong = 0;                                                           \
        for (int r = 0; r < npes; r++)                                              \
            for (size_t e = 0; e < n; e++)                                          \
                wrong += dest[(size_t)r * n + e] != (T)(r * 7 + (int)(e % 101));    \
        if (wrong)                                                                  \
            fprintf(stderr, "PE %d: %s fcollect of %zu: %zu wrong\n", me, #NAME, n,  \
                    wrong);                                                         \
        CHECK(wrong == 0);                                                          \
        shmem_free(dest);                                                           \
        shmem_free(source);                                                         \
    } while (0)

static void fcollects(void)
{
    FCOLLECT_CHECK(int, int, 1);
    FCOLLECT_CHECK(int, int, MANY);
    FCOLLECT_CHECK(long, long, 1);
    FCOLLECT_CHECK(long, long, MANY);
    FCOLLECT_CHECK(double, double, 5);
    FCOLLECT_CHECK(unsigned char, uchar, 3);            /* a result of odd length */
    FCOLLECT_CHECK(unsigned char, uchar, MANY + 1);

    /* The byte form and the generic spelling. */
    char *source = shmem_malloc(10), *dest = shmem_malloc(10 * (size_t)npes);
    long *lsource = shmem_malloc(2 * sizeof *lsource);
    long *ldest = shmem_malloc(2 * (size_t)npes * sizeof *ldest);
    CHECK_FATAL(source && dest && lsource && ldest);
    for (int e = 0; e < 10; e++)
        source[e] = (char)('a' + (me + e) % 26);
    lsource[0] = me;
    lsource[1] = -me;
    shmem_barrier_all();
    CHECK(shmem_fcollectmem(SHMEM_TEAM_WORLD, dest, source, 10) == 0);
    CHECK(shmem_fcollect(SHMEM_TEAM_WORLD, ldest, lsource, 2) == 0);
    size_t wrong = 0;
    for (int r = 0; r < npes; r++) {
        for (int e = 0; e < 10; e++)
            wrong += dest[r * 10 + e] != (char)('a' + (r + e) % 26);
        wrong += ldest[2 * r] != r || ldest[2 * r + 1] != -r;
    }
    CHECK(wrong == 0);
    shmem_free(ldest);
    shmem_free(lsource);
    shmem_free(dest);
    shmem_free(source);
}

/* PE r's block, `lengths(r)` elements, r * 1000 + e each. */
static size_t differing(int r)
{
    return (size_t)(r % 3) * 700;       /* 0, 700, 1400, 0, ...: empty ones among them */
}

static size_t only_last(int r)
{
    return r == npes - 1 ? 5 : 0;
}

static void collect_with(size_t (*lengths)(int), int generic)
{
    /* shmem_malloc takes the same size on every PE: the largest block. */
    size_t total = 0, largest = 1;
    for (int r = 0; r < npes; r++) {
        total += lengths(r);
        if (lengths(r) > largest)
            largest = lengths(r);
    }
    size_t mine = lengths(me);
    int *source = shmem_malloc(largest * sizeof *source);
    int *dest = shmem_malloc((total ? total : 1) * sizeof *dest);
    CHECK_FATAL(source && dest);
    for (size_t e = 0; e < mine; e++)
        source[e] = me * 1000 + (int)e;
    for (size_t i = 0; i < total; i++)
        dest[i] = -1;
    shmem_barrier_all();
    CHECK((generic ? shmem_collect(SHMEM_TEAM_WORLD, dest, source, mine)
                   : shmem_int_collect(SHMEM_TEAM_WORLD, dest, source, mine)) == 0);
    size_t wrong = 0, place = 0;
    for (int r = 0; r < npes; r++)
        for (size_t e = 0; e < lengths(r); e++)
            wrong += dest[place++] != r * 1000 + (int)e;
    if (wrong)
        fprintf(stderr, "PE %d: collect of %zu in all: %zu wrong\n", me, total, wrong);
    CHECK(wrong == 0);
    shmem_free(dest);
    shmem_free(source);
}

static void collects(void)
{
    /* collect's room for the lengths is given back as taken: the next
     * allocation lands where the one before this collect did. */
    void *before = shmem_malloc(64);
    shmem_free(before);
    collect_with(differing, 0);
    collect_with(only_last, 1);
    void *after = shmem_malloc(64);
    CHECK(after == before);
    shmem_free(after);

    /* collectmem, bytes of differing counts. */
    size_t mine = (size_t)me + 1, total = (size_t)npes * (npes + 1) / 2;
    char *source = shmem_malloc((size_t)npes), *dest = shmem_malloc(total);
    CHECK_FATAL(source && dest);
    memset(source, 'A' + me, mine);
    shmem_barrier_all();
    CHECK(shmem_collectmem(SHMEM_TEAM_WORLD, dest, source, mine) == 0);
    size_t wrong = 0, place = 0;
    for (int r = 0; r < npes; r++)
        for (int e = 0; e <= r; e++)
            wrong += dest[place++] != (char)('A' + r);
    CHECK(wrong == 0);
    shmem_free(dest);
    shmem_free(source);
}

static void active_set_forms(void)
{
    uint32_t *s32 = shmem_malloc(2 * sizeof *s32);
    uint32_t *d32 = shmem_malloc(2 * (size_t)npes * sizeof *d32);
    uint64_t *s64 = shmem_malloc(3 * sizeof *s64);
    uint64_t *d64 = shmem_malloc(3 * (size_t)npes * sizeof *d64);
    CHECK_FATAL(s32 && d32 && s64 && d64);
    s32[0] = (uint32_t)me;
    s32[1] = (uint32_t)me + 100;
    for (int e = 0; e < 3; e++)
        s64[e] = (uint64_t)me << 32 | (uint64_t)e;
    shmem_barrier_all();
    shmem_fcollect32(d32, s32, 2, 0, 0, npes, pSync);
    shmem_fcollect64(d64, s64, 3, 0, 0, npes, pSync);
    size_t wrong = 0;
    for (int r = 0; r < npes; r++) {
        wrong += d32[2 * r] != (uint32_t)r || d32[2 * r + 1] != (uint32_t)r + 100;
        for (int e = 0; e < 3; e++)
            wrong += d64[3 * r + e] != ((uint64_t)r << 32 | (uint64_t)e);
    }
    CHECK(wrong == 0);
    /* collect32 and 64: PE r gives r % 2 + 1 elements. */
    shmem_collect32(d32, s32, (size_t)(me % 2 + 1), 0, 0, npes, pSync);
    shmem_collect64(d64, s64, (size_t)(me % 2 + 1), 0, 0, npes, pSync);
    size_t place = 0;
    for (int r = 0; r < npes; r++)
        for (int e = 0; e < r % 2 + 1; e++, place++) {
            wrong += d32[place] != (uint32_t)(e == 0 ? r : r + 100);
            wrong += d64[place] != ((uint64_t)r << 32 | (uint64_t)e);
        }
    CHECK(wrong == 0);
    for (int i = 0; i < SHMEM_COLLECT_SYNC_SIZE; i++)
        CHECK(pSync[i] == SHMEM_SYNC_VALUE);
    shmem_free(d64);
    shmem_free(s64);
    shmem_free(d32);
    shmem_free(s32);
}

/* The node's own team: its members in local order, whatever their ranks. */
static void shared_team(void)
{
    int n = shmem_team_n_pes(SHMEM_TEAM_SHARED), mine = shmem_team_my_pe(SHMEM_TEAM_SHARED);
    int *source = shmem_malloc(4 * sizeof *source);
    int *dest = shmem_malloc(4 * (size_t)npes * sizeof *dest);
    CHECK_FATAL(source && dest);
    for (int e = 0; e < 4; e++)
        source[e] = mine * 10 + e;
    shmem_barrier_all();
    CHECK(shmem_int_fcollect(SHMEM_TEAM_SHARED, dest, source, 4) == 0);
    size_t wrong = 0;
    for (int r = 0; r < n; r++)
        for (int e = 0; e < 4; e++)
            wrong += dest[4 * r + e] != r * 10 + e;
    CHECK(wrong == 0);
    shmem_barrier_all();

    /* A collect over PE 0's node's team alone - its lengths' room taken and
     * given back there and nowhere else - then an allocation over the job and
     * a put across nodes into it: the heaps must still agree. */
    if (shmem_ptr(source, 0)) {
        size_t length = (size_t)(mine % 4);
        size_t total = 0;
        for (int r = 0; r < n; r++)
            total += (size_t)(r % 4);
        CHECK(shmem_int_collect(SHMEM_TEAM_SHARED, dest, source, length) == 0);
        size_t place = 0;
        for (int r = 0; r < n; r++)
            for (int e = 0; e < r % 4; e++)
                wrong += dest[place++] != r * 10 + e;
        CHECK(place == total);
    }
    long *after = shmem_malloc(sizeof *after);
    CHECK_FATAL(after);
    *after = -1;
    shmem_barrier_all();
    shmem_long_p(after, me, (me + 1) % npes);
    shmem_barrier_all();
    wrong += *after != (me + npes - 1) % npes;
    CHECK(wrong == 0);
    shmem_free(after);
    shmem_free(dest);
    shmem_free(source);
}

/* Three collectives into one dest, no barrier between: each result must be
 * its own, however far ahead of the others one node runs. */
static void back_to_back(void)
{
    long *source = shmem_malloc(MANY * sizeof *source);
    long *dest = shmem_malloc((size_t)npes * MANY * sizeof *dest);
    CHECK_FATAL(source && dest);
    size_t wrong = 0;
    shmem_barrier_all();
    for (int round = 0; round < 4; round++) {
        for (int e = 0; e < MANY; e++)
            source[e] = round * 1000000L + me * 1000L + e % 1000;
        CHECK(shmem_long_fcollect(SHMEM_TEAM_WORLD, dest, source, MANY) == 0);
        for (int r = 0; r < npes; r++)
            for (int e = 0; e < MANY; e++)
                wrong += dest[(size_t)r * MANY + e] != round * 1000000L + r * 1000L + e % 1000;
        CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, MANY) == 0);
        long base = (long)npes * round * 1000000L + 1000L * npes * (npes - 1) / 2;
        for (int e = 0; e < MANY; e++)
            wrong += dest[e] != base + (long)npes * (e % 1000);
    }
    if (wrong)
        fprintf(stderr, "PE %d: %zu wrong back to back\n", me, wrong);
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
    for (int i = 0; i < SHMEM_COLLECT_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;
    shmem_barrier_all();

    const char *partial = getenv("OSOE_TEST_PARTIAL");
    if (partial && partial[0] == '1') {
        long *x = shmem_malloc((size_t)npes * sizeof *x);
        CHECK_FATAL(x);
        if (me == npes - 1) {
            shmem_fcollect64(x, x, 1, 0, 0, npes - 1, pSync);
            printf("PE %d: a call from outside its active set returned, which it must not\n",
                   me);
            return 1;
        }
        shmem_barrier_all();            /* the job ends while this waits */
        return 1;
    }

    fcollects();
    collects();
    active_set_forms();
    shared_team();
    back_to_back();

    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
