/*
 * The node's progress engine, as a PE runs it (§3.12, §5.3).
 *
 * See progress-host.h for what this is and why it is separate from progress.c.
 */
#include <stdatomic.h>
#include <stdlib.h>

#include "base/deadline.h"
#include "base/epoch.h"
#include "base/futex.h"
#include "connection-area.h"
#include "fatal.h"
#include "job.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "wire.h"

/*
 * How long the lead's agent sleeps between passes when nothing moved. Far
 * under OSOE_SWEEP_NS, because this is the interval at which *arriving* bytes
 * are noticed and it therefore lands on the latency of every internode
 * operation - see progress-host.h for why step 2 looks rather than waits.
 */
#define OSOE_PROGRESS_SLEEP_NS UINT64_C(250000)     /* 250 us */

/*
 * The epoch this agent has already taken out of each team's handoff word.
 * Private to the agent thread: nothing else reads or writes it, which is the
 * whole point of the word it shadows.
 */
static uint32_t consumed[OSOE_TEAMS];

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

static struct osoe_landing_slot *landing(void *context, uint32_t pe)
{
    (void)context;
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
 * §5.3's release, applied on this node: advance the team's `released` counter
 * and wake every local member waiting on it. A divisible event, so wake-many
 * (§3.11).
 *
 * The bump is checked for the same reason the single-node path checks it: a
 * release that does not land exactly on k+1 means this node's members have run
 * a different number of collectives from the rest of the job, and §5.4's
 * equality waits turn that into a hang rather than an error unless it is named
 * here.
 */
static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context;
    struct osoe_epoch *released;
    const char *name;
    if (team == OSOE_TEAM_WORLD) {
        released = &osoe.node.hdr->world_released;
        name = "SHMEM_TEAM_WORLD";
    } else {
        released = &osoe.node.hdr->shared_released;
        name = "SHMEM_TEAM_SHARED";
    }
    if (!osoe_epoch_release_bump_eq(released, epoch))
        osoe_fatal("the internode release for %s arrived for epoch %u out of order: "
                   "the PEs of this job called different numbers of collectives", name,
                   epoch);
    osoe_epoch_wake_all(released);
}

/* ---- §5.2's node participation ----------------------------------------- */

/*
 * The root of a team's internode tree, and how many nodes take part.
 *
 * §5.2 spans the tree over the nodes holding at least one member. For the two
 * predefined teams that needs no computation: the world team has a member on
 * every node, so every node participates and the lowest, node 0, is the root.
 * SHMEM_TEAM_SHARED is one node by definition (§4.8) and never has an
 * internode phase, so reaching here with it is a bug rather than a case.
 * shmem_team_split_* (§7.3) is what turns this into a real computation.
 */
static uint32_t participants(uint32_t team)
{
    if (team != OSOE_TEAM_WORLD)
        osoe_fatal("team %u has no internode phase; only SHMEM_TEAM_WORLD spans nodes "
                   "in this build", team);
    return osoe.job.node_count;
}

#define OSOE_COLLECTIVE_ROOT 0u

/* ---- the handoff, both ends (§5.3) ------------------------------------- */

void osoe_progress_host_wake_lead(void)
{
    if (!osoe.internode)
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
    struct osoe_connection *conn = osoe_connection_to(&osoe.area, peer);
    return conn ? osoe_connection_acked(conn) : 0;
}

void osoe_progress_host_post_arrival(uint32_t team, uint32_t epoch)
{
    struct osoe_arrival_handoff *h = &osoe.node.hdr->arrival[team];
    /*
     * A single-writer publication store (§3.16): only one member posts per
     * epoch, and the release of the previous epoch orders it after the last
     * one. Then the wake word, a locked add, which on x86-TSO also drains the
     * store buffer so the agent cannot see the bump without the epoch.
     */
    atomic_store_explicit(&h->epoch, epoch, memory_order_release);

    /* The lead is local id 0 (§6.2). It is the only agent that runs the
     * engine, so it is the only one worth waking for this. */
    struct osoe_pe_slot *lead = &osoe.node.slots[0];
    atomic_fetch_add_explicit(&lead->agent_wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&lead->agent_wake, 1, OSOE_FUTEX_SHARED);
}

/*
 * Take whatever has been posted and perform the arrivals.
 *
 * **The epoch is recorded consumed before the arrival is performed, and from
 * the value just loaded rather than by re-reading the word.** On the root node
 * osoe_progress_arrive can complete the collective and release the team
 * immediately, so a local member can be back in the next barrier - and posting
 * its epoch here - before this function's next line would run. Re-reading the
 * word at that point marks an epoch consumed that was never forwarded, and
 * that node never arrives again: `consume_reread` in
 * docs/design/evidence/am-backend/arrival_model.py, where the whole job hangs
 * and every other node is the one that looks stuck.
 */
static bool consume_arrivals(void)
{
    bool moved = false;
    for (uint32_t team = 0; team < OSOE_TEAMS; team++) {
        uint32_t epoch = atomic_load_explicit(&osoe.node.hdr->arrival[team].epoch,
                                              memory_order_acquire);
        if (epoch == consumed[team])
            continue;
        consumed[team] = epoch;
        osoe_progress_arrive(&osoe.progress, team, epoch, OSOE_COLLECTIVE_ROOT,
                             participants(team));
        moved = true;
    }
    return moved;
}

/* ---- lifecycle --------------------------------------------------------- */

void osoe_progress_host_start(uint32_t session)
{
    /* osoe.internode and osoe.lead are set by session_start, before anything
     * that reads them - including osoe_transport_init, which sizes the dirty
     * set from the first (init.c). */
    if (!osoe.internode)
        return;

    /* Every local PE maps the area: its application threads reserve into these
     * buffers like any other writer (§3.5, §3.12). */
    osoe_connection_area_open(&osoe.area, &osoe.bootstrap, osoe.job.node_count, session);
    if (!osoe.lead)
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
    uint32_t per_connection = osoe.area.credits +
                              OSOE_COLLECTIVE_DEGREE * OSOE_TEAMS + OSOE_REPLY_SLACK;
    uint32_t descriptors = peers * per_connection;
    osoe.wire = calloc(peers, sizeof *osoe.wire);
    osoe.connection = calloc(peers, sizeof *osoe.connection);
    osoe.descriptor = calloc(descriptors, sizeof *osoe.descriptor);
    osoe.scratch = malloc(osoe.area.chunk_bytes);
    if (!osoe.wire || !osoe.connection || !osoe.descriptor || !osoe.scratch)
        osoe_fatal("out of memory setting up the AM backend for %u peer nodes", peers);

    /* Peer-slot order, which is node order with this node left out - the same
     * order osoe_progress_slot computes (§3.4). */
    uint32_t slot = 0;
    for (uint32_t n = 0; n < osoe.job.node_count; n++)
        if (n != osoe.job.node)
            osoe_wire_open(&osoe.wire[slot++], &osoe.bootstrap, n, session);

    struct osoe_progress_host host = { resolve, landing, notify, release, NULL };
    osoe_progress_init(&osoe.progress, &osoe.area, osoe.wire, osoe.descriptor,
                       descriptors, osoe.scratch, (uint32_t)osoe.area.chunk_bytes,
                       &host);
    /* Count into the control segment, where osoe-stat and the tests read it. */
    osoe.progress.metrics = &osoe.node.hdr->am;
    osoe_progress_attach(&osoe.progress, osoe.connection);
    for (uint32_t team = 0; team < OSOE_TEAMS; team++)
        consumed[team] = 0;
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
    struct osoe_deadline until = osoe_deadline_in(osoe.bootstrap.timeout_ns);
    while (!osoe_progress_idle(&osoe.progress)) {
        osoe_progress_poll(&osoe.progress);
        if (osoe_deadline_expired(&until) && !osoe_progress_idle(&osoe.progress))
            osoe_fatal("timed out flushing this node's last messages at shmem_finalize; "
                       "a peer node has stopped reading its wire while this one still "
                       "had something to send it");
    }
}

void osoe_progress_host_stop(void)
{
    if (!osoe.internode)
        return;
    if (osoe.lead) {
        flush_before_close();
        /*
         * Announce to every peer first and only then wait (wire.h), or this
         * node's teardown costs one peer's latency after another. Both ends of
         * a wire wait for each other, so none of this may run while the agent
         * might still poll - shmem_finalize joins that thread first.
         */
        for (uint32_t slot = 0; slot + 1 < osoe.job.node_count; slot++)
            osoe_wire_shutdown(&osoe.wire[slot]);
        for (uint32_t slot = 0; slot + 1 < osoe.job.node_count; slot++)
            osoe_wire_close(&osoe.wire[slot]);
        free(osoe.wire);
        free(osoe.connection);
        free(osoe.descriptor);
        free(osoe.scratch);
        osoe.wire = NULL;
        osoe.connection = NULL;
        osoe.descriptor = NULL;
        osoe.scratch = NULL;
    }
    osoe_connection_area_close(&osoe.area);
    osoe.internode = false;
}

bool osoe_progress_host_poll(void)
{
    if (!osoe.internode || !osoe.lead)
        return false;
    bool moved = consume_arrivals();
    return osoe_progress_poll(&osoe.progress) || moved;
}

uint64_t osoe_progress_host_idle_ns(void)
{
    return osoe.internode && osoe.lead ? OSOE_PROGRESS_SLEEP_NS : OSOE_SWEEP_NS;
}
