/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node's progress engine, as a PE runs it (§3.12, §5.3).
 *
 * See progress-host.h for what this is and why it is separate from progress.c.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/deadline.h"
#include "base/epoch.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "base/wait-word.h"
#include "abort.h"
#include "am-state.h"
#include "blocked.h"
#include "connection-area.h"
#include "deadlock.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "listener.h"
#include "metrics.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "wire.h"

struct osoe_am_state osoe_am;

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
    if (region != OSOE_REGION_HEAP && region != OSOE_REGION_INTERNAL &&
        region != OSOE_REGION_STATICS)
        osoe_fatal("a peer addressed region %u, which this build does not carry; "
                   "the nodes are running different libraries", region);
    if (pe >= osoe.bootstrap.world_size)
        osoe_fatal("a peer addressed PE %u in a job of %u", pe,
                   osoe.bootstrap.world_size);
    int32_t local = osoe_node_local_id(&osoe.node, pe);
    if (local < 0)
        return NULL;
    if (region == OSOE_REGION_INTERNAL) {
        /* The library's own region, the tail of the same mapping
         * (internal-region.h): sized by the fan-out, the pipeline's depth,
         * the team count and the job's node count, which every node agrees on
         * and every PE of a node (node.c), so this PE's size is the target's. */
        if (offset > osoe.node.internal_bytes || length > osoe.node.internal_bytes - offset)
            osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's internal "
                       "region, which is %llu bytes; the nodes disagree about "
                       "OSOE_COLLECTIVE_FANOUT, OSOE_COLLECTIVE_PIPELINE or OSOE_MAX_TEAMS, "
                       "or run different libraries",
                       length,
                       (unsigned long long)offset, pe,
                       (unsigned long long)osoe.node.internal_bytes);
        return (char *)osoe.node.heap_of[local] + osoe.node.internal_offset + offset;
    }
    if (region == OSOE_REGION_STATICS) {
        /* The program's statics (§4.3), which every PE of the job has alike if
         * it runs one program linked by oshcc. */
        uint64_t bytes = osoe.node.statics_bytes;
        if (offset > bytes || length > bytes - offset)
            osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's symmetric "
                       "statics, which are %llu bytes; the nodes run different programs, "
                       "or one not linked with oshcc", length,
                       (unsigned long long)offset, pe, (unsigned long long)bytes);
        return osoe_node_statics_at(&osoe.node, (uint32_t)local,
                                    (uint32_t)local == osoe.bootstrap.local_id, offset);
    }
    if (offset > osoe.node.heap_size || length > osoe.node.heap_size - offset)
        osoe_fatal("a peer addressed %u bytes at offset %llu of PE %u's heap, which is "
                   "%llu bytes; the nodes disagree about SHMEM_SYMMETRIC_SIZE",
                   length, (unsigned long long)offset, pe,
                   (unsigned long long)osoe.node.heap_size);
    return (char *)osoe.node.heap_of[local] + offset;
}

/* The local id of `pe`, or -1: a reply for a PE not on this node is refused. */
static int32_t local_of(uint32_t pe)
{
    return pe < osoe.bootstrap.world_size ? osoe_node_local_id(&osoe.node, pe) : -1;
}

/*
 * A local PE's get `token`, blocking or not, in flight: found by the extent
 * its token names, and only if the record there holds that very token and is
 * still in flight - a reply to a get already landed or copied out, or to none,
 * is refused (progress.h). The piece's bytes are the extents from there.
 */
static unsigned char *landing_request(void *context, uint32_t pe, uint64_t token,
                                      uint32_t *length)
{
    (void)context;
    int32_t local = local_of(pe);
    uint64_t index = token & OSOE_LANDING_EXTENT_MASK;
    if (local < 0 || index >= osoe.node.landing_requests)
        return NULL;
    struct osoe_landing_record *r = &osoe_node_landing_records(&osoe.node, (uint32_t)local)[index];
    if (atomic_load_explicit(&r->state, memory_order_acquire) != OSOE_LANDING_ISSUED ||
        atomic_load_explicit(&r->token, memory_order_acquire) != token)
        return NULL;
    uint32_t bytes = atomic_load_explicit(&r->length, memory_order_acquire);
    if ((uint64_t)index * OSOE_LANDING_EXTENT_BYTES + bytes > osoe.node.landing_arena_bytes)
        return NULL;
    *length = bytes;
    return osoe_node_landing_arena(&osoe.node, (uint32_t)local) +
           (size_t)index * OSOE_LANDING_EXTENT_BYTES;
}

/*
 * A nonblocking get's reply has landed whole (landing_request has checked it).
 * Its record goes to LANDED first, with release, so that a second reply naming
 * it is refused and any of the PE's threads may now claim it for copying out
 * (transport-am.c); then the context block the token names counts its bytes -
 * the value half of base/wait-word.h's two-sided wait, a seq_cst add, woken
 * only if a thread waits there - and the zone's `freed` word is bumped too,
 * since a thread waiting for room may copy this reply out and make some. The
 * add also releases the reply's bytes, which the parser wrote before this
 * call, to the thread that reads the counter and then them.
 */
static void landed(void *context, uint32_t pe, uint64_t token, uint32_t bytes)
{
    (void)context;
    int32_t local = local_of(pe);
    uint32_t block = (uint32_t)((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK);
    if (local < 0 || block >= osoe.node.contexts_per_pe)
        osoe_fatal("a nonblocking get's reply for PE %u names context block %u of %u: its "
                   "token was accepted, so this node made it, and it made none past its pool",
                   pe, block, osoe.node.contexts_per_pe);
    struct osoe_landing_record *r =
        &osoe_node_landing_records(&osoe.node, (uint32_t)local)[token & OSOE_LANDING_EXTENT_MASK];
    /* seq_cst: the value of two wait-word waits, the context's and the zone's
     * `freed`, whose wakes are skipped when nobody is registered
     * (base/wait-word.h; review of slice 2, S1). */
    atomic_store_explicit(&r->state, OSOE_LANDING_LANDED, memory_order_seq_cst);
    struct osoe_context_block *c = osoe_node_context(&osoe.node, (uint32_t)local, block);
    atomic_fetch_add_explicit(&c->bytes_landed, bytes, memory_order_seq_cst);
    osoe_wait_word_wake_all(&c->counter_wake);
    osoe_wait_word_wake_all(&osoe_node_landing(&osoe.node, (uint32_t)local)->freed);
}

/*
 * A blocking get's reply, or a fetching AMO's value, for the thread the token
 * names (§4.11 item 5): refused unless that thread armed its slot for this very
 * token (§3.4's trust boundary; second review, D4). A get's record goes to
 * LANDED_BLOCKING, which no other thread claims, so that a duplicate is
 * refused; then the length or value, then `ready`, then the wake - the thread
 * loads the wake word before the flag (§4.6), so a bump between its load and
 * its wait returns at once instead of sleeping stale.
 */
static bool completed(void *context, uint32_t pe, uint64_t token, uint32_t length,
                      uint64_t value)
{
    (void)context;
    int32_t local = local_of(pe);
    uint32_t thread = (uint32_t)((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK);
    if (local < 0 || thread >= osoe.node.threads_per_pe)
        return false;
    struct osoe_thread_record *t = osoe_node_thread(&osoe.node, (uint32_t)local, thread);
    if (atomic_load_explicit(&t->armed_token, memory_order_acquire) != token)
        return false;
    uint64_t index = token & OSOE_LANDING_EXTENT_MASK;
    if (index != OSOE_LANDING_NO_EXTENT) {
        if (index >= osoe.node.landing_requests)
            return false;
        atomic_store_explicit(&osoe_node_landing_records(&osoe.node, (uint32_t)local)[index].state,
                              OSOE_LANDING_LANDED_BLOCKING, memory_order_release);
    }
    atomic_store_explicit(&t->length, length, memory_order_release);
    atomic_store_explicit(&t->value, value, memory_order_release);
    atomic_store_explicit(&t->token, token, memory_order_release);
    atomic_store_explicit(&t->ready, 1, memory_order_release);
    atomic_fetch_add_explicit(&t->wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&t->wake, INT_MAX, OSOE_FUTEX_SHARED);
    return true;
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
 * A team the engine meets for the first time (progress.h): its record in its
 * block on this node (node.h), loaded as it was published - `published` with
 * acquire, then the rest (§3.16) - and turned into the engine's terms: the
 * node numbers its messages go to, and the team's name on each of those nodes,
 * its number and the local id there of its local lead, rather than the
 * neighbours' leads' ranks the record keeps (the team redesign's step D); and
 * the block's generation, which the engine learnt it at.
 * False for a team with no record here, or one spanning a single node: the
 * engine then ends the job naming it, as it did an unregistered team. The
 * record's counts are this library's own, but they are read from shared
 * memory, so a child count past the fan-out is refused rather than followed.
 */
static bool team_of_record(void *context, uint32_t team, struct osoe_progress_team *learnt)
{
    (void)context;
    struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
    if (!b || !atomic_load_explicit(&b->record.published, memory_order_acquire))
        return false;
    const struct osoe_team_record *r = &b->record;
    uint32_t number = atomic_load_explicit(&b->name.number, memory_order_acquire);
    if (r->nodes < 2)
        return false;
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    if (r->children > fanout || r->index >= r->nodes)
        osoe_fatal("team %u's record on this node says %u children at index %u of %u nodes, "
                   "at a fan-out of %u", number, r->children, r->index, r->nodes, fanout);
    learnt->tree = (struct osoe_collective_tree){
        .count = r->nodes,
        .index = r->index,
        .fanout = fanout,
        .parent = r->parent == UINT32_MAX ? OSOE_COLLECTIVE_TREE_NONE
                                          : osoe_node_of(&osoe.job, r->parent),
        .children = r->children,
    };
    learnt->number = number;
    learnt->parent_lead = r->parent == UINT32_MAX ? 0
                                                  : osoe_job_local_index(&osoe.job, r->parent);
    for (uint32_t j = 0; j < r->children; j++) {
        learnt->tree.child[j] = osoe_node_of(&osoe.job, r->child[j]);
        learnt->child_lead[j] = osoe_job_local_index(&osoe.job, r->child[j]);
    }
    learnt->published = &b->tree;
    learnt->generation = atomic_load_explicit(&b->name.generation, memory_order_acquire);
    return true;
}

/* A team's block on this node by the name a message carries, and the block's
 * generation (progress.h): read with acquire (§3.16), though the map entry's
 * acquire already carries the name that was written before it (node.h). */
static uint32_t team_by_name(void *context, uint32_t lead, uint32_t number,
                             uint32_t *generation)
{
    (void)context;
    uint32_t block = osoe_node_team_find(&osoe.node, lead, number);
    if (block != UINT32_MAX)
        *generation = atomic_load_explicit(&osoe_node_team(&osoe.node, block)->name.generation,
                                           memory_order_acquire);
    return block;
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

/* A deadlock detector message from another node's lead (progress.h). */
static void deadlock_arrived(void *context, uint32_t node, const struct osoe_deadlock_message *m)
{
    (void)context;
    osoe_deadlock_message(node, m);
}

/* A peer's abortive goodbye (progress.h): this node aborts too, on behalf of
 * the same PE and with the same status and disposition - unless it already has,
 * and then the first declaration stands (node.c) - and its agent, woken by the
 * declaration, tells its own peers and ends (abort.h). */
static void abort_arrived(void *context, uint32_t pe, int32_t status, uint32_t disposition)
{
    (void)context;
    osoe_node_declare_abort(&osoe.node, pe, status,
                            disposition == OSOE_ABORT_GLOBAL_EXIT ? OSOE_ABORT_GLOBAL_EXIT
                                                                  : OSOE_ABORT_FATAL);
}

/* ---- waking the engine (§3.12) ------------------------------------------ */

/*
 * Wake the lead only if it sleeps (§3.10; lead_wake_model.py). The caller has
 * already set its connection's dirty bit (osoe_connection_area_mark), a seq_cst
 * RMW; this load of the flag is the second half of the writer's side of a
 * two-sided protocol, so it is seq_cst too (§3.16). If it reads 0 - the lead has
 * not raised its flag, or another writer claimed this sleep's wake - the lead's
 * seq_cst load of the bits after raising it will see this writer's bit, or the
 * wake already claimed is on its way. If it wins the flag, the bump changes the
 * word the lead sleeps on, so a futex_wait that has not started yet returns at
 * once.
 *
 * An awake lead costs a writer one load of a line it shares with every other
 * writer, read-mostly, instead of a locked add on it and a futex_wake syscall.
 */
void osoe_progress_host_wake_lead(void)
{
    if (!osoe_am.internode)
        return;
    /*
     * Claim the wake: only the writer whose CAS takes the flag from 1 to 0
     * makes the syscall; the rest see 0 and skip until the lead raises it
     * again. Without the claim every writer during the lead's wake-up latency
     * saw 1, and a burst of puts still woke the lead for 93% of them (the
     * model's `every_writer`). The load first, so an awake lead's line stays
     * shared rather than taken exclusive by every writer's CAS.
     */
    uint32_t sleeping = 1;
    if (atomic_load_explicit(osoe_am.area.lead_sleeping, memory_order_seq_cst) == 0 ||
        !atomic_compare_exchange_strong_explicit(osoe_am.area.lead_sleeping, &sleeping, 0,
                                                 memory_order_seq_cst, memory_order_seq_cst)) {
        osoe_count(OSOE_C_AM_WAKES_SKIPPED, 1);
        return;
    }
    osoe_count(OSOE_C_AM_WAKES_SENT, 1);
    struct osoe_pe_slot *lead = &osoe.node.slots[0];
    atomic_fetch_add_explicit(&lead->agent_wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&lead->agent_wake, 1, OSOE_FUTEX_SHARED);
}

/* Whether this lead has raised lead_sleeping and not lowered it. Private to the
 * agent thread, so the flag is stored only when it changes. */
static bool flag_raised;

/*
 * The check keepalive cannot make (progress.h's osoe_progress_check_silent):
 * the budget keepalive is given - idle, then count probes an interval apart -
 * and at most one look an interval, so a vanished peer with data in flight is
 * found as soon as an idle one would be. 0: keepalive is off, and so is this.
 */
static uint32_t silent_budget_ms;
static uint64_t silent_interval_ns, next_silent_check_ns;

static void silent_check_start(const struct osoe_bootstrap *b)
{
    next_silent_check_ns = 0;
    if (b->keepalive_off) {
        silent_budget_ms = 0;
        return;
    }
    uint32_t idle = b->keepalive_idle_s ? b->keepalive_idle_s : OSOE_DEFAULT_KEEPALIVE_IDLE_S;
    uint32_t interval = b->keepalive_interval_s ? b->keepalive_interval_s
                                                : OSOE_DEFAULT_KEEPALIVE_INTERVAL_S;
    uint32_t count = b->keepalive_count ? b->keepalive_count : OSOE_DEFAULT_KEEPALIVE_COUNT;
    silent_budget_ms = (idle + interval * count) * 1000;
    silent_interval_ns = (uint64_t)interval * 1000000000;
}

/*
 * Read from another process, since only the lead runs the engine. The value is
 * the lead's own bookkeeping and is written by that thread alone, so a reader
 * sees either the old value or the new one - and the old one only makes a wait
 * go round again, which is why this needs no protocol of its own.
 */
uint64_t osoe_progress_host_acked(uint32_t peer)
{
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    return connection ? osoe_connection_acked(connection) : 0;
}

/* ---- lifecycle --------------------------------------------------------- */

/* This lead's node-pair listener, from advertise to the mesh (osoe_wires_open
 * closes it). Outside osoe_am, which osoe_transport_init starts afresh after the
 * rendezvous that carried its port. */
static struct osoe_listener mesh_listener = { .fd = -1 };

void osoe_progress_host_advertise(void *blob)
{
    osoe_listener_open_any_port(&mesh_listener, &osoe.bootstrap, osoe.node.hdr->session, NULL);
    uint16_t port = mesh_listener.port;
    memcpy(blob, &port, sizeof port);
}

void osoe_progress_host_start(uint32_t session)
{
    /* osoe_am.internode and osoe_am.lead are set at the start of
     * osoe_transport_init, before anything that reads them - including the
     * dirty set, which is sized from the first (transport-am.c). */
    if (!osoe_am.internode)
        return;

    /* Every local PE maps the area: its application threads reserve into these
     * buffers like any other writer (§3.5, §3.12). */
    osoe_connection_area_open(&osoe_am.area, &osoe.bootstrap, osoe.job.node_count, session,
                              osoe.node.hdr->creator_pid);
    if (!osoe_am.lead)
        return;

    uint32_t peers = osoe.job.node_count - 1;
    /*
     * §3.8's two arguments. C per connection bounds the replies - the origin
     * takes a credit per get or fetching AMO and returns it when the reply
     * lands (argument 1). degree x teams, node-wide, bounds the collective
     * forwards: while collectives are blocking, a node of the internode tree
     * has at most one ARRIVE or its k RELEASEs queued per team (argument 2,
     * reply.h). The pool is node-wide, so an idle connection costs nothing.
     *
     * Before slice 2 this was peers x C, with no term for the forwards, so a
     * node holding C gets toward a peer had nothing left for its own barrier
     * arrival and the request was dropped (second review, D1). Slice 2 added
     * the forwards per connection, which was the star's shape - its root
     * released every peer at once. No legal program can empty the pool, and
     * progress.c ends the job naming the peer if one does.
     */
    /*
     * The forwards' term counts the teams this node can hold at once, since
     * its engine forwards for those and no others, and the pool is node-wide,
     * so it costs a descriptor per team per degree and not one per connection:
     * OSOE_TEAM_BLOCKS since the team redesign's step D (D5), its pool of team
     * blocks - a team's members are released only once its RELEASEs have left
     * the control queues (release_local, progress.c, holds the local release
     * until then), so a block given back and claimed again finds none of the
     * old team's still queued. Until step D's second part it
     * was OSOE_MAX_TEAMS, the teams that could exist job-wide, which a node
     * could not exceed while numbers were job-wide; a node may now hold more
     * teams than any one PE's numbers. 5,120 descriptors at the defaults,
     * about 240 KiB. The per-team state below is sized by the pool too, since
     * it is indexed by block.
     */
    /*
     * And one a peer for the deadlock detector's messages (§3.15), of which a
     * connection has at most one queued: the root asks each node once a
     * collection and a node answers once, each only after what came before
     * has been answered (deadlock.c).
     */
    uint32_t teams = osoe.bootstrap.team_blocks;
    uint32_t descriptors = peers * (osoe_am.area.credits + OSOE_REPLY_SLACK + 1) +
                           OSOE_COLLECTIVE_DEGREE(osoe.bootstrap.collective_fanout) * teams;
    osoe_am.wire = calloc(peers, sizeof *osoe_am.wire);
    osoe_am.connection = calloc(peers, sizeof *osoe_am.connection);
    osoe_am.busy = calloc((peers + 63) / 64, sizeof *osoe_am.busy);
    osoe_am.descriptor = calloc(descriptors, sizeof *osoe_am.descriptor);
    osoe_am.scratch = malloc(osoe_am.area.chunk_bytes);
    osoe_am.team = calloc(teams, sizeof *osoe_am.team);
    osoe_am.pending = calloc(teams, sizeof *osoe_am.pending);
    if (!osoe_am.wire || !osoe_am.connection || !osoe_am.busy || !osoe_am.descriptor ||
        !osoe_am.scratch || !osoe_am.team || !osoe_am.pending)
        osoe_fatal("out of memory setting up the AM backend for %u peer nodes", peers);

    /*
     * The node-pair connections, one per peer in peer-slot order - node order
     * with this node left out, as osoe_progress_slot computes it (§3.4) - and the
     * ring that carries them, disabled until this lead's agent enables it on its
     * first pass: the agent is its only submitter (ring.h). Each peer's address
     * is the rendezvous's, and its port the one its blob advertised (§6.2).
     * O(N) once, for the eager mesh.
     */
    struct sockaddr_in *peers_address = calloc(osoe.job.node_count, sizeof *peers_address);
    if (!peers_address)
        osoe_fatal("out of memory for %u node addresses", osoe.job.node_count);
    for (uint32_t n = 0; n < osoe.job.node_count; n++) {
        uint16_t port;
        memcpy(&port, osoe_job_backend_blob(&osoe.job, n), sizeof port);
        peers_address[n] = (struct sockaddr_in){ .sin_family = AF_INET,
                                                 .sin_port = htons(port),
                                                 .sin_addr.s_addr = osoe_job_node_address(&osoe.job, n) };
    }
    struct osoe_mesh mesh = { &mesh_listener, peers_address };
    osoe_wires_open(osoe_am.wire, &osoe_am.ring, &osoe.bootstrap, session, &mesh);
    free(peers_address);

    struct osoe_progress_host host = { resolve, notify, release, NULL, abort_arrived,
                                       landing_request, landed, completed, deadlock_arrived };
    osoe_progress_init(&osoe_am.progress, &osoe_am.area, osoe_am.wire, &osoe_am.ring,
                       osoe_am.descriptor, descriptors, osoe_am.scratch,
                       (uint32_t)osoe_am.area.chunk_bytes, &host);
    osoe_progress_attach_teams(&osoe_am.progress, osoe_am.team, osoe_am.pending, teams,
                               team_of_record, team_by_name);
    /* Count into the control segment, where osoe-stat and the tests read it. */
    osoe_am.progress.metrics = &osoe.node.hdr->am;
    /* §3.14's connection scope, in the same segment: the engine writes it,
     * osoe-stat and the tests read it. Each team's tree state goes to its own
     * block, which the engine learns with the team (team_of_record). */
    osoe_am.progress.connection_metrics = osoe.node.connection_metrics;
    for (uint32_t c = 0; c < osoe.node.connection_count; c++)
        atomic_store_explicit(&osoe.node.connection_metrics[c].peer,
                              c < osoe.job.node ? c : c + 1, memory_order_release);
    osoe_progress_attach(&osoe_am.progress, osoe_am.connection, osoe_am.busy);
    silent_check_start(&osoe.bootstrap);
    flag_raised = false;
}

/*
 * Put everything this node still owes the network on the wire, before the
 * engine that would have sent it goes away. On the agent thread, in its exit
 * (transport.h), whose engine this is.
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
 * This is the agent's last turn, so it is the only thread touching the engine
 * and polls it directly (§3.12).
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
        if (!osoe_progress_poll(&osoe_am.progress))
            osoe_ring_wait(&osoe_am.ring, OSOE_SWEEP_NS);
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

/*
 * The close (§6.4, close_model.py): every connection sends its goodbye and ends
 * its stream, and the lead keeps reading - and applying, since a peer may still
 * be paying an acknowledgement - until every peer's stream has ended after its
 * own goodbye. Only then are the sockets closed: closing one while the peer
 * still sends makes the kernel reset it, and the peer's send fails on a node
 * that did nothing wrong. The no-progress rule watches how many connections
 * have ended (§3.15); by default a peer that never finalizes is waited for.
 */
static void close_connections(void)
{
    uint32_t peers = osoe.job.node_count - 1;
    osoe_progress_close(&osoe_am.progress);
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (!osoe_progress_closed(&osoe_am.progress)) {
        /* A poll can take a peer's end of stream and say it moved nothing, so
         * the condition is asked again before the sleep, which would otherwise
         * last the whole sweep (the release fixes' review, conformance F1). */
        if (!osoe_progress_poll(&osoe_am.progress) && !osoe_progress_closed(&osoe_am.progress))
            osoe_ring_wait(&osoe_am.ring, OSOE_SWEEP_NS);
        if (!osoe_deadline_expired(&sweep))
            continue;
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_sweep_expired("shmem_finalize");
        uint32_t ended = 0;
        for (uint32_t slot = 0; slot < peers; slot++)
            ended += osoe_wire_ended(&osoe_am.wire[slot]);
        if (osoe_stall_watch_stalled(&watch, ended))
            osoe_fatal("shmem_finalize: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT) "
                       "closing this node's connections - %u of %u peers have ended theirs; "
                       "the others have not finalized",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), ended, peers);
    }
    for (uint32_t slot = 0; slot < peers; slot++)
        osoe_wire_close(&osoe_am.wire[slot]);
    osoe_ring_close(&osoe_am.ring);
}

/*
 * The job is ending: tell every peer - an abortive goodbye on each connection
 * that still has someone to tell - before this process ends (abort.h), and
 * keep receiving until each peer's kernel has acknowledged it, since this
 * process's end with input unread is a reset, which would drop it
 * (osoe_progress_aborted). Bounded by OSOE_ABORT_GRACE_SECONDS, since a peer
 * that is stopped with a full receive window could hold the bytes forever, and
 * this process is ending either way: a peer told nothing takes its end of
 * stream for a death, which is what it is for that peer. Called on the agent
 * thread, whose ring this is.
 */
/* Whether world rank `rank` is one of this node's PEs. */
static bool local_rank(uint32_t rank)
{
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if (osoe.bootstrap.gtids[i] == rank)
            return true;
    return false;
}

void osoe_progress_host_abort(void)
{
    if (!osoe_am.internode || !osoe_am.lead)
        return;
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    if (!osoe_am.ring.enabled)
        osoe_ring_enable(&osoe_am.ring);
    osoe_progress_abort(&osoe_am.progress, pe, status, (uint32_t)how,
                        how == OSOE_ABORT_GLOBAL_EXIT);
    struct osoe_deadline grace = osoe_deadline_in(UINT64_C(1000000000) * OSOE_ABORT_GRACE_SECONDS);
    /* Each wait asks its condition again after a poll that moved nothing, as
     * close_connections does: an end of stream taken by the poll would
     * otherwise be seen a sweep late, which under srun -K1 is the window in
     * which the first node to end can have another node's PEs killed. */
    while (!osoe_progress_aborted(&osoe_am.progress) && !osoe_deadline_expired(&grace))
        if (!osoe_progress_poll(&osoe_am.progress) && !osoe_progress_aborted(&osoe_am.progress))
            osoe_ring_wait(&osoe_am.ring, OSOE_SWEEP_NS);
    /*
     * A global exit called on this node: the PE carrying its status leaves
     * last (abort.h), so this lead outlasts every other node first. Each other
     * lead ends only after its own node's PEs, and its end ends its stream, so
     * once every stream has ended nothing of the job runs but this node. Not
     * a node whose own goodbye named another caller: it carries that one, and
     * each waiting for the other would only run both to the bound.
     */
    if (how != OSOE_ABORT_GLOBAL_EXIT || !local_rank(pe))
        return;
    struct osoe_deadline last = osoe_deadline_in(UINT64_C(1000000000) *
                                                 OSOE_ABORT_LEAD_WAIT_SECONDS);
    while (!osoe_progress_peers_ended(&osoe_am.progress, pe) && !osoe_deadline_expired(&last))
        if (!osoe_progress_poll(&osoe_am.progress) &&
            !osoe_progress_peers_ended(&osoe_am.progress, pe))
            osoe_ring_wait(&osoe_am.ring, OSOE_SWEEP_NS);
}

void osoe_progress_host_exit(void)
{
    if (!osoe_am.internode || !osoe_am.lead)
        return;
    if (!osoe_am.ring.enabled)          /* an agent stopped before its first pass */
        osoe_ring_enable(&osoe_am.ring);
    flush_before_close();
    close_connections();
}

void osoe_progress_host_stop(void)
{
    if (!osoe_am.internode)
        return;
    if (osoe_am.lead) {
        free(osoe_am.wire);
        free(osoe_am.connection);
        free(osoe_am.busy);
        free(osoe_am.descriptor);
        free(osoe_am.scratch);
        free(osoe_am.team);
        free(osoe_am.pending);
        osoe_am.wire = NULL;
        osoe_am.connection = NULL;
        osoe_am.busy = NULL;
        osoe_am.descriptor = NULL;
        osoe_am.scratch = NULL;
        osoe_am.team = NULL;
        osoe_am.pending = NULL;
    }
    osoe_connection_area_close(&osoe_am.area);
    osoe_am.internode = false;
}

/*
 * The agent's pass, and the lead's half of the sleep protocol (§3.10;
 * lead_wake_model.py). A pass that moved nothing is about to be followed by the
 * agent's wait on agent_wake - in the ring, at the value it loaded before the
 * pass. So raise lead_sleeping first - seq_cst, the store side of the two-sided
 * protocol - and then look at the dirty bits again with seq_cst loads: a writer
 * whose bit landed after this pass's exchange either shows here, and the agent
 * goes round again, or read the flag as 1 and bumps agent_wake and wakes it,
 * which completes the ring's armed wait. The flag comes down at the next pass,
 * which is after the wait.
 */
bool osoe_progress_host_poll(void)
{
    if (!osoe_am.internode || !osoe_am.lead)
        return false;
    /* The first pass is on the agent thread, which enabling makes the ring's only
     * submitter (ring.h). It is also where the engine first meets the teams
     * whose records the node's lead wrote before this thread started. */
    if (!osoe_am.ring.enabled) {
        osoe_ring_enable(&osoe_am.ring);
        osoe_progress_meet_teams(&osoe_am.progress, OSOE_PREDEFINED_TEAMS);
    }
    struct osoe_connection_area *a = &osoe_am.area;
    if (flag_raised) {
        atomic_store_explicit(a->lead_sleeping, 0, memory_order_seq_cst);
        flag_raised = false;
    }
    if (silent_budget_ms) {
        uint64_t now = osoe_now_ns();
        if (now >= next_silent_check_ns) {
            next_silent_check_ns = now + silent_interval_ns;
            osoe_progress_check_silent(&osoe_am.progress, silent_budget_ms);
        }
    }
    bool moved = osoe_internode_am_take_arrivals();
    if (osoe_progress_poll(&osoe_am.progress) || moved)
        return true;
    atomic_store_explicit(a->lead_sleeping, 1, memory_order_seq_cst);
    flag_raised = true;
    for (uint32_t w = 0; w < a->dirty_words; w++)
        if (atomic_load_explicit(&a->dirty[w], memory_order_seq_cst) != 0)
            return true;
    return false;
}

bool osoe_progress_host_idle(uint64_t *applied)
{
    if (!osoe_am.internode || !osoe_am.lead) {
        *applied = 0;
        return true;
    }
    *applied = osoe_am.progress.application_applied;
    return osoe_progress_idle(&osoe_am.progress);
}

bool osoe_progress_host_quiet(uint64_t *applied)
{
    if (!osoe_am.internode || !osoe_am.lead) {
        *applied = 0;
        return true;
    }
    return osoe_progress_quiet(&osoe_am.progress, applied);
}

void osoe_progress_host_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m)
{
    if (!osoe_am.internode || !osoe_am.lead)
        osoe_fatal("a deadlock detector message toward node %u from a node with no network",
                   node);
    osoe_progress_deadlock_send(&osoe_am.progress, node, m);
}

bool osoe_progress_host_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    /* The lead of a multi-node job waits in its ring, on its sockets and the
     * word at once (§3.10); every other agent has only the word. Either way
     * the wait ends by a sweep interval at the latest (§4.10). */
    if (osoe_am.internode && osoe_am.lead)
        return osoe_ring_wait_wake(&osoe_am.ring, wake, seen, OSOE_SWEEP_NS);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    return osoe_futex_wait(wake, seen, &sweep, OSOE_FUTEX_SHARED) == 0;
}
