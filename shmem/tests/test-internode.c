/*
 * The data path across a node boundary: put, fence and quiet (§4.6, §4.10).
 *
 * Every PE writes a pattern derived from (origin, target) into every other PE's
 * buffer, quiets, and then checks what it received. Because the same program
 * runs with and without §8.1's force-remote flag, the intranode path and the AM
 * path carry identical traffic and must produce identical results - which is
 * the differential test this step exists to make possible, at the size it can
 * reach before get and the atomics land.
 *
 * **Three sizes, and the middle one is the point.** A put shorter than an AM
 * chunk is one message; one longer is several (§3.2), and the loop that splits
 * it is where an off-by-one lands in someone else's bytes rather than in a
 * crash. The chunk is pinned small by the test's environment so that the
 * splitting path runs in a test that finishes in a second.
 *
 * **What quiet is checked for here is that it waited, not that it ordered.**
 * A node pair has one connection and segments are applied in order, so ordering
 * between two puts to the same PE is free and a test asserting it would pass
 * against a quiet that did nothing. Completion is the part that is not free, and
 * it has no observable value on the origin - so it is read out of the control
 * segment instead (§3.14, and the technique step 1 used for the same reason):
 * after a quiet that covered remote peers, transport.wait.dirty_peers must have
 * counted them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SMALL = 64, CHUNKED = 5000, TINY = 1, PATTERN_MOD = 251 };

static int me, npes, local_id;

/*
 * This PE's total for a thread-scope counter (§3.14), read the way osoe-stat
 * reads it rather than through the library's own headers.
 *
 * **Indexed by local id, not by world rank.** The counter blocks belong to the
 * node segment and are laid out per local PE, and the two numbers are equal
 * only on a single-node job - which is why every existing test of this kind
 * gets away with the rank. On three nodes rank 2 reads block 2 of its own
 * node, which belongs to nobody, and the counter reads zero.
 */
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
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, (uint32_t)local_id * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

/* What PE `from` writes at byte `i` of the buffer it sends to PE `to`. Distinct
 * per (from, to, i), so a byte landing in the wrong place, from the wrong peer,
 * or at the wrong offset is all one check. */
static unsigned char pattern(int from, int to, size_t i)
{
    return (unsigned char)((from * 31 + to * 7 + (int)(i % PATTERN_MOD)) & 0xff);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    local_id = atoi(getenv("OSOE_LOCALID") ? getenv("OSOE_LOCALID") : "0");

    /* One inbox per origin, so every PE may write concurrently without two
     * origins sharing bytes. */
    size_t stride = CHUNKED;
    unsigned char *inbox = shmem_malloc(stride * (size_t)npes);
    unsigned char *source = malloc(stride);
    CHECK(inbox != NULL);
    CHECK(source != NULL);
    memset(inbox, 0, stride * (size_t)npes);
    shmem_barrier_all();

    const size_t sizes[] = { TINY, SMALL, CHUNKED };
    for (unsigned round = 0; round < sizeof sizes / sizeof *sizes; round++) {
        size_t len = sizes[round];

        for (int to = 0; to < npes; to++) {
            for (size_t i = 0; i < len; i++)
                source[i] = pattern(me, to, i);
            shmem_putmem(inbox + stride * (size_t)me, source, len, to);
        }
        /* Completion at the target is what a barrier alone does not give: the
         * barrier makes every PE arrive, quiet makes this PE's writes applied
         * (§4.6). Both are needed, and in this order. */
        shmem_quiet();
        shmem_barrier_all();

        for (int from = 0; from < npes; from++) {
            const unsigned char *got = inbox + stride * (size_t)from;
            int bad = -1;
            for (size_t i = 0; i < len && bad < 0; i++)
                if (got[i] != pattern(from, me, i))
                    bad = (int)i;
            if (bad >= 0)
                fprintf(stderr, "PE %d: byte %d of %zu from PE %d is %u, expected %u\n",
                        me, bad, len, from, got[bad], pattern(from, me, (size_t)bad));
            CHECK(bad < 0);
        }
        shmem_barrier_all();
        memset(inbox, 0, stride * (size_t)npes);
        shmem_barrier_all();
    }

    /*
     * §3.14's technique, because completion has no value the origin can read:
     * a quiet that covered remote peers must have counted them. On a job of one
     * logical node there are none, and the counter staying at zero is then the
     * correct answer rather than a missing check - which is why this asserts a
     * different thing in the two cases instead of being skipped in one.
     */
    int nodes = atoi(getenv("OSOE_STEP_NUM_NODES") ? getenv("OSOE_STEP_NUM_NODES") : "1");
    uint64_t dirty = counter("transport.wait.dirty_peers");
    CHECK(dirty != UINT64_MAX);
    if (nodes > 1)
        CHECK(dirty > 0);
    else
        CHECK(dirty == 0);

    /*
     * fence does not clear the dirty set (§4.6): those operations remain
     * unacknowledged, so a quiet after a fence still has peers to wait for. If
     * fence cleared it, this quiet would count zero and the check above would
     * be the only thing standing between that and silence.
     */
    for (int to = 0; to < npes; to++) {
        source[0] = pattern(me, to, 0);
        shmem_putmem(inbox + stride * (size_t)me, source, 1, to);
    }
    uint64_t before = counter("transport.wait.dirty_peers");
    shmem_fence();
    shmem_quiet();
    if (nodes > 1)
        CHECK(counter("transport.wait.dirty_peers") > before);
    shmem_barrier_all();

    /*
     * spec §9.12.3: pe_quiet over an empty list returns at once and must not
     * dereference the pointer; over a list it completes those PEs and leaves
     * the rest dirty, which is the property the dirty set exists to make
     * possible at all.
     */
    shmem_pe_quiet(NULL, 0);
    int one = (me + 1) % npes;
    for (int to = 0; to < npes; to++) {
        source[0] = pattern(me, to, 0);
        shmem_putmem(inbox + stride * (size_t)me, source, 1, to);
    }
    shmem_pe_quiet(&one, 1);
    shmem_quiet();
    shmem_barrier_all();

    CHECK_INVARIANTS();
    free(source);
    shmem_free(inbox);
    shmem_finalize();
    return CHECK_DONE();
}
