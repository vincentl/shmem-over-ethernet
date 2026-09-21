/*
 * The transport boundary (§4.10): dispatch on locality, and the far half.
 *
 * This file owns §4.10's entry points. Each operation goes one of two ways on
 * the answer §8.1's single locality function gives: a PE reachable by ordinary
 * loads and stores takes the intranode path (transport-local.h) and is applied
 * in the calling thread; every other PE goes over the AM layer of §3.
 *
 * **That is one backend with two paths, not two backends.** §4.10 chooses
 * backends at build time and has no table of function pointers, and this keeps
 * both properties: the branch below is on where a PE *is*, which is a fact
 * about the job rather than a configuration, and it is the same branch
 * osoe_transport_direct already answers for shmem_ptr. A job of one logical
 * node never takes the far path at all.
 *
 * **What crosses, and what does not.** An application thread reserves into a
 * connection buffer and copies its own bytes (§3.5, §3.12) - that much it may
 * do, and it is what keeps the data path off the lead's critical path. What it
 * must never do is touch the progress thread's state or a wire. So the far half
 * here is: reserve, copy, commit, record, wake the lead. The lead's agent seals
 * and sends (§3.9), and the peer's agent applies.
 *
 * **Completion evidence is the peer's acknowledgement** (§4.6). A put returns
 * at local completion - src is reusable at once - and the bytes are complete at
 * the target only once a wait covering them returns. The dirty set below is
 * what makes that wait cost the application's communication degree rather than
 * the job's size.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "am.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "base/order.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "blocked.h"
#include "connection-area.h"
#include "fatal.h"
#include "job.h"
#include "metrics.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "transport-local.h"

/*
 * Per-context ordering state (§4.6).
 *
 * The dirty set is the peer nodes this context has written to since its last
 * wait, with the highest segment sequence it put a write into for each. quiet
 * waits for each of those peers to have acknowledged up to that sequence, and
 * fence flushes them without clearing the set, because those operations remain
 * unacknowledged.
 *
 * 1.6 change #18 limits fence to operations issued on the **same context**
 * (§9.12.1), which is the weakest thing the spec asks for and exactly what a
 * per-context set delivers. Indexed by peer slot, so its cost is node count and
 * never PE count (§1); `order` keeps the touched slots so a wait scans the
 * application's communication degree - a halo exchange scans six peers whatever
 * the job size - rather than every connection.
 */
struct osoe_transport_context {
    uint64_t *high_segment;     /* per peer slot: the highest segment written into */
    uint32_t *order;            /* the slots touched, in the order first touched */
    bool *touched;              /* membership, so marking twice is not quadratic */
    uint32_t count;
};

static struct osoe_transport_context default_context;

/* The peer slot for a node, as the connection area and the progress loop both
 * index it: node order with this node left out (§3.4). */
static uint32_t peer_slot(uint32_t peer)
{
    return peer < osoe.job.node ? peer : peer - 1;
}

static uint32_t peer_count(void)
{
    return osoe.job.node_count - 1;
}

void osoe_transport_init(void)
{
    struct osoe_transport_context *c = &default_context;
    *c = (struct osoe_transport_context){ 0 };
    if (!osoe.internode)
        return;
    uint32_t peers = peer_count();
    c->high_segment = calloc(peers, sizeof *c->high_segment);
    c->order = calloc(peers, sizeof *c->order);
    c->touched = calloc(peers, sizeof *c->touched);
    if (!c->high_segment || !c->order || !c->touched)
        osoe_fatal("out of memory for the ordering state of %u peer nodes", peers);
}

void osoe_transport_fini(void)
{
    struct osoe_transport_context *c = &default_context;
    free(c->high_segment);
    free(c->order);
    free(c->touched);
    *c = (struct osoe_transport_context){ 0 };
}

struct osoe_transport_context *osoe_transport_context_default(void)
{
    return &default_context;
}

/*
 * §8.1's single locality function, seen from the boundary (§4.10).
 *
 * Non-NULL says this backend orders the calling thread's stores through the
 * address against its own writes by the next fence or wait (§4.6), which is
 * true exactly of the intranode path, since it applies every write in the
 * calling thread. So the answer is NULL for a PE on another logical node -
 * whose heap this process never mapped, since PEs rendezvous only with the PEs
 * in their own node's OSOE_GTIDS - and also for a local PE under §8.1's
 * force-remote flag, which denies the pointer to a node-mate without changing
 * the route (§8.1) - so that a program cannot be written against the
 * shared-memory shortcut and then quietly depend on it.
 *
 * shmem_ptr, shmem_team_ptr and shmem_addr_accessible consult this and nothing
 * else (§4.8). Which PEs are actually carried over the AM layer is decided by
 * the node map, not by the flag.
 */
void *osoe_transport_direct(int pe, enum osoe_region region, uint64_t offset)
{
    if (region != OSOE_REGION_HEAP)
        return NULL;
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size)
        return NULL;
    if (!osoe_is_local(&osoe.job, (uint32_t)pe))
        return NULL;
    int32_t local = osoe.node.local_of_rank[pe];
    if (local < 0)
        return NULL;
    return (char *)osoe.node.heap_of[local] + offset;
}

/* ---- The far half ------------------------------------------------------ */

/* Record that this context has written into `seq` toward `peer` (§4.6). */
static void mark(struct osoe_transport_context *c, uint32_t peer, uint64_t seq)
{
    uint32_t slot = peer_slot(peer);
    if (!c->touched[slot]) {
        c->touched[slot] = true;
        c->order[c->count++] = slot;
    }
    if (seq > c->high_segment[slot])
        c->high_segment[slot] = seq;
}

/*
 * One blocking wait in this backend, and §4.10's contract for it: sweep at
 * least every OSOE_SWEEP_NS - read the abort word, check the local PEs'
 * liveness, count pe.sweeps - publish a "blocked in" record naming the routine
 * and the peer node, and give up only by the no-progress rule (§3.15): never,
 * by default, and otherwise when the word the wait watches has not moved for
 * OSOE_NO_PROGRESS_TIMEOUT. Before slice 2 none of the four loops below swept
 * or published anything, and each gave up after the *bootstrap* timeout, so a
 * dead sibling went unnoticed for 60 s and a healthy quiet on a slow network
 * was killed (second review, D2 and D3).
 */
struct am_wait {
    const char *routine;
    const char *watching;          /* what the progress word is, for the message */
    uint32_t peer;                 /* the node waited on */
    int pe;                        /* the target PE, or -1 when the wait is node-wide */
    struct osoe_deadline sweep;
    struct osoe_stall_watch watch;
};

/* `pe` is the specific PE this wait is for, or -1 when there is none - a
 * quiet's wait is over a whole node's queue, not one PE's (§4.10, D-6). */
static void am_wait_begin(struct am_wait *w, const char *routine, const char *watching,
                          uint32_t peer, int pe, uint64_t progress)
{
    w->routine = routine;
    w->watching = watching;
    w->peer = peer;
    w->pe = pe;
    w->sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    osoe_stall_watch_begin(&w->watch, progress, osoe.bootstrap.no_progress_ns);
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_NODE, peer);
}

/* Called on every turn of a wait's loop with the progress word as it stands.
 * Sweeps if the sweep interval has passed; fatal only on no progress. */
static void am_wait_check(struct am_wait *w, uint64_t progress)
{
    if (!osoe_deadline_expired(&w->sweep))
        return;
    w->sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    osoe_sweep_expired(w->routine);
    if (!osoe_stall_watch_stalled(&w->watch, progress))
        return;
    if (w->pe >= 0)
        osoe_fatal("%s: no progress toward node %u (PE %d) for %llu s "
                   "(OSOE_NO_PROGRESS_TIMEOUT): %s has stayed at %llu", w->routine, w->peer,
                   w->pe, (unsigned long long)osoe_stall_watch_seconds(&w->watch), w->watching,
                   (unsigned long long)progress);
    else
        osoe_fatal("%s: no progress toward node %u for %llu s (OSOE_NO_PROGRESS_TIMEOUT): "
                   "%s has stayed at %llu", w->routine, w->peer,
                   (unsigned long long)osoe_stall_watch_seconds(&w->watch), w->watching,
                   (unsigned long long)progress);
}

static void am_wait_end(void)
{
    osoe_blocked_leave();
}

/*
 * The re-wake polling in the loops below: wake the lead, sleep 20 us, look
 * again. A step-2 stand-in, kept until step 3 gives these waits a word the lead
 * bumps when it frees a buffer, returns a credit or lands an acknowledgement.
 * At 256 PEs in one quiet it is 256 locked adds and 256 syscalls per period on
 * one line (second review, atomics F3); it must not survive step 3.
 */
static void poll_lead(void)
{
    osoe_progress_host_wake_lead();
    struct timespec back = { 0, 20000 };        /* 20 us */
    nanosleep(&back, NULL);
}

/* How far a connection has turned over: the newest sequence either buffer has
 * been opened with. It rises every time the lead frees and reopens a buffer,
 * which is the progress a writer waiting for room is waiting on. */
static uint64_t turnover(const struct osoe_connection *conn)
{
    uint64_t a = atomic_load_explicit(&conn->s->buffer[0].seq, memory_order_acquire);
    uint64_t b = atomic_load_explicit(&conn->s->buffer[1].seq, memory_order_acquire);
    return a > b ? a : b;
}

/*
 * Reserve, waiting for room.
 *
 * Only the lead's agent frees space here, by sealing the buffer and sending it
 * (§3.5, §3.12), so a writer that finds none asks for a seal and tries again.
 * **It must not spin without asking**: the lead may be asleep, and the only
 * thing that wakes it for this connection is the bump below.
 *
 * §7.1 step 2 backs off with a short sleep rather than blocking on a word the
 * lead bumps when it frees a buffer. That word would be a second wake protocol,
 * and this step's policy is slow mode first (§3.14) - so the cost is a
 * scheduler round trip on a full buffer, counted by am.reserve_stalls so that
 * it is visible rather than assumed rare. A buffer that stays full is a stall
 * the counter names; step 3 is where it becomes a wait.
 */
static void reserve_waiting(struct osoe_connection *conn, uint32_t peer, int pe,
                            const char *routine, uint32_t len, struct osoe_reservation *r)
{
    if (osoe_connection_reserve(conn, len, r) == OSOE_RESERVATION_OK)
        return;
    osoe_count(OSOE_C_AM_RESERVE_STALLS, 1);
    struct am_wait w;
    am_wait_begin(&w, routine, "the connection's buffer turnover", peer, pe, turnover(conn));
    for (;;) {
        poll_lead();
        if (osoe_connection_reserve(conn, len, r) == OSOE_RESERVATION_OK)
            break;
        am_wait_check(&w, turnover(conn));
    }
    am_wait_end();
}

/*
 * The largest payload one message of this kind may carry.
 *
 * An AM chunk is §3.2's fragmentation size and so the largest message the wire
 * ever carries; the connection buffer must also be able to hold it whole, or a
 * message would be reserved that never fits and the writer would wait forever
 * for room that cannot exist. Taking the smaller of the two is what makes a
 * small OSOE_AM_BUFFER a slow configuration rather than a hanging one.
 */
static uint32_t payload_limit(struct osoe_connection *conn, uint32_t peer, uint32_t fixed_bytes)
{
    uint32_t chunk = (uint32_t)osoe.area.chunk_bytes;
    uint32_t room = osoe_connection_payload_capacity(conn);
    if (room < chunk)
        chunk = room;
    uint32_t overhead = (uint32_t)sizeof(struct osoe_message_header) + fixed_bytes;
    if (chunk <= overhead)
        osoe_fatal("toward node %u: a connection buffer of %llu bytes cannot carry a "
                   "message with a %u-byte fixed part; raise OSOE_AM_BUFFER or OSOE_AM_CHUNK",
                   peer, (unsigned long long)osoe.area.buffer_bytes, fixed_bytes);
    return chunk - overhead;
}

static void remote_put(struct osoe_transport_context *c, int pe, enum osoe_region region,
                       uint64_t offset, const void *src, size_t len, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *conn = osoe_connection_to(&osoe.area, peer);
    if (!conn)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);

    uint32_t limit = payload_limit(conn, peer, (uint32_t)sizeof(struct osoe_am_put));
    const unsigned char *at = src;
    /* A zero-length put is a no-op the spec still allows to be issued; it must
     * not become a message, or the peer applies an empty write and the loop
     * below never terminates. */
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        struct osoe_am_put m = { .target_pe = (uint32_t)pe, .region = (uint16_t)region,
                                 .flags = (uint16_t)flags, .offset = offset };
        uint32_t body = (uint32_t)sizeof m + n;
        struct osoe_message_header h = { OSOE_AM_PUT, 0, body };
        struct osoe_reservation r;
        reserve_waiting(conn, peer, pe, "shmem_put", osoe_message_padded(body), &r);
        unsigned char *to = osoe_reservation_bytes(conn, &r);
        memcpy(to, &h, sizeof h);
        memcpy(to + sizeof h, &m, sizeof m);
        memcpy(to + sizeof h + sizeof m, at, n);
        osoe_connection_commit(conn, &r);
        mark(c, peer, r.seq);
        at += n;
        offset += n;
        len -= n;
    }
    /*
     * §3.12: ask the lead for a wake. Once, after the whole operation, because
     * one wake covers every segment it left behind.
     *
     * **OSOE_TRANSPORT_MORE defers this wake, and nothing else** (§4.10). The
     * bytes are already copied and committed, so `src` is reusable and the
     * operation is as complete as a put ever is before its acknowledgement;
     * what is held back is only the futex wake asking the lead to run. The
     * next operation without the flag wakes it, and so does any fence, wait,
     * get or fetching rmw - and if none of those ever comes, the lead's own
     * sweep finds the committed bytes anyway, so the flag costs latency at
     * worst and never correctness. §3.9's aggregation self-clocks off the send
     * regardless, so this is a saving in syscalls rather than in segments.
     */
    if (flags & OSOE_TRANSPORT_MORE) {
        osoe_count(OSOE_C_AM_WAKES_DEFERRED, 1);
        return;
    }
    osoe_progress_host_wake_lead();
}

/*
 * This PE's landing zone, and the capacity of its payload area (§3.12).
 *
 * The zone holds one AM chunk, and a get request is therefore issued in pieces
 * no larger than that. **The origin chunks the request; the target does not
 * chunk beyond it.** A reply may still be split when the connection buffer is
 * tight - the drain sends one chunk per descriptor per pass (§3.8) - and every
 * piece then lands at an offset inside the range this request asked for, which
 * is what keeps `destination()` writing inside the slot.
 *
 * The alternative was to let one request cover the whole get and size the zone
 * for it, which is unbounded, or to flow-control the chunks, which needs a
 * handshake per chunk. Issuing sequentially costs a round trip per chunk and is
 * what "slow mode first" (§3.14) means here; pipelining the requests, and
 * landing straight into a symmetric `dest` where there is one, are the two
 * obvious ways to take that back.
 */
static struct osoe_landing_slot *my_landing(void)
{
    return osoe_node_landing(&osoe.node, osoe.bootstrap.local_id);
}

static uint32_t landing_capacity(void)
{
    return osoe.node.landing_slot_bytes - (uint32_t)sizeof(struct osoe_landing_slot);
}

/*
 * Take a reply credit for this connection, waiting if none is free.
 *
 * §4.6 permits blocking rather than failing, and §3.8 needs the bound kept at
 * the origin: this is what stops the node's PEs between them putting more
 * outstanding replies on a peer than its descriptor pool holds. A PE holds its
 * credit from issue until its reply *lands*, not until it is sent.
 */
static void take_credit(struct osoe_connection *conn, uint32_t peer, int pe, const char *routine)
{
    if (osoe_credit_take(osoe_connection_credit(conn)))
        return;
    /* Credits come back as replies land, so the connection's acknowledgements
     * are what moves while one is awaited: a peer applying nothing returns
     * nothing. */
    struct am_wait w;
    am_wait_begin(&w, routine, "the peer's acknowledged sequence", peer, pe,
                  osoe_connection_acked(conn));
    for (;;) {
        poll_lead();
        if (osoe_credit_take(osoe_connection_credit(conn)))
            break;
        am_wait_check(&w, osoe_connection_acked(conn));
    }
    am_wait_end();
}

/*
 * Wait for the reply carrying `token`, which is what §4.6 means by a blocking
 * get or fetching AMO being complete per operation: the caller waits for *its*
 * reply, not for a byte count. Under SERIALIZED a PE has one outstanding, so
 * the slot holds one completion - and the token is what makes "its" checkable
 * rather than assumed, since a reply for an abandoned operation is otherwise
 * indistinguishable from the one being waited for.
 *
 * The wake word is loaded **before** the flag it guards (§4.6), so a bump
 * between the load and the wait makes the wait return at once rather than sleep
 * on a value that has already moved.
 */
static void await_reply(struct osoe_landing_slot *slot, uint64_t token, uint32_t peer, int pe,
                        const char *routine)
{
    bool waiting = false;
    struct am_wait w;
    for (;;) {
        uint32_t seen = atomic_load_explicit(&slot->wake, memory_order_acquire);
        if (atomic_load_explicit(&slot->ready, memory_order_acquire) &&
            atomic_load_explicit(&slot->token, memory_order_acquire) == token)
            break;
        /* The slot's wake word is bumped by every landing, so it is also the
         * progress word: it stands still only while nothing arrives here. */
        if (!waiting) {
            am_wait_begin(&w, routine, "the landing slot's wake word", peer, pe, seen);
            waiting = true;
        }
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&slot->wake, seen, &sweep, OSOE_FUTEX_SHARED);
        am_wait_check(&w, atomic_load_explicit(&slot->wake, memory_order_acquire));
    }
    if (waiting)
        am_wait_end();
}

/* Arm the landing slot for one operation, and give it a token nothing else
 * this PE has used. */
static uint64_t arm(struct osoe_landing_slot *slot)
{
    static uint64_t next_token;
    atomic_store_explicit(&slot->ready, 0, memory_order_seq_cst);
    /* Published before the request goes out: the send's reservation and
     * commit order it before any reply can exist. */
    atomic_store_explicit(&slot->armed_token, ++next_token, memory_order_release);
    return next_token;
}

/* Reserve, fill and commit one message of `fixed_bytes` plus `payload` bytes. */
static uint64_t send_message(struct osoe_connection *conn, uint32_t peer, int pe,
                             const char *routine, uint16_t kind, const void *fixed,
                             uint32_t fixed_bytes, const void *payload, uint32_t payload_bytes)
{
    uint32_t body = fixed_bytes + payload_bytes;
    struct osoe_message_header h = { kind, 0, body };
    struct osoe_reservation r;
    reserve_waiting(conn, peer, pe, routine, osoe_message_padded(body), &r);
    unsigned char *to = osoe_reservation_bytes(conn, &r);
    memcpy(to, &h, sizeof h);
    memcpy(to + sizeof h, fixed, fixed_bytes);
    if (payload_bytes)
        memcpy(to + sizeof h + fixed_bytes, payload, payload_bytes);
    osoe_connection_commit(conn, &r);
    return r.seq;
}

static void remote_get(int pe, enum osoe_region region, uint64_t offset, void *dst,
                       size_t len)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *conn = osoe_connection_to(&osoe.area, peer);
    if (!conn)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    struct osoe_landing_slot *slot = my_landing();
    uint32_t limit = landing_capacity();
    if (limit > payload_limit(conn, peer, (uint32_t)sizeof(struct osoe_am_get_reply)))
        limit = payload_limit(conn, peer, (uint32_t)sizeof(struct osoe_am_get_reply));

    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        uint64_t token = arm(slot);
        take_credit(conn, peer, pe, "shmem_get");
        struct osoe_am_get_request m = { .target_pe = (uint32_t)pe,
                                         .origin_pe = osoe.bootstrap.rank,
                                         .region = (uint16_t)region, .flags = 0,
                                         .length = n, .offset = offset,
                                         .token = token };
        send_message(conn, peer, pe, "shmem_get", OSOE_AM_GET_REQUEST, &m, (uint32_t)sizeof m,
                     NULL, 0);
        osoe_progress_host_wake_lead();
        await_reply(slot, token, peer, pe, "shmem_get");
        uint32_t landed = atomic_load_explicit(&slot->length, memory_order_acquire);
        if (landed != n)
            osoe_fatal("a get of %u bytes from PE %d landed %u", n, pe, landed);
        memcpy(at, slot->data, n);
        /* Returned when the reply has *landed* (§3.8), which is here and not
         * when the target sent it: the bound being kept is on replies in
         * flight toward this node, not on replies the peer has finished with. */
        osoe_credit_give(osoe_connection_credit(conn));
        at += n;
        offset += n;
        len -= n;
    }
}

static uint64_t remote_rmw(struct osoe_transport_context *c, int pe,
                           enum osoe_region region, uint64_t offset,
                           enum osoe_rmw_op op, unsigned width, uint64_t operand,
                           uint64_t compare, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *conn = osoe_connection_to(&osoe.area, peer);
    if (!conn)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    bool fetching = (flags & OSOE_TRANSPORT_FETCH) != 0;
    struct osoe_landing_slot *slot = my_landing();
    uint64_t token = 0;

    if (fetching) {
        token = arm(slot);
        take_credit(conn, peer, pe, "shmem_atomic");
    }
    struct osoe_am_rmw_request m = { .target_pe = (uint32_t)pe,
                                     .origin_pe = osoe.bootstrap.rank,
                                     .region = (uint16_t)region, .op = (uint16_t)op,
                                     .width = width, .offset = offset,
                                     .operand = operand, .compare = compare,
                                     .token = token, .flags = flags };
    uint64_t seq = send_message(conn, peer, pe, "shmem_atomic", OSOE_AM_RMW_REQUEST, &m,
                                (uint32_t)sizeof m, NULL, 0);
    osoe_progress_host_wake_lead();
    if (!fetching) {
        /* §4.10: without FETCH an rmw completes as a put does, so it joins the
         * dirty set and a later quiet is what says it has been applied. */
        mark(c, peer, seq);
        return 0;
    }
    await_reply(slot, token, peer, pe, "shmem_atomic");
    uint64_t previous = atomic_load_explicit(&slot->value, memory_order_acquire);
    osoe_credit_give(osoe_connection_credit(conn));
    return previous;
}

/* ---- §4.10's data operations ------------------------------------------- */

void osoe_transport_put(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags)
{
    if (osoe_is_local(&osoe.job, (uint32_t)pe)) {
        osoe_transport_local_put(pe, region, offset, src, len, flags);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_PUT_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_PUT_BYTES, len);
    remote_put(context, pe, region, offset, src, len, flags);
}

void osoe_transport_get(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len)
{
    (void)context;
    if (osoe_is_local(&osoe.job, (uint32_t)pe)) {
        osoe_transport_local_get(pe, region, offset, dst, len);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_GET_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_GET_BYTES, len);
    /* A blocking get is complete on return, so it never joins the dirty set:
     * §4.6 makes gets a quiet-only concern and there is nothing left for a
     * quiet to wait for once the bytes are in dst. */
    remote_get(pe, region, offset, dst, len);
}

uint64_t osoe_transport_rmw(struct osoe_transport_context *context, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags)
{
    if (osoe_is_local(&osoe.job, (uint32_t)pe))
        return osoe_transport_local_rmw(pe, region, offset, op, width, operand, compare,
                                        flags);
    osoe_count(OSOE_C_TRANSPORT_RMW_CALLS, 1);
    if (flags & OSOE_TRANSPORT_FETCH)
        osoe_count(OSOE_C_TRANSPORT_RMW_FETCHING, 1);
    return remote_rmw(context, pe, region, offset, op, width, operand, compare, flags);
}

/* ---- Ordering and completion (§4.6) ------------------------------------ */

/* Has `peer` acknowledged everything this context wrote toward it? Read out of
 * the lead's connection state, which only the lead writes (§3.12). */
static bool acknowledged(const struct osoe_transport_context *c, uint32_t slot)
{
    uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
    return osoe_progress_host_acked(peer) >= c->high_segment[slot];
}

/* Ask every dirty peer that has not already acknowledged this context's writes
 * to acknowledge through them (§3.4), and wake the lead if any was asked. A
 * peer that already has acknowledged is skipped: fence used to ask it again on
 * every call until the next quiet (second review, algorithmic-scaling 10).
 * Shared by fence and wait, which differ only in what they do afterwards. */
static void flush_dirty(struct osoe_transport_context *c)
{
    bool asked = false;
    for (uint32_t i = 0; i < c->count; i++) {
        uint32_t slot = c->order[i];
        if (acknowledged(c, slot))
            continue;
        uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
        struct osoe_connection *conn = osoe_connection_to(&osoe.area, peer);
        /* "Acknowledge through the highest segment this context wrote into":
         * what quiet then waits for, and nothing more. */
        if (conn) {
            osoe_connection_request_ack(conn, c->high_segment[slot]);
            asked = true;
        }
    }
    if (asked)
        osoe_progress_host_wake_lead();
}

/*
 * §4.6: fence orders writes becoming visible, and does **not** clear the dirty
 * set - those operations remain unacknowledged, and a later quiet still has to
 * wait for them. Conditional sealing costs nothing extra, because the dirty set
 * already answers "which peers has this context touched".
 *
 * The serializing store comes first on both paths, and it is what puts the
 * calling thread's ordinary memory stores under fence and quiet as Table 14
 * requires (§4.6). It is not decoration: on the intranode path it is the entire
 * implementation.
 */
void osoe_transport_fence(struct osoe_transport_context *context)
{
    osoe_count(OSOE_C_TRANSPORT_FENCE_CALLS, 1);
    osoe_serializing_store();
    if (osoe.internode)
        flush_dirty(context);
}


/*
 * §4.10's wait: complete every operation this context has outstanding, to every
 * PE or to the named ones. The spec's quiet and pe_quiet are both this call
 * (§4.6), which is why the boundary has a wait rather than three routines.
 *
 * `pes == NULL` means every PE; a non-NULL list with npes == 0 has already
 * returned in quiet.c, where the spec says it (§9.12.3).
 */
void osoe_transport_wait(struct osoe_transport_context *context, const int *pes, size_t npes)
{
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TRANSPORT_WAIT_CALLS, 1);
    uint64_t t0 = osoe_now_ns();
    osoe_serializing_store();

    struct osoe_transport_context *c = context;
    if (!osoe.internode || c->count == 0) {
        /* Nothing taking the intranode path is ever outstanding, so a wait that
         * finds nothing has genuinely nothing to do. The counter says so rather
         * than being omitted, because on the far path the same name means
         * something quite different. */
        osoe_count(OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING, 1);
        return;
    }

    /*
     * A peer list narrows the set to the nodes those PEs are on. §9.12.3's
     * pe_quiet is the dirty set with a caller-supplied peer list substituted
     * for the accumulated one, which is close to free here and is the routine
     * that *validates* the data structure: an implementation tracking
     * completion by a node-wide global sequence could not do it without
     * quieting far more than asked.
     */
    bool *wanted = NULL;
    if (pes) {
        wanted = calloc(peer_count(), sizeof *wanted);
        if (!wanted)
            osoe_fatal("out of memory in shmem_pe_quiet");
        for (size_t i = 0; i < npes; i++) {
            uint32_t node = osoe_node_of(&osoe.job, (uint32_t)pes[i]);
            if (node != osoe.job.node && node != UINT32_MAX)
                wanted[peer_slot(node)] = true;
        }
    }

    flush_dirty(c);
    uint32_t waited = 0, kept = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        uint32_t slot = c->order[i];
        if (wanted && !wanted[slot]) {
            c->order[kept++] = slot;        /* not asked for: stays dirty */
            continue;
        }
        waited++;
        if (!acknowledged(c, slot)) {
            uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
            struct am_wait w;
            /* No single PE: a quiet waits for a whole node's queue, not one
             * PE's (D-6). */
            am_wait_begin(&w, pes ? "shmem_pe_quiet" : "shmem_quiet",
                          "the peer's acknowledged sequence", peer, -1,
                          osoe_progress_host_acked(peer));
            while (!acknowledged(c, slot)) {
                poll_lead();
                am_wait_check(&w, osoe_progress_host_acked(peer));
            }
            am_wait_end();
        }
        c->touched[slot] = false;
        c->high_segment[slot] = 0;
    }
    c->count = kept;
    free(wanted);
    osoe_count(OSOE_C_TRANSPORT_WAIT_DIRTY_PEERS, waited);
    osoe_count(OSOE_C_TRANSPORT_WAIT_NS, osoe_now_ns() - t0);
}
