#include <stdatomic.h>
#include <limits.h>
#include <string.h>

#include "base/futex.h"
#include "fatal.h"
#include "progress.h"
#include "rmw.h"

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
 * A get reply is checked before a byte of it lands (§3.4's trust boundary;
 * second review, D4). Its token must be the one the owner armed the slot for,
 * and its chunk must lie inside both the whole reply and the slot. A reply that
 * fails is a peer's bug - the same job, the same build - and the landing zone
 * sits in the control segment beside the other PEs' zones, so writing it would
 * corrupt a sibling rather than fail. The measured case: an unsolicited reply
 * with chunk_offset past the zone wrote 64 bytes beyond it and the test passed.
 */
static void check_get_reply(struct osoe_progress *p, uint32_t slot,
                            struct osoe_landing_slot *landing, uint32_t capacity,
                            const struct osoe_am_get_reply *m, uint32_t chunk)
{
    uint64_t armed = atomic_load_explicit(&landing->armed_token, memory_order_acquire);
    if (m->token != armed)
        osoe_fatal("a get reply from node %u for PE %u carries token %llu; that PE is "
                   "waiting for %llu", peer_of(p, slot), m->origin_pe,
                   (unsigned long long)m->token, (unsigned long long)armed);
    if ((uint64_t)m->chunk_offset + chunk > m->total || m->total > capacity)
        osoe_fatal("a get reply from node %u for PE %u puts %u bytes at offset %u of a "
                   "%u-byte reply, into a landing zone of %u bytes", peer_of(p, slot),
                   m->origin_pe, chunk, m->chunk_offset, m->total, capacity);
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
        uint32_t capacity = 0;
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe,
                                                         &capacity);
        if (!slot)
            osoe_fatal("a get reply from node %u names PE %u, which is not on this node",
                       peer_of(p, a->slot), m.origin_pe);
        check_get_reply(p, a->slot, slot, capacity, &m, payload_bytes);
        return slot->data + m.chunk_offset;
    }
    return NULL;
}

/*
 * Take a descriptor and queue it. Fallible on purpose (§3.8): a handler that
 * could not get one leaves the request and the progress thread carries on
 * reaping, which is what makes §3.7's "never reserve" satisfiable without any
 * blocking path at all.
 */
/* The node-wide high-water marks (§3.14), after a push: the deepest this
 * connection's queues have been, and the most descriptors out at once. */
static void note_high_water(struct osoe_progress *p, uint32_t slot)
{
    const struct osoe_reply_queues *q = &p->connection[slot].queues;
    osoe_counter_word_max(&p->metrics->control_queue_high_water, q->control.high_water);
    osoe_counter_word_max(&p->metrics->bulk_queue_high_water, q->bulk.high_water);
    osoe_counter_word_max(&p->metrics->pool_high_water, p->pool.high_water);
}

static const char *reply_kind_name(uint16_t kind)
{
    switch (kind) {
    case OSOE_REPLY_AMO:     return "fetching AMO";
    case OSOE_REPLY_GET:     return "get";
    case OSOE_REPLY_RELEASE: return "collective release";
    case OSOE_REPLY_ARRIVE:  return "collective arrival";
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
}


/* Publish a landing zone's completion: length or value, then ready, then the
 * wake. The waiter loads the wake word before the flag (§4.6), so a bump
 * between its load and its wait returns at once instead of sleeping stale. */
static void land(struct osoe_landing_slot *slot)
{
    atomic_store_explicit(&slot->ready, 1, memory_order_seq_cst);
    atomic_fetch_add_explicit(&slot->wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&slot->wake, INT_MAX, OSOE_FUTEX_SHARED);
}

static void complete(void *context, const struct osoe_message_header *header,
                     const void *fixed, void *payload, uint32_t payload_bytes)
{
    struct arrival *a = context;
    struct osoe_progress *p = a->progress;
    struct osoe_progress_connection *c = &p->connection[a->slot];
    osoe_counter_word_add(&p->metrics->messages_applied, 1);

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
        uint32_t capacity = 0;
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe,
                                                         &capacity);
        if (!slot)
            break;              /* destination() has already refused it */
        /*
         * A get larger than one AM chunk arrives as several replies (§3.2), so
         * `ready` is published only once the whole thing has landed - the
         * caller waits for *its* reply (§4.6), and a reply is not its reply
         * until it is complete. An earlier version set it on every chunk and
         * the caller read a quarter of a get and carried on.
         *
         * The wake is bumped either way, because a waiter that re-checks is
         * cheap and a waiter that sleeps through the last chunk is a hang.
         */
        uint32_t landed = m.chunk_offset + payload_bytes;
        atomic_store_explicit(&slot->length, landed, memory_order_release);
        atomic_store_explicit(&slot->token, m.token, memory_order_release);
        if (landed >= m.total) {
            land(slot);
        } else {
            atomic_fetch_add_explicit(&slot->wake, 1, memory_order_seq_cst);
            osoe_futex_wake(&slot->wake, INT_MAX, OSOE_FUTEX_SHARED);
        }
        break;
    }

    case OSOE_AM_BARRIER_ARRIVE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        if (m.team >= OSOE_TEAMS)
            osoe_fatal("a barrier arrival names team %u; this build has %u", m.team,
                       OSOE_TEAMS);
        /*
         * The root counts arrivals for the epoch. A node that arrives for the
         * next epoch while this one is still open cannot happen: a member
         * cannot reach epoch k+1 without this node having been released from k
         * (§5.4's bounded skew), which is the same argument §3.8 uses to bound
         * the forward queue.
         */
        if (p->arrivals[m.team] == 0)
            p->arrival_epoch[m.team] = m.epoch;
        else if (p->arrival_epoch[m.team] != m.epoch)
            osoe_fatal("team %u has arrivals for epochs %u and %u at once; collective "
                       "epochs have diverged", m.team, p->arrival_epoch[m.team], m.epoch);
        p->arrivals[m.team]++;
        p->barrier_arrivals++;
        if (p->arrivals[m.team] == p->participants[m.team]) {
            p->arrivals[m.team] = 0;
            /*
             * Release everyone. A handler may enqueue but never reserve (§3.7),
             * so each peer's release goes onto its control queue and the drain
             * step writes it; this node's own release is applied here, because
             * it is not a message.
             */
            for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
                struct osoe_reply r = { .kind = OSOE_REPLY_RELEASE,
                                        .origin_pe = m.team, .length = 0,
                                        .value = m.epoch };
                enqueue(p, slot, &p->connection[slot].queues.control, &r);
            }
            p->host.release(p->host.context, m.team, m.epoch);
        }
        break;
    }

    case OSOE_AM_BARRIER_RELEASE: {
        struct osoe_am_barrier m;
        memcpy(&m, fixed, sizeof m);
        /*
         * Applied by the progress thread, which is §5.3's rule: it advances the
         * team's released counter and wakes all local waiters. A divisible
         * event, so wake-many (§3.11).
         */
        p->host.release(p->host.context, m.team, m.epoch);
        break;
    }

    case OSOE_AM_RMW_REPLY: {
        struct osoe_am_rmw_reply m;
        memcpy(&m, fixed, sizeof m);
        uint32_t capacity = 0;
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe,
                                                         &capacity);
        if (!slot)
            osoe_fatal("an atomic's reply from node %u names PE %u, which is not on this "
                       "node", peer_of(p, a->slot), m.origin_pe);
        uint64_t armed = atomic_load_explicit(&slot->armed_token, memory_order_acquire);
        if (m.token != armed)
            osoe_fatal("an atomic's reply from node %u for PE %u carries token %llu; that "
                       "PE is waiting for %llu", peer_of(p, a->slot), m.origin_pe,
                       (unsigned long long)m.token, (unsigned long long)armed);
        atomic_store_explicit(&slot->value, m.value, memory_order_release);
        atomic_store_explicit(&slot->token, m.token, memory_order_release);
        land(slot);
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

    /*
     * Applied, not merely received: §4.6 makes that the difference, because a
     * peer's acknowledgement is the origin's evidence that its writes are in
     * place. Recording it here - after every message of the segment has been
     * applied - is what makes the acknowledgement mean what quiet needs.
     */
    c->applied = segment->seq;
    if (segment->flags & OSOE_AM_SEGMENT_ACK_REQUESTED)
        c->owe_ack = true;
    if (segment->ack > c->acked) {
        c->acked = segment->ack;
        /* Publish it where a waiting PE can see it: quiet runs in whichever
         * process issued the writes, which is usually not this one (§4.6). */
        struct osoe_connection *conn = osoe_connection_to(p->area,
                                          a->slot < p->node ? a->slot : a->slot + 1);
        if (conn)
            osoe_connection_publish_acked(conn, c->acked);
    }
}

/* ---- Receiving -------------------------------------------------------- */

static bool receive(struct osoe_progress *p, uint32_t slot)
{
    struct arrival a = { p, slot };
    struct osoe_am_sink sink = { destination, complete, segment_applied, &a };
    bool moved = false;
    for (;;) {
        size_t n = osoe_wire_recv(&p->wire[slot], p->scratch, p->scratch_bytes);
        if (n == 0)
            return moved;
        osoe_am_parse(&p->connection[slot].parser, p->scratch, n, &sink);
        moved = true;
    }
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

static bool reserve_for_reply(struct osoe_connection *conn, struct drain_target *t,
                              uint32_t len, struct osoe_reservation *r)
{
    if (t->private_buffer == OSOE_CONNECTION_NONE)
        return osoe_connection_reserve(conn, len, r) == OSOE_RESERVATION_OK;
    if (len > t->budget || !osoe_connection_drain_reserve(conn, t->private_buffer, len, r))
        return false;
    t->budget -= len;
    return true;
}

static bool drain_into(struct osoe_progress *p, uint32_t slot, struct osoe_connection *conn,
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
        uint32_t fixed_bytes = barrier ? (uint32_t)sizeof(struct osoe_am_barrier)
                                       : (uint32_t)sizeof(struct osoe_am_rmw_reply);
        if (!reserve_for_reply(conn, t, osoe_message_padded(fixed_bytes), &resv))
            return moved;                   /* no room: try again next pass */
        uint16_t kind = r->kind == OSOE_REPLY_RELEASE ? OSOE_AM_BARRIER_RELEASE
                      : r->kind == OSOE_REPLY_ARRIVE  ? OSOE_AM_BARRIER_ARRIVE
                                                      : OSOE_AM_RMW_REPLY;
        struct osoe_message_header h = { kind, 0, fixed_bytes };
        unsigned char *at = osoe_reservation_bytes(conn, &resv);
        memcpy(at, &h, sizeof h);
        if (barrier) {
            struct osoe_am_barrier m = { .team = r->origin_pe, .epoch = (uint32_t)r->value,
                                         .from_node = p->node };
            memcpy(at + sizeof h, &m, sizeof m);
        } else {
            struct osoe_am_rmw_reply m = { .origin_pe = r->origin_pe, .token = r->token,
                                           .value = r->value };
            memcpy(at + sizeof h, &m, sizeof m);
        }
        osoe_connection_commit(conn, &resv);
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
        if (len > osoe_connection_payload_capacity(conn)) {
            /* The buffer cannot hold this chunk even empty: shrink it to what
             * fits rather than deadlocking on a message that never fits. */
            uint32_t room = osoe_connection_payload_capacity(conn) -
                            (uint32_t)sizeof(struct osoe_message_header) -
                            (uint32_t)sizeof(struct osoe_am_get_reply);
            chunk = room & ~UINT32_C(7);
            len = osoe_message_padded((uint32_t)sizeof(struct osoe_am_get_reply) + chunk);
        }
        struct osoe_reservation resv;
        if (!reserve_for_reply(conn, t, len, &resv))
            return moved;
        const void *source = p->host.resolve(p->host.context, r->target_pe, r->region,
                                             r->offset + r->sent, chunk);
        struct osoe_message_header h = { OSOE_AM_GET_REPLY, 0,
                                         (uint32_t)sizeof(struct osoe_am_get_reply) + chunk };
        struct osoe_am_get_reply m = { .origin_pe = r->origin_pe,
                                       .chunk_offset = r->sent, .total = r->length,
                                       .token = r->token };
        unsigned char *at = osoe_reservation_bytes(conn, &resv);
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, &m, sizeof m);
        if (source)
            memcpy(at + sizeof h + sizeof m, source, chunk);
        else
            memset(at + sizeof h + sizeof m, 0, chunk);
        osoe_connection_commit(conn, &resv);
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
    struct osoe_connection *conn = osoe_connection_to(p->area,
                                                      slot < p->node ? slot : slot + 1);
    if (!conn || c->open == OSOE_CONNECTION_NONE)
        return false;
    struct drain_target open = { OSOE_CONNECTION_NONE, 0 };
    return drain_into(p, slot, conn, &open);
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

static void finalize(struct osoe_progress *p, uint32_t slot, struct osoe_connection *conn,
                     uint32_t k, uint32_t bytes, uint64_t wanted)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    unsigned char *base = conn->data + (size_t)k * conn->capacity;
    uint32_t count = 0;
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
    }
    uint64_t seq = atomic_load_explicit(&conn->s->buffer[k].seq, memory_order_acquire);
    bool ask = asks_for_ack(c, seq, wanted);
    struct osoe_am_segment_header sh = {
        .seq = seq,
        .ack = c->applied,
        .flags = ask ? OSOE_AM_SEGMENT_ACK_REQUESTED : 0,
        .message_count = count,
        .total_len = bytes,
    };
    memcpy(base, &sh, sizeof sh);
    if (ask)
        c->flagged = seq;
    c->owe_ack = false;         /* this segment's `ack` pays any debt */
}

static bool flush(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_connection *conn = osoe_connection_to(p->area,
                                                      slot < p->node ? slot : slot + 1);
    if (!conn)
        return false;
    bool moved = false;

    /*
     * What this node's writers want acknowledged (§3.4), read once a pass and
     * before the seal below, as the flag it decides is. A request raised after
     * this load is seen by the next pass; the late-request rule in
     * asks_for_ack is what makes that safe.
     */
    uint64_t wanted = osoe_connection_ack_wanted(conn);
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);

    /* Finish an in-flight send. A short write is the normal case (§3.3): keep
     * the remainder and offer it again. */
    if (c->sending != OSOE_CONNECTION_NONE) {
        unsigned char *base = conn->data + (size_t)c->sending * conn->capacity;
        size_t n = osoe_wire_send(&p->wire[slot], base + c->sent,
                                  c->sending_bytes - c->sent);
        c->sent += (uint32_t)n;
        if (n > 0)
            moved = true;
        if (c->sent == c->sending_bytes) {
            osoe_connection_release(conn, c->sending);
            c->state[c->sending] = OSOE_BUFFER_FREE;
            c->sending = OSOE_CONNECTION_NONE;
            osoe_counter_word_add(&p->metrics->segments_sent, 1);
            moved = true;
        }
    }

    /* Promote a sealed buffer once every writer in it has committed. Never
     * spin waiting: a writer may be mid-copy, and at 256 PEs a spin here is a
     * hang (§3.5). */
    if (c->sending == OSOE_CONNECTION_NONE && c->sealed != OSOE_CONNECTION_NONE) {
        uint32_t bytes;
        if (osoe_connection_sendable(conn, c->sealed, &bytes)) {
            finalize(p, slot, conn, c->sealed, bytes, wanted);
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
    if (c->sealed == OSOE_CONNECTION_NONE && c->open != OSOE_CONNECTION_NONE) {
        uint32_t used = OSOE_BUFFER_USED(
            atomic_load_explicit(&conn->s->buffer[c->open].head, memory_order_acquire));
        /* Seal for data, to pay a debt, or for a late request that no
         * unfinalized segment will carry - the one header-only segment the
         * model says a quiesced pair may still exchange. */
        bool late = wanted > c->acked && c->flagged < wanted;
        if (used > header || c->owe_ack || late) {
            if (used == header)
                osoe_counter_word_add(&p->metrics->header_only_sent, 1);   /* §3.4's ack-only */
            osoe_connection_seal(conn, c->open);
            c->state[c->open] = OSOE_BUFFER_SEALED_STATE;
            c->sealed = c->open;
            c->open = OSOE_CONNECTION_NONE;
            moved = true;
        }
    }

    /* At most one reservable buffer (§3.5): open a free one only when none is
     * open, and never the one being sealed or sent. */
    if (c->open == OSOE_CONNECTION_NONE) {
        for (uint32_t k = 0; k < 2; k++)
            if (c->state[k] == OSOE_BUFFER_FREE && k != c->sealed && k != c->sending) {
                /* The pre-open drain (§3.8): replies first, while k is still
                 * sealed and no writer can reach it. */
                c->state[k] = OSOE_BUFFER_DRAINING;
                struct drain_target preopen = { k, conn->capacity / 2 };
                drain_into(p, slot, conn, &preopen);
                osoe_connection_open(conn, k, c->next_seq++);
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
                        struct osoe_wire *wires, struct osoe_reply *descriptors,
                        uint32_t descriptor_count, unsigned char *scratch,
                        uint32_t scratch_bytes, const struct osoe_progress_host *host)
{
    memset(p, 0, sizeof *p);
    p->area = area;
    p->wire = wires;
    p->descriptors = descriptors;
    p->scratch = scratch;
    p->scratch_bytes = scratch_bytes;
    p->node = area->node;
    p->nodes = area->nodes;
    p->host = *host;
    p->metrics = &p->own_metrics;
    osoe_reply_pool_init(&p->pool, descriptors, descriptor_count);
}

void osoe_progress_attach(struct osoe_progress *p,
                          struct osoe_progress_connection *connections)
{
    p->connection = connections;
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

bool osoe_progress_poll(struct osoe_progress *p)
{
    bool moved = false;
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        moved |= receive(p, slot);
        moved |= drain(p, slot);
        moved |= flush(p, slot);
    }
    return moved;
}

bool osoe_progress_idle(struct osoe_progress *p)
{
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
        struct osoe_progress_connection *c = &p->connection[slot];
        if (c->sending != OSOE_CONNECTION_NONE || c->sealed != OSOE_CONNECTION_NONE)
            return false;
        if (c->queues.control.depth != 0 || c->queues.bulk.depth != 0)
            return false;
        struct osoe_connection *conn = osoe_connection_to(p->area,
                                                          slot < p->node ? slot : slot + 1);
        if (!conn || c->open == OSOE_CONNECTION_NONE)
            continue;
        /* Bytes a writer has committed into the open buffer that no segment
         * has carried yet. Reserved-but-uncommitted is not possible here: every
         * writer has passed shmem_finalize's barrier. */
        uint32_t used = OSOE_BUFFER_USED(
            atomic_load_explicit(&conn->s->buffer[c->open].head, memory_order_acquire));
        if (used > header)
            return false;
    }
    return true;
}

/*
 * **Why the internode phase is not expressed over the §4.10 boundary.**
 *
 * §10 left open whether a collective's internode phase should cross the
 * boundary as a mandatory osoe_transport_sync(ctx, group), and scheduled the
 * decision for the step that writes its only caller. Writing it, two obstacles
 * are concrete rather than estimated.
 *
 * The first is addressing. §4.10's entire model is (pe, region, offset) - its
 * opening sentence, and invariant 20 - while this phase is addressed to a
 * *node*, and the counter it moves is the node's, shared by every local member
 * of the team. A sync over a group of nodes is not one more operation in that
 * vocabulary; it is a second addressing model beside it.
 *
 * The second is the release. §5.4 makes each epoch counter *its own futex
 * word*, because the kernel implements only 32-bit futexes, and a member waits
 * on the counter itself. Expressing the release as an rmw would need the
 * backend to know that this particular word has futex waiters on it - which
 * §4.10's notify does not mean: notify is §4.9's watch mechanism, over watched
 * ranges, and the collective's waiters hold no watch. Making them hold one
 * would rewrite §5.4's wait, which is where the bounded-skew argument lives.
 *
 * So the cost is not the fifteen lines §10 estimated. §2.2 put collectives on
 * AM deliberately and §4.10 keeps them outside the boundary deliberately, and
 * having now written the phase, that remains the right call: the boundary stays
 * point-to-point and PE-addressed, and a backend that wants a native group
 * barrier implements §5's internode phase rather than growing a new primitive.
 * Recorded in §10 with this reasoning.
 */
void osoe_progress_arrive(struct osoe_progress *p, uint32_t team, uint32_t epoch,
                          uint32_t root, uint32_t participants)
{
    if (team >= OSOE_TEAMS)
        osoe_fatal("a barrier arrival names team %u; this build has %u", team,
                   OSOE_TEAMS);
    p->participants[team] = participants;

    if (p->node == root) {
        /* No message to send: this node is where the arrivals are counted. */
        if (p->arrivals[team] == 0)
            p->arrival_epoch[team] = epoch;
        p->arrivals[team]++;
        p->barrier_arrivals++;
        if (p->arrivals[team] == participants) {
            p->arrivals[team] = 0;
            for (uint32_t slot = 0; slot + 1 < p->nodes; slot++) {
                struct osoe_reply r = { .kind = OSOE_REPLY_RELEASE, .origin_pe = team,
                                        .length = 0, .value = epoch };
                enqueue(p, slot, &p->connection[slot].queues.control, &r);
            }
            p->host.release(p->host.context, team, epoch);
        }
        return;
    }

    /*
     * Reserve it onto the root's connection. This runs on the lead's agent -
     * the same thread that will send it - so the reservation below can never
     * wait on a seal only this thread performs. The loop is kept because
     * osoe_connection_reserve is fallible by contract (§3.5); it cannot spin
     * here, since no other buffer state is reachable from inside this pass.
     */
    uint32_t slot = osoe_progress_slot(p, root);
    if (slot == OSOE_CONNECTION_NONE || !osoe_connection_to(p->area, root))
        osoe_fatal("no connection to the collective root, node %u", root);
    struct osoe_reply r = { .kind = OSOE_REPLY_ARRIVE, .origin_pe = team,
                            .length = 0, .value = epoch };
    enqueue(p, slot, &p->connection[slot].queues.control, &r);
}

uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer)
{
    uint32_t slot = osoe_progress_slot(p, peer);
    return slot == OSOE_CONNECTION_NONE ? 0 : p->connection[slot].acked;
}
