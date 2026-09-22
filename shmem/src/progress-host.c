/*
 * The node's progress engine, as a PE runs it (§3.12, §5.3).
 *
 * See progress-host.h for what this is and why it is separate from progress.c.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "base/deadline.h"
#include "base/epoch.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "am-state.h"
#include "connection-area.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "wire.h"

struct osoe_am_state osoe_am;

/*
 * How long the lead's agent sleeps between passes when nothing moved. Far
 * under OSOE_SWEEP_NS, because this is the interval at which *arriving* bytes
 * are noticed and it therefore lands on the latency of every internode
 * operation - see progress-host.h for why step 2 looks rather than waits.
 */
#define OSOE_PROGRESS_SLEEP_NS UINT64_C(250000)     /* 250 us */

/* ---- the four host callbacks (progress.h) ------------------------------ */

/*
 * (pe, region, offset) as an address in this process, or NULL if this node
 * does not hold that PE. Every local heap is mapped in every process of the
 * node (§4.1), so for a local PE it is pointer arithmetic.
 *
 * The bounds check is not defensive tidiness: a message naming an offset past
 * the end of a region is a peer that disagrees about the job - a different
 * SHMEM_SYMMETRIC_SIZE, or a different build - and writing it would corrupt
 * whatever follows the heap rather than say so (§3.15).
 */
static void *resolve(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                     uint32_t length)
{
    (void)context;
    if (region != OSOE_REGION_HEAP)
        osoe_fatal("a peer addressed region %u, which this build does not carry; "
                   "the nodes are running different libraries", region);
    if (pe >= osoe.bootstrap.world_size)
        osoe_fatal("a peer addressed PE %u in a job of %u", pe,
                   osoe.bootstrap.world_size);
    int32_t local = osoe.node.local_of_rank[pe];
    if (local < 0)
        return NULL;
    if (offset > osoe.node.heap_size || length > osoe.node.heap_size - offset)
        osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's heap, which is "
                   "%llu bytes; the nodes disagree about SHMEM_SYMMETRIC_SIZE",
                   length, (unsigned long long)offset, pe,
                   (unsigned long long)osoe.node.heap_size);
    return (char *)osoe.node.heap_of[local] + offset;
}

static struct osoe_landing_slot *landing(void *context, uint32_t pe, uint32_t *capacity)
{
    (void)context;
    *capacity = osoe.node.landing_slot_bytes - (uint32_t)sizeof(struct osoe_landing_slot);
    if (pe >= osoe.bootstrap.world_size)
        return NULL;
    int32_t local = osoe.node.local_of_rank[pe];
    return local < 0 ? NULL : osoe_node_landing(&osoe.node, (uint32_t)local);
}

/* §4.9's watch half. The same function the intranode backend calls, for the
 * reason §4.1 gives about one mechanism rather than two that agree. */
static void notify(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length)
{
    (void)context;
    osoe_transport_notify((int)pe, (enum osoe_region)region, offset, length);
}

/*
 * §5.3's release, applied on this node. The engine knows only (team, epoch);
 * what a release *does* - advance the team's counter and wake its members - is
 * the collective layer's, reached through internode.h's upcall. Before slice 3
 * this function switched on the team number and bumped the node header's
 * counters itself, so the backend knew which futex words the SHMEM layer waits
 * on - the knowledge §10 once said it could not do without.
 */
static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context;
    osoe_internode_released(team, epoch);
}

/* ---- waking the engine (§3.12) ------------------------------------------ */

void osoe_progress_host_wake_lead(void)
{
    if (!osoe_am.internode)
        return;
    struct osoe_pe_slot *lead = &osoe.node.slots[0];
    atomic_fetch_add_explicit(&lead->agent_wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&lead->agent_wake, 1, OSOE_FUTEX_SHARED);
}

/*
 * Read from another process, since only the lead runs the engine. The value is
 * the lead's own bookkeeping and is written by that thread alone, so a reader
 * sees either the old value or the new one - and the old one only makes a wait
 * go round again, which is why this needs no protocol of its own.
 */
uint64_t osoe_progress_host_acked(uint32_t peer)
{
    struct osoe_connection *conn = osoe_connection_to(&osoe_am.area, peer);
    return conn ? osoe_connection_acked(conn) : 0;
}

/* ---- lifecycle --------------------------------------------------------- */

void osoe_progress_host_start(uint32_t session)
{
    /* osoe_am.internode and osoe_am.lead are set at the start of
     * osoe_transport_init, before anything that reads them - including the
     * dirty set, which is sized from the first (transport-am.c). */
    if (!osoe_am.internode)
        return;

    /* Every local PE maps the area: its application threads reserve into these
     * buffers like any other writer (§3.5, §3.12). */
    osoe_connection_area_open(&osoe_am.area, &osoe.bootstrap, osoe.job.node_count, session);
    if (!osoe_am.lead)
        return;

    uint32_t peers = osoe.job.node_count - 1;
    /*
     * §3.8's formula, per connection: C + degree x teams + slack. C bounds the
     * replies - the origin takes a credit per get or fetching AMO and returns
     * it when the reply lands (argument 1). degree x teams bounds the
     * collective forwards: while collectives are blocking, a team has at most
     * one forward outstanding per tree edge (argument 2). The pool is
     * node-wide, so an idle connection costs nothing.
     *
     * Before slice 2 this was peers x C, with no term for the forwards, so a
     * node holding C gets toward a peer had nothing left for its own barrier
     * arrival and the request was dropped (second review, D1). No legal
     * program can empty the pool now, and progress.c ends the job naming the
     * peer if one does.
     */
    uint32_t per_connection = osoe_am.area.credits +
                              OSOE_COLLECTIVE_DEGREE * OSOE_TEAMS + OSOE_REPLY_SLACK;
    uint32_t descriptors = peers * per_connection;
    osoe_am.wire = calloc(peers, sizeof *osoe_am.wire);
    osoe_am.connection = calloc(peers, sizeof *osoe_am.connection);
    osoe_am.descriptor = calloc(descriptors, sizeof *osoe_am.descriptor);
    osoe_am.scratch = malloc(osoe_am.area.chunk_bytes);
    if (!osoe_am.wire || !osoe_am.connection || !osoe_am.descriptor || !osoe_am.scratch)
        osoe_fatal("out of memory setting up the AM backend for %u peer nodes", peers);

    /* Peer-slot order, which is node order with this node left out - the same
     * order osoe_progress_slot computes (§3.4). */
    uint32_t slot = 0;
    for (uint32_t n = 0; n < osoe.job.node_count; n++)
        if (n != osoe.job.node)
            osoe_wire_open(&osoe_am.wire[slot++], &osoe.bootstrap, n, session);

    struct osoe_progress_host host = { resolve, landing, notify, release, NULL };
    osoe_progress_init(&osoe_am.progress, &osoe_am.area, osoe_am.wire, osoe_am.descriptor,
                       descriptors, osoe_am.scratch, (uint32_t)osoe_am.area.chunk_bytes,
                       &host);
    /* Count into the control segment, where osoe-stat and the tests read it. */
    osoe_am.progress.metrics = &osoe.node.hdr->am;
    osoe_progress_attach(&osoe_am.progress, osoe_am.connection);
}

/*
 * Put everything this node still owes the network on the wire, before the
 * engine that would have sent it goes away.
 *
 * **A node must not stop its engine while it owes a peer bytes** (§6.4), and
 * the barrier does not establish that it does not. §5.3's root releases its own
 * node the instant the last node arrives, and only *enqueues* the releases that
 * free the others - the drain and flush that put them on the wire are later
 * passes. So the root's own PEs can leave the final barrier, reach
 * shmem_finalize and stop the agent while the message every other node is
 * waiting for is still sitting in a buffer that only that agent sends. Every
 * other node then waits forever for a node that has exited.
 *
 * Measured before this existed: 3 hangs in 12 runs of the suite, all on the
 * map where node 0 holds three PEs and the others hold two and one - the root
 * having the most PEs is what makes it likeliest to be the last to arrive and
 * the first to leave. The two PEs of node 0 that were not the lead exited
 * cleanly; every other rank was killed at the launcher's timeout, which is the
 * signature to recognise it by.
 *
 * The agent has been joined by the time this runs, so this thread is the only
 * one touching the engine and may poll it (§3.12).
 */
static void flush_before_close(void)
{
    /*
     * Not a bare spin: a pass that moved nothing sleeps a moment and sweeps, as
     * every wait does (§4.10), and the wait gives up only by the no-progress
     * rule (§3.15) - watching this node's segments sent, which is what moves
     * while the peers are reading. Before slice 2 it spun on the CPU and died
     * after the bootstrap timeout (second review, atomics F7).
     */
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, osoe_counter_word_read(&osoe_am.progress.metrics->segments_sent),
                           osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (!osoe_progress_idle(&osoe_am.progress)) {
        if (!osoe_progress_poll(&osoe_am.progress)) {
            struct timespec back = { 0, 20000 };
            nanosleep(&back, NULL);
        }
        if (!osoe_deadline_expired(&sweep))
            continue;
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_sweep_expired("shmem_finalize");
        uint64_t sent = osoe_counter_word_read(&osoe_am.progress.metrics->segments_sent);
        if (osoe_stall_watch_stalled(&watch, sent))
            osoe_fatal("shmem_finalize: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT) "
                       "flushing this node's last messages - segments sent has stayed at "
                       "%llu; a peer node has stopped reading its wire",
                       (unsigned long long)osoe_stall_watch_seconds(&watch),
                       (unsigned long long)sent);
    }
}

void osoe_progress_host_stop(void)
{
    if (!osoe_am.internode)
        return;
    if (osoe_am.lead) {
        flush_before_close();
        /*
         * Announce to every peer first and only then wait (wire.h), or this
         * node's teardown costs one peer's latency after another. Both ends of
         * a wire wait for each other, so none of this may run while the agent
         * might still poll - shmem_finalize joins that thread first.
         */
        for (uint32_t slot = 0; slot + 1 < osoe.job.node_count; slot++)
            osoe_wire_shutdown(&osoe_am.wire[slot]);
        for (uint32_t slot = 0; slot + 1 < osoe.job.node_count; slot++)
            osoe_wire_close(&osoe_am.wire[slot]);
        free(osoe_am.wire);
        free(osoe_am.connection);
        free(osoe_am.descriptor);
        free(osoe_am.scratch);
        osoe_am.wire = NULL;
        osoe_am.connection = NULL;
        osoe_am.descriptor = NULL;
        osoe_am.scratch = NULL;
    }
    osoe_connection_area_close(&osoe_am.area);
    osoe_am.internode = false;
}

bool osoe_progress_host_poll(void)
{
    if (!osoe_am.internode || !osoe_am.lead)
        return false;
    bool moved = osoe_internode_am_take_arrivals();
    return osoe_progress_poll(&osoe_am.progress) || moved;
}

uint64_t osoe_progress_host_idle_ns(void)
{
    return osoe_am.internode && osoe_am.lead ? OSOE_PROGRESS_SLEEP_NS : OSOE_SWEEP_NS;
}
