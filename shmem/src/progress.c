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
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe);
        if (!slot)
            return NULL;            /* not ours: the bytes are consumed and dropped */
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
static void enqueue(struct osoe_progress *p, struct osoe_reply_queue *q,
                    const struct osoe_reply *value)
{
    uint32_t i = osoe_reply_take(&p->pool);
    if (i == OSOE_REPLY_NONE) {
        p->reply_reservation_failures++;
        return;
    }
    struct osoe_reply *r = osoe_reply_at(&p->pool, i);
    *r = *value;
    osoe_reply_push(&p->pool, q, i);
}

/*
 * A collective's arrival or release, which must not be dropped.
 *
 * §3.8 makes an exhausted pool the fallible path for a *reply*: the origin's
 * credit bounds what can be outstanding, so running out means that distributed
 * invariant is broken, and reply.h already says such a break is "a hard
 * assertion naming the peer, not a silent drop". For a collective it is worse
 * than data loss. A dropped arrival means the root waits for a node that will
 * never arrive and a dropped release means every member of a node waits on an
 * epoch counter nothing will advance - in both cases a silent hang whose
 * symptom appears on the other nodes.
 */
static void enqueue_collective(struct osoe_progress *p, struct osoe_reply_queue *q,
                               const struct osoe_reply *value, uint32_t peer)
{
    uint32_t i = osoe_reply_take(&p->pool);
    if (i == OSOE_REPLY_NONE)
        osoe_fatal("the reply descriptor pool is empty while forwarding a collective "
                   "toward node %u; §3.8's per-connection credit is not holding", peer);
    struct osoe_reply *r = osoe_reply_at(&p->pool, i);
    *r = *value;
    osoe_reply_push(&p->pool, q, i);
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
    p->messages_applied++;

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
        enqueue(p, &c->queues.bulk, &r);
        break;
    }

    case OSOE_AM_RMW_REQUEST: {
        struct osoe_am_rmw_request m;
        memcpy(&m, fixed, sizeof m);
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
            enqueue(p, &c->queues.control, &r);
        }
        break;
    }

    case OSOE_AM_GET_REPLY: {
        struct osoe_am_get_reply m;
        memcpy(&m, fixed, sizeof m);
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe);
        if (!slot)
            break;
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
                enqueue(p, &p->connection[slot].queues.control, &r);
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
        struct osoe_landing_slot *slot = p->host.landing(p->host.context, m.origin_pe);
        if (!slot)
            break;
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
    p->segments_applied++;

    /*
     * Applied, not merely received: §4.6 makes that the difference, because a
     * peer's acknowledgement is the origin's evidence that its writes are in
     * place. Recording it here - after every message of the segment has been
     * applied - is what makes the acknowledgement mean what quiet needs.
     */
    c->applied = segment->seq;
    if (segment->flags & OSOE_AM_SEGMENT_ACK_REQUESTED)
        c->owe_ack = true;
    if (segment->ack > c->acked)
        c->acked = segment->ack;
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
static bool drain(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_connection *conn = osoe_connection_to(p->area, 
                                                      slot < p->node ? slot : slot + 1);
    if (!conn || c->open == OSOE_CONNECTION_NONE)
        return false;
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
        if (osoe_connection_reserve(conn, osoe_message_padded(fixed_bytes), &resv) !=
            OSOE_RESERVATION_OK)
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
        if (osoe_connection_reserve(conn, len, &resv) != OSOE_RESERVATION_OK)
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

/* ---- Sealing and sending ---------------------------------------------- */

/*
 * Fill in the segment header at seal time (§3.4): the sequence number, the
 * acknowledgement of the reverse direction, the flags, and the two deliberately
 * redundant fields the receiver cross-checks. The message count can only be had
 * by walking the buffer - the writers are in other processes, so the progress
 * thread never saw their reservations.
 */
static void finalize(struct osoe_progress *p, uint32_t slot, struct osoe_connection *conn,
                     uint32_t k, uint32_t bytes)
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
    struct osoe_am_segment_header sh = {
        .seq = atomic_load_explicit(&conn->s->buffer[k].seq, memory_order_acquire),
        .ack = c->applied,
        .flags = c->owe_ack ? OSOE_AM_SEGMENT_ACK_REQUESTED : 0,
        .message_count = count,
        .total_len = bytes,
    };
    memcpy(base, &sh, sizeof sh);
    c->owe_ack = false;
}

static bool flush(struct osoe_progress *p, uint32_t slot)
{
    struct osoe_progress_connection *c = &p->connection[slot];
    struct osoe_connection *conn = osoe_connection_to(p->area,
                                                      slot < p->node ? slot : slot + 1);
    if (!conn)
        return false;
    bool moved = false;
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
            p->segments_sent++;
            moved = true;
        }
    }

    /* Promote a sealed buffer once every writer in it has committed. Never
     * spin waiting: a writer may be mid-copy, and at 256 PEs a spin here is a
     * hang (§3.5). */
    if (c->sending == OSOE_CONNECTION_NONE && c->sealed != OSOE_CONNECTION_NONE) {
        uint32_t bytes;
        if (osoe_connection_sendable(conn, c->sealed, &bytes)) {
            finalize(p, slot, conn, c->sealed, bytes);
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
        if (used > header || c->owe_ack) {
            if (used == header)
                p->acks_only++;     /* a header-only segment: §3.4's ack-only */
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
        osoe_am_parser_init(&c->parser);
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

void osoe_progress_forget_acks(struct osoe_progress *p)
{
    for (uint32_t slot = 0; slot + 1 < p->nodes; slot++)
        p->connection[slot].owe_ack = false;
}

void osoe_progress_request_ack(struct osoe_progress *p, uint32_t peer)
{
    uint32_t slot = osoe_progress_slot(p, peer);
    if (slot != OSOE_CONNECTION_NONE)
        p->connection[slot].owe_ack = true;
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
                enqueue_collective(p, &p->connection[slot].queues.control, &r,
                                   slot < p->node ? slot : slot + 1);
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
    enqueue_collective(p, &p->connection[slot].queues.control, &r, root);
}

uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer)
{
    uint32_t slot = osoe_progress_slot(p, peer);
    return slot == OSOE_CONNECTION_NONE ? 0 : p->connection[slot].acked;
}
