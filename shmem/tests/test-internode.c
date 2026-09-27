/* SPDX-License-Identifier: LGPL-3.0-or-later */
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

/* The world ranks on this node, from the launcher's GTIDS (§6.2, launcher.h), so the
 * test can tell a node-mate from a peer without asking the library. */
enum { LOCAL_MAX = 256 };
static int local_ranks[LOCAL_MAX];
static int local_count;

static void read_local_ranks(void)
{
    const char *gtids = osoe_test_launcher("GTIDS");
    if (!gtids)
        return;
    for (const char *at = gtids; *at; ) {
        char *end;
        long v = strtol(at, &end, 10);
        if (end == at)
            break;
        if (local_count < LOCAL_MAX)
            local_ranks[local_count++] = (int)v;
        at = (*end == ',') ? end + 1 : end;
    }
}

static int is_local(int pe)
{
    for (int i = 0; i < local_count; i++)
        if (local_ranks[i] == pe)
            return 1;
    return 0;
}

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
    local_id = atoi(osoe_test_launcher("LOCALID") ? osoe_test_launcher("LOCALID") : "0");
    read_local_ranks();

    /* One inbox per origin, so every PE may write concurrently without two
     * origins sharing bytes. */
    size_t stride = CHUNKED;
    unsigned char *inbox = shmem_malloc(stride * (size_t)npes);
    unsigned char *source = malloc(stride);
    CHECK_FATAL(inbox != NULL);  /* NULL would be dereferenced just below */
    CHECK_FATAL(source != NULL);
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
    const char *step_nodes = osoe_test_launcher("STEP_NUM_NODES");
    int nodes = atoi(step_nodes ? step_nodes : "1");
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

    /*
     * Refill every inbox, because the rounds above end by clearing them and the
     * get below reads what is there. Leaving that out made the get read zeroes
     * and fail on the single-node map as well as the others, which is the
     * useful part: it said "test bug" rather than "transport bug" immediately.
     */
    for (int to = 0; to < npes; to++) {
        for (size_t i = 0; i < CHUNKED; i++)
            source[i] = pattern(me, to, i);
        shmem_putmem(inbox + stride * (size_t)me, source, CHUNKED, to);
    }
    shmem_quiet();
    shmem_barrier_all();

    /*
     * get, over the same boundary. A get is complete per operation on return
     * (§4.6), so there is nothing to quiet and nothing to barrier: if the bytes
     * are not in `dst` when the call returns, the check below fails at once.
     * The long one crosses the landing zone's capacity, which is what makes the
     * origin issue it in pieces (§3.12).
     */
    for (int from = 0; from < npes; from++) {
        for (size_t i = 0; i < CHUNKED; i++)
            source[i] = 0;
        /*
         * What this reads is PE `from`'s own inbox slot `from`, which holds
         * what that PE sent to *itself* - not what it sent here. Expecting the
         * latter is the mistake that makes a get test pass on one node and fail
         * on several for the wrong reason, so the expectation names both ends
         * explicitly.
         */
        shmem_getmem(source, inbox + stride * (size_t)from, CHUNKED, from);
        int bad = -1;
        for (size_t i = 0; i < CHUNKED && bad < 0; i++)
            if (source[i] != pattern(from, from, i))
                bad = (int)i;
        if (bad >= 0)
            fprintf(stderr, "PE %d: get byte %d from PE %d is %u, expected %u\n",
                    me, bad, from, source[bad], pattern(from, from, (size_t)bad));
        CHECK(bad < 0);
    }
    shmem_barrier_all();

    /*
     * Enough gets to outlast the credit several times over, which is what makes
     * §3.8's bound a thing the test can break. The environment pins
     * OSOE_AM_CREDITS low for exactly this: at the default the bound would not
     * bind until tens of thousands of operations, and a credit that is never
     * reached is a credit nothing checks. A credit that is taken and not
     * returned stalls here rather than passing quietly.
     */
    for (int repeat = 0; repeat < 12; repeat++) {
        int from = (me + repeat) % npes;
        shmem_getmem(source, inbox + stride * (size_t)from, SMALL, from);
        for (size_t i = 0; i < SMALL; i++)
            CHECK(source[i] == pattern(from, from, i));
    }
    shmem_barrier_all();

    /*
     * The atomics, fetching and not.
     *
     * Every PE adds to every PE's counter, so the total each one ends with is
     * the whole job's contribution and a lost or duplicated atomic shows up as
     * a wrong sum rather than as nothing. A non-fetching add completes as a put
     * does (§4.10), so it needs the quiet; the fetching one is complete on
     * return, and its returned value is checked for being a value some add
     * actually left there rather than a torn mix.
     */
    long *counter_cell = shmem_malloc(sizeof *counter_cell);
    CHECK_FATAL(counter_cell != NULL);  /* NULL would be dereferenced just below */
    *counter_cell = 0;
    shmem_barrier_all();
    for (int to = 0; to < npes; to++)
        shmem_long_atomic_add(counter_cell, 1, to);
    shmem_quiet();
    shmem_barrier_all();
    CHECK(*counter_cell == npes);

    shmem_barrier_all();
    long got = shmem_long_atomic_fetch_add(counter_cell, 100, (me + 1) % npes);
    /* Every PE adds 100 to exactly one cell, and each cell started at npes, so
     * any return is npes plus a multiple of 100 below the final total. */
    CHECK(got >= npes && (got - npes) % 100 == 0);
    shmem_barrier_all();
    CHECK(*counter_cell == npes + 100);

    /* fetch and set, on a cell nobody else touches this round. */
    shmem_barrier_all();
    long previous = shmem_long_atomic_fetch(counter_cell, me);
    CHECK(previous == npes + 100);
    shmem_barrier_all();

    /*
     * put_signal is a put followed by a fenced atomic (§4.10, invariant 5): the
     * signal must never announce bytes that have not landed. The waiter blocks
     * on the signal and then reads the payload, which is the ordering that
     * would break if the fence were dropped.
     */
    uint64_t *signal = shmem_malloc(sizeof *signal);
    CHECK_FATAL(signal != NULL);  /* NULL would be dereferenced just below */
    *signal = 0;
    memset(inbox, 0, stride * (size_t)npes);
    shmem_barrier_all();
    int partner = (me + 1) % npes;
    for (size_t i = 0; i < SMALL; i++)
        source[i] = pattern(me, partner, i);
    shmem_putmem_signal(inbox + stride * (size_t)me, source, SMALL, signal, 1,
                        SHMEM_SIGNAL_SET, partner);
    shmem_quiet();
    int sender = (me + npes - 1) % npes;
    shmem_uint64_wait_until(signal, SHMEM_CMP_NE, 0);
    {
        const unsigned char *got_bytes = inbox + stride * (size_t)sender;
        int bad = -1;
        for (size_t i = 0; i < SMALL && bad < 0; i++)
            if (got_bytes[i] != pattern(sender, me, i))
                bad = (int)i;
        if (bad >= 0)
            fprintf(stderr, "PE %d: signal arrived before byte %d from PE %d\n",
                    me, bad, sender);
        CHECK(bad < 0);
    }
    shmem_barrier_all();

    /*
     * §4.10's OSOE_TRANSPORT_MORE has a caller now, and put_signal is it: the
     * signal follows the put on the same context to the same PE, so "more
     * follows" is a fact rather than a guess. The flag defers the wake and
     * nothing else, which has no observable effect on results by design - so
     * the only way to say it did anything is to count it (§3.14). On one
     * logical node there is no wake to defer, and zero is then the right
     * answer rather than a missing check.
     */
    /*
     * Per PE, not per job: the flag is only reached when the signalled put
     * actually crosses a node, and on a map with several PEs to a node this
     * PE's partner is usually a node-mate. Asserting it job-wide failed on
     * exactly those maps - a check that was measuring the map rather than the
     * flag.
     */
    if (nodes > 1 && !is_local(partner))
        CHECK(counter("am.wakes_deferred") > 0);
    else
        CHECK(counter("am.wakes_deferred") == 0);

    shmem_free(signal);
    shmem_free(counter_cell);

    CHECK_INVARIANTS();
    free(source);
    shmem_free(inbox);
    shmem_finalize();
    return CHECK_DONE();
}
