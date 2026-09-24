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
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "am.h"
#include "am-state.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "base/order.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "base/wait-word.h"
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

/*
 * §4.10's lifecycle. The two facts about the job come first, because the dirty
 * set below is sized from one of them and the engine's start-up reads both;
 * they were once set by the engine itself, after the dirty set, and the first
 * internode put wrote through a null one. Then the engine, which must be
 * complete before this returns: the agent thread starts next and polls it.
 */
void osoe_transport_init(void)
{
    osoe_am = (struct osoe_am_state){ 0 };
    osoe_am.internode = osoe.job.node_count > 1;
    osoe_am.lead = osoe.bootstrap.local_id == 0;

    struct osoe_transport_context *c = &default_context;
    *c = (struct osoe_transport_context){ 0 };
    if (osoe_am.internode) {
        uint32_t peers = peer_count();
        c->high_segment = calloc(peers, sizeof *c->high_segment);
        c->order = calloc(peers, sizeof *c->order);
        c->touched = calloc(peers, sizeof *c->touched);
        if (!c->high_segment || !c->order || !c->touched)
            osoe_fatal("out of memory for the ordering state of %u peer nodes", peers);
    }
    osoe_progress_host_start(osoe.node.hdr->session);
}

/* After the agent has been joined (transport.h). The engine drained what this
 * node owed the network and closed its connections on that thread, in
 * osoe_transport_agent_exit; what is left is memory. */
void osoe_transport_fini(void)
{
    osoe_progress_host_stop();
    struct osoe_transport_context *c = &default_context;
    free(c->high_segment);
    free(c->order);
    free(c->touched);
    *c = (struct osoe_transport_context){ 0 };
    osoe_am = (struct osoe_am_state){ 0 };
}

/*
 * §6.2's blob, on AM: the port this lead's node-pair listener took. The
 * listener is opened here, before the rendezvous, so its port can ride in the
 * blob to every node; the connections are made in osoe_transport_init, once the
 * rendezvous has said where every node is (osoe_wires_open).
 */
void osoe_transport_advertise(void *blob)
{
    osoe_progress_host_advertise(blob);
}

/* The agent thread, lent to this backend (transport.h, §3.12). */
bool osoe_transport_agent_pass(void)
{
    return osoe_progress_host_poll();
}

bool osoe_transport_agent_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    return osoe_progress_host_wait(wake, seen);
}

void osoe_transport_agent_exit(void)
{
    osoe_progress_host_exit();
}

void osoe_transport_agent_abort(void)
{
    osoe_progress_host_abort();
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
 * in their own node's OSOE_GTIDS - and also for a node-mate under §8.1's
 * force-remote flag, which denies the pointer without changing the route.
 *
 * **The route is the other predicate** (job.h): the operations above dispatch on
 * osoe_routes_in_process, so under the flag a put to a node-mate is still a
 * memcpy in this thread. One predicate answering both questions is what sent
 * that put to a connection that does not exist (§8.1).
 *
 * shmem_ptr and shmem_team_ptr consult this and nothing else (§4.8);
 * shmem_addr_accessible does not, and must not - accessibility is reachability
 * through OpenSHMEM, which every PE of the job has.
 */
void *osoe_transport_direct(int pe, enum osoe_region region, uint64_t offset)
{
    if (pe < 0 || !osoe_direct_access(&osoe.job, (uint32_t)pe))
        return NULL;
    return osoe_transport_local_address(pe, region, offset);
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
 * quiet's wait is over a whole node's queue, not one PE's (§4.10, D-6). `kind`
 * names the word the wait sleeps on, for the "blocked in" record (node.h). */
/* As am_wait_begin, with the blocked record's detail given rather than taken
 * to be the peer: for a wait toward no one node (UINT32_MAX), whose detail is
 * something else. */
static void am_wait_begin_detail(struct am_wait *w, const char *routine,
                                 enum osoe_blocked_detail kind, uint64_t detail,
                                 const char *watching, uint32_t peer, int pe, uint64_t progress)
{
    w->routine = routine;
    w->watching = watching;
    w->peer = peer;
    w->pe = pe;
    w->sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    osoe_stall_watch_begin(&w->watch, progress, osoe.bootstrap.no_progress_ns);
    osoe_blocked_enter(routine, kind, detail);
}

static void am_wait_begin(struct am_wait *w, const char *routine, enum osoe_blocked_detail kind,
                          const char *watching, uint32_t peer, int pe, uint64_t progress)
{
    am_wait_begin_detail(w, routine, kind, peer, watching, peer, pe, progress);
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
    if (w->peer == UINT32_MAX)
        osoe_fatal("%s: no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): %s has stayed "
                   "at %llu", w->routine, (unsigned long long)osoe_stall_watch_seconds(&w->watch),
                   w->watching, (unsigned long long)progress);
    else if (w->pe >= 0)
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
 * One sleep of a wait on a wait word (base/wait-word.h): at the value loaded
 * before the caller's check, until the word moves or a sweep interval passes
 * (§3.10). True if it was the sweep.
 */
static bool sleep_on(struct osoe_wait_word *word, uint32_t seen)
{
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    return osoe_futex_wait(&word->word, seen, &sweep, OSOE_FUTEX_SHARED) == ETIMEDOUT;
}

/*
 * A sweep expired, and then the caller's check passed. The wake was lost if
 * the word still holds the value slept on a margin later: every move that can
 * make a registered waiter's check pass bumps the word, and this waiter was
 * registered (base/wait-word.h). The margin lets a bump that is on its way land
 * - an agent descheduled between its store and its bump looks the same for an
 * instant, and base/epoch.c measured how often that instant meets a sweep under
 * load. invariant.missed_wake is what every tier-2 test checks at the end.
 */
static void count_if_lost(const _Atomic uint32_t *word, uint32_t seen, const char *routine)
{
    struct timespec margin = { 0, 10000000 };       /* 10 ms, as epoch.c's */
    nanosleep(&margin, NULL);
    if (atomic_load_explicit(word, memory_order_acquire) == seen)
        osoe_missed_wake(routine);
}

/* How far a connection has turned over: the newest sequence either buffer has
 * been opened with. It rises every time the lead frees and reopens a buffer,
 * which is the progress a writer waiting for room is waiting on. */
static uint64_t turnover(const struct osoe_connection *connection)
{
    uint64_t a = atomic_load_explicit(&connection->s->buffer[0].seq, memory_order_acquire);
    uint64_t b = atomic_load_explicit(&connection->s->buffer[1].seq, memory_order_acquire);
    return a > b ? a : b;
}

/*
 * Reserve, waiting for room.
 *
 * Only the lead's agent makes room here, by sealing the buffer, sending it and
 * opening it again (§3.5, §3.12). So a writer that finds none asks for a seal -
 * it marks the connection and wakes the lead, which visits only connections
 * with work (§3.10) - and waits on the connection's turnover word, which the
 * agent bumps when it opens a buffer and anyone is registered
 * (base/wait-word.h, §3.11). Every waiter is woken: room is divisible, and each
 * has its own reservation to retry. It asks again before every sleep, since
 * the buffer that just opened may have filled before this writer got to it.
 *
 * Step 2 slept 20 us and looked again instead, which on the ring held a stream
 * to 3% of the floor (§8.4). am.reserve_stalls counts every wait, so a buffer
 * that is often full is visible rather than assumed rare.
 */
static void reserve_waiting(struct osoe_connection *connection, uint32_t peer, int pe,
                            const char *routine, uint32_t len, struct osoe_reservation *r)
{
    if (osoe_connection_reserve(connection, len, r) == OSOE_RESERVATION_OK)
        return;
    osoe_count(OSOE_C_AM_RESERVE_STALLS, 1);
    struct osoe_wait_word *word = osoe_connection_turnover_wait(connection);
    struct am_wait w;
    am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_TURNOVER, "the connection's buffer turnover",
                  peer, pe, turnover(connection));
    osoe_wait_word_enter(word);
    uint32_t slept_on = 0;
    bool swept = false;
    for (;;) {
        uint32_t seen = osoe_wait_word_load(word);
        if (osoe_connection_reserve(connection, len, r) == OSOE_RESERVATION_OK) {
            if (swept)
                count_if_lost(&word->word, slept_on, routine);
            break;
        }
        osoe_connection_area_mark(&osoe_am.area, peer);
        osoe_progress_host_wake_lead();
        slept_on = seen;
        swept = sleep_on(word, seen);
        am_wait_check(&w, turnover(connection));
    }
    osoe_wait_word_leave(word);
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
static uint32_t payload_limit(struct osoe_connection *connection, uint32_t peer,
                              uint32_t fixed_bytes)
{
    uint32_t chunk = (uint32_t)osoe_am.area.chunk_bytes;
    uint32_t room = osoe_connection_payload_capacity(connection);
    if (room < chunk)
        chunk = room;
    uint32_t overhead = (uint32_t)sizeof(struct osoe_message_header) + fixed_bytes;
    if (chunk <= overhead)
        osoe_fatal("toward node %u: a connection buffer of %llu bytes cannot carry a "
                   "message with a %u-byte fixed part; raise OSOE_AM_BUFFER or OSOE_AM_CHUNK",
                   peer, (unsigned long long)osoe_am.area.buffer_bytes, fixed_bytes);
    return chunk - overhead;
}

static void remote_put(struct osoe_transport_context *c, int pe, enum osoe_region region,
                       uint64_t offset, const void *src, size_t len, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);

    uint32_t limit = payload_limit(connection, peer, (uint32_t)sizeof(struct osoe_am_put));
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
        reserve_waiting(connection, peer, pe, "shmem_put", osoe_message_padded(body), &r);
        unsigned char *to = osoe_reservation_bytes(connection, &r);
        memcpy(to, &h, sizeof h);
        memcpy(to + sizeof h, &m, sizeof m);
        memcpy(to + sizeof h + sizeof m, at, n);
        osoe_connection_commit(connection, &r);
        /* After every commit, not once per put: the next chunk may have to
         * wait for room, and the lead makes room only in a connection it
         * knows has bytes (§3.10). Marking once at the end hung every
         * multi-chunk put that filled a buffer. */
        osoe_connection_area_mark(&osoe_am.area, peer);
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
    /* Each chunk set the dirty bit whatever the flag says: it is how the lead
     * finds these bytes at all, even on the pass the sweep forces (§3.10). */
    if (flags & OSOE_TRANSPORT_MORE) {
        osoe_count(OSOE_C_AM_WAKES_DEFERRED, 1);
        return;
    }
    osoe_progress_host_wake_lead();
}

/*
 * This PE's landing slot, and the capacity of its payload area (§3.12) - the
 * blocking operations' part of the landing zone, which since layout 16 also
 * holds the nonblocking gets' records and arena (below).
 *
 * The slot holds one AM chunk, and a blocking get is therefore issued in pieces
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
    return osoe.node.landing_data_bytes;
}

/*
 * Take a reply credit for this connection, waiting in its queue if none is
 * free (reply.h, §3.11).
 *
 * §4.6 permits blocking rather than failing, and §3.8 needs the bound kept at
 * the origin: this is what stops the node's PEs between them putting more
 * outstanding replies on a peer than its descriptor pool holds. A PE holds its
 * credit from issue until its reply *lands*, not until it is sent. A queued PE
 * is served in ticket order: the PE whose reply lands next gives its credit
 * straight to the oldest ticket and wakes that ticket's futex class, so no PE
 * arriving meanwhile can take it (credit_wait_model.py). Step 2 polled here,
 * waking the lead and sleeping 20 us.
 */
static void take_credit(struct osoe_connection *connection, uint32_t peer, int pe,
                        const char *routine)
{
    struct osoe_credit *credit = osoe_connection_credit(connection);
    uint32_t ticket;
    if (osoe_credit_take(credit, &ticket))
        return;
    /* Credits come back as replies land, so the tickets served are what moves
     * while one is awaited: a peer that replies to nothing serves nobody. A wait
     * submits whatever this PE has held back (OSOE_TRANSPORT_MORE, transport.h):
     * its own requests may be what the credits it waits for are out on. */
    osoe_progress_host_wake_lead();
    struct am_wait w;
    am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_CREDIT, "the connection's reply credits",
                  peer, pe, osoe_credit_served_count(credit));
    uint32_t slept_on = 0;
    bool swept = false;
    for (;;) {
        uint32_t seen = atomic_load_explicit(&credit->wake, memory_order_acquire);
        if (osoe_credit_served(credit, ticket)) {
            if (swept)
                count_if_lost(&credit->wake, slept_on, routine);
            break;
        }
        slept_on = seen;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        swept = osoe_futex_wait_bitset(&credit->wake, seen, osoe_credit_bit(ticket), &sweep,
                                       OSOE_FUTEX_SHARED) == ETIMEDOUT;
        am_wait_check(&w, osoe_credit_served_count(credit));
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
            am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_REPLY, "the landing slot's wake word",
                          peer, pe, seen);
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
    atomic_store_explicit(&slot->ready, 0, memory_order_release);
    /* Published before the request goes out: the send's reservation and
     * commit order it before any reply can exist. */
    atomic_store_explicit(&slot->armed_token, ++next_token, memory_order_release);
    return next_token;
}

/* Reserve, fill and commit one message of `fixed_bytes` plus `payload` bytes. */
static uint64_t send_message(struct osoe_connection *connection, uint32_t peer, int pe,
                             const char *routine, uint16_t kind, const void *fixed,
                             uint32_t fixed_bytes, const void *payload, uint32_t payload_bytes)
{
    uint32_t body = fixed_bytes + payload_bytes;
    struct osoe_message_header h = { kind, 0, body };
    struct osoe_reservation r;
    reserve_waiting(connection, peer, pe, routine, osoe_message_padded(body), &r);
    unsigned char *to = osoe_reservation_bytes(connection, &r);
    memcpy(to, &h, sizeof h);
    memcpy(to + sizeof h, fixed, fixed_bytes);
    if (payload_bytes)
        memcpy(to + sizeof h + fixed_bytes, payload, payload_bytes);
    osoe_connection_commit(connection, &r);
    osoe_connection_area_mark(&osoe_am.area, peer);
    return r.seq;
}

static void remote_get(int pe, enum osoe_region region, uint64_t offset, void *dst,
                       size_t len)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    struct osoe_landing_slot *slot = my_landing();
    uint32_t limit = landing_capacity();
    if (limit > payload_limit(connection, peer, (uint32_t)sizeof(struct osoe_am_get_reply)))
        limit = payload_limit(connection, peer, (uint32_t)sizeof(struct osoe_am_get_reply));

    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        uint64_t token = arm(slot);
        take_credit(connection, peer, pe, "shmem_get");
        struct osoe_am_get_request m = { .target_pe = (uint32_t)pe,
                                         .origin_pe = osoe.bootstrap.rank,
                                         .region = (uint16_t)region, .flags = 0,
                                         .length = n, .offset = offset,
                                         .token = token };
        send_message(connection, peer, pe, "shmem_get", OSOE_AM_GET_REQUEST, &m, (uint32_t)sizeof m,
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
        osoe_credit_give(osoe_connection_credit(connection));
        at += n;
        offset += n;
        len -= n;
    }
}

/*
 * This PE's nonblocking gets in flight (§4.6, as built). Private: under
 * SERIALIZED one thread issues them and completes them, and the lead sees only
 * the landing records. `copy` holds, per record, where its reply goes once it
 * has landed - a `dst` the lead's process may not be able to see (§3.12).
 */
struct in_flight_copy {
    void *dst;
    uint32_t offset;            /* into the landing arena */
    uint32_t length;
};

static struct {
    uint32_t count;             /* records in use, from index 0 */
    uint32_t arena_used;        /* bytes of the arena handed out */
    uint64_t sequence;          /* the count of tokens made */
    uint32_t capacity;          /* entries in `copy` */
    struct in_flight_copy *copy;
} in_flight;

/*
 * Complete every nonblocking get this PE has in flight: wait until the lead has
 * landed as many bytes as the PE asked for (§4.6's counters), then copy each
 * reply out and give its record back. A wait calls it - a quiet, and all that
 * completes as one - and so does an issue that finds the records or the arena
 * used up, which is how the landing zone stays bounded without anything
 * failing.
 */
static void complete_gets(const char *routine)
{
    if (in_flight.count == 0)
        return;
    struct osoe_landing_slot *slot = my_landing();
    uint64_t issued = atomic_load_explicit(&slot->bytes_issued, memory_order_acquire);
    uint64_t landed = atomic_load_explicit(&slot->bytes_landed, memory_order_seq_cst);
    if (landed != issued) {
        /* Whatever this PE held back goes out before it waits (transport.h). The
         * gets may be toward any number of nodes, so the wait names none: its
         * record says how many bytes are still to land. */
        osoe_progress_host_wake_lead();
        struct am_wait w;
        am_wait_begin_detail(&w, routine, OSOE_BLOCKED_DETAIL_GETS_IN_FLIGHT, issued - landed,
                             "the bytes its nonblocking gets have landed", UINT32_MAX, -1,
                             landed);
        osoe_wait_word_enter(&slot->counter_wake);
        uint32_t slept_on = 0;
        bool swept = false;
        for (;;) {
            uint32_t seen = osoe_wait_word_load(&slot->counter_wake);
            landed = atomic_load_explicit(&slot->bytes_landed, memory_order_seq_cst);
            /* Past equality it can never return: a reply counted twice, which
             * the lead's checks exist to refuse (progress.c). */
            if (landed > issued)
                osoe_fatal("%s: the lead has counted %llu bytes landed for this PE's "
                           "nonblocking gets, which asked for %llu", routine,
                           (unsigned long long)landed, (unsigned long long)issued);
            if (landed == issued) {
                if (swept)
                    count_if_lost(&slot->counter_wake.word, slept_on, routine);
                break;
            }
            slept_on = seen;
            swept = sleep_on(&slot->counter_wake, seen);
            am_wait_check(&w, landed);
        }
        osoe_wait_word_leave(&slot->counter_wake);
        am_wait_end();
    }
    /* Each record was retired by the lead as its reply landed (progress-host.c),
     * so every one is free to be taken again from here. */
    const unsigned char *arena = osoe_node_landing_arena(&osoe.node, osoe.bootstrap.local_id);
    for (uint32_t i = 0; i < in_flight.count; i++)
        memcpy(in_flight.copy[i].dst, arena + in_flight.copy[i].offset, in_flight.copy[i].length);
    in_flight.count = 0;
    in_flight.arena_used = 0;
}

/*
 * A nonblocking get (§4.6, as built): ask, and return. Each piece - no larger
 * than the blocking slot's chunk, the wire's limit and the arena - takes a
 * record and a range of the landing arena, publishes them with its token, and
 * goes out; the lead lands its reply there, counts it and returns its credit,
 * and the next wait copies it into `dst`. A piece that finds no record or no
 * room completes what is in flight first.
 *
 * The request holds its credit, like any get: §3.8's bound on replies in
 * flight toward this node is unchanged. What is new is who returns it - the
 * lead, as the reply lands, rather than the caller after reading it - which
 * reply.h has said the difference would be since the credits were built.
 */
static void remote_get_nonblocking(int pe, enum osoe_region region, uint64_t offset, void *dst,
                                   size_t len, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    uint32_t requests = osoe.node.landing_requests;
    uint32_t arena_bytes = osoe.node.landing_arena_bytes;
    if (in_flight.capacity != requests) {
        complete_gets("shmem_get_nbi");         /* a new session's zone; empty in practice */
        free(in_flight.copy);
        in_flight.copy = calloc(requests, sizeof *in_flight.copy);
        if (!in_flight.copy)
            osoe_fatal("out of memory for %u nonblocking gets in flight", requests);
        in_flight.capacity = requests;
    }
    uint32_t limit = landing_capacity();
    if (limit > arena_bytes)
        limit = arena_bytes;
    uint32_t wire = payload_limit(connection, peer, (uint32_t)sizeof(struct osoe_am_get_reply));
    if (limit > wire)
        limit = wire;

    struct osoe_landing_slot *slot = my_landing();
    struct osoe_landing_request *records =
        osoe_node_landing_requests(&osoe.node, osoe.bootstrap.local_id);
    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        if (in_flight.count == requests || in_flight.arena_used + (uint64_t)n > arena_bytes) {
            osoe_count(OSOE_C_AM_LANDING_FULL, 1);
            complete_gets("shmem_get_nbi");
        }
        uint32_t index = in_flight.count++;
        uint32_t place = in_flight.arena_used;
        /* Each reply starts 8-byte aligned, for the copy out. */
        in_flight.arena_used += (n + 7u) & ~7u;
        uint64_t token = OSOE_LANDING_NONBLOCKING | (++in_flight.sequence << 16) | index;
        atomic_store_explicit(&records[index].offset, place, memory_order_release);
        atomic_store_explicit(&records[index].length, n, memory_order_release);
        /* Published before the request goes out: the send's reservation and
         * commit order it before any reply can exist (as arm() for the slot). */
        atomic_store_explicit(&records[index].token, token, memory_order_release);
        in_flight.copy[index] = (struct in_flight_copy){ at, place, n };
        atomic_fetch_add_explicit(&slot->bytes_issued, n, memory_order_seq_cst);

        take_credit(connection, peer, pe, "shmem_get_nbi");
        struct osoe_am_get_request m = { .target_pe = (uint32_t)pe,
                                         .origin_pe = osoe.bootstrap.rank,
                                         .region = (uint16_t)region, .flags = 0,
                                         .length = n, .offset = offset,
                                         .token = token };
        send_message(connection, peer, pe, "shmem_get_nbi", OSOE_AM_GET_REQUEST, &m,
                     (uint32_t)sizeof m, NULL, 0);
        osoe_count(OSOE_C_AM_GETS_NONBLOCKING, 1);
        at += n;
        offset += n;
        len -= n;
        /* More pieces, or more gets (OSOE_TRANSPORT_MORE), follow: hold the wake
         * for the last, as a put does. */
        if (len > 0 || (flags & OSOE_TRANSPORT_MORE))
            osoe_count(OSOE_C_AM_WAKES_DEFERRED, 1);
        else
            osoe_progress_host_wake_lead();
    }
}

static uint64_t remote_rmw(struct osoe_transport_context *c, int pe,
                           enum osoe_region region, uint64_t offset,
                           enum osoe_rmw_op op, unsigned width, uint64_t operand,
                           uint64_t compare, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    bool fetching = (flags & OSOE_TRANSPORT_FETCH) != 0;
    struct osoe_landing_slot *slot = my_landing();
    uint64_t token = 0;

    if (fetching) {
        token = arm(slot);
        take_credit(connection, peer, pe, "shmem_atomic");
    }
    struct osoe_am_rmw_request m = { .target_pe = (uint32_t)pe,
                                     .origin_pe = osoe.bootstrap.rank,
                                     .region = (uint16_t)region, .op = (uint16_t)op,
                                     .width = width, .offset = offset,
                                     .operand = operand, .compare = compare,
                                     .token = token, .flags = flags };
    uint64_t seq = send_message(connection, peer, pe, "shmem_atomic", OSOE_AM_RMW_REQUEST, &m,
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
    osoe_credit_give(osoe_connection_credit(connection));
    return previous;
}

/* ---- §4.10's data operations ------------------------------------------- */

void osoe_transport_put(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe)) {
        osoe_transport_local_put(pe, region, offset, src, len, flags);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_PUT_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_PUT_BYTES, len);
    remote_put(context, pe, region, offset, src, len, flags);
}

void osoe_transport_get(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len, unsigned flags)
{
    (void)context;
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe)) {
        /* Applied before it returns, nonblocking or not: complete early, which
         * a nonblocking get may be (transport.h). */
        osoe_transport_local_get(pe, region, offset, dst, len);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_GET_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_GET_BYTES, len);
    if (flags & OSOE_TRANSPORT_NONBLOCKING) {
        remote_get_nonblocking(pe, region, offset, dst, len, flags);
        return;
    }
    /* A blocking get is complete on return, so it never joins the dirty set:
     * §4.6 makes gets a quiet-only concern and there is nothing left for a
     * quiet to wait for once the bytes are in dst. */
    remote_get(pe, region, offset, dst, len);
}

uint64_t osoe_transport_rmw(struct osoe_transport_context *context, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe))
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
        struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
        /* "Acknowledge through the highest segment this context wrote into":
         * what quiet then waits for, and nothing more. */
        if (connection) {
            osoe_connection_request_ack(connection, c->high_segment[slot]);
            osoe_connection_area_mark(&osoe_am.area, peer);
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
 * A fence comes first on both paths, and it is what puts the calling thread's
 * ordinary memory stores under fence and quiet as Table 14 requires (§4.6): a
 * release fence here, since fence orders stores against stores, and the
 * serializing fence in wait, since quiet also orders them against later loads
 * (base/order.h). It is not decoration: on the intranode path it is the entire
 * implementation.
 */
void osoe_transport_fence(struct osoe_transport_context *context)
{
    osoe_count(OSOE_C_TRANSPORT_FENCE_CALLS, 1);
    osoe_release_fence();
    if (osoe_am.internode)
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
void osoe_transport_wait(struct osoe_transport_context *context, const int *pes, size_t npes,
                         const char *routine)
{
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TRANSPORT_WAIT_CALLS, 1);
    uint64_t t0 = osoe_now_ns();
    /* This PE's nonblocking gets, every one of them, whatever PEs the wait
     * names: their completion is counted per PE, not per target (§4.6). First,
     * so that the stores copying them into place come before quiet's fence
     * and are ordered as every other store before it is. */
    complete_gets(routine);
    osoe_serializing_fence();

    struct osoe_transport_context *c = context;
    if (!osoe_am.internode || c->count == 0) {
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
            /* flush_dirty has asked for the acknowledgement and woken the lead;
             * the agent bumps this connection's acknowledgement word when one
             * lands and anyone is registered (base/wait-word.h). Every waiter
             * is woken, each checking its own writes. */
            uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
            struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
            if (!connection)
                osoe_fatal("no connection toward node %u, which this context wrote to", peer);
            struct osoe_wait_word *word = osoe_connection_acked_wait(connection);
            struct am_wait w;
            /* No single PE: a quiet waits for a whole node's queue, not one
             * PE's (D-6). */
            am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_ACKNOWLEDGEMENT,
                          "the peer's acknowledged sequence", peer, -1,
                          osoe_progress_host_acked(peer));
            osoe_wait_word_enter(word);
            uint32_t slept_on = 0;
            bool swept = false;
            for (;;) {
                uint32_t seen = osoe_wait_word_load(word);
                if (acknowledged(c, slot)) {
                    if (swept)
                        count_if_lost(&word->word, slept_on, routine);
                    break;
                }
                slept_on = seen;
                swept = sleep_on(word, seen);
                am_wait_check(&w, osoe_progress_host_acked(peer));
            }
            osoe_wait_word_leave(word);
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

void osoe_transport_wait_gets(struct osoe_transport_context *context, const char *routine)
{
    (void)context;              /* a PE's gets complete together, whatever the context */
    complete_gets(routine);
}

/* ---- The two entry points that are this backend's, not a path's --------- */

/*
 * These are the backend's own, whichever path an operation takes, so they live
 * in the file a second backend replaces rather than in transport-local.c,
 * which a second backend keeps: an RDMA backend has its own set_range, and a
 * definition in the shared intranode file would collide with it at link time
 * (second review, roce-seam F9).
 */

/*
 * Every wait's predicate load (wait.c, signal.c). It follows the waiter's
 * seq_cst publication of its watch, and the writer's half is a seq_cst store or
 * rmw followed by notify's load of that watch: a two-sided protocol, so this
 * load is seq_cst (§3.16). Acquire is not enough - C11 does not order a seq_cst
 * store before a later acquire load, and GCC for Armv8.3 and later (a Pi 5's
 * Cortex-A76) emits it as ldapr, which passes the stlr: measured, 289 to 4,449
 * lost-wake outcomes in 2 million rounds (fence-litmus/store-buffer-litmus).
 * A seq_cst load is an ldar on every aarch64 target and a plain mov on x86-64.
 */
uint64_t osoe_transport_load(enum osoe_region region, uint64_t offset, unsigned width)
{
    /* This PE's own word: the address, not the pointer the application may
     * have (transport-local.h). A PE is direct to itself either way, and this
     * says so without depending on it. */
    void *ptr = osoe_transport_local_address((int)osoe.bootstrap.rank, region, offset);
    switch (width) {
    case 8:
        return atomic_load_explicit((_Atomic uint64_t *)ptr, memory_order_seq_cst);
    case 4:
        return atomic_load_explicit((_Atomic uint32_t *)ptr, memory_order_seq_cst);
    default:
        return atomic_load_explicit((_Atomic uint16_t *)ptr, memory_order_seq_cst);
    }
}

/*
 * Nothing to do: every operation on this backend is a memory operation on a
 * mapping the process already holds, so no range needs preparing (§4.10). The
 * call is still made by the allocator, because the contract is the backend's to
 * ignore, not the caller's to skip.
 */
void osoe_transport_set_range(enum osoe_region region, uint64_t offset, uint64_t len,
                       unsigned attributes)
{
    (void)region;
    (void)offset;
    (void)len;
    (void)attributes;
    osoe_count(OSOE_C_TRANSPORT_SET_RANGE_CALLS, 1);
}
