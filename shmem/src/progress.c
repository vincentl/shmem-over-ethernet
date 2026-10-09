/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* liburing.h, through wire.h, uses POSIX types (sigset_t, AT_FDCWD) that plain
 * C11 hides (portability review F12: feature macros are declared per file). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdbit.h>
#include <string.h>

#include "base/futex.h"
#include "fatal.h"
#include "deadlock.h"
#include "owner-map.h"
#include "progress.h"
#include "ring.h"
#include "rmw.h"
#include "wire.h"

/* The sink is bound to one wire: a reply must be enqueued on the queues of the
 * connection its request arrived on, and the parser does not know which that
 * is. A data wire is owner `owner`'s wire `index` (owner-map.h); a control
 * wire, owner 0's control wire `index`, by peer slot. */
struct arrival {
    struct osoe_progress_owner *owner;
    uint32_t index;
    bool control;
};

uint32_t osoe_progress_slot(const struct osoe_progress *p, uint32_t peer)
{
    if (peer == p->node || peer >= p->nodes)
        return OSOE_CONNECTION_NONE;
    return peer < p->node ? peer : peer - 1;
}

/* ---- Applying what arrived ------------------------------------------- */

/* The peer slot of owner `o`'s wire `i`, and the node at its other end. */
static uint32_t slot_of(const struct osoe_progress_owner *o, uint32_t i)
{
    return osoe_owner_slot(o->index, i, o->engine->helpers);
}

static uint32_t peer_of(const struct osoe_progress_owner *o, uint32_t i)
{
    uint32_t slot = slot_of(o, i);
    return slot < o->engine->node ? slot : slot + 1;
}

/* This node's connection toward the peer of owner `o`'s wire `i`. */
static struct osoe_connection *connection_of(const struct osoe_progress_owner *o, uint32_t i)
{
    return osoe_connection_to(o->engine->area, peer_of(o, i));
}

/* The node at the other end of control wire `slot`, by peer slot. */
static uint32_t control_peer(const struct osoe_progress *p, uint32_t slot)
{
    return slot < p->node ? slot : slot + 1;
}

/* The control plane's messages and nothing else go on a control wire, and
 * nothing of theirs on a data wire (the progress helpers proposal's §3.1); an
 * abortive goodbye goes on every wire. */
static bool control_kind(uint16_t kind)
{
    return kind == OSOE_AM_BARRIER_ARRIVE || kind == OSOE_AM_BARRIER_RELEASE ||
           kind == OSOE_AM_DEADLOCK;
}

/* A message's kind as the job's last words name it. */
static const char *message_kind_name(uint16_t kind)
{
    switch (kind) {
    case OSOE_AM_PUT: return "put";
    case OSOE_AM_GET_REQUEST: return "get request";
    case OSOE_AM_GET_REPLY: return "get reply";
    case OSOE_AM_RMW_REQUEST: return "atomic request";
    case OSOE_AM_RMW_REPLY: return "atomic's result";
    case OSOE_AM_BARRIER_ARRIVE: return "barrier arrival";
    case OSOE_AM_BARRIER_RELEASE: return "barrier release";
    case OSOE_AM_ABORT: return "abort";
    case OSOE_AM_DEADLOCK: return "deadlock detector message";
    default: return "message of no known kind";
    }
}

/*
 * §3.4's trust boundary, and the planes kept apart (the progress helpers
 * proposal's §3.1): the tree's state and the detector are the control plane's,
 * and nothing else rides a control wire; the abortive goodbye goes on every
 * wire. Asked before a payload's destination is resolved - so that a put or a
 * get reply on a control wire lands no byte, and is never looked up as a data
 * connection's, which a control wire's index is not (the adversarial read of
 * Part 2) - and again as a message without a payload completes.
 */
static void check_plane(const struct arrival *a, const struct osoe_message_header *header)
{
    if (header->kind == OSOE_AM_ABORT || control_kind(header->kind) == a->control)
        return;
    const struct osoe_progress *p = a->owner->engine;
    osoe_fatal("node %u: a %s from node %u arrived on its %s wire, which does not carry it "
               "(kind %u)", p->node, message_kind_name(header->kind),
               a->control ? control_peer(p, a->index) : peer_of(a->owner, a->index),
               a->control ? "control" : "data", (unsigned)header->kind);
}

/*
 * Where a payload goes, resolved before any payload byte is consumed so that
 * bytes land where they belong rather than in a reassembly buffer (§3.3). This
 * is the only place an incoming message becomes an address, and its wire is
 * checked first (check_plane).
 */

/*
 * Where a get's reply lands - its PE's zone, in the piece its token names - and
 * checked before a byte of it lands (§3.4's trust boundary; second review,
 * D4): a token that names no get of that PE's in flight, or bytes outside its
 * piece, are a peer's bug and fatal before anything is written or counted. The
 * zone sits in the control segment beside the other PEs' zones, so writing past
 * a piece would corrupt a sibling rather than fail. The measured case, before
 * the check: an unsolicited reply with chunk_offset past the zone wrote 64
 * bytes beyond it and the test passed.
 */
static unsigned char *reply_landing(struct osoe_progress_owner *o, uint32_t i,
                                    const struct osoe_am_get_reply *m, uint32_t payload_bytes)
{
    const struct osoe_progress_host *host = &o->engine->host;
    uint32_t length = 0;
    unsigned char *at = host->landing_request
        ? host->landing_request(host->context, m->origin_pe, m->token, &length)
        : NULL;
    if (!at)
        osoe_fatal("a get reply from node %u for PE %u carries token %#llx, which names no "
                   "get of that PE's in flight", peer_of(o, i), m->origin_pe,
                   (unsigned long long)m->token);
    if ((uint64_t)m->chunk_offset + payload_bytes > m->total || m->total != length)
        osoe_fatal("a get reply from node %u for PE %u puts %u bytes at offset %u of a "
                   "%u-byte reply, for a get of %u bytes", peer_of(o, i), m->origin_pe,
                   payload_bytes, m->chunk_offset, m->total, length);
    return at;
}

static void *destination(void *context, const struct osoe_message_header *header,
                         const void *fixed, uint32_t payload_bytes)
{
    struct arrival *a = context;
    const struct osoe_progress_host *host = &a->owner->engine->host;
    check_plane(a, header);

    if (header->kind == OSOE_AM_PUT) {
        struct osoe_am_put m;
        memcpy(&m, fixed, sizeof m);
        return host->resolve(host->context, m.target_pe, m.region, m.offset, payload_bytes);
    }
    if (header->kind == OSOE_AM_GET_REPLY) {
        struct osoe_am_get_reply m;
        memcpy(&m, fixed, sizeof m);
        return reply_landing(a->owner, a->index, &m, payload_bytes) + m.chunk_offset;
    }
    return NULL;
}

/*
 * Take a descriptor and queue it. Fallible on purpose (§3.8): a handler that
 * could not get one leaves the request and the progress thread carries on
 * reaping, which is what makes §3.7's "never reserve" satisfiable without any
 * blocking path at all.
 */
/* This connection's record in the control segment (§3.14), or NULL: by peer
 * slot, as osoe-stat finds it. */
static struct osoe_am_connection_metrics *conn_metrics(struct osoe_progress_owner *o, uint32_t i)
{
    struct osoe_am_connection_metrics *m = o->engine->connection_metrics;
    return m ? &m[slot_of(o, i)] : NULL;
}

/* The queue gauges of one data connection, published where osoe-stat reads
 * them. Called where they change and when the owner visits the connection.
 * The record's control-queue gauges are owner 0's, of the control wire
 * (publish_control_queue): each word of a record has one writer. */
static void publish_queues(struct osoe_progress_owner *o, uint32_t i)
{
    struct osoe_am_connection_metrics *cm = conn_metrics(o, i);
    if (!cm)
        return;
    const struct osoe_reply_queues *q = &o->connection[i].queues;
    atomic_store_explicit(&cm->value_queue_depth, q->values.depth, memory_order_release);
    atomic_store_explicit(&cm->bulk_queue_depth, q->bulk.depth, memory_order_release);
    atomic_store_explicit(&cm->value_queue_high_water, q->values.high_water,
                          memory_order_release);
    atomic_store_explicit(&cm->bulk_queue_high_water, q->bulk.high_water, memory_order_release);
}

/* The control wire's queue gauges, in the record of its peer slot. */
static void publish_control_queue(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_am_connection_metrics *m = p->connection_metrics;
    if (!m)
        return;
    const struct osoe_reply_queue *q = &p->owner[0].control[slot].queue;
    atomic_store_explicit(&m[slot].control_queue_depth, q->depth, memory_order_release);
    atomic_store_explicit(&m[slot].control_queue_high_water, q->high_water,
                          memory_order_release);
}

/* The owner's high-water marks (§3.14), after a push: the deepest this
 * connection's queues have been, and the most descriptors out at once. */
static void note_high_water(struct osoe_progress_owner *o, uint32_t i)
{
    const struct osoe_reply_queues *q = &o->connection[i].queues;
    osoe_counter_word_max(&o->metrics->value_queue_high_water, q->values.high_water);
    osoe_counter_word_max(&o->metrics->bulk_queue_high_water, q->bulk.high_water);
    osoe_counter_word_max(&o->metrics->pool_high_water, o->pool.high_water);
    publish_queues(o, i);
}

static void note_control_high_water(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_owner *o = &p->owner[0];
    osoe_counter_word_max(&o->metrics->control_queue_high_water,
                          o->control[slot].queue.high_water);
    osoe_counter_word_max(&o->metrics->pool_high_water, o->pool.high_water);
    publish_control_queue(p, slot);
}

static const char *reply_kind_name(uint16_t kind)
{
    switch (kind) {
    case OSOE_REPLY_AMO:     return "fetching AMO";
    case OSOE_REPLY_GET:     return "get";
    case OSOE_REPLY_RELEASE: return "collective release";
    case OSOE_REPLY_ARRIVE:  return "collective arrival";
    case OSOE_REPLY_DEADLOCK: return "deadlock detector message";
    default:                 return "unknown";
    }
}

/*
 * The pool is empty, and it cannot be (§3.8; reply.h). Record it where
 * osoe-stat reads it, then end the job on this node naming everything that
 * says which invariant broke: the peer, what it asked for, how full the pool
 * and this connection's queues are, and who enforces the bound.
 *
 * Dropping is never the alternative. A dropped get request leaves its origin
 * waiting for a reply that is not coming, and blaming the wrong node when it
 * gives up; a dropped collective arrival leaves the root waiting for a node
 * that has arrived, and a dropped release leaves a whole node's members on an
 * epoch counter nothing will advance - each a hang whose symptom is elsewhere.
 */
static _Noreturn void pool_exhausted(struct osoe_progress_owner *o, uint32_t i,
                                     const struct osoe_reply *value)
{
    osoe_counter_word_add(&o->metrics->reply_reservation_failures, 1);
    const struct osoe_reply_queues *q = &o->connection[i].queues;
    osoe_fatal("node %u: the reply descriptor pool is empty (%u of %u in use) for a %s "
               "request from node %u; toward it the value queue is %u deep and the bulk "
               "queue %u. Requests are bounded by the credit the origin takes, so the break "
               "is on that node's side: its OSOE_AM_CREDITS differs from this node's, or its "
               "credit accounting is wrong (§3.8, argument 1)", o->engine->node,
               o->pool.in_use, o->pool.count, reply_kind_name(value->kind), peer_of(o, i),
               q->values.depth, q->bulk.depth);
}

/* The same for a control message toward the peer of control wire `slot`. */
static _Noreturn void control_pool_exhausted(struct osoe_progress *p, uint32_t slot,
                                             const struct osoe_reply *value)
{
    struct osoe_progress_owner *o = &p->owner[0];
    osoe_counter_word_add(&o->metrics->reply_reservation_failures, 1);
    osoe_fatal("node %u: the reply descriptor pool is empty (%u of %u in use) for a %s toward "
               "node %u; toward it the control queue is %u deep. The pool reserves a place "
               "for every forward a blocking collective can have outstanding, and one a peer "
               "for the deadlock detector, so this is a bug in this node's collective "
               "bookkeeping (§3.8, argument 2)", p->node, o->pool.in_use, o->pool.count,
               reply_kind_name(value->kind), control_peer(p, slot),
               o->control[slot].queue.depth);
}

/* The connection has send-side work (busy()): visit it on the passes to come.
 * Its bit is cleared by the visit that finds it idle. */
static void mark_busy(struct osoe_progress_owner *o, uint32_t i)
{
    o->busy[i / 64] |= UINT64_C(1) << (i % 64);
}

/* Onto connection `i`'s queue `q`, from the owner's own pool: an owner queues
 * on its own connections only. */
static void enqueue(struct osoe_progress_owner *o, uint32_t i, struct osoe_reply_queue *q,
                    const struct osoe_reply *value)
{
    uint32_t d = osoe_reply_take(&o->pool);
    if (d == OSOE_REPLY_NONE)
        pool_exhausted(o, i, value);
    struct osoe_reply *r = osoe_reply_at(&o->pool, d);
    *r = *value;
    osoe_reply_push(&o->pool, q, d);
    note_high_water(o, i);
    mark_busy(o, i);
}

/* The control wire has something to send or say (control_busy()). */
static void mark_control_busy(struct osoe_progress_owner *o, uint32_t slot)
{
    o->control_busy[slot / 64] |= UINT64_C(1) << (slot % 64);
}

/* A control message onto the queue of control wire `slot`, from owner 0's
 * pool: the control plane is owner 0's alone, whichever thread's pass it is
 * reached from - a receive on owner 0's ring, or the lead's own arrival. */
static void enqueue_control(struct osoe_progress *p, uint32_t slot, const struct osoe_reply *value)
{
    struct osoe_progress_owner *o = &p->owner[0];
    /* Nothing goes on a wire after its goodbye. Only the deadlock detector
     * speaks so late - a veto or a declaration during the close - and its word
     * is dropped, the peers ending anyway; a message of the tree's then would be
     * this node out of step, which finalize's barrier rules out (the
     * adversarial read of Part 2, N3). */
    if (o->control[slot].goodbye != OSOE_GOODBYE_NONE) {
        if (value->kind != OSOE_REPLY_DEADLOCK)
            osoe_fatal("node %u: a %s toward node %u after its control wire's goodbye", p->node,
                       reply_kind_name(value->kind), control_peer(p, slot));
        return;
    }
    uint32_t d = osoe_reply_take(&o->pool);
    if (d == OSOE_REPLY_NONE)
        control_pool_exhausted(p, slot, value);
    struct osoe_reply *r = osoe_reply_at(&o->pool, d);
    *r = *value;
    osoe_reply_push(&o->pool, &o->control[slot].queue, d);
    note_control_high_water(p, slot);
    mark_control_busy(o, slot);         /* possibly another wire than the one being read */
}


/* ---- The internode tree (§5.3) ------------------------------------------ */

/*
 * Publish a team's tree state where a hung collective is read from (§3.14).
 *
 * `lowest_missing` is the point of it: with the tree, the node that has not
 * arrived is found by walking down from the root, each node naming the lowest
 * child that owes it an arrival, in O(log N) visits instead of N. A few
 * single-writer stores (§3.16) per barrier, by the only thread that owns this
 * state.
 */
static void publish_team(struct osoe_progress *p, uint32_t team)
{
    const struct osoe_progress_team *t = &p->team[team];
    struct osoe_collective_state *c = t->published;
    if (!c)
        return;
    uint32_t missing = OSOE_COLLECTIVE_TREE_NONE, arrived = 0;
    /* Only while an epoch is open: between collectives the counts are reset,
     * and naming a child then would say "has not arrived" of an epoch nobody
     * has begun. A node at rest owes nothing, and the walk that reaches it
     * reads epoch_open 0 and looks at its own PEs instead. */
    for (uint32_t j = 0; t->open && j < t->tree.children; j++) {
        if (t->arrived & (UINT32_C(1) << j))
            arrived++;
        else if (missing == OSOE_COLLECTIVE_TREE_NONE)
            missing = t->tree.child[j];
    }
    atomic_store_explicit(&c->epoch_open, t->open, memory_order_release);
    atomic_store_explicit(&c->epoch_closed, t->closed, memory_order_release);
    atomic_store_explicit(&c->own_arrived, t->own ? 1 : 0, memory_order_release);
    atomic_store_explicit(&c->children, t->tree.children, memory_order_release);
    atomic_store_explicit(&c->children_arrived, arrived, memory_order_release);
    atomic_store_explicit(&c->lowest_missing, missing, memory_order_release);
    atomic_store_explicit(&c->parent, t->tree.parent, memory_order_release);
    atomic_store_explicit(&c->fanout, t->tree.fanout, memory_order_release);
}

/* Queue one collective message toward a node of the tree, naming the team as
 * that node knows it: its number, and `lead`, the local id of the team's local
 * lead there (am.h; the team redesign's step D) - on the control wire toward
 * it, the control plane's (the progress helpers proposal's §3.1). A handler may
 * enqueue but never reserve (§3.7): the control wire's flush seals it. */
static void forward(struct osoe_progress *p, uint32_t team, uint32_t node, uint32_t lead,
                    uint16_t kind, uint32_t epoch)
{
    uint32_t slot = osoe_progress_slot(p, node);
    if (slot == OSOE_CONNECTION_NONE || slot >= p->owner[0].controls)
        osoe_fatal("node %u: no control wire toward node %u, a neighbour in team %u's tree",
                   p->node, node, p->team[team].number);
    struct osoe_reply r = { .kind = kind, .origin_pe = p->team[team].number,
                            .target_pe = lead, .length = 0, .value = epoch, .token = team };
    enqueue_control(p, slot, &r);
    if (kind == OSOE_REPLY_RELEASE)
        p->team[team].releases_queued++;
}

/* A RELEASE has left a control wire's queue for the wire: one fewer of its team's
 * holds the team's local release (release_local). The descriptor carries the
 * team as this engine indexes it, in `token`, which no collective message
 * uses. */
static void release_left_queue(struct osoe_progress *p, uint64_t team)
{
    if (team >= p->team_count || p->team[team].releases_queued == 0)
        osoe_fatal("node %u: a RELEASE left a control queue for team %llu, which has none "
                   "queued", p->node, (unsigned long long)team);
    p->team[team].releases_queued--;
}

/*
 * Release down: queue RELEASE to every child, then leave this node's own
 * release for the end of the pass (release_pending), listing the team among
 * those the pass's end applies. O(k).
 */
static void release_down(struct osoe_progress *p, uint32_t team, uint32_t epoch)
{
    struct osoe_progress_team *t = &p->team[team];
    for (uint32_t j = 0; j < t->tree.children; j++)
        forward(p, team, t->tree.child[j], t->child_lead[j], OSOE_REPLY_RELEASE, epoch);
    if (t->release_pending == 0)
        p->pending[p->pending_count++] = team;
    t->release_pending = epoch;
}

/*
 * Book one arrival for (team, epoch): this node's own members (child ==
 * OSOE_COLLECTIVE_TREE_NONE) or child number `child`. Closes the epoch when
 * both are complete. Every refusal here is a node out of step with the job, and
 * each names what it saw: an epoch already closed, two epochs at once, or the
 * same arrival twice.
 */
static void book_arrival(struct osoe_progress *p, uint32_t team, uint32_t epoch,
                         uint32_t child)
{
    struct osoe_progress_team *t = &p->team[team];
    if (epoch <= t->closed)
        osoe_fatal("node %u: team %u has an arrival for epoch %u, and has already closed "
                   "epoch %u; collective epochs have diverged", p->node, t->number, epoch,
                   t->closed);
    if (t->open != 0 && t->open != epoch)
        osoe_fatal("node %u: team %u has arrivals for epochs %u and %u at once; "
                   "collective epochs have diverged", p->node, t->number, t->open, epoch);
    t->open = epoch;
    if (child == OSOE_COLLECTIVE_TREE_NONE) {
        if (t->own)
            osoe_fatal("node %u: team %u's members arrived twice for epoch %u", p->node,
                       t->number, epoch);
        t->own = true;
    } else {
        uint32_t bit = UINT32_C(1) << child;
        if (t->arrived & bit)
            osoe_fatal("node %u: team %u had two arrivals for epoch %u from node %u",
                       p->node, t->number, epoch, t->tree.child[child]);
        t->arrived |= bit;
    }
    uint32_t all = t->tree.children == 32 ? UINT32_MAX
                                          : (UINT32_C(1) << t->tree.children) - 1;
    if (!t->own || t->arrived != all) {
        publish_team(p, team);          /* who is still owed (§3.14) */
        return;
    }
    /* Close it, and reset, before anything is released (the struct's note). */
    t->closed = epoch;
    t->open = 0;
    t->own = false;
    t->arrived = 0;
    publish_team(p, team);
    if (t->tree.parent == OSOE_COLLECTIVE_TREE_NONE)
        release_down(p, team, epoch);
    else
        forward(p, team, t->tree.parent, t->parent_lead, OSOE_REPLY_ARRIVE, epoch);
}

/*
 * The state of the team in block `team` at generation `generation`, or NULL if
 * it has no internode phase on this node. Filled from the host's source the
 * first time the engine meets the team (progress.h), which is how a team
 * created while this thread runs becomes known to it: the host reads the
 * team's record, which is published before the team's first arrival anywhere.
 * O(k) once per team.
 *
 * And again whenever the block shows another generation than the one learnt
 * (the team redesign's step D, D4; tools/modelcheck/team_block_model.py's
 * engine, whose no_relearn breakage applies one team's message to another's
 * state): the block has been given back and claimed for another team since.
 * Nothing of the old team is pending then - its members left only once this
 * node had applied its last release, which waits for its RELEASEs to leave the
 * control queues (release_local), and nothing for it arrives afterwards (the
 * proposal's "A late message") - and an old team caught with an epoch open, a
 * release unapplied or a RELEASE queued is that argument broken, named rather
 * than overwritten.
 */
static struct osoe_progress_team *known_team(struct osoe_progress *p, uint32_t team,
                                             uint32_t generation)
{
    if (team >= p->team_count)
        return NULL;
    struct osoe_progress_team *t = &p->team[team];
    if ((!t->registered || t->generation != generation) && p->team_source) {
        if (t->registered && (t->open != 0 || t->release_pending != 0 ||
                              t->releases_queued != 0))
            osoe_fatal("node %u: team %u's block %u holds another team (generation %u, "
                       "learnt at %u) while epoch %u is open, %u's release is unapplied or "
                       "%u of its RELEASEs are queued: a message for the team arrived after "
                       "its members left it, or they left before its forwards", p->node,
                       t->number, team, generation, t->generation, t->open,
                       t->release_pending, t->releases_queued);
        struct osoe_progress_team learnt = { 0 };
        if (p->team_source(p->host.context, team, &learnt) && learnt.generation == generation) {
            learnt.registered = true;
            *t = learnt;
            publish_team(p, team);
        } else {
            t->registered = false;
        }
    }
    return t->registered && t->generation == generation ? t : NULL;
}

void osoe_progress_meet_teams(struct osoe_progress *p, uint32_t count)
{
    for (uint32_t team = 0; team < count && team < p->team_count; team++) {
        struct osoe_progress_team learnt = { 0 };
        if (p->team_source && p->team_source(p->host.context, team, &learnt))
            known_team(p, team, learnt.generation);
    }
}

/*
 * The team a collective message names, checked (§3.4's trust boundary), and
 * its block here in `*block`. The message names the team as this node knows
 * it - the local id of its local lead here, and its number (am.h) - which the
 * node's map resolves, with the block's generation (the team redesign's step
 * D); a name with no block is never learnt from the message alone, but named.
 * With no finder - the engine's own tests, whose teams are registered by hand -
 * the number is the block, the lead local id 0 and the generation 0. It came on
 * the control wire from `peer` (complete() refuses it on any other).
 */
static struct osoe_progress_team *barrier_team(struct osoe_progress *p, uint32_t peer,
                                               const struct osoe_am_barrier *m,
                                               const char *what, uint32_t *block)
{
    if (m->from_node != peer)
        osoe_fatal("node %u: a barrier %s on node %u's control wire says it is from node %u",
                   p->node, what, peer, m->from_node);
    uint32_t generation = 0;
    *block = p->team_find ? p->team_find(p->host.context, m->lead, m->team, &generation)
                          : m->lead == 0 ? m->team : UINT32_MAX;
    struct osoe_progress_team *t =
        *block == UINT32_MAX ? NULL : known_team(p, *block, generation);
    if (!t)
        osoe_fatal("node %u: a barrier %s from node %u names team %u, led here by local PE %u, "
                   "under which no block with an internode phase is mapped on this node",
                   p->node, what, peer, m->team, m->lead);
    return t;
}

static void complete(void *context, const struct osoe_message_header *header,
                     const void *fixed, void *payload, uint32_t payload_bytes)
{
    struct arrival *a = context;
    struct osoe_progress_owner *o = a->owner;
    struct osoe_progress *p = o->engine;
    uint32_t i = a->index;
    uint32_t peer = a->control ? control_peer(p, i) : peer_of(o, i);
    check_plane(a, header);             /* a payload's was asked by destination() */
    struct osoe_progress_connection *c = a->control ? NULL : &o->connection[i];
    /* The control plane's traffic is counted apart, owner 0's; a connection's
     * record counts its data wire's, which its owner writes, and the node's
     * data counts are their sum (each counter has one writer). */
    if (a->control) {
        osoe_counter_word_add(&o->metrics->control_messages_applied, 1);
    } else {
        osoe_counter_word_add(&o->metrics->messages_applied, 1);
        if (conn_metrics(o, i))
            osoe_counter_word_add(&conn_metrics(o, i)->messages_applied, 1);
    }
    if (header->kind != OSOE_AM_DEADLOCK)
        osoe_counter_word_add(&o->application_applied, 1);

    switch (header->kind) {
    case OSOE_AM_PUT: {
        struct osoe_am_put m;
        memcpy(&m, fixed, sizeof m);
        /* The bytes are already in place; announce them. Best effort for a put
         * (§4.10): a memcpy does not make the writer's half of §4.9's two-sided
         * protocol sound, and the waiter's sweep deadline bounds the loss. */
        if (payload)
            p->host.notify(p->host.context, m.target_pe, m.region, m.offset,
                           payload_bytes);
        break;
    }

    case OSOE_AM_GET_REQUEST: {
        struct osoe_am_get_request m;
        memcpy(&m, fixed, sizeof m);
        /* A reference, not a copy: the source stays in the heap and is sampled
         * at drain time (§3.8), which is what makes a descriptor 48 bytes. */
        struct osoe_reply r = { .kind = OSOE_REPLY_GET, .region = m.region,
                                .origin_pe = m.origin_pe, .target_pe = m.target_pe,
                                .length = m.length, .sent = 0, .offset = m.offset,
                                .token = m.token };
        enqueue(o, i, &c->queues.bulk, &r);
        break;
    }

    case OSOE_AM_RMW_REQUEST: {
        struct osoe_am_rmw_request m;
        memcpy(&m, fixed, sizeof m);
        /* §3.4's trust boundary: an atomic is 4 or 8 bytes, an operation this
         * build knows, at an address aligned to its width - checked before
         * osoe_rmw_apply, which assumes all three (second review, D4). */
        if ((m.width != 4 && m.width != 8) || m.op > OSOE_RMW_XOR || m.offset % m.width != 0)
            osoe_fatal("an atomic from node %u is %u bytes wide, operation %u, at offset "
                       "%llu; this build takes 4 or 8 bytes, a known operation, aligned to "
                       "the width", peer_of(o, i), m.width, m.op,
                       (unsigned long long)m.offset);
        void *at = p->host.resolve(p->host.context, m.target_pe, m.region, m.offset,
                                   m.width);
        uint64_t previous = 0;
        if (at) {
            /*
             * Applied here, at handler time, atomically - a fetching AMO cannot
             * be deferred, because it modifies the target and that modification
             * is ordered on the stream against every later operation touching
             * the location (§3.8). Deferring it would reorder what the stream
             * ordered.
             */
            previous = osoe_rmw_apply(at, (enum osoe_rmw_op)m.op, m.width, m.operand,
                                      m.compare, m.flags | OSOE_TRANSPORT_FETCH);
            /* Exact for rmw (§4.10): every applied rmw notifies. */
            p->host.notify(p->host.context, m.target_pe, m.region, m.offset, m.width);
        }
        if (m.flags & OSOE_TRANSPORT_FETCH) {
            /* The descriptor carries the *value*, sampled now - the sampling
             * asymmetry §3.8 says not to unify. */
            struct osoe_reply r = { .kind = OSOE_REPLY_AMO, .origin_pe = m.origin_pe,
                                    .target_pe = m.target_pe, .length = 0,
                                    .value = previous, .token = m.token };
            enqueue(o, i, &c->queues.values, &r);
        }
        break;
    }

    case OSOE_AM_GET_REPLY: {
        struct osoe_am_get_reply m;
        memcpy(&m, fixed, sizeof m);
        /* destination() has placed and checked any bytes; a reply with none
         * never passed through it, so it is checked here, before it counts a
         * byte or completes anything - and every piece is, since the record it
         * names must still be in flight. Nothing is published until the whole
         * reply is in: a get larger than an AM chunk arrives as several replies
         * (§3.2), and a reply is not its caller's until it is complete (§4.6;
         * an early version released the caller on the first chunk). */
        (void)reply_landing(o, i, &m, payload_bytes);
        if (m.chunk_offset + payload_bytes < m.total)
            break;
        if (!(m.token & OSOE_LANDING_NONBLOCKING)) {
            /* A blocking get: its thread copies the bytes out and returns the
             * credit once it has read them. */
            if (!p->host.completed(p->host.context, m.origin_pe, m.token, m.total, 0))
                osoe_fatal("a get reply from node %u for PE %u carries token %#llx, which no "
                           "thread of that PE waits for", peer_of(o, i), m.origin_pe,
                           (unsigned long long)m.token);
            break;
        }
        p->host.landed(p->host.context, m.origin_pe, m.token, m.total);
        /*
         * §3.8's credit comes back as the reply lands, which is here: the PE
         * copies it out only at its next quiet, and holding the credit until
         * then would keep its node-mates' gets waiting on replies delivered long
         * ago (reply.h). A blocking operation's credit its caller returns, once
         * it has read its reply.
         */
        struct osoe_connection *connection = connection_of(o, i);
        if (!connection)
            osoe_fatal("a get reply arrived from node %u, toward which this node has no "
                       "connection", peer_of(o, i));
        osoe_credit_give(osoe_connection_credit(connection));
        break;
    }

    case OSOE_AM_BARRIER_ARRIVE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        uint32_t block;
        struct osoe_progress_team *t = barrier_team(p, peer, &m, "arrival", &block);
        /* §3.4's trust boundary: only a child sends ARRIVE up to this node. A
         * node that disagrees about the tree - a different
         * OSOE_COLLECTIVE_FANOUT, or a different node set - is named rather
         * than counted. */
        uint32_t child = osoe_collective_tree_child(&t->tree, m.from_node);
        if (child == OSOE_COLLECTIVE_TREE_NONE)
            osoe_fatal("node %u: team %u's arrival for epoch %u came from node %u, which is "
                       "not a child of this node in its tree (fan-out %u over %u nodes); "
                       "the nodes disagree about the tree", p->node, t->number, m.epoch,
                       m.from_node, t->tree.fanout, t->tree.count);
        book_arrival(p, block, m.epoch, child);
        break;
    }

    case OSOE_AM_BARRIER_RELEASE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        uint32_t block;
        struct osoe_progress_team *t = barrier_team(p, peer, &m, "release", &block);
        if (m.from_node != t->tree.parent)
            osoe_fatal("node %u: team %u's release for epoch %u came from node %u; this "
                       "node's parent in the tree is node %u", p->node, m.team, m.epoch,
                       m.from_node, t->tree.parent);
        if (m.epoch != t->closed || t->open != 0 || t->release_pending != 0)
            osoe_fatal("node %u: team %u's release for epoch %u arrived with epoch %u the "
                       "last closed here and %u open; collective epochs have diverged",
                       p->node, m.team, m.epoch, t->closed, t->open);
        release_down(p, block, m.epoch);
        break;
    }

    case OSOE_AM_RMW_REPLY: {
        struct osoe_am_rmw_reply m;
        memcpy(&m, fixed, sizeof m);
        if (!p->host.completed(p->host.context, m.origin_pe, m.token, 0, m.value))
            osoe_fatal("an atomic's reply from node %u for PE %u carries token %#llx, which "
                       "no thread of that PE waits for", peer_of(o, i), m.origin_pe,
                       (unsigned long long)m.token);
        break;
    }

    case OSOE_AM_ABORT: {
        /* A peer's abortive goodbye: its FIN follows, and is not a death (the
         * segment's goodbye flag says so), and nothing need be told to it. */
        struct osoe_am_abort m;
        memcpy(&m, fixed, sizeof m);
        if (a->control) {
            o->control[i].peer_aborted = true;
            o->control[i].peer_abort_origin = m.origin_pe;
        } else {
            c->peer_aborted = true;
            c->peer_abort_origin = m.origin_pe;
        }
        if (p->host.abort)
            p->host.abort(p->host.context, m.origin_pe, m.status, m.disposition);
        break;
    }

    case OSOE_AM_DEADLOCK: {
        struct osoe_am_deadlock m;
        memcpy(&m, fixed, sizeof m);
        if (m.from_node != peer)
            osoe_fatal("a deadlock detector message from node %u names node %u", peer,
                       m.from_node);
        struct osoe_deadlock_message d = { .round = m.round, .kind = m.kind, .phase = m.phase,
                                           .ok = m.ok };
        if (p->host.deadlock)
            p->host.deadlock(p->host.context, m.from_node, &d);
        break;
    }

    default:
        break;
    }
}

static void segment_applied(void *context, const struct osoe_am_segment_header *segment)
{
    struct arrival *a = context;
    struct osoe_progress_owner *o = a->owner;
    /* A control wire's: applied, and acknowledged, as a data wire's below; its
     * peer's goodbye. */
    if (a->control) {
        struct osoe_progress_control *c = &o->control[a->index];
        osoe_counter_word_add(&o->metrics->control_segments_applied, 1);
        c->applied = segment->seq;
        if (segment->flags & OSOE_AM_SEGMENT_ACK_REQUESTED)
            c->owe_ack = true;
        if (segment->ack > c->acked)
            c->acked = segment->ack;
        if (segment->flags & OSOE_AM_SEGMENT_GOODBYE)
            c->peer_goodbye = true;
        return;
    }
    osoe_counter_word_add(&o->metrics->segments_applied, 1);
    struct osoe_progress_connection *c = &o->connection[a->index];
    struct osoe_am_connection_metrics *cm = conn_metrics(o, a->index);
    if (cm) {
        osoe_counter_word_add(&cm->segments_applied, 1);
        osoe_counter_word_add(&cm->bytes_applied, segment->total_len);
    }

    /*
     * Applied, not merely received: §4.6 makes that the difference, because a
     * peer's acknowledgement is the origin's evidence that its writes are in
     * place. Recording it here - after every message of the segment has been
     * applied - is what makes the acknowledgement mean what quiet needs.
     */
    c->applied = segment->seq;
    if (segment->flags & OSOE_AM_SEGMENT_ACK_REQUESTED)
        c->owe_ack = true;
    /* Its last segment: the end of stream that follows is a peer that finished
     * (§6.4). Header-only, so nothing of it is applied but this. */
    if (segment->flags & OSOE_AM_SEGMENT_GOODBYE)
        c->peer_goodbye = true;
    if (segment->ack > c->acked) {
        c->acked = segment->ack;
        /* Publish it where a waiting PE can see it: quiet runs in whichever
         * process issued the writes, which is usually not this one (§4.6). */
        struct osoe_connection *connection = connection_of(o, a->index);
        if (connection)
            osoe_connection_publish_acked(connection, c->acked);
    }
}

/* ---- Receiving -------------------------------------------------------- */

/* Whether the node's abort is declared (progress.h's host.aborting). */
static bool node_aborting(const struct osoe_progress *p)
{
    return p->host.aborting && p->host.aborting(p->host.context);
}

/*
 * Take everything that has arrived on wire `w`, through `parser` and the sink
 * bound to `a`. Called for every wire the ring's reap marked readable, and it
 * must empty it: a provided buffer left queued would be held across the next
 * reap, which the ring refuses (§3.3, invariant 23). Then judge the wire's end,
 * by its own goodbye and its own end of stream (§6.4; close_model.py's
 * two_wires): `name` says which wire it is toward `peer`, for a fatal to name.
 * `other_aborted`, when the same owner holds the peer's other wire, is that
 * wire's word that the peer's abort has arrived on it.
 */
static bool receive_wire(struct osoe_progress_owner *o, struct osoe_wire *w,
                         struct osoe_am_parser *parser, struct arrival *a,
                         const bool *peer_goodbye, const bool *peer_aborted,
                         const bool *other_aborted, bool *abort_done, uint32_t peer,
                         const char *name)
{
    struct osoe_am_sink sink = { destination, complete, segment_applied, a };
    uint32_t node = o->engine->node;
    bool moved = false;
    /* Parsed where the kernel put the bytes (§3.3): the parser keeps no pointer
     * into them, so each buffer goes back to the kernel as soon as it is read. */
    for (;;) {
        const unsigned char *bytes;
        size_t n = osoe_wire_peek(w, &bytes);
        if (n == 0)
            break;
        osoe_am_parse(parser, bytes, n, &sink);
        osoe_wire_consume(w, n);
        moved = true;
    }
    /* Ending anyway, the job has nothing to fear from a peer that is ending
     * too: its connection failing, or its stream ending without a goodbye. Nor
     * from one whose abortive goodbye has arrived, before this node's agent has
     * begun its own: that peer's exit may reset the connection behind the
     * goodbye, and this node is about to end with the status it carried. With
     * no helpers, on either of the peer's wires (the adversarial read of Part
     * 2, N2): its abort read on one excuses the other's reset - in one pass,
     * its control wire's abort the data wire's reset, the pass taking the
     * control wires first; the converse needs a control wire reset, and so its
     * receive window full, which nothing that rides it can fill (the second
     * read, N1). With helpers the other wire is another owner's, whose reading
     * of the abort declared the node's (abort_arrived, progress-host.c): so a
     * wire is excused while the node is ending, by any cause, too - before this
     * owner has begun its own abort, which it does at its next turn. */
    if ((w->error || osoe_wire_ended(w)) &&
        (o->aborting || *peer_aborted || (other_aborted && *other_aborted) ||
         node_aborting(o->engine))) {
        *abort_done = *abort_done || w->error || !*peer_goodbye;
        return moved;
    }
    /*
     * What only the owner of the wire can tell (am.h): that the stream ended,
     * and whether it ended where a segment did and after the peer's goodbye. A
     * crashed lead's kernel closes its sockets, so without the goodbye an end
     * of stream is a peer that died, and the job ends here naming it rather
     * than waiting for something that node will never send (§3.15, §6.4).
     */
    if (w->error == -ETIMEDOUT)
        osoe_fatal("node %u: node %u stopped answering: its kernel acknowledged neither "
                   "keepalive probes nor retransmissions (§3.15). The host has vanished, or "
                   "the network between is down", node, peer);
    if (w->error)
        osoe_fatal("node %u: the %s to node %u failed: %s", node, name, peer,
                   strerror(-w->error));
    if (osoe_wire_ended(w)) {
        if (!osoe_am_parser_between_segments(parser))
            osoe_fatal("node %u: node %u's stream ended in the middle of a segment: its "
                       "lead has exited or died mid-send", node, peer);
        if (!*peer_goodbye && w->send_error)
            osoe_fatal("node %u: node %u reset its %s without its goodbye (%s): its lead has "
                       "exited or died (§6.4)", node, peer, name, strerror(-w->send_error));
        if (!*peer_goodbye)
            osoe_fatal("node %u: node %u ended its %s without its goodbye: its lead has "
                       "exited or died (§6.4)", node, peer, name);
    }
    return moved;
}

/* A data wire, owner `o`'s wire `i`. With no helpers owner 0 holds the peer's
 * control wire too, by the same number (owner-map.h). */
static bool receive(struct osoe_progress_owner *o, uint32_t i)
{
    struct arrival a = { o, i, false };
    struct osoe_progress_connection *c = &o->connection[i];
    const bool *other = o->engine->helpers == 0 && i < o->controls
                            ? &o->control[i].peer_aborted : NULL;
    return receive_wire(o, &o->wire[i], &c->parser, &a, &c->peer_goodbye, &c->peer_aborted,
                        other, &c->abort_done, peer_of(o, i), "connection");
}

/* A control wire, owner 0's, by peer slot; with no helpers its peer's data wire
 * is owner 0's by the same number. */
static bool receive_control(struct osoe_progress_owner *o, uint32_t slot)
{
    struct arrival a = { o, slot, true };
    struct osoe_progress_control *c = &o->control[slot];
    const bool *other = o->engine->helpers == 0 && slot < o->count
                            ? &o->connection[slot].peer_aborted : NULL;
    return receive_wire(o, &o->control_wire[slot], &c->parser, &a, &c->peer_goodbye,
                        &c->peer_aborted, other, &c->abort_done,
                        control_peer(o->engine, slot), "control wire");
}

/* ---- Draining replies into the open buffer ---------------------------- */

/*
 * Values first and to empty, then bulk round-robin (§3.8). An AMO's result is a
 * value, small, which a thread is blocked on; the value queue drains to empty,
 * so it cannot starve the bulk queue in practice, while the reverse would
 * happen constantly. The control plane's messages, which once led this queue
 * for the barrier's sake, have a wire of their own (flush_control).
 */
/*
 * Where drained replies go (§3.8). Either the open buffer, reserved against the
 * application's writers through the ordinary reservation - the opportunistic
 * drain, which keeps reply latency low when there is room - or a released
 * buffer that is still sealed and so private to this thread, just before it is
 * opened: the pre-open drain, which is what guarantees a reply room however
 * hard the writers write. The second is capped at half the buffer, so replies
 * cannot starve the writers either (§3.8, round one's D8).
 */
struct drain_target {
    uint32_t private_buffer;        /* OSOE_CONNECTION_NONE: the open buffer */
    uint32_t budget;                /* bytes still allowed, for a private buffer */
};

static bool reserve_for_reply(struct osoe_connection *connection, struct drain_target *t,
                              uint32_t len, struct osoe_reservation *r)
{
    if (t->private_buffer == OSOE_CONNECTION_NONE)
        return osoe_connection_reserve(connection, len, r) == OSOE_RESERVATION_OK;
    if (len > t->budget || !osoe_connection_drain_reserve(connection, t->private_buffer, len, r))
        return false;
    t->budget -= len;
    return true;
}

static bool drain_into(struct osoe_progress_owner *o, uint32_t i,
                       struct osoe_connection *connection, struct drain_target *t)
{
    struct osoe_progress *p = o->engine;
    struct osoe_progress_connection *c = &o->connection[i];
    bool moved = false;

    while (osoe_reply_peek(&c->queues.values) != OSOE_REPLY_NONE) {
        uint32_t d = osoe_reply_peek(&c->queues.values);
        struct osoe_reply *r = osoe_reply_at(&o->pool, d);
        struct osoe_reservation resv;
        const uint32_t fixed_bytes = (uint32_t)sizeof(struct osoe_am_rmw_reply);
        if (!reserve_for_reply(connection, t, osoe_message_padded(fixed_bytes), &resv))
            return moved;                   /* no room: try again next pass */
        struct osoe_message_header h = { OSOE_AM_RMW_REPLY, 0, fixed_bytes };
        struct osoe_am_rmw_reply m = { .origin_pe = r->origin_pe, .token = r->token,
                                       .value = r->value };
        unsigned char *at = osoe_reservation_bytes(connection, &resv);
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, &m, sizeof m);
        osoe_connection_commit(connection, &resv);
        osoe_reply_give(&o->pool, osoe_reply_pop(&o->pool, &c->queues.values));
        moved = true;
    }

    /* One chunk per descriptor per pass: strict FIFO would let a large reply
     * block the small ones behind it, and replies to different PEs have no
     * mutual ordering requirement, so rotation is legal (§3.8). */
    uint32_t rotations = c->queues.bulk.depth;
    while (rotations-- > 0 && osoe_reply_peek(&c->queues.bulk) != OSOE_REPLY_NONE) {
        uint32_t d = osoe_reply_peek(&c->queues.bulk);
        struct osoe_reply *r = osoe_reply_at(&o->pool, d);
        uint32_t remaining = r->length - r->sent;
        uint32_t chunk = remaining;
        if (chunk > p->area->chunk_bytes)
            chunk = (uint32_t)p->area->chunk_bytes;
        uint32_t len = osoe_message_padded((uint32_t)sizeof(struct osoe_am_get_reply) + chunk);
        if (len > osoe_connection_payload_capacity(connection)) {
            /* The buffer cannot hold this chunk even empty: shrink it to what
             * fits rather than deadlocking on a message that never fits. */
            uint32_t room = osoe_connection_payload_capacity(connection) -
                            (uint32_t)sizeof(struct osoe_message_header) -
                            (uint32_t)sizeof(struct osoe_am_get_reply);
            chunk = room & ~UINT32_C(7);
            len = osoe_message_padded((uint32_t)sizeof(struct osoe_am_get_reply) + chunk);
        }
        struct osoe_reservation resv;
        if (!reserve_for_reply(connection, t, len, &resv))
            return moved;
        const void *source = p->host.resolve(p->host.context, r->target_pe, r->region,
                                             r->offset + r->sent, chunk);
        struct osoe_message_header h = { OSOE_AM_GET_REPLY, 0,
                                         (uint32_t)sizeof(struct osoe_am_get_reply) + chunk };
        struct osoe_am_get_reply m = { .origin_pe = r->origin_pe,
                                       .chunk_offset = r->sent, .total = r->length,
                                       .token = r->token };
        unsigned char *at = osoe_reservation_bytes(connection, &resv);
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, &m, sizeof m);
        if (source)
            memcpy(at + sizeof h + sizeof m, source, chunk);
        else
            memset(at + sizeof h + sizeof m, 0, chunk);
        osoe_connection_commit(connection, &resv);
        moved = true;

        r->sent += chunk;
        if (r->sent == r->length)              /* pop only when it owes nothing */
            osoe_reply_give(&o->pool, osoe_reply_pop(&o->pool, &c->queues.bulk));
        else
            osoe_reply_rotate(&o->pool, &c->queues.bulk);
    }
    return moved;
}

/* The opportunistic drain, into the open buffer. */
static bool drain(struct osoe_progress_owner *o, uint32_t i)
{
    struct osoe_progress_connection *c = &o->connection[i];
    struct osoe_connection *connection = connection_of(o, i);
    if (!connection || c->open == OSOE_CONNECTION_NONE)
        return false;
    struct drain_target open = { OSOE_CONNECTION_NONE, 0 };
    return drain_into(o, i, connection, &open);
}

/* ---- Sealing and sending ---------------------------------------------- */

/*
 * Fill in the segment header at seal time (§3.4): the sequence number, the
 * acknowledgement of the reverse direction, the flags, and the two deliberately
 * redundant fields the receiver cross-checks. The message count can only be had
 * by walking the buffer - the writers are in other processes, so the progress
 * thread never saw their reservations.
 */
/*
 * Does a segment finalized now carry ACK_REQUESTED (§3.4)?
 *
 * If its sequence is past what the peer has acknowledged and at or below what
 * this node's writers want acknowledged - or, the late request, if what they
 * want is still unacknowledged and no segment this node has sent yet asked for
 * it. The late case is what a request made after its segment was finalized
 * needs: the header-only segment flush() seals for it has a sequence past
 * `wanted`, and its acknowledgement is cumulative, so it covers `wanted` too.
 * (Given `seq > acked`, `seq <= wanted` implies the late condition, since
 * segments are finalized in order; it is written out because it is the rule.)
 *
 * What never asks: a header-only segment sealed only because the peer asked.
 * When nothing is wanted, both conditions are false. That is the whole fix for
 * the echo - an answer that is itself a request - and ack_model.py's
 * QUIESCES is the check that it holds.
 *
 * One rule for both kinds of wire: a data connection's numbers, or a control
 * wire's (seal_control), whose only asker is the deadlock detector.
 */
static bool asks_for_ack(uint64_t acked, uint64_t flagged, uint64_t seq, uint64_t wanted)
{
    if (seq <= acked)
        return false;
    bool late = wanted > acked && flagged < wanted;
    return seq <= wanted || late;
}

/* What is wanted acknowledged is not, and no segment has asked for it yet: a
 * request still to carry (asks_for_ack's late case). */
static bool request_owed(uint64_t acked, uint64_t flagged, uint64_t wanted)
{
    return wanted > acked && flagged < wanted;
}

static void finalize(struct osoe_progress_owner *o, uint32_t i,
                     struct osoe_connection *connection, uint32_t k, uint32_t bytes,
                     uint64_t wanted)
{
    struct osoe_progress_connection *c = &o->connection[i];
    unsigned char *base = connection->data + (size_t)k * connection->capacity;
    uint32_t count = 0, application = 0;
    for (uint32_t off = (uint32_t)sizeof(struct osoe_am_segment_header); off < bytes; ) {
        struct osoe_message_header h;
        memcpy(&h, base + off, sizeof h);
        uint32_t fixed = 0;
        uint32_t table_count;
        const struct osoe_am_descriptor *t = osoe_am_table(&table_count);
        if (h.kind >= table_count)
            osoe_fatal("sealing a segment carrying message kind %u", h.kind);
        fixed = t[h.kind].fixed_bytes;
        (void)fixed;
        off += osoe_message_padded(h.len);
        count++;
        if (h.kind != OSOE_AM_DEADLOCK)
            application++;
    }
    uint64_t seq = atomic_load_explicit(&connection->s->buffer[k].seq, memory_order_acquire);
    if (application > 0)
        c->last_application_seq = seq;          /* the deadlock detector's (§3.15) */
    /* The goodbye is sealed last and alone (flush), so the sealed buffer is it
     * once one has been sealed; it asks nothing, since no answer could follow. */
    bool goodbye = c->goodbye == OSOE_GOODBYE_SEALED;
    bool ask = !goodbye && asks_for_ack(c->acked, c->flagged, seq, wanted);
    struct osoe_am_segment_header sh = {
        .seq = seq,
        .ack = c->applied,
        .flags = (ask ? OSOE_AM_SEGMENT_ACK_REQUESTED : 0) |
                 (goodbye ? OSOE_AM_SEGMENT_GOODBYE : 0),
        .message_count = count,
        .total_len = bytes,
    };
    memcpy(base, &sh, sizeof sh);
    if (ask)
        c->flagged = seq;
    c->owe_ack = false;         /* this segment's `ack` pays any debt */
}

/*
 * The abort's visit (osoe_progress_abort): finish the segment on the wire, then
 * send the abortive goodbye and end the stream. Returns whether anything moved.
 */
static bool flush_aborting(struct osoe_progress_owner *o, uint32_t i,
                           struct osoe_connection *connection)
{
    struct osoe_progress_connection *c = &o->connection[i];
    struct osoe_wire *w = &o->wire[i];
    if (c->abort_done)
        return false;
    if (w->error || w->send_error || c->peer_aborted || c->goodbye == OSOE_GOODBYE_SENT ||
        (osoe_wire_ended(w) && !c->peer_goodbye)) {
        c->abort_done = true;           /* nobody to tell */
        return true;
    }
    bool moved = false;
    if (c->sending != OSOE_CONNECTION_NONE) {
        /* A segment half on the wire is finished, so that the peer's stream
         * stays whole: the abortive goodbye is read after it, not inside it. */
        unsigned char *base = connection->data + (size_t)c->sending * connection->capacity;
        size_t n = osoe_wire_send(w, base + c->sent, c->sending_bytes - c->sent);
        c->sent += (uint32_t)n;
        if (c->sent < c->sending_bytes)
            return n > 0;
        c->sending = OSOE_CONNECTION_NONE;
        if (c->goodbye == OSOE_GOODBYE_SEALED) {
            osoe_wire_shutdown(w);      /* that was its normal goodbye */
            c->goodbye = OSOE_GOODBYE_SENT;
            c->abort_done = true;
            return true;
        }
        moved = true;
    }
    size_t n = osoe_wire_send(w, o->abort_segment + c->abort_sent,
                              o->abort_segment_bytes - c->abort_sent);
    c->abort_sent += (uint32_t)n;
    if (c->abort_sent == o->abort_segment_bytes) {
        if (!o->abort_keeps_stream)
            osoe_wire_shutdown(w);      /* a global exit's ends with the process */
        c->abort_done = true;
        return true;
    }
    return moved || n > 0;
}

static bool flush(struct osoe_progress_owner *o, uint32_t i)
{
    struct osoe_progress_connection *c = &o->connection[i];
    struct osoe_connection *connection = connection_of(o, i);
    struct osoe_wire *w = &o->wire[i];
    if (!connection)
        return false;
    if (o->aborting)
        return flush_aborting(o, i, connection);
    /* A peer whose abortive goodbye has arrived is ending: nothing more of the
     * job's goes to it, and its exit may already have reset the connection,
     * which a send would take for a failure - a fatal on this thread, ending
     * this node before its agent could tell its other peers. */
    if (c->peer_aborted)
        return false;
    /* Nor to a peer that has reset the connection, which reads nothing more:
     * whether it was ending or died, its stream's end says (receive). */
    if (w->send_error)
        return false;
    bool moved = false;

    /*
     * What this node's writers want acknowledged (§3.4), read once a pass and
     * before the seal below, as the flag it decides is. A request raised after
     * this load is seen by the next pass; the late-request rule in
     * asks_for_ack is what makes that safe.
     */
    uint64_t wanted = osoe_connection_ack_wanted(connection);
    if (c->detector_wanted > wanted)
        wanted = c->detector_wanted;            /* the deadlock detector asks too (§3.15) */
    c->wanted = wanted;                         /* for busy(): a late request still to seal */
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    struct osoe_am_connection_metrics *cm = conn_metrics(o, i);
    if (cm) {
        /* The three sequences a stuck connection is read by (§3.14): what the
         * peer has acknowledged, what this node's writers are waiting to have
         * acknowledged, and how far this node has sent. */
        atomic_store_explicit(&cm->acked_through, c->acked, memory_order_release);
        atomic_store_explicit(&cm->wanted_through, wanted, memory_order_release);
        atomic_store_explicit(&cm->sent_through, c->next_seq ? c->next_seq - 1 : 0,
                              memory_order_release);
        publish_queues(o, i);
    }

    /*
     * Release a buffer lent to the kernel once it has let go of it: sent whole
     * by reference, and every notification of its sends come (the zero-copy
     * send, wire.h). The same release as a copied buffer's, at its last
     * notification instead of its last send's completion - reservation_model.py's
     * zero_copy, with the release at the completion a breakage that fails.
     */
    for (uint32_t k = 0; k < 2; k++)
        if (c->state[k] == OSOE_BUFFER_LENT && osoe_wire_owed(w, k) == 0) {
            osoe_connection_release(connection, k);
            c->state[k] = OSOE_BUFFER_FREE;
            moved = true;
        }

    /* Finish an in-flight send. A short write is the normal case (§3.3): keep
     * the remainder and offer it again. */
    if (c->sending != OSOE_CONNECTION_NONE) {
        unsigned char *base = connection->data + (size_t)c->sending * connection->capacity;
        size_t n = osoe_wire_send_lent(w, base + c->sent, c->sending_bytes - c->sent,
                                       c->sending);
        c->sent += (uint32_t)n;
        if (n > 0)
            moved = true;
        if (c->sent == c->sending_bytes) {
            /* The wire is free at once; the buffer only when the kernel owes
             * nothing for it - at once, unless it went by reference. */
            if (osoe_wire_owed(w, c->sending) == 0) {
                osoe_connection_release(connection, c->sending);
                c->state[c->sending] = OSOE_BUFFER_FREE;
            } else {
                /* Never both: the wire sends by reference only while the other
                 * buffer owes nothing (wire.h; reservation_model.py, BOTH LENT). */
                if (c->state[c->sending ^ 1] == OSOE_BUFFER_LENT)
                    osoe_fatal("node %u: both buffers toward node %u are lent to the "
                               "kernel (wire.h)", o->engine->node, peer_of(o, i));
                c->state[c->sending] = OSOE_BUFFER_LENT;
            }
            c->sending = OSOE_CONNECTION_NONE;
            /* That was the goodbye (nothing is sealed after it): end the stream. */
            if (c->goodbye == OSOE_GOODBYE_SEALED && c->sealed == OSOE_CONNECTION_NONE) {
                osoe_wire_shutdown(w);
                c->goodbye = OSOE_GOODBYE_SENT;
            }
            osoe_counter_word_add(&o->metrics->segments_sent, 1);
            if (cm) {
                osoe_counter_word_add(&cm->segments_sent, 1);
                osoe_counter_word_add(&cm->bytes_sent, c->sending_bytes);
            }
            moved = true;
        }
    }

    /* Promote a sealed buffer once every writer in it has committed. Never
     * spin waiting: a writer may be mid-copy, and at 256 PEs a spin here is a
     * hang (§3.5). */
    if (c->sending == OSOE_CONNECTION_NONE && c->sealed != OSOE_CONNECTION_NONE) {
        uint32_t bytes;
        if (osoe_connection_sendable(connection, c->sealed, &bytes)) {
            finalize(o, i, connection, c->sealed, bytes, wanted);
            c->sending = c->sealed;
            c->sealed = OSOE_CONNECTION_NONE;
            c->state[c->sending] = OSOE_BUFFER_SENDING;
            c->sent = 0;
            c->sending_bytes = bytes;
            moved = true;
        }
    }

    /*
     * Seal the open buffer when there is something to send. §3.9 self-clocks
     * off the send: with nothing in flight this happens at once, so an
     * operation on an idle connection goes out as a batch of one; with a send
     * outstanding the buffer keeps accumulating, and the batch grows under
     * load. Nothing waits for a buffer to fill.
     */
    if (c->sealed == OSOE_CONNECTION_NONE && c->open != OSOE_CONNECTION_NONE &&
        c->goodbye == OSOE_GOODBYE_NONE) {
        uint32_t used = OSOE_BUFFER_USED(
            atomic_load_explicit(&connection->s->buffer[c->open].head, memory_order_acquire));
        /* Seal for data, to pay a debt, or for a late request that no
         * unfinalized segment will carry - the one header-only segment the
         * model says a quiesced pair may still exchange. */
        bool late = wanted > c->acked && c->flagged < wanted;
        /* Or, closing, for the goodbye: once the connection owes the peer
         * nothing - nothing sealed or on the wire, no replies queued, nothing in
         * the open buffer - and never before (close_model.py, goodbye_early). */
        bool goodbye = o->closing && c->sending == OSOE_CONNECTION_NONE && used == header &&
                       !c->carrying && c->queues.values.depth == 0 &&
                       c->queues.bulk.depth == 0;
        if (goodbye)
            c->goodbye = OSOE_GOODBYE_SEALED;
        if (used > header || c->owe_ack || late || goodbye) {
            if (used == header) {
                osoe_counter_word_add(&o->metrics->header_only_sent, 1);   /* §3.4's ack-only */
                if (cm)
                    osoe_counter_word_add(&cm->header_only_sent, 1);
            }
            osoe_connection_seal(connection, c->open);
            c->carrying = false;
            c->state[c->open] = OSOE_BUFFER_SEALED_STATE;
            c->sealed = c->open;
            c->open = OSOE_CONNECTION_NONE;
            moved = true;
        }
    }

    /* At most one reservable buffer (§3.5): open a free one only when none is
     * open, and never the one being sealed or sent. */
    if (c->open == OSOE_CONNECTION_NONE && c->goodbye == OSOE_GOODBYE_NONE) {
        for (uint32_t k = 0; k < 2; k++)
            if (c->state[k] == OSOE_BUFFER_FREE && k != c->sealed && k != c->sending) {
                /* Nothing writes a buffer the kernel may still read: a free one
                 * owes no notification (the zero-copy send). Checked here, where
                 * the buffer is first written, and not where it was released, so
                 * that a release too early is caught by another hand. */
                if (osoe_wire_owed(w, k) != 0)
                    osoe_fatal("node %u: buffer %u toward node %u would be written with %u "
                               "zero-copy notifications still owed - the kernel may still "
                               "be reading it (wire.h)", o->engine->node, k, peer_of(o, i),
                               osoe_wire_owed(w, k));
                /* The pre-open drain (§3.8): replies first, while k is still
                 * sealed and no writer can reach it. */
                c->state[k] = OSOE_BUFFER_DRAINING;
                struct drain_target preopen = { k, connection->capacity / 2 };
                if (drain_into(o, i, connection, &preopen))
                    c->carrying = true;
                osoe_connection_open(connection, k, c->next_seq++);
                c->state[k] = OSOE_BUFFER_OPEN;
                c->open = k;
                moved = true;
                break;
            }
    }
    return moved;
}

/* ---- The control wire (the progress helpers proposal's §3.1) ------------ */

/*
 * A control message from its descriptor, at `at`; its length, padded. The two
 * collective messages are the same payload in both directions (§5.3) - a team,
 * an epoch, and which node it came from - and a detector message carries its
 * round, kind, phase and verdict (§3.15).
 */
static uint32_t control_message(const struct osoe_progress *p, const struct osoe_reply *r,
                                unsigned char *at)
{
    if (r->kind == OSOE_REPLY_DEADLOCK) {
        struct osoe_am_deadlock m = { .round = (uint32_t)r->value,
                                      .kind = (uint16_t)(r->value >> 32),
                                      .phase = (uint16_t)(r->value >> 48),
                                      .ok = (uint32_t)r->token, .from_node = p->node };
        struct osoe_message_header h = { OSOE_AM_DEADLOCK, 0, (uint32_t)sizeof m };
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, &m, sizeof m);
        return osoe_message_padded(h.len);
    }
    struct osoe_am_barrier m = { .team = r->origin_pe, .epoch = (uint32_t)r->value,
                                 .from_node = p->node, .lead = r->target_pe };
    struct osoe_message_header h = {
        r->kind == OSOE_REPLY_RELEASE ? OSOE_AM_BARRIER_RELEASE : OSOE_AM_BARRIER_ARRIVE, 0,
        (uint32_t)sizeof m };
    memcpy(at, &h, sizeof h);
    memcpy(at + sizeof h, &m, sizeof m);
    return osoe_message_padded(h.len);
}

/*
 * Seal control wire `slot`'s next segment into its private buffer: as many of
 * its queued messages as fit; or none, for an acknowledgement owed or a
 * request still to carry (§3.4); or - closing, the queue empty - its goodbye,
 * header-only, the last it sends (close_model.py's two_wires). Its `ack` pays
 * whatever acknowledgement is owed, and it asks for one by a data wire's rule
 * (asks_for_ack), the goodbye never, since no answer could follow.
 */
static void seal_control(struct osoe_progress_owner *o, uint32_t slot)
{
    struct osoe_progress *p = o->engine;
    struct osoe_progress_control *c = &o->control[slot];
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    uint32_t used = header, count = 0, application = 0;
    /* Decided by the queue, not by an empty segment: one sealed only to
     * acknowledge is empty too (the adversarial read of Part 2, N3). */
    bool goodbye = o->closing && osoe_reply_peek(&c->queue) == OSOE_REPLY_NONE;
    while (osoe_reply_peek(&c->queue) != OSOE_REPLY_NONE) {
        struct osoe_reply *r = osoe_reply_at(&o->pool, osoe_reply_peek(&c->queue));
        uint32_t len = osoe_message_padded(r->kind == OSOE_REPLY_DEADLOCK
                                               ? (uint32_t)sizeof(struct osoe_am_deadlock)
                                               : (uint32_t)sizeof(struct osoe_am_barrier));
        if (used + len > OSOE_PROGRESS_CONTROL_BYTES)
            break;
        used += control_message(p, r, c->buffer + used);
        if (r->kind == OSOE_REPLY_RELEASE)
            release_left_queue(p, r->token);
        /* The tree's two are application work; the detector's are not (§3.15). */
        if (r->kind != OSOE_REPLY_DEADLOCK)
            application++;
        osoe_reply_give(&o->pool, osoe_reply_pop(&o->pool, &c->queue));
        count++;
    }
    publish_control_queue(p, slot);
    uint64_t seq = c->next_seq++;
    if (application > 0)
        c->last_application_seq = seq;          /* the deadlock detector's (§3.15) */
    if (goodbye)
        c->goodbye = OSOE_GOODBYE_SEALED;
    bool ask = !goodbye && asks_for_ack(c->acked, c->flagged, seq, c->detector_wanted);
    struct osoe_am_segment_header sh = {
        .seq = seq,
        .ack = c->applied,
        .flags = (ask ? OSOE_AM_SEGMENT_ACK_REQUESTED : 0) |
                 (goodbye ? OSOE_AM_SEGMENT_GOODBYE : 0),
        .message_count = count,
        .total_len = used,
    };
    memcpy(c->buffer, &sh, sizeof sh);
    if (ask)
        c->flagged = seq;
    c->owe_ack = false;                 /* this segment's `ack` pays any debt */
    c->bytes = used;
    c->sent = 0;
}

/*
 * The abort's visit to a control wire (osoe_progress_abort): finish the segment
 * on the wire, so the peer's stream stays whole, then send the abortive
 * goodbye and end the stream - unless it has nobody to tell, as a data wire's
 * (flush_aborting).
 */
static bool flush_control_aborting(struct osoe_progress_owner *o, uint32_t slot)
{
    struct osoe_progress_control *c = &o->control[slot];
    struct osoe_wire *w = &o->control_wire[slot];
    if (c->abort_done)
        return false;
    if (w->error || w->send_error || c->peer_aborted || c->goodbye == OSOE_GOODBYE_SENT ||
        (osoe_wire_ended(w) && !c->peer_goodbye)) {
        c->abort_done = true;           /* nobody to tell */
        return true;
    }
    bool moved = false;
    if (c->bytes) {
        size_t n = osoe_wire_send(w, c->buffer + c->sent, c->bytes - c->sent);
        c->sent += (uint32_t)n;
        if (c->sent < c->bytes)
            return n > 0;
        c->bytes = 0;
        if (c->goodbye == OSOE_GOODBYE_SEALED) {
            osoe_wire_shutdown(w);      /* that was its normal goodbye */
            c->goodbye = OSOE_GOODBYE_SENT;
            c->abort_done = true;
            return true;
        }
        moved = true;
    }
    size_t n = osoe_wire_send(w, o->abort_segment + c->abort_sent,
                              o->abort_segment_bytes - c->abort_sent);
    c->abort_sent += (uint32_t)n;
    if (c->abort_sent == o->abort_segment_bytes) {
        if (!o->abort_keeps_stream)
            osoe_wire_shutdown(w);      /* a global exit's ends with the process */
        c->abort_done = true;
        return true;
    }
    return moved || n > 0;
}

/*
 * Control wire `slot`'s send side, on owner 0's pass: finish the segment on the
 * wire - a short write is the normal case (§3.3) - and once it has gone whole,
 * seal the next from the queue, so that what queues behind a send goes out
 * together when it completes (§3.9's self-clocking, here a buffer's worth), or
 * one for an acknowledgement owed or asked for. Its goodbye, closing, once the
 * queue is empty and nothing is on the wire; its end of stream once the goodbye
 * has gone.
 */
static bool flush_control(struct osoe_progress_owner *o, uint32_t slot)
{
    struct osoe_progress_control *c = &o->control[slot];
    struct osoe_wire *w = &o->control_wire[slot];
    if (o->aborting)
        return flush_control_aborting(o, slot);
    /* A peer ending, or one that reset the wire, is told nothing more (flush). */
    if (c->peer_aborted || w->send_error)
        return false;
    bool moved = false;
    if (c->bytes) {
        size_t n = osoe_wire_send(w, c->buffer + c->sent, c->bytes - c->sent);
        c->sent += (uint32_t)n;
        if (n > 0)
            moved = true;
        if (c->sent < c->bytes)
            return moved;
        c->bytes = 0;
        osoe_counter_word_add(&o->metrics->control_segments_sent, 1);
        if (c->goodbye == OSOE_GOODBYE_SEALED) {
            osoe_wire_shutdown(w);
            c->goodbye = OSOE_GOODBYE_SENT;
            return true;
        }
        moved = true;
    }
    if (c->goodbye != OSOE_GOODBYE_NONE)
        return moved;
    if (c->queue.depth != 0 || o->closing || c->owe_ack ||
        request_owed(c->acked, c->flagged, c->detector_wanted)) {
        seal_control(o, slot);
        /* The first offer starts the send (wire.h). */
        c->sent += (uint32_t)osoe_wire_send(w, c->buffer, c->bytes);
        moved = true;
    }
    return moved;
}

/* Does control wire `slot` have work its owner already knows about: a segment
 * on the wire, messages queued, an acknowledgement owed or a request to carry,
 * or - closing - its goodbye to send; once its goodbye is sealed, only that
 * still on the wire; aborting, its abortive goodbye. */
static bool control_busy(const struct osoe_progress_owner *o, uint32_t slot)
{
    const struct osoe_progress_control *c = &o->control[slot];
    if (o->aborting)
        return !c->abort_done;
    if (c->goodbye != OSOE_GOODBYE_NONE)
        return c->bytes != 0;
    return c->bytes != 0 || c->queue.depth != 0 || c->owe_ack || o->closing ||
           request_owed(c->acked, c->flagged, c->detector_wanted);
}

static void settle_control(struct osoe_progress_owner *o, uint32_t slot)
{
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (control_busy(o, slot))
        o->control_busy[slot / 64] |= bit;
    else
        o->control_busy[slot / 64] &= ~bit;
}

/* ---- The pass --------------------------------------------------------- */

void osoe_progress_init(struct osoe_progress *p, struct osoe_connection_area *area,
                        struct osoe_progress_owner *owners,
                        const struct osoe_progress_host *host)
{
    memset(p, 0, sizeof *p);
    p->area = area;
    p->node = area->node;
    p->nodes = area->nodes;
    p->helpers = area->helpers;
    p->owner = owners;
    p->host = *host;
    p->team = p->own_team;
    p->pending = p->own_pending;
    p->team_count = OSOE_PREDEFINED_TEAMS;
    for (uint32_t o = 0; o <= p->helpers; o++) {
        memset(&owners[o], 0, sizeof owners[o]);
        owners[o].engine = p;
        owners[o].index = o;
        owners[o].metrics = &owners[o].own_metrics;
    }
}

void osoe_progress_attach_teams(struct osoe_progress *p, struct osoe_progress_team *teams,
                                uint32_t *pending, uint32_t count,
                                osoe_progress_team_source *source,
                                osoe_progress_team_finder *find)
{
    memset(teams, 0, sizeof *teams * count);
    p->team = teams;
    p->pending = pending;
    p->team_count = count;
    p->pending_count = 0;
    p->team_source = source;
    p->team_find = find;
}

void osoe_progress_attach(struct osoe_progress *p, uint32_t owner, struct osoe_ring *ring,
                          struct osoe_wire *wires, struct osoe_progress_connection *connections,
                          uint64_t *busy, struct osoe_reply *descriptors,
                          uint32_t descriptor_count)
{
    struct osoe_progress_owner *o = &p->owner[owner];
    o->count = osoe_owner_wires(owner, p->nodes - 1, p->helpers);
    o->ring = ring;
    o->wire = wires;
    o->connection = connections;
    o->busy = busy;
    osoe_reply_pool_init(&o->pool, descriptors, descriptor_count);
    /* Every connection starts busy - it has no buffer open yet - so the first
     * pass visits each once (O(N), once), and after that only what is active. */
    memset(busy, 0, (size_t)((o->count + 63) / 64) * sizeof *busy);
    for (uint32_t i = 0; i < o->count; i++)
        mark_busy(o, i);
    for (uint32_t i = 0; i < o->count; i++) {
        struct osoe_progress_connection *c = &o->connection[i];
        memset(c, 0, sizeof *c);
        c->open = c->sealed = c->sending = OSOE_CONNECTION_NONE;
        c->next_seq = 1;
        c->state[0] = c->state[1] = OSOE_BUFFER_FREE;
        osoe_am_parser_init(&c->parser, peer_of(o, i));
        osoe_reply_queues_init(&c->queues);
    }
}

void osoe_progress_attach_control(struct osoe_progress *p, struct osoe_wire *wires,
                                  struct osoe_progress_control *controls, uint64_t *busy)
{
    struct osoe_progress_owner *o = &p->owner[0];
    o->controls = p->nodes - 1;
    o->control_wire = wires;
    o->control = controls;
    o->control_busy = busy;
    memset(busy, 0, (size_t)((o->controls + 63) / 64) * sizeof *busy);
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        struct osoe_progress_control *c = &o->control[slot];
        memset(c, 0, sizeof *c);
        c->next_seq = 1;
        osoe_am_parser_init(&c->parser, control_peer(p, slot));
        osoe_reply_queue_init(&c->queue);
    }
}

/*
 * Does this connection have send-side work the lead already knows about,
 * without a writer saying so? A buffer to open, one sealed or on the wire,
 * replies queued, an acknowledgement owed, or replies the pre-open drain put in
 * the open buffer. The last was missed at first: the pre-open drain runs after
 * the pass's chance to seal, and the first barrier of an 8-node job hung about
 * one run in eight with a leaf's ARRIVE sitting unsealed in its open buffer.
 * The rule since: **whatever wants a seal keeps the connection busy**, since the
 * open step runs after the seal step. So too a buffer lent to the kernel (the
 * zero-copy send), a late acknowledgement request not yet carried, and, closing,
 * the goodbye not yet sealed: with buffers lent, both can come back in one visit,
 * which then opens one after the seal step has passed, and a quiet waited for
 * the deadlock detector's sweep to bring the connection back (the fourth
 * adversarial read of v0.2 step 4, F1; visit_model.py). Private state only:
 * O(1), and no shared line touched.
 */
static bool busy(const struct osoe_progress_owner *o, uint32_t i)
{
    const struct osoe_progress_connection *c = &o->connection[i];
    if (o->aborting)
        return !c->abort_done;
    return c->open == OSOE_CONNECTION_NONE || c->sending != OSOE_CONNECTION_NONE ||
           c->sealed != OSOE_CONNECTION_NONE || c->owe_ack || c->carrying ||
           c->queues.values.depth != 0 || c->queues.bulk.depth != 0 ||
           c->state[0] == OSOE_BUFFER_LENT || c->state[1] == OSOE_BUFFER_LENT ||
           (c->wanted > c->acked && c->flagged < c->wanted) ||
           (o->closing && c->goodbye == OSOE_GOODBYE_NONE);
}

/* After a visit: keep the connection's bit while it is busy, clear it once not
 * - stored only when it changes, so that a visit that leaves it as it was
 * writes no line (the adversarial read of Part 3, S2). */
static void settle(struct osoe_progress_owner *o, uint32_t i)
{
    uint64_t bit = UINT64_C(1) << (i % 64);
    uint64_t word = o->busy[i / 64];
    uint64_t now = busy(o, i) ? word | bit : word & ~bit;
    if (now != word)
        o->busy[i / 64] = now;
}

/*
 * The releases this pass forwarded, applied to this node's own members last.
 * Each child's control wire is flushed first, so the RELEASE is sealed into
 * the wire's segment before the local wake, which is O(L) futex work on this
 * thread (§5.3). O(k) per release. A RELEASE behind a segment still on the
 * wire - a control wire has one buffer, sealed again only once its last
 * segment has gone (flush_control) - stays queued, and the local release
 * waits with it for a later pass (releases_queued): the members leave the
 * team's block only once its forwards are out of the reply pool (§3.8). Not
 * while the job aborts: the release is applied at once then, whatever is
 * still queued, since an aborting engine sends nothing more but goodbyes.
 * Owner 0's, on its pass: the tree and the control wires its forwards went on
 * are the control plane's.
 */
static bool release_local(struct osoe_progress *p)
{
    struct osoe_progress_owner *o = &p->owner[0];
    bool moved = false;
    uint32_t kept = 0;
    /* The list, not every team number: O(k) per release this pass forwarded,
     * whatever OSOE_MAX_TEAMS is. A team is listed once however many passes
     * its release waits, and taken off as it is applied. */
    for (uint32_t k = 0; k < p->pending_count; k++) {
        uint32_t team = p->pending[k];
        struct osoe_progress_team *t = &p->team[team];
        for (uint32_t j = 0; j < t->tree.children; j++) {
            /* Found by forward, which refuses a child without one; refused here
             * too, rather than an index past owner 0's control wires (the
             * adversarial read of Part 1, N4). */
            uint32_t slot = osoe_progress_slot(p, t->tree.child[j]);
            if (slot == OSOE_CONNECTION_NONE || slot >= o->controls)
                osoe_fatal("node %u: no control wire toward node %u, a child in team %u's "
                           "tree", p->node, t->tree.child[j], t->number);
            flush_control(o, slot);
            settle_control(o, slot);
        }
        if (t->releases_queued != 0 && !o->aborting) {
            p->pending[kept++] = team;
            osoe_counter_word_add(&o->metrics->releases_held, 1);
            continue;
        }
        uint32_t epoch = t->release_pending;
        t->release_pending = 0;
        p->host.release(p->host.context, team, epoch);
        moved = true;
    }
    p->pending_count = kept;
    return moved;
}

/* The ring's counts since the last pass, into the owner's record (§3.14). */
static void publish_ring(struct osoe_progress_owner *o)
{
    struct osoe_ring_counts *k = &o->ring->counts;
    if (k->completions == 0 && k->sends == 0)
        return;
    osoe_counter_word_add(&o->metrics->wakes, k->wakes);
    osoe_counter_word_add(&o->metrics->completions, k->completions);
    osoe_counter_word_add(&o->metrics->receive_buffers, k->receives);
    osoe_counter_word_add(&o->metrics->receive_enobufs, k->enobufs);
    osoe_counter_word_add(&o->metrics->receive_rearms, k->rearms);
    osoe_counter_word_add(&o->metrics->sends, k->sends);
    osoe_counter_word_add(&o->metrics->short_sends, k->short_sends);
    osoe_counter_word_add(&o->metrics->zero_copy_sends, k->zero_copy_sends);
    osoe_counter_word_add(&o->metrics->zero_copy_notifications, k->zero_copy_notifications);
    osoe_counter_word_add(&o->metrics->zero_copy_copied, k->zero_copy_copied);
    *k = (struct osoe_ring_counts){ 0 };
}

/*
 * One pass (§3.10, §3.12).
 *
 * **Both sides visit only the connections with work** (phase 1 step 3). The
 * ring's reap says which wires have received something - bytes, or the end of
 * a stream - and a pass takes all of it from those and no others: the receive
 * side found by completions, which on the step-2 wire was a look at every one
 * of N - 1 connections per pass. The send side is as slice 3 made it: the
 * connections whose dirty bit a writer set since the last pass, those that
 * received something (an acknowledgement may be owed), and those the lead
 * already knows are busy - now kept in a private bitmap as that work comes and
 * goes, rather than asked of every connection. Each bitmap is read a word at a
 * time, so a pass costs N/64 loads of each and O(active) connections visited.
 *
 * The pass ends with a submit, so that the sends and re-arms it queued go to
 * the kernel now rather than at the next reap.
 *
 * An owner's pass is all of this over its own wires (owner-map.h): its ring's
 * readable bits, its bitmap of the area's dirty bits and its own busy bits,
 * all by index. Owner 0's visits its control wires first, and runs the control
 * plane's releases besides.
 */
bool osoe_progress_poll(struct osoe_progress_owner *o)
{
    struct osoe_progress *p = o->engine;
    bool lead = o->index == 0;
    bool moved = lead && release_local(p);     /* a release osoe_progress_arrive started */
    osoe_ring_reap(o->ring);
    uint32_t visits = 0;
    /* Owner 0's control wires first - after its data wires on the ring
     * (owner-map.h), before them in the pass: a barrier's message reaped beside
     * a stream's bytes is applied before them, and what it forwards reaches the
     * kernel ahead of the pass's data sends; no ordering joins the planes (the
     * adversarial read of Part 2, N2). Those that received something, and those
     * it knows have something to send; no writer has a bit here, since no PE
     * writes a control message. */
    for (uint32_t w = 0; w * 64 < o->controls; w++) {
        uint32_t first = w * 64, here = o->controls - first < 64 ? o->controls - first : 64;
        uint64_t readable = osoe_ring_take_readable(o->ring, o->count + first, here);
        for (uint64_t visit = readable | o->control_busy[w]; visit != 0; visit &= visit - 1) {
            uint32_t bit = (uint32_t)stdc_trailing_zeros(visit);
            uint32_t slot = first + bit;
            if ((readable >> bit) & 1)
                moved |= receive_control(o, slot);
            moved |= flush_control(o, slot);
            settle_control(o, slot);
            visits++;
        }
    }
    /* Its data wires: those that received something, those a writer marked
     * dirty, and those it knows are busy. */
    _Atomic uint64_t *dirty_bits = p->area->owner ? p->area->owner[o->index].dirty : NULL;
    for (uint32_t w = 0; w * 64 < o->count; w++) {
        uint64_t dirty = 0;
        if (dirty_bits && atomic_load_explicit(&dirty_bits[w], memory_order_acquire) != 0)
            dirty = atomic_exchange_explicit(&dirty_bits[w], 0, memory_order_seq_cst);
        uint32_t first = w * 64, here = o->count - first < 64 ? o->count - first : 64;
        uint64_t readable = osoe_ring_take_readable(o->ring, first, here);
        for (uint64_t visit = dirty | readable | o->busy[w]; visit != 0; visit &= visit - 1) {
            uint32_t bit = (uint32_t)stdc_trailing_zeros(visit);
            uint32_t i = w * 64 + bit;
            if ((readable >> bit) & 1)
                moved |= receive(o, i);
            if (!o->aborting)
                moved |= drain(o, i);
            moved |= flush(o, i);
            settle(o, i);
            visits++;
        }
    }
    /* A release this pass received is forwarded before the submit, so the
     * children's RELEASE goes to the kernel now and not at the next pass - or,
     * with nothing more to do, after the idle wait. */
    if (lead)
        moved |= release_local(p);
    osoe_ring_submit(o->ring);
    publish_ring(o);
    osoe_counter_word_add(&o->metrics->passes, 1);
    osoe_counter_word_add(&o->metrics->send_visits, visits);
    return moved;
}

bool osoe_progress_idle(struct osoe_progress_owner *o)
{
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    if (o->index == 0 && o->engine->pending_count != 0)
        return false;
    for (uint32_t i = 0; i < o->count; i++) {
        struct osoe_progress_connection *c = &o->connection[i];
        if (o->wire[i].send_error)
            continue;                   /* a peer that reset the connection is owed nothing */
        if (c->sending != OSOE_CONNECTION_NONE || c->sealed != OSOE_CONNECTION_NONE)
            return false;
        if (c->queues.values.depth != 0 || c->queues.bulk.depth != 0)
            return false;
        struct osoe_connection *connection = connection_of(o, i);
        if (!connection || c->open == OSOE_CONNECTION_NONE)
            continue;
        /* Bytes a writer has committed into the open buffer that no segment
         * has carried yet. Reserved-but-uncommitted is not possible here: every
         * writer has passed shmem_finalize's barrier. */
        uint32_t used = OSOE_BUFFER_USED(
            atomic_load_explicit(&connection->s->buffer[c->open].head, memory_order_acquire));
        if (used > header)
            return false;
    }
    /* And owner 0's control wires: nothing queued, nothing on the wire. */
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        const struct osoe_progress_control *c = &o->control[slot];
        if (!o->control_wire[slot].send_error && (c->bytes != 0 || c->queue.depth != 0))
            return false;
    }
    return true;
}

/*
 * The internode phase of a collective, as the AM backend carries it. It is
 * reached through internode.h - a sibling of §4.10's boundary, addressed by
 * (team, epoch) - and not through transport.h, which stays point-to-point and
 * PE-addressed. This note once argued that the phase could not cross any
 * boundary at all, because it is addressed to nodes and its release is a futex
 * wake; the code already passed (team, epoch) down and one release call up, and
 * §10 records how that decision was reversed (2026-09-20).
 */
void osoe_progress_team(struct osoe_progress *p, uint32_t team,
                        const struct osoe_collective_tree *tree)
{
    if (team >= p->team_count)
        osoe_fatal("team %u registered an internode phase; this engine has room for %u",
                   team, p->team_count);
    p->team[team] = (struct osoe_progress_team){ .registered = true, .tree = *tree,
                                                 .number = team };
    publish_team(p, team);
}

void osoe_progress_arrive(struct osoe_progress *p, uint32_t team, uint32_t generation,
                          uint32_t epoch)
{
    if (!known_team(p, team, generation))
        osoe_fatal("node %u: the team in block %u at generation %u arrived at an internode "
                   "phase it never registered", p->node, team, generation);
    book_arrival(p, team, epoch, OSOE_COLLECTIVE_TREE_NONE);
}

void osoe_progress_close(struct osoe_progress_owner *o)
{
    o->closing = true;
    /* Every wire is visited until its goodbye has gone (O(N), once): each says
     * its own, after its own last bytes (close_model.py's two_wires). */
    for (uint32_t i = 0; i < o->count; i++)
        mark_busy(o, i);
    for (uint32_t slot = 0; slot < o->controls; slot++)
        mark_control_busy(o, slot);
}

void osoe_progress_abort(struct osoe_progress_owner *o, uint32_t pe, int32_t status,
                         uint32_t disposition, bool keep_stream)
{
    if (o->aborting)
        return;
    o->aborting = true;
    o->abort_keeps_stream = keep_stream;
    /* One segment for every peer: a header, and one message whose fixed part is
     * the abort. Its seq and ack are the ones nothing checks at the end of a
     * stream; the goodbye flag is what the peer's end of stream is judged by. */
    struct osoe_message_header h = { OSOE_AM_ABORT, 0, (uint32_t)sizeof(struct osoe_am_abort) };
    struct osoe_am_abort m = { .origin_pe = pe, .status = status, .disposition = disposition };
    uint32_t bytes = (uint32_t)sizeof(struct osoe_am_segment_header) +
                     osoe_message_padded((uint32_t)sizeof m);
    struct osoe_am_segment_header sh = { .flags = OSOE_AM_SEGMENT_GOODBYE, .message_count = 1,
                                         .total_len = bytes };
    _Static_assert(sizeof(struct osoe_am_segment_header) + sizeof(struct osoe_message_header) +
                   sizeof(struct osoe_am_abort) <= sizeof o->abort_segment,
                   "the abortive goodbye fits its buffer");
    memset(o->abort_segment, 0, sizeof o->abort_segment);
    memcpy(o->abort_segment, &sh, sizeof sh);
    memcpy(o->abort_segment + sizeof sh, &h, sizeof h);
    memcpy(o->abort_segment + sizeof sh + sizeof h, &m, sizeof m);
    o->abort_segment_bytes = bytes;
    /* Every wire is visited until its abortive goodbye has gone (O(N), once). */
    for (uint32_t i = 0; i < o->count; i++)
        mark_busy(o, i);
    for (uint32_t slot = 0; slot < o->controls; slot++)
        mark_control_busy(o, slot);
}

bool osoe_progress_aborted(const struct osoe_progress_owner *o)
{
    for (uint32_t i = 0; i < o->count; i++) {
        const struct osoe_progress_connection *c = &o->connection[i];
        if (!c->abort_done)
            return false;
        /*
         * Sent is not received. A process whose end finds input it has not read
         * resets the connection, and a reset drops whatever is still queued -
         * the goodbye among it, behind the bulk of a stream or waiting on a
         * retransmission - so a peer not yet ending would read a reset without
         * a goodbye, and end with a failure. So a goodbye counts once the peer's
         * kernel has acknowledged it, and the engine keeps receiving until then
         * (the host's grace bounds it). Not where nobody was told, nor once the
         * peer is ending too.
         */
        if (c->abort_sent == o->abort_segment_bytes && !c->peer_aborted &&
            !o->wire[i].error && !o->wire[i].send_error &&
            osoe_wire_unacknowledged(&o->wire[i]))
            return false;
    }
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        const struct osoe_progress_control *c = &o->control[slot];
        const struct osoe_wire *w = &o->control_wire[slot];
        if (!c->abort_done)
            return false;
        if (c->abort_sent == o->abort_segment_bytes && !c->peer_aborted && !w->error &&
            !w->send_error && osoe_wire_unacknowledged(w))
            return false;
    }
    return true;
}

/*
 * Whether the peer of data wire `i`, or of control wire `i` (`control`), has
 * said it ends carrying a caller other than `carrier`: on that wire, or with no
 * helpers - where this owner holds both of a peer's wires, by the same number -
 * on either. Then it is not waited for on either wire. Its abortive goodbye may
 * come on one only: each side tells a peer nothing more on a wire where the
 * peer's own has arrived, so two callers' goodbyes that cross arrive on
 * different wires, and two carriers judging per wire waited each for the other
 * to the bound (the second adversarial read of Part 2, S1; carrier_model.py).
 */
static bool peer_carries_another(const struct osoe_progress_owner *o, bool control, uint32_t i,
                                 uint32_t carrier)
{
    bool both = o->engine->helpers == 0;
    const struct osoe_progress_connection *on_data =
        !control ? &o->connection[i] : both && i < o->count ? &o->connection[i] : NULL;
    const struct osoe_progress_control *on_control =
        control ? &o->control[i] : both && i < o->controls ? &o->control[i] : NULL;
    return (on_data && on_data->peer_aborted && on_data->peer_abort_origin != carrier) ||
           (on_control && on_control->peer_aborted && on_control->peer_abort_origin != carrier);
}

bool osoe_progress_peers_ended(const struct osoe_progress_owner *o, uint32_t carrier)
{
    for (uint32_t i = 0; i < o->count; i++) {
        if (peer_carries_another(o, false, i, carrier))
            continue;
        if (!osoe_wire_ended(&o->wire[i]) && !o->wire[i].error)
            return false;
    }
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        if (peer_carries_another(o, true, slot, carrier))
            continue;
        if (!osoe_wire_ended(&o->control_wire[slot]) && !o->control_wire[slot].error)
            return false;
    }
    return true;
}

bool osoe_progress_closed(const struct osoe_progress_owner *o)
{
    for (uint32_t i = 0; i < o->count; i++) {
        const struct osoe_progress_connection *c = &o->connection[i];
        /* A connection whose sends met the peer's reset needs no goodbye of this
         * side's once the peer's own has arrived and its stream ended: the peer
         * reads nothing more - it crashed during the close (abort_model.py). */
        if ((c->goodbye != OSOE_GOODBYE_SENT && !o->wire[i].send_error) ||
            !c->peer_goodbye || !osoe_wire_ended(&o->wire[i]))
            return false;
    }
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        const struct osoe_progress_control *c = &o->control[slot];
        const struct osoe_wire *w = &o->control_wire[slot];
        if ((c->goodbye != OSOE_GOODBYE_SENT && !w->send_error) || !c->peer_goodbye ||
            !osoe_wire_ended(w))
            return false;
    }
    return true;
}

void osoe_progress_check_silent(struct osoe_progress_owner *o, uint32_t budget_ms)
{
    for (uint32_t i = 0; i < o->count; i++) {
        struct osoe_wire *w = &o->wire[i];
        if (w->error || w->send_error || osoe_wire_ended(w))
            continue;               /* already failed or ended: judged elsewhere */
        uint32_t silent = osoe_wire_silent_ms(w);
        if (silent >= budget_ms)
            osoe_fatal("node %u: node %u stopped answering: its kernel has acknowledged "
                       "nothing sent to it for %u s (§3.15). The host has vanished, or the "
                       "network between is down", o->engine->node, peer_of(o, i),
                       silent / 1000);
    }
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        struct osoe_wire *w = &o->control_wire[slot];
        if (w->error || w->send_error || osoe_wire_ended(w))
            continue;
        uint32_t silent = osoe_wire_silent_ms(w);
        if (silent >= budget_ms)
            osoe_fatal("node %u: node %u stopped answering: its kernel has acknowledged "
                       "nothing sent to it for %u s (§3.15). The host has vanished, or the "
                       "network between is down", o->engine->node,
                       control_peer(o->engine, slot), silent / 1000);
    }
}

bool osoe_progress_quiet(struct osoe_progress_owner *o, uint64_t *applied)
{
    *applied = osoe_counter_word_read(&o->application_applied);
    if (!osoe_progress_idle(o))
        return false;
    bool quiet = true;
    for (uint32_t i = 0; i < o->count; i++) {
        struct osoe_progress_connection *c = &o->connection[i];
        if (o->wire[i].send_error || c->acked >= c->last_application_seq)
            continue;                   /* a reset peer is owed nothing */
        quiet = false;
        if (c->detector_wanted < c->last_application_seq) {
            c->detector_wanted = c->last_application_seq;
            mark_busy(o, i);            /* so a pass seals the request */
        }
    }
    /* And owner 0's control wires, whose ARRIVE and RELEASE are application
     * work too (progress.h; the adversarial read of Part 2, F1). */
    for (uint32_t slot = 0; slot < o->controls; slot++) {
        struct osoe_progress_control *c = &o->control[slot];
        if (o->control_wire[slot].send_error || c->acked >= c->last_application_seq)
            continue;
        quiet = false;
        if (c->detector_wanted < c->last_application_seq) {
            c->detector_wanted = c->last_application_seq;
            mark_control_busy(o, slot);
        }
    }
    return quiet;
}

void osoe_progress_deadlock_send(struct osoe_progress *p, uint32_t node,
                                 const struct osoe_deadlock_message *m)
{
    uint32_t slot = osoe_progress_slot(p, node);
    if (slot == OSOE_CONNECTION_NONE || slot >= p->owner[0].controls)
        osoe_fatal("node %u: no control wire toward node %u for the deadlock detector",
                   p->node, node);
    struct osoe_reply r = { .kind = OSOE_REPLY_DEADLOCK,
                            .value = (uint64_t)m->round | (uint64_t)m->kind << 32 |
                                     (uint64_t)m->phase << 48,
                            .token = m->ok };
    enqueue_control(p, slot, &r);
}

uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer)
{
    uint32_t slot = osoe_progress_slot(p, peer);
    if (slot == OSOE_CONNECTION_NONE)
        return 0;
    return p->owner[osoe_owner_of(slot, p->helpers)]
        .connection[osoe_owner_index(slot, p->helpers)].acked;
}
