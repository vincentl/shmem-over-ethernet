/*
 * The differential test: one program, every logical layout, identical results
 * (§8.1, §8.2).
 *
 * This is the answer to "is the §4.10 boundary really carrying two paths" that
 * is running code rather than analysis. The same program runs over §8.1's four
 * poles of the logical node map - all PEs on one node, one PE per node, uneven
 * sizes, and ranks not contiguous per node - and every run must produce the
 * same transcript. On the first map every operation is a memory operation in
 * the calling thread; on the second every one crosses the AM layer; the middle
 * two mix them, which is where a path that is right at both extremes and wrong
 * in between shows up.
 *
 * **It compares outputs rather than checking assertions**, and that is the
 * point of writing it as a transcript. A CHECK only catches what it was told to
 * look for; a diff catches anything that differs, including things nobody
 * thought to assert. The two are complementary and shmem.internode does the
 * other half.
 *
 * **Every line must be independent of the layout**, or the diff is measuring
 * the map instead of the transport. So nothing here prints a node id, a local
 * id, an address, a duration or a count of anything the map decides. What it
 * prints is values: what landed, what was read back, what the atomics
 * accumulated.
 *
 * **One deterministic line out of a nondeterministic set.** The values a
 * fetching add returns depend on the order the adds are applied, which no map
 * fixes. Their *sum* does not: N PEs each adding D to a cell that started at S
 * take back S, S+D, ... S+(N-1)D in some order, and that sums to N*S +
 * D*N*(N-1)/2 whatever the order. Printing the sum tests the atomics carried
 * every operation exactly once, which is the property worth diffing, without
 * pinning an order the spec does not promise.
 *
 * Lines are sorted by the driver before comparison, because the launcher
 * interleaves the output of its ranks in whatever order they run.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

enum { BUFFER = 3000, SMALL = 200, ADD = 7, FETCH_ADD = 100 };

static uint64_t fnv(const void *bytes, size_t len)
{
    const unsigned char *p = bytes;
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static unsigned char pattern(int from, int to, size_t i)
{
    return (unsigned char)((from * 131 + to * 17 + (int)(i % 241)) & 0xff);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();

    unsigned char *inbox = shmem_malloc((size_t)BUFFER * (size_t)npes);
    unsigned char *scratch = malloc(BUFFER);
    long *cell = shmem_malloc(sizeof *cell);
    uint64_t *signal = shmem_malloc(sizeof *signal);
    if (!inbox || !scratch || !cell || !signal) {
        fprintf(stderr, "allocation failed\n");
        return 2;
    }

    /* ---- put, at three sizes ------------------------------------------ */
    memset(inbox, 0, (size_t)BUFFER * (size_t)npes);
    *cell = 0;
    *signal = 0;
    shmem_barrier_all();

    for (int to = 0; to < npes; to++) {
        for (size_t i = 0; i < BUFFER; i++)
            scratch[i] = pattern(me, to, i);
        shmem_putmem(inbox + (size_t)BUFFER * (size_t)me, scratch, BUFFER, to);
    }
    shmem_quiet();
    shmem_barrier_all();

    for (int from = 0; from < npes; from++)
        printf("pe %d put-from %d %016llx\n", me, from,
               (unsigned long long)fnv(inbox + (size_t)BUFFER * (size_t)from, BUFFER));
    shmem_barrier_all();

    /* ---- get ------------------------------------------------------------ */
    for (int from = 0; from < npes; from++) {
        memset(scratch, 0, BUFFER);
        shmem_getmem(scratch, inbox + (size_t)BUFFER * (size_t)from, BUFFER, from);
        printf("pe %d get-from %d %016llx\n", me, from,
               (unsigned long long)fnv(scratch, BUFFER));
    }

    /* A short get too: the long one crosses the landing zone and is issued in
     * pieces, the short one is a single request, and both must agree. */
    for (int from = 0; from < npes; from++) {
        memset(scratch, 0, BUFFER);
        shmem_getmem(scratch, inbox + (size_t)BUFFER * (size_t)from, SMALL, from);
        printf("pe %d get-short %d %016llx\n", me, from,
               (unsigned long long)fnv(scratch, SMALL));
    }
    shmem_barrier_all();

    /* ---- atomics -------------------------------------------------------- */
    for (int to = 0; to < npes; to++)
        shmem_long_atomic_add(cell, ADD, to);
    shmem_quiet();
    shmem_barrier_all();
    printf("pe %d add-total %ld\n", me, *cell);
    /*
     * **A barrier after reading, not only before.** Without it a PE that has
     * printed goes straight into the next section and its atomics land on a
     * cell whose owner has not read it yet, so the value printed depends on who
     * was scheduled first - which made this test fail against correct code, on
     * the all-intranode map, where nothing crosses a node at all. Every
     * observation of shared state below is fenced on both sides for the same
     * reason.
     */
    shmem_barrier_all();

    /*
     * Every PE takes one fetching add from every PE's cell, so each cell sees
     * npes of them. The individual returns depend on the order; their sum over
     * the whole job does not.
     */
    long mine = 0;
    for (int to = 0; to < npes; to++)
        mine += shmem_long_atomic_fetch_add(cell, FETCH_ADD, to);
    shmem_quiet();
    shmem_barrier_all();

    long *sum = shmem_malloc(sizeof *sum);
    if (!sum)
        return 2;
    *sum = 0;
    shmem_barrier_all();
    shmem_long_atomic_add(sum, mine, 0);
    shmem_quiet();
    shmem_barrier_all();
    if (me == 0)
        printf("pe - fetch-add-sum %ld\n", *sum);
    printf("pe %d cell-after %ld\n", me, *cell);
    shmem_barrier_all();

    /* ---- compare-and-swap ----------------------------------------------- */
    if (me == 0)
        *cell = 0;
    shmem_barrier_all();
    /* Exactly one PE may win a swap from 0 to its own rank plus one. */
    long won = shmem_long_atomic_compare_swap(cell, 0, me + 1, 0);
    shmem_quiet();
    shmem_barrier_all();
    long winners = (won == 0) ? 1 : 0;
    long *tally = shmem_malloc(sizeof *tally);
    if (!tally)
        return 2;
    *tally = 0;
    shmem_barrier_all();
    shmem_long_atomic_add(tally, winners, 0);
    shmem_quiet();
    shmem_barrier_all();
    if (me == 0)
        printf("pe - cswap-winners %ld\n", *tally);
    shmem_barrier_all();

    /* ---- put with signal ------------------------------------------------ */
    memset(inbox, 0, (size_t)BUFFER * (size_t)npes);
    *signal = 0;
    shmem_barrier_all();
    int partner = (me + 1) % npes;
    for (size_t i = 0; i < SMALL; i++)
        scratch[i] = pattern(me, partner, i);
    shmem_putmem_signal(inbox + (size_t)BUFFER * (size_t)me, scratch, SMALL, signal, 1,
                        SHMEM_SIGNAL_SET, partner);
    shmem_quiet();
    shmem_uint64_wait_until(signal, SHMEM_CMP_NE, 0);
    {
        int sender = (me + npes - 1) % npes;
        printf("pe %d signal-payload %016llx\n", me,
               (unsigned long long)fnv(inbox + (size_t)BUFFER * (size_t)sender, SMALL));
    }
    shmem_barrier_all();
    shmem_barrier_all();

    /* ---- what the boundary says about reachability ---------------------- */
    /* Counted, not listed: which PEs are addressable is the one thing the map
     * legitimately changes, so the diff must not see it. That the count is
     * between 1 and npes, and that a PE can always reach itself, is checked
     * rather than printed. */
    int reachable = 0;
    for (int pe = 0; pe < npes; pe++)
        if (shmem_ptr(inbox, pe))
            reachable++;
    if (reachable < 1 || reachable > npes || !shmem_ptr(inbox, me)) {
        fprintf(stderr, "pe %d: reachable %d of %d, self %s\n", me, reachable, npes,
                shmem_ptr(inbox, me) ? "yes" : "no");
        return 2;
    }

    shmem_free(tally);
    shmem_free(sum);
    shmem_free(signal);
    shmem_free(cell);
    shmem_free(inbox);
    free(scratch);
    shmem_finalize();
    return 0;
}
