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
#include "progress.h"
#include "ring.h"
#include "rmw.h"
#include "wire.h"

/* The sink is bound to one connection: a reply must be enqueued on the queues
 * of the connection its request arrived on, and the parser does not know which
 * that is. */
struct arrival {
    struct osoe_progress *progress;
    uint32_t slot;
};

uint32_t osoe_progress_slot(const struct osoe_progress *p, uint32_t peer)
{
    if (peer == p->node || peer >= p->nodes)
        return OSOE_CONNECTION_NONE;
    return peer < p->node ? peer : peer - 1;
}

/* ---- Applying what arrived ------------------------------------------- */

/*
 * Where a payload goes, resolved before any payload byte is consumed so that
 * bytes land where they belong rather than in a reassembly buffer (§3.3). This
 * is the only place an incoming message becomes an address.
 */
static uint32_t peer_of(const struct osoe_progress *p, uint32_t slot)
{
    return slot < p->node ? slot : slot + 1;
}

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
static unsigned char *reply_landing(struct osoe_progress *p, uint32_t slot,
                                          const struct osoe_am_get_reply *m,
                                          uint32_t payload_bytes)
{
    uint32_t length = 0;
    unsigned char *at = p->host.landing_request
        ? p->host.landing_request(p->host.context, m->origin_pe, m->token, &length)
        : NULL;
    if (!at)
        osoe_fatal("a get reply from node %u for PE %u carries token %#llx, which names no "
                   "get of that PE's in flight", peer_of(p, slot), m->origin_pe,
                   (unsigned long long)m->token);
    if ((uint64_t)m->chunk_offset + payload_bytes > m->total || m->total != length)
        osoe_fatal("a get reply from node %u for PE %u puts %u bytes at offset %u of a "
                   "%u-byte reply, for a get of %u bytes", peer_of(p, slot), m->origin_pe,
                   payload_bytes, m->chunk_offset, m->total, length);
    return at;
}

static void *destination(void *context, const struct osoe_message_header *header,
                         const void *fixed, uint32_t payload_bytes)
{
    struct arrival *a = context;
    struct osoe_progress *p = a->progress;

    if (header->kind == OSOE_AM_PUT) {
        struct osoe_am_put m;
        memcpy(&m, fixed, sizeof m);
        return p->host.resolve(p->host.context, m.target_pe, m.region, m.offset,
                               payload_bytes);
    }
    if (header->kind == OSOE_AM_GET_REPLY) {
        struct osoe_am_get_reply m;
        memcpy(&m, fixed, sizeof m);
        return reply_landing(p, a->slot, &m, payload_bytes) + m.chunk_offset;
    }
    return NULL;
}

/*
 * Take a descriptor and queue it. Fallible on purpose (§3.8): a handler that
 * could not get one leaves the request and the progress thread carries on
 * reaping, which is what makes §3.7's "never reserve" satisfiable without any
 * blocking path at all.
 */
/* This connection's record in the control segment (§3.14), or NULL. */
static struct osoe_am_connection_metrics *conn_metrics(struct osoe_progress *p, uint32_t slot)
{
    return p->connection_metrics ? &p->connection_metrics[slot] : NULL;
}

/* The queue gauges of one connection, published where osoe-stat reads them.
 * Called where they change and when the lead visits the connection. */
static void publish_queues(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_am_connection_metrics *cm = conn_metrics(p, slot);
    if (!cm)
        return;
    const struct osoe_reply_queues *q = &p->connection[slot].queues;
    atomic_store_explicit(&cm->control_queue_depth, q->control.depth, memory_order_release);
    atomic_store_explicit(&cm->bulk_queue_depth, q->bulk.depth, memory_order_release);
    atomic_store_explicit(&cm->control_queue_high_water, q->control.high_water,
                          memory_order_release);
    atomic_store_explicit(&cm->bulk_queue_high_water, q->bulk.high_water, memory_order_release);
}

/* The node-wide high-water marks (§3.14), after a push: the deepest this
 * connection's queues have been, and the most descriptors out at once. */
static void note_high_water(struct osoe_progress *p, uint32_t slot)
{
    const struct osoe_reply_queues *q = &p->connection[slot].queues;
    osoe_counter_word_max(&p->metrics->control_queue_high_water, q->control.high_water);
    osoe_counter_word_max(&p->metrics->bulk_queue_high_water, q->bulk.high_water);
    osoe_counter_word_max(&p->metrics->pool_high_water, p->pool.high_water);
    publish_queues(p, slot);
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
static _Noreturn void pool_exhausted(struct osoe_progress *p, uint32_t slot,
                                     const struct osoe_reply *value)
{
    osoe_counter_word_add(&p->metrics->reply_reservation_failures, 1);
    const struct osoe_reply_queues *q = &p->connection[slot].queues;
    uint32_t peer = slot < p->node ? slot : slot + 1;
    bool collective = value->kind == OSOE_REPLY_RELEASE || value->kind == OSOE_REPLY_ARRIVE;
    osoe_fatal("node %u: the reply descriptor pool is empty (%u of %u in use) for a %s "
               "%s node %u; toward it the control queue is %u deep and the bulk queue %u. "
               "%s", p->node, p->pool.in_use, p->pool.count, reply_kind_name(value->kind),
               collective ? "toward" : "request from", peer, q->control.depth, q->bulk.depth,
               collective ? "The pool reserves a place for every forward a blocking "
                            "collective can have outstanding, so this is a bug in this "
                            "node's collective bookkeeping (§3.8, argument 2)"
                          : "Requests are bounded by the credit the origin takes, so the "
                            "break is on that node's side: its OSOE_AM_CREDITS differs from "
                            "this node's, or its credit accounting is wrong (§3.8, "
                            "argument 1)");
}

/* The connection has send-side work (busy()): visit it on the passes to come.
 * Its bit is cleared by the visit that finds it idle. */
static void mark_busy(struct osoe_progress *p, uint32_t slot)
{
    p->busy[slot / 64] |= UINT64_C(1) << (slot % 64);
}

static void enqueue(struct osoe_progress *p, uint32_t slot, struct osoe_reply_queue *q,
                    const struct osoe_reply *value)
{
    uint32_t i = osoe_reply_take(&p->pool);
    if (i == OSOE_REPLY_NONE)
        pool_exhausted(p, slot, value);
    struct osoe_reply *r = osoe_reply_at(&p->pool, i);
    *r = *value;
    osoe_reply_push(&p->pool, q, i);
    note_high_water(p, slot);
    mark_busy(p, slot);         /* possibly another connection than the one being read */
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

/* Queue one collective message toward a node of the tree. A handler may
 * enqueue but never reserve (§3.7): the drain step writes it. */
static void forward(struct osoe_progress *p, uint32_t team, uint32_t node, uint16_t kind,
                    uint32_t epoch)
{
    uint32_t slot = osoe_progress_slot(p, node);
    if (slot == OSOE_CONNECTION_NONE || !osoe_connection_to(p->area, node))
        osoe_fatal("node %u: no connection toward node %u, a neighbour in team %u's tree",
                   p->node, node, team);
    struct osoe_reply r = { .kind = kind, .origin_pe = team, .length = 0, .value = epoch };
    enqueue(p, slot, &p->connection[slot].queues.control, &r);
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
        forward(p, team, t->tree.child[j], OSOE_REPLY_RELEASE, epoch);
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
                   "epoch %u; collective epochs have diverged", p->node, team, epoch,
                   t->closed);
    if (t->open != 0 && t->open != epoch)
        osoe_fatal("node %u: team %u has arrivals for epochs %u and %u at once; "
                   "collective epochs have diverged", p->node, team, t->open, epoch);
    t->open = epoch;
    if (child == OSOE_COLLECTIVE_TREE_NONE) {
        if (t->own)
            osoe_fatal("node %u: team %u's members arrived twice for epoch %u", p->node,
                       team, epoch);
        t->own = true;
    } else {
        uint32_t bit = UINT32_C(1) << child;
        if (t->arrived & bit)
            osoe_fatal("node %u: team %u had two arrivals for epoch %u from node %u",
                       p->node, team, epoch, t->tree.child[child]);
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
        forward(p, team, t->tree.parent, OSOE_REPLY_ARRIVE, epoch);
}

/*
 * Team number `team`'s state, or NULL if it has no internode phase on this
 * node. Filled from the host's source the first time the engine meets the team
 * (progress.h), which is how a team created while this thread runs becomes
 * known to it: the host reads the team's record, which is published before
 * the team's first arrival anywhere. O(k) once per team.
 */
static struct osoe_progress_team *known_team(struct osoe_progress *p, uint32_t team)
{
    if (team >= p->team_count)
        return NULL;
    struct osoe_progress_team *t = &p->team[team];
    if (!t->registered && p->team_source) {
        struct osoe_collective_tree tree;
        struct osoe_collective_state *published = NULL;
        if (p->team_source(p->host.context, team, &tree, &published)) {
            *t = (struct osoe_progress_team){ .registered = true, .tree = tree,
                                              .published = published };
            publish_team(p, team);
        }
    }
    return t->registered ? t : NULL;
}

void osoe_progress_meet_teams(struct osoe_progress *p, uint32_t count)
{
    for (uint32_t team = 0; team < count && team < p->team_count; team++)
        known_team(p, team);
}

/* The team a collective message names, checked (§3.4's trust boundary). */
static struct osoe_progress_team *barrier_team(struct osoe_progress *p, uint32_t slot,
                                               const struct osoe_am_barrier *m,
                                               const char *what)
{
    struct osoe_progress_team *t = known_team(p, m->team);
    if (!t)
        osoe_fatal("node %u: a barrier %s from node %u names team %u, which has no "
                   "internode phase on this node", p->node, what, peer_of(p, slot), m->team);
    if (m->from_node != peer_of(p, slot))
        osoe_fatal("node %u: a barrier %s on node %u's connection says it is from node %u",
                   p->node, what, peer_of(p, slot), m->from_node);
    return t;
}

static void complete(void *context, const struct osoe_message_header *header,
                     const void *fixed, void *payload, uint32_t payload_bytes)
{
    struct arrival *a = context;
    struct osoe_progress *p = a->progress;
    struct osoe_progress_connection *c = &p->connection[a->slot];
    osoe_counter_word_add(&p->metrics->messages_applied, 1);
    if (conn_metrics(p, a->slot))
        osoe_counter_word_add(&conn_metrics(p, a->slot)->messages_applied, 1);
    if (header->kind != OSOE_AM_DEADLOCK)
        p->application_applied++;

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
        enqueue(p, a->slot, &c->queues.bulk, &r);
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
                       "the width", peer_of(p, a->slot), m.width, m.op,
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
            enqueue(p, a->slot, &c->queues.control, &r);
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
        (void)reply_landing(p, a->slot, &m, payload_bytes);
        if (m.chunk_offset + payload_bytes < m.total)
            break;
        if (!(m.token & OSOE_LANDING_NONBLOCKING)) {
            /* A blocking get: its thread copies the bytes out and returns the
             * credit once it has read them. */
            if (!p->host.completed(p->host.context, m.origin_pe, m.token, m.total, 0))
                osoe_fatal("a get reply from node %u for PE %u carries token %#llx, which no "
                           "thread of that PE waits for", peer_of(p, a->slot), m.origin_pe,
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
        struct osoe_connection *connection = osoe_connection_to(p->area, peer_of(p, a->slot));
        if (!connection)
            osoe_fatal("a get reply arrived from node %u, toward which this node has no "
                       "connection", peer_of(p, a->slot));
        osoe_credit_give(osoe_connection_credit(connection));
        break;
    }

    case OSOE_AM_BARRIER_ARRIVE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        struct osoe_progress_team *t = barrier_team(p, a->slot, &m, "arrival");
        /* §3.4's trust boundary: only a child sends ARRIVE up to this node. A
         * node that disagrees about the tree - a different
         * OSOE_COLLECTIVE_FANOUT, or a different node set - is named rather
         * than counted. */
        uint32_t child = osoe_collective_tree_child(&t->tree, m.from_node);
        if (child == OSOE_COLLECTIVE_TREE_NONE)
            osoe_fatal("node %u: team %u's arrival for epoch %u came from node %u, which is "
                       "not a child of this node in its tree (fan-out %u over %u nodes); "
                       "the nodes disagree about the tree", p->node, m.team, m.epoch,
                       m.from_node, t->tree.fanout, t->tree.count);
        book_arrival(p, m.team, m.epoch, child);
        break;
    }

    case OSOE_AM_BARRIER_RELEASE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        struct osoe_progress_team *t = barrier_team(p, a->slot, &m, "release");
        if (m.from_node != t->tree.parent)
            osoe_fatal("node %u: team %u's release for epoch %u came from node %u; this "
                       "node's parent in the tree is node %u", p->node, m.team, m.epoch,
                       m.from_node, t->tree.parent);
        if (m.epoch != t->closed || t->open != 0 || t->release_pending != 0)
            osoe_fatal("node %u: team %u's release for epoch %u arrived with epoch %u the "
                       "last closed here and %u open; collective epochs have diverged",
                       p->node, m.team, m.epoch, t->closed, t->open);
        release_down(p, m.team, m.epoch);
        break;
    }

    case OSOE_AM_RMW_REPLY: {
        struct osoe_am_rmw_reply m;
        memcpy(&m, fixed, sizeof m);
        if (!p->host.completed(p->host.context, m.origin_pe, m.token, 0, m.value))
            osoe_fatal("an atomic's reply from node %u for PE %u carries token %#llx, which "
                       "no thread of that PE waits for", peer_of(p, a->slot), m.origin_pe,
                       (unsigned long long)m.token);
        break;
    }

    case OSOE_AM_ABORT: {
        /* A peer's abortive goodbye: its FIN follows, and is not a death (the
         * segment's goodbye flag says so), and nothing need be told to it. */
        struct osoe_am_abort m;
        memcpy(&m, fixed, sizeof m);
        c->peer_aborted = true;
        c->peer_abort_origin = m.origin_pe;
        if (p->host.abort)
            p->host.abort(p->host.context, m.origin_pe, m.status, m.disposition);
        break;
    }

    case OSOE_AM_DEADLOCK: {
        struct osoe_am_deadlock m;
        memcpy(&m, fixed, sizeof m);
        if (m.from_node != peer_of(p, a->slot))
            osoe_fatal("a deadlock detector message from node %u names node %u",
                       peer_of(p, a->slot), m.from_node);
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
    struct osoe_progress *p = a->progress;
    struct osoe_progress_connection *c = &p->connection[a->slot];
    osoe_counter_word_add(&p->metrics->segments_applied, 1);
    struct osoe_am_connection_metrics *cm = conn_metrics(p, a->slot);
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
        struct osoe_connection *connection = osoe_connection_to(p->area,
                                          a->slot < p->node ? a->slot : a->slot + 1);
        if (connection)
            osoe_connection_publish_acked(connection, c->acked);
    }
}

/* ---- Receiving -------------------------------------------------------- */

/*
 * Take everything that has arrived on the connection. Called for every wire the
 * ring's reap marked readable, and it must empty it: a provided buffer left
 * queued would be held across the next reap, which the ring refuses (§3.3,
 * invariant 23).
 */
static bool receive(struct osoe_progress *p, uint32_t slot)
{
    struct arrival a = { p, slot };
    struct osoe_am_sink sink = { destination, complete, segment_applied, &a };
    struct osoe_wire *w = &p->wire[slot];
    struct osoe_progress_connection *c = &p->connection[slot];
    bool moved = false;
    for (;;) {
        size_t n = osoe_wire_recv(w, p->scratch, p->scratch_bytes);
        if (n == 0)
            break;
        osoe_am_parse(&c->parser, p->scratch, n, &sink);
        moved = true;
    }
    /* Ending anyway, the job has nothing to fear from a peer that is ending
     * too: its connection failing, or its stream ending without a goodbye. Nor
     * from one whose abortive goodbye has arrived, before this node's agent has
     * begun its own: that peer's exit may reset the connection behind the
     * goodbye, and this node is about to end with the status it carried. */
    if ((p->aborting || c->peer_aborted) && (w->error || osoe_wire_ended(w))) {
        c->abort_done = c->abort_done || w->error || !c->peer_goodbye;
        return moved;
    }
    /*
     * What only the owner of the connection can tell (am.h): that the stream
     * ended, and whether it ended where a segment did and after the peer's
     * goodbye. A crashed lead's kernel closes its sockets, so without the
     * goodbye an end of stream is a peer that died, and the job ends here
     * naming it rather than waiting for something that node will never send
     * (§3.15, §6.4).
     */
    if (w->error == -ETIMEDOUT)
        osoe_fatal("node %u: node %u stopped answering: its kernel acknowledged neither "
                   "keepalive probes nor retransmissions (§3.15). The host has vanished, or "
                   "the network between is down", p->node, peer_of(p, slot));
    if (w->error)
        osoe_fatal("node %u: the connection to node %u failed: %s", p->node,
                   peer_of(p, slot), strerror(-w->error));
    if (osoe_wire_ended(w)) {
        if (!osoe_am_parser_between_segments(&c->parser))
            osoe_fatal("node %u: node %u's stream ended in the middle of a segment: its "
                       "lead has exited or died mid-send", p->node, peer_of(p, slot));
        if (!c->peer_goodbye && w->send_error)
            osoe_fatal("node %u: node %u reset its connection without its goodbye (%s): "
                       "its lead has exited or died (§6.4)", p->node, peer_of(p, slot),
                       strerror(-w->send_error));
        if (!c->peer_goodbye)
            osoe_fatal("node %u: node %u ended its connection without its goodbye: its "
                       "lead has exited or died (§6.4)", p->node, peer_of(p, slot));
    }
    return moved;
}

/* ---- Draining replies into the open buffer ---------------------------- */

/*
 * Control first and to empty, then bulk round-robin (§3.8). Barrier latency
 * gates the whole job, so a forward stuck behind a large get reply stalls every
 * PE; the control queue is small and drains to empty, so it cannot starve the
 * bulk queue in practice, while the reverse would happen constantly.
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

static bool drain_into(struct osoe_progress *p, uint32_t slot, struct osoe_connection *connection,
                       struct drain_target *t)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    bool moved = false;

    while (osoe_reply_peek(&c->queues.control) != OSOE_REPLY_NONE) {
        uint32_t i = osoe_reply_peek(&c->queues.control);
        struct osoe_reply *r = osoe_reply_at(&p->pool, i);
        struct osoe_reservation resv;
        /* The two collective messages are the same payload in both directions
         * (§5.3): a team, an epoch, and which node it came from. */
        bool barrier = r->kind == OSOE_REPLY_RELEASE || r->kind == OSOE_REPLY_ARRIVE;
        bool detector = r->kind == OSOE_REPLY_DEADLOCK;
        uint32_t fixed_bytes = barrier    ? (uint32_t)sizeof(struct osoe_am_barrier)
                               : detector ? (uint32_t)sizeof(struct osoe_am_deadlock)
                                          : (uint32_t)sizeof(struct osoe_am_rmw_reply);
        if (!reserve_for_reply(connection, t, osoe_message_padded(fixed_bytes), &resv))
            return moved;                   /* no room: try again next pass */
        uint16_t kind = r->kind == OSOE_REPLY_RELEASE ? OSOE_AM_BARRIER_RELEASE
                      : r->kind == OSOE_REPLY_ARRIVE  ? OSOE_AM_BARRIER_ARRIVE
                      : detector                      ? OSOE_AM_DEADLOCK
                                                      : OSOE_AM_RMW_REPLY;
        struct osoe_message_header h = { kind, 0, fixed_bytes };
        unsigned char *at = osoe_reservation_bytes(connection, &resv);
        memcpy(at, &h, sizeof h);
        if (detector) {
            struct osoe_am_deadlock m = { .round = (uint32_t)r->value,
                                          .kind = (uint16_t)(r->value >> 32),
                                          .phase = (uint16_t)(r->value >> 48),
                                          .ok = (uint32_t)r->token, .from_node = p->node };
            memcpy(at + sizeof h, &m, sizeof m);
        } else if (barrier) {
            struct osoe_am_barrier m = { .team = r->origin_pe, .epoch = (uint32_t)r->value,
                                         .from_node = p->node };
            memcpy(at + sizeof h, &m, sizeof m);
        } else {
            struct osoe_am_rmw_reply m = { .origin_pe = r->origin_pe, .token = r->token,
                                           .value = r->value };
            memcpy(at + sizeof h, &m, sizeof m);
        }
        osoe_connection_commit(connection, &resv);
        osoe_reply_give(&p->pool, osoe_reply_pop(&p->pool, &c->queues.control));
        moved = true;
    }

    /* One chunk per descriptor per pass: strict FIFO would let a large reply
     * block the small ones behind it, and replies to different PEs have no
     * mutual ordering requirement, so rotation is legal (§3.8). */
    uint32_t rotations = c->queues.bulk.depth;
    while (rotations-- > 0 && osoe_reply_peek(&c->queues.bulk) != OSOE_REPLY_NONE) {
        uint32_t i = osoe_reply_peek(&c->queues.bulk);
        struct osoe_reply *r = osoe_reply_at(&p->pool, i);
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
            osoe_reply_give(&p->pool, osoe_reply_pop(&p->pool, &c->queues.bulk));
        else
            osoe_reply_rotate(&p->pool, &c->queues.bulk);
    }
    return moved;
}

/* The opportunistic drain, into the open buffer. */
static bool drain(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_connection *connection = osoe_connection_to(p->area,
                                                            slot < p->node ? slot : slot + 1);
    if (!connection || c->open == OSOE_CONNECTION_NONE)
        return false;
    struct drain_target open = { OSOE_CONNECTION_NONE, 0 };
    return drain_into(p, slot, connection, &open);
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
 */
static bool asks_for_ack(const struct osoe_progress_connection *c, uint64_t seq,
                         uint64_t wanted)
{
    if (seq <= c->acked)
        return false;
    bool late = wanted > c->acked && c->flagged < wanted;
    return seq <= wanted || late;
}

static void finalize(struct osoe_progress *p, uint32_t slot, struct osoe_connection *connection,
                     uint32_t k, uint32_t bytes, uint64_t wanted)
{
    struct osoe_progress_connection *c = &p->connection[slot];
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
    bool ask = !goodbye && asks_for_ack(c, seq, wanted);
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
static bool flush_aborting(struct osoe_progress *p, uint32_t slot,
                           struct osoe_connection *connection)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_wire *w = &p->wire[slot];
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
    size_t n = osoe_wire_send(w, p->abort_segment + c->abort_sent,
                              p->abort_segment_bytes - c->abort_sent);
    c->abort_sent += (uint32_t)n;
    if (c->abort_sent == p->abort_segment_bytes) {
        if (!p->abort_keeps_stream)
            osoe_wire_shutdown(w);      /* a global exit's ends with the process */
        c->abort_done = true;
        return true;
    }
    return moved || n > 0;
}

static bool flush(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_connection *connection = osoe_connection_to(p->area,
                                                            slot < p->node ? slot : slot + 1);
    if (!connection)
        return false;
    if (p->aborting)
        return flush_aborting(p, slot, connection);
    /* A peer whose abortive goodbye has arrived is ending: nothing more of the
     * job's goes to it, and its exit may already have reset the connection,
     * which a send would take for a failure - a fatal on this thread, ending
     * this node before its agent could tell its other peers. */
    if (c->peer_aborted)
        return false;
    /* Nor to a peer that has reset the connection, which reads nothing more:
     * whether it was ending or died, its stream's end says (receive). */
    if (p->wire[slot].send_error)
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
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    struct osoe_am_connection_metrics *cm = conn_metrics(p, slot);
    if (cm) {
        /* The three sequences a stuck connection is read by (§3.14): what the
         * peer has acknowledged, what this node's writers are waiting to have
         * acknowledged, and how far this node has sent. */
        atomic_store_explicit(&cm->acked_through, c->acked, memory_order_release);
        atomic_store_explicit(&cm->wanted_through, wanted, memory_order_release);
        atomic_store_explicit(&cm->sent_through, c->next_seq ? c->next_seq - 1 : 0,
                              memory_order_release);
        publish_queues(p, slot);
    }

    /* Finish an in-flight send. A short write is the normal case (§3.3): keep
     * the remainder and offer it again. */
    if (c->sending != OSOE_CONNECTION_NONE) {
        unsigned char *base = connection->data + (size_t)c->sending * connection->capacity;
        size_t n = osoe_wire_send(&p->wire[slot], base + c->sent,
                                  c->sending_bytes - c->sent);
        c->sent += (uint32_t)n;
        if (n > 0)
            moved = true;
        if (c->sent == c->sending_bytes) {
            osoe_connection_release(connection, c->sending);
            c->state[c->sending] = OSOE_BUFFER_FREE;
            c->sending = OSOE_CONNECTION_NONE;
            /* That was the goodbye (nothing is sealed after it): end the stream. */
            if (c->goodbye == OSOE_GOODBYE_SEALED && c->sealed == OSOE_CONNECTION_NONE) {
                osoe_wire_shutdown(&p->wire[slot]);
                c->goodbye = OSOE_GOODBYE_SENT;
            }
            osoe_counter_word_add(&p->metrics->segments_sent, 1);
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
            finalize(p, slot, connection, c->sealed, bytes, wanted);
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
        bool goodbye = p->closing && c->sending == OSOE_CONNECTION_NONE && used == header &&
                       !c->carrying && c->queues.control.depth == 0 &&
                       c->queues.bulk.depth == 0;
        if (goodbye)
            c->goodbye = OSOE_GOODBYE_SEALED;
        if (used > header || c->owe_ack || late || goodbye) {
            if (used == header) {
                osoe_counter_word_add(&p->metrics->header_only_sent, 1);   /* §3.4's ack-only */
                if (conn_metrics(p, slot))
                    osoe_counter_word_add(&conn_metrics(p, slot)->header_only_sent, 1);
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
                /* The pre-open drain (§3.8): replies first, while k is still
                 * sealed and no writer can reach it. */
                c->state[k] = OSOE_BUFFER_DRAINING;
                struct drain_target preopen = { k, connection->capacity / 2 };
                if (drain_into(p, slot, connection, &preopen))
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

/* ---- The pass --------------------------------------------------------- */

void osoe_progress_init(struct osoe_progress *p, struct osoe_connection_area *area,
                        struct osoe_wire *wires, struct osoe_ring *ring,
                        struct osoe_reply *descriptors, uint32_t descriptor_count,
                        unsigned char *scratch, uint32_t scratch_bytes,
                        const struct osoe_progress_host *host)
{
    memset(p, 0, sizeof *p);
    p->area = area;
    p->wire = wires;
    p->ring = ring;
    p->descriptors = descriptors;
    p->scratch = scratch;
    p->scratch_bytes = scratch_bytes;
    p->node = area->node;
    p->nodes = area->nodes;
    p->host = *host;
    p->metrics = &p->own_metrics;
    p->team = p->own_team;
    p->pending = p->own_pending;
    p->team_count = OSOE_PREDEFINED_TEAMS;
    osoe_reply_pool_init(&p->pool, descriptors, descriptor_count);
}

void osoe_progress_attach_teams(struct osoe_progress *p, struct osoe_progress_team *teams,
                                uint32_t *pending, uint32_t count,
                                osoe_progress_team_source *source)
{
    memset(teams, 0, sizeof *teams * count);
    p->team = teams;
    p->pending = pending;
    p->team_count = count;
    p->pending_count = 0;
    p->team_source = source;
}

void osoe_progress_attach(struct osoe_progress *p,
                          struct osoe_progress_connection *connections, uint64_t *busy)
{
    p->connection = connections;
    p->busy = busy;
    /* Every connection starts busy - it has no buffer open yet - so the first
     * pass visits each once (O(N), once), and after that only what is active. */
    memset(busy, 0, (size_t)((p->nodes - 1 + 63) / 64) * sizeof *busy);
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++)
        mark_busy(p, slot);
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        struct osoe_progress_connection *c = &p->connection[slot];
        memset(c, 0, sizeof *c);
        c->open = c->sealed = c->sending = OSOE_CONNECTION_NONE;
        c->next_seq = 1;
        c->state[0] = c->state[1] = OSOE_BUFFER_FREE;
        osoe_am_parser_init(&c->parser, peer_of(p, slot));
        osoe_reply_queues_init(&c->queues);
    }
}

/*
 * Does this connection have send-side work the lead already knows about,
 * without a writer saying so? A buffer to open, one sealed or on the wire,
 * replies queued, an acknowledgement owed, or replies the pre-open drain put in
 * the open buffer. The last was missed at first: the pre-open drain runs after
 * the pass's chance to seal, and the first barrier of an 8-node job hung about
 * one run in eight with a leaf's ARRIVE sitting unsealed in its open buffer. Private state only: O(1), and no
 * shared line touched.
 */
static bool busy(const struct osoe_progress *p, uint32_t slot)
{
    const struct osoe_progress_connection *c = &p->connection[slot];
    if (p->aborting)
        return !c->abort_done;
    return c->open == OSOE_CONNECTION_NONE || c->sending != OSOE_CONNECTION_NONE ||
           c->sealed != OSOE_CONNECTION_NONE || c->owe_ack || c->carrying ||
           c->queues.control.depth != 0 || c->queues.bulk.depth != 0;
}

/* After a visit: keep the connection's bit while it is busy, clear it once not. */
static void settle(struct osoe_progress *p, uint32_t slot)
{
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (busy(p, slot))
        p->busy[slot / 64] |= bit;
    else
        p->busy[slot / 64] &= ~bit;
}

/*
 * The releases this pass forwarded, applied to this node's own members last.
 * Each child's connection is drained and flushed first, so the RELEASE is in
 * the wire - or in the open buffer behind a send already in flight - before the
 * local wake, which is O(L) futex work on this thread (§5.3). O(k) per release.
 */
static bool release_local(struct osoe_progress *p)
{
    bool moved = false;
    /* The list, not every team number: O(k) per release this pass forwarded,
     * whatever OSOE_MAX_TEAMS is. A team is listed once however many passes
     * its release waits, and taken off as it is applied. */
    for (uint32_t i = 0; i < p->pending_count; i++) {
        uint32_t team = p->pending[i];
        struct osoe_progress_team *t = &p->team[team];
        for (uint32_t j = 0; j < t->tree.children; j++) {
            uint32_t slot = osoe_progress_slot(p, t->tree.child[j]);
            drain(p, slot);
            flush(p, slot);
            settle(p, slot);
        }
        uint32_t epoch = t->release_pending;
        t->release_pending = 0;
        p->host.release(p->host.context, team, epoch);
        moved = true;
    }
    p->pending_count = 0;
    return moved;
}

/* The ring's counts since the last pass, into the node's record (§3.14). */
static void publish_ring(struct osoe_progress *p)
{
    struct osoe_ring_counts *k = &p->ring->counts;
    if (k->completions == 0 && k->sends == 0)
        return;
    osoe_counter_word_add(&p->metrics->completions, k->completions);
    osoe_counter_word_add(&p->metrics->receive_buffers, k->receives);
    osoe_counter_word_add(&p->metrics->receive_enobufs, k->enobufs);
    osoe_counter_word_add(&p->metrics->receive_rearms, k->rearms);
    osoe_counter_word_add(&p->metrics->sends, k->sends);
    osoe_counter_word_add(&p->metrics->short_sends, k->short_sends);
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
 */
bool osoe_progress_poll(struct osoe_progress *p)
{
    bool moved = release_local(p);     /* a release osoe_progress_arrive started */
    osoe_ring_reap(p->ring);
    uint32_t peers = p->nodes - 1, visits = 0;
    for (uint32_t w = 0; w * 64 < peers; w++) {
        uint64_t dirty = 0;
        if (p->area->dirty &&
            atomic_load_explicit(&p->area->dirty[w], memory_order_acquire) != 0)
            dirty = atomic_exchange_explicit(&p->area->dirty[w], 0, memory_order_seq_cst);
        uint64_t readable = osoe_ring_take_readable(p->ring, w);
        for (uint64_t visit = dirty | readable | p->busy[w]; visit != 0; visit &= visit - 1) {
            uint32_t bit = (uint32_t)stdc_trailing_zeros(visit);
            uint32_t slot = w * 64 + bit;
            if ((readable >> bit) & 1)
                moved |= receive(p, slot);
            if (!p->aborting)
                moved |= drain(p, slot);
            moved |= flush(p, slot);
            settle(p, slot);
            visits++;
        }
    }
    /* A release this pass received is forwarded before the submit, so the
     * children's RELEASE goes to the kernel now and not at the next pass - or,
     * with nothing more to do, after the idle wait. */
    moved |= release_local(p);
    osoe_ring_submit(p->ring);
    publish_ring(p);
    osoe_counter_word_add(&p->metrics->passes, 1);
    osoe_counter_word_add(&p->metrics->send_visits, visits);
    return moved;
}

bool osoe_progress_idle(struct osoe_progress *p)
{
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    if (p->pending_count != 0)
        return false;
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        struct osoe_progress_connection *c = &p->connection[slot];
        if (p->wire[slot].send_error)
            continue;                   /* a peer that reset the connection is owed nothing */
        if (c->sending != OSOE_CONNECTION_NONE || c->sealed != OSOE_CONNECTION_NONE)
            return false;
        if (c->queues.control.depth != 0 || c->queues.bulk.depth != 0)
            return false;
        struct osoe_connection *connection = osoe_connection_to(p->area,
                                                                slot < p->node ? slot : slot + 1);
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
    p->team[team] = (struct osoe_progress_team){ .registered = true, .tree = *tree };
    publish_team(p, team);
}

void osoe_progress_arrive(struct osoe_progress *p, uint32_t team, uint32_t epoch)
{
    if (!known_team(p, team))
        osoe_fatal("node %u: team %u arrived at an internode phase it never registered",
                   p->node, team);
    book_arrival(p, team, epoch, OSOE_COLLECTIVE_TREE_NONE);
}

void osoe_progress_close(struct osoe_progress *p)
{
    p->closing = true;
    /* Every connection is visited until its goodbye has gone (O(N), once). */
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++)
        mark_busy(p, slot);
}

void osoe_progress_abort(struct osoe_progress *p, uint32_t pe, int32_t status,
                         uint32_t disposition, bool keep_stream)
{
    if (p->aborting)
        return;
    p->aborting = true;
    p->abort_keeps_stream = keep_stream;
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
                   sizeof(struct osoe_am_abort) <= sizeof p->abort_segment,
                   "the abortive goodbye fits its buffer");
    memset(p->abort_segment, 0, sizeof p->abort_segment);
    memcpy(p->abort_segment, &sh, sizeof sh);
    memcpy(p->abort_segment + sizeof sh, &h, sizeof h);
    memcpy(p->abort_segment + sizeof sh + sizeof h, &m, sizeof m);
    p->abort_segment_bytes = bytes;
    /* Every connection is visited until its abortive goodbye has gone (O(N), once). */
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++)
        mark_busy(p, slot);
}

bool osoe_progress_aborted(const struct osoe_progress *p)
{
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        const struct osoe_progress_connection *c = &p->connection[slot];
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
        if (c->abort_sent == p->abort_segment_bytes && !c->peer_aborted &&
            !p->wire[slot].error && !p->wire[slot].send_error &&
            osoe_wire_unacknowledged(&p->wire[slot]))
            return false;
    }
    return true;
}

bool osoe_progress_peers_ended(const struct osoe_progress *p, uint32_t carrier)
{
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        const struct osoe_progress_connection *c = &p->connection[slot];
        if (c->peer_aborted && c->peer_abort_origin != carrier)
            continue;
        if (!osoe_wire_ended(&p->wire[slot]) && !p->wire[slot].error)
            return false;
    }
    return true;
}

bool osoe_progress_closed(const struct osoe_progress *p)
{
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        const struct osoe_progress_connection *c = &p->connection[slot];
        /* A connection whose sends met the peer's reset needs no goodbye of this
         * side's once the peer's own has arrived and its stream ended: the peer
         * reads nothing more - it crashed during the close (abort_model.py). */
        if ((c->goodbye != OSOE_GOODBYE_SENT && !p->wire[slot].send_error) ||
            !c->peer_goodbye || !osoe_wire_ended(&p->wire[slot]))
            return false;
    }
    return true;
}

void osoe_progress_check_silent(struct osoe_progress *p, uint32_t budget_ms)
{
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        struct osoe_wire *w = &p->wire[slot];
        if (w->error || w->send_error || osoe_wire_ended(w))
            continue;               /* already failed or ended: judged elsewhere */
        uint32_t silent = osoe_wire_silent_ms(w);
        if (silent >= budget_ms)
            osoe_fatal("node %u: node %u stopped answering: its kernel has acknowledged "
                       "nothing sent to it for %u s (§3.15). The host has vanished, or the "
                       "network between is down", p->node, peer_of(p, slot), silent / 1000);
    }
}

bool osoe_progress_quiet(struct osoe_progress *p, uint64_t *applied)
{
    *applied = p->application_applied;
    if (!osoe_progress_idle(p))
        return false;
    bool quiet = true;
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        struct osoe_progress_connection *c = &p->connection[slot];
        if (p->wire[slot].send_error || c->acked >= c->last_application_seq)
            continue;                   /* a reset peer is owed nothing */
        quiet = false;
        if (c->detector_wanted < c->last_application_seq) {
            c->detector_wanted = c->last_application_seq;
            mark_busy(p, slot);         /* so a pass seals the request */
        }
    }
    return quiet;
}

void osoe_progress_deadlock_send(struct osoe_progress *p, uint32_t node,
                                 const struct osoe_deadlock_message *m)
{
    uint32_t slot = osoe_progress_slot(p, node);
    if (slot == OSOE_CONNECTION_NONE || !osoe_connection_to(p->area, node))
        osoe_fatal("node %u: no connection toward node %u for the deadlock detector", p->node,
                   node);
    struct osoe_reply r = { .kind = OSOE_REPLY_DEADLOCK,
                            .value = (uint64_t)m->round | (uint64_t)m->kind << 32 |
                                     (uint64_t)m->phase << 48,
                            .token = m->ok };
    enqueue(p, slot, &p->connection[slot].queues.control, &r);
}

uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer)
{
    uint32_t slot = osoe_progress_slot(p, peer);
    return slot == OSOE_CONNECTION_NONE ? 0 : p->connection[slot].acked;
}
