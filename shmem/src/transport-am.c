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
#include <pthread.h>
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
    uint32_t block;             /* its block in this PE's pool (node.h): element index too */
    unsigned options;           /* OSOE_TRANSPORT_CONTEXT_*, from creation */
    bool live;
    /* Several threads may call on it at once (OSOE_TRANSPORT_CONTEXT_CONCURRENT):
     * then every call holds `mutex` (phase 1 step 8 slice 2's interim, until
     * §4.11's lock-free shared mode). */
    bool concurrent;
    pthread_mutex_t mutex;
    uint64_t *high_segment;     /* per peer slot: the highest segment written into */
    uint32_t *order;            /* the slots touched, in the order first touched */
    bool *touched;              /* membership, so marking twice is not quadratic */
    uint32_t count;
    /* This context's nonblocking gets in flight, as (extent, token) pairs:
     * the pieces its wait copies out, if no other thread of the PE has
     * (copy_out). Touched only by calls on this context. */
    struct pending_piece *pending;
    uint32_t pending_count, pending_capacity;
};

/*
 * Every context this PE may have, OSOE_MAX_CONTEXTS of them, element i owning
 * block i of this PE's pool in the node segment (§4.11; phase 1 step 8).
 * Element 0 is the default context, live from init to fini; the others are
 * live between osoe_transport_context_create and _destroy, and an element's
 * block is claimed and released with it. The pool's state words are the
 * truth about which blocks are taken - a compare-and-swap, as §4.11 item 4
 * says, though below MULTIPLE nothing contends for them.
 */
static struct osoe_transport_context *contexts;
static uint32_t context_count;

struct pending_piece {
    uint32_t extent;
    uint64_t token;
};

/*
 * Where each nonblocking piece's reply goes once it has landed, by its first
 * extent: a `dst` the lead's process may not be able to see (§3.12), so it is
 * this process's, written by the issuer before its record goes ISSUED and read
 * by whichever thread wins the record's copy-out claim (copy_out). The zone's
 * extent count long, made at init.
 */
static void **copy_destination;

static struct osoe_context_block *block_of(const struct osoe_transport_context *c)
{
    return osoe_node_context(&osoe.node, osoe.bootstrap.local_id, c->block);
}

/* The dirty set's arrays, O(peer nodes) virtual, resident as touched (§4.6).
 * A job of one node has no peers and none. */
static bool dirty_set_allocate(struct osoe_transport_context *c)
{
    if (!osoe_am.internode)
        return true;
    uint32_t peers = peer_count();
    c->high_segment = calloc(peers, sizeof *c->high_segment);
    c->order = calloc(peers, sizeof *c->order);
    c->touched = calloc(peers, sizeof *c->touched);
    return c->high_segment && c->order && c->touched;
}

static void dirty_set_free(struct osoe_transport_context *c)
{
    free(c->high_segment);
    free(c->order);
    free(c->touched);
    free(c->pending);
    c->high_segment = NULL;
    c->order = NULL;
    c->touched = NULL;
    c->pending = NULL;
    c->count = 0;
    c->pending_count = c->pending_capacity = 0;
}

/* Make element `index` live on its block, which the caller has claimed. */
static bool context_open(struct osoe_transport_context *c, uint32_t index, unsigned options)
{
    *c = (struct osoe_transport_context){ .block = index, .options = options, .live = true,
                                          .concurrent =
                                              (options & OSOE_TRANSPORT_CONTEXT_CONCURRENT) != 0 };
    pthread_mutex_init(&c->mutex, NULL);
    return dirty_set_allocate(c);
}

/*
 * The interim of phase 1 step 8 slice 2: a context several threads may use at
 * once is held for the whole of each call on it. Never across a wait outside
 * the call, so no thread waits on another's program while it holds one: a
 * quiet or a blocking get holding it waits only for the network. What every
 * context shares - the landing zone's extents and records - is claimed by
 * compare-and-swap and needs no lock (§4.11 items 7 and 8). Below
 * THREAD_MULTIPLE nothing is concurrent and no mutex is taken.
 */
static bool threads_concurrent;

static void context_enter(struct osoe_transport_context *c)
{
    if (c->concurrent)
        pthread_mutex_lock(&c->mutex);
}

static void context_leave(struct osoe_transport_context *c)
{
    if (c->concurrent)
        pthread_mutex_unlock(&c->mutex);
}

void osoe_transport_threads(bool concurrent)
{
    threads_concurrent = concurrent;
    contexts[0].concurrent = concurrent;
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

    context_count = osoe.node.contexts_per_pe;
    contexts = calloc(context_count, sizeof *contexts);
    if (!contexts)
        osoe_fatal("out of memory for %u contexts", context_count);
    /* The default context's block is block 0, claimed here as any other is
     * claimed at creation. The segment is this session's, made zeroed by the
     * lead, so the claim can fail only on a segment that is not. */
    uint32_t expected = OSOE_CONTEXT_FREE;
    struct osoe_context_block *b = osoe_node_context(&osoe.node, osoe.bootstrap.local_id, 0);
    if (!atomic_compare_exchange_strong_explicit(&b->state, &expected, OSOE_CONTEXT_LIVE,
                                                 memory_order_seq_cst, memory_order_seq_cst))
        osoe_fatal("this PE's context block 0 is already live in a fresh control segment");
    if (!context_open(&contexts[0], 0, 0))
        osoe_fatal("out of memory for the ordering state of %u peer nodes", peer_count());
    copy_destination = calloc(osoe.node.landing_requests, sizeof *copy_destination);
    if (!copy_destination)
        osoe_fatal("out of memory for %u landing extents", osoe.node.landing_requests);
    osoe_progress_host_start(osoe.node.hdr->session);
}

/* After the agent has been joined (transport.h). The engine drained what this
 * node owed the network and closed its connections on that thread, in
 * osoe_transport_agent_exit; what is left is memory. */
void osoe_transport_fini(void)
{
    osoe_progress_host_stop();
    /* Every created context was destroyed before the finalize barrier
     * (context.c), so only the default one is live here; a block left live
     * would be a defect, and the segment goes with the session either way. */
    threads_concurrent = false;
    for (uint32_t i = 0; i < context_count; i++)
        if (contexts[i].live) {
            pthread_mutex_destroy(&contexts[i].mutex);
            dirty_set_free(&contexts[i]);
            atomic_store_explicit(&block_of(&contexts[i])->state, OSOE_CONTEXT_FREE,
                                  memory_order_release);
            contexts[i].live = false;
        }
    free(contexts);
    contexts = NULL;
    context_count = 0;
    free(copy_destination);
    copy_destination = NULL;
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
    return &contexts[0];
}

/*
 * transport.h: a context from this PE's pool. The first free block from 1 up
 * is claimed by a compare-and-swap on its state word - O(OSOE_MAX_CONTEXTS)
 * per creation, a setup-time cost, and the segment's words rather than a
 * process-memory free list so that the pool has one truth (§4.11 item 4).
 * NULL when every block is live or the dirty set's memory is short; a claim
 * whose memory then fails is given back.
 */
struct osoe_transport_context *osoe_transport_context_create(unsigned options)
{
    for (uint32_t i = 1; i < context_count; i++) {
        struct osoe_context_block *b = osoe_node_context(&osoe.node, osoe.bootstrap.local_id, i);
        uint32_t expected = OSOE_CONTEXT_FREE;
        if (!atomic_compare_exchange_strong_explicit(&b->state, &expected, OSOE_CONTEXT_LIVE,
                                                     memory_order_seq_cst, memory_order_seq_cst))
            continue;
        struct osoe_transport_context *c = &contexts[i];
        if (context_open(c, i, options))
            return c;
        dirty_set_free(c);
        c->live = false;
        atomic_store_explicit(&b->state, OSOE_CONTEXT_FREE, memory_order_release);
        return NULL;
    }
    return NULL;
}

/* transport.h: the implicit wait of spec §9.5.3, then the block and the
 * memory go back. The counters in the block are cumulative and never reset,
 * so a later claimant of the block starts from equal counters, whatever they
 * count to. */
void osoe_transport_context_destroy(struct osoe_transport_context *c, const char *routine)
{
    osoe_transport_wait(c, NULL, 0, routine);
    pthread_mutex_destroy(&c->mutex);
    dirty_set_free(c);
    c->live = false;
    atomic_store_explicit(&block_of(c)->state, OSOE_CONTEXT_FREE, memory_order_release);
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
 * This PE's landing zone (§3.12, §4.6, §4.11 items 7 and 8; node.h): every
 * reply to its gets lands in extents of it, a piece of a get being at most
 * `landing_data_bytes`, a run of at most 64 extents within one word of the
 * zone's bitmap. **The origin cuts a get into pieces; the target does not cut
 * beyond them.** A reply may still be split when the connection buffer is
 * tight - the drain sends one chunk per descriptor per pass (§3.8) - and every
 * part then lands at an offset inside its piece, which is what keeps
 * `destination()` writing inside it.
 */
static struct osoe_landing_zone *my_zone(void)
{
    return osoe_node_landing(&osoe.node, osoe.bootstrap.local_id);
}

static struct osoe_landing_record *my_records(void)
{
    return osoe_node_landing_records(&osoe.node, osoe.bootstrap.local_id);
}

static uint32_t extents_for(uint32_t bytes)
{
    uint32_t n = (bytes + OSOE_LANDING_EXTENT_BYTES - 1) / OSOE_LANDING_EXTENT_BYTES;
    return n ? n : 1;
}

/* A run of `count` clear bits of word `word` of the bitmap, set by
 * compare-and-swap: its first extent, or UINT32_MAX if the word has no such
 * run (landing_extent_model.py: a load and a store here lets two threads take
 * one extent). */
static uint32_t claim_in_word(uint32_t word, uint32_t count)
{
    uint32_t extents = osoe.node.landing_requests;
    uint32_t valid = extents - word * 64 < 64 ? extents - word * 64 : 64;
    if (count > valid)
        return UINT32_MAX;
    uint64_t run = count == 64 ? ~UINT64_C(0) : (UINT64_C(1) << count) - 1;
    _Atomic uint64_t *bits = &my_zone()->bits[word];
    uint64_t seen = atomic_load_explicit(bits, memory_order_acquire);
    for (;;) {
        uint32_t at = UINT32_MAX;
        for (uint32_t b = 0; b + count <= valid; b++)
            if (!(seen & (run << b))) {
                at = b;
                break;
            }
        if (at == UINT32_MAX)
            return UINT32_MAX;
        if (atomic_compare_exchange_weak_explicit(bits, &seen, seen | (run << at),
                                                  memory_order_seq_cst, memory_order_acquire))
            return word * 64 + at;
    }
}

static uint32_t claim_run(uint32_t count)
{
    uint32_t words = (osoe.node.landing_requests + 63) / 64;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t first = claim_in_word(w, count);
        if (first != UINT32_MAX)
            return first;
    }
    return UINT32_MAX;
}

/* Give a run back: clear its bits, then wake whoever waits for room - the
 * clear first, or a waiter wakes, finds the word still full, and sleeps for
 * good (the model's `wake_first`). */
static void release_run(uint32_t first, uint32_t count)
{
    uint64_t run = count == 64 ? ~UINT64_C(0) : (UINT64_C(1) << count) - 1;
    atomic_fetch_and_explicit(&my_zone()->bits[first / 64], ~(run << (first % 64)),
                              memory_order_seq_cst);
    osoe_wait_word_wake_all(&my_zone()->freed);
}

/*
 * Copy out the landed nonblocking piece at `extent`, if this thread wins its
 * claim (copy_out_claim_model.py): LANDED to COPYING by compare-and-swap, so
 * that no two threads copy one reply, and no copy starts before it has landed.
 * The winner copies into the piece's `dst`, counts the bytes copied in the
 * context block its token names and wakes a wait there, and gives the record
 * and the extents back. Whichever of the PE's threads it is: its context's
 * wait, or any issue that found no room (make_room).
 */
static bool copy_out(uint32_t extent)
{
    struct osoe_landing_record *r = &my_records()[extent];
    uint32_t landed_state = OSOE_LANDING_LANDED;
    if (!atomic_compare_exchange_strong_explicit(&r->state, &landed_state, OSOE_LANDING_COPYING,
                                                 memory_order_seq_cst, memory_order_acquire))
        return false;
    uint64_t token = atomic_load_explicit(&r->token, memory_order_acquire);
    uint32_t length = atomic_load_explicit(&r->length, memory_order_acquire);
    memcpy(copy_destination[extent],
           osoe_node_landing_arena(&osoe.node, osoe.bootstrap.local_id) +
               (size_t)extent * OSOE_LANDING_EXTENT_BYTES,
           length);
    struct osoe_context_block *c = osoe_node_context(
        &osoe.node, osoe.bootstrap.local_id,
        (uint32_t)((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK));
    atomic_store_explicit(&r->token, 0, memory_order_release);
    atomic_store_explicit(&r->state, OSOE_LANDING_FREE, memory_order_release);
    atomic_fetch_add_explicit(&c->bytes_copied, length, memory_order_seq_cst);
    osoe_wait_word_wake_all(&c->counter_wake);
    release_run(extent, extents_for(length));
    osoe_count(OSOE_C_AM_GETS_COMPLETED, 1);
    return true;
}

/* Copy out every landed piece this thread can claim, of any context. */
static uint32_t copy_out_any(void)
{
    uint32_t copied = 0;
    for (uint32_t e = 0; e < osoe.node.landing_requests; e++)
        if (atomic_load_explicit(&my_records()[e].state, memory_order_acquire) ==
                OSOE_LANDING_LANDED && copy_out(e))
            copied++;
    return copied;
}

/*
 * A run of `count` extents for a piece, waiting if the zone has none: an
 * issue that finds no room first copies out every landed piece it can claim,
 * of any context - an early completion those contexts' waits then find done,
 * which the boundary allows (transport.h) - and then sleeps on the zone's
 * `freed` word, which every release and every landing bumps, with the value
 * it read before it looked (landing_extent_model.py). A get whose pieces are
 * this PE's own, none landed yet, wakes when the first lands.
 */
static uint32_t make_room(uint32_t count, const char *routine)
{
    uint32_t first = claim_run(count);
    if (first != UINT32_MAX)
        return first;
    osoe_count(OSOE_C_AM_LANDING_FULL, 1);
    struct osoe_wait_word *freed = &my_zone()->freed;
    osoe_wait_word_enter(freed);
    osoe_progress_host_wake_lead();         /* what this PE held back may be what lands */
    struct am_wait w;
    bool waiting = false;
    for (;;) {
        uint32_t seen = osoe_wait_word_load(freed);
        first = claim_run(count);
        if (first != UINT32_MAX)
            break;
        if (copy_out_any())
            continue;
        if (!waiting) {
            am_wait_begin_detail(&w, routine, OSOE_BLOCKED_DETAIL_GETS_IN_FLIGHT, count,
                                 "the landing zone's released extents", UINT32_MAX, -1, seen);
            waiting = true;
        }
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&freed->word, seen, &sweep, OSOE_FUTEX_SHARED);
        am_wait_check(&w, osoe_wait_word_load(freed));
    }
    osoe_wait_word_leave(freed);
    if (waiting)
        am_wait_end();
    return first;
}

/* This thread's completion slot (node.h, struct osoe_thread_record). */
static struct osoe_thread_record *my_thread(uint32_t *index)
{
    *index = osoe_metrics_thread_index();
    return osoe_node_thread(&osoe.node, osoe.bootstrap.local_id, *index);
}

/* Tokens this thread has made, for the sequence bits of the next (node.h). */
static _Thread_local uint64_t token_sequence;

static uint64_t make_token(bool nonblocking, uint32_t owner, uint64_t extent)
{
    return (nonblocking ? OSOE_LANDING_NONBLOCKING : 0) |
           ((uint64_t)owner << OSOE_LANDING_OWNER_SHIFT) |
           ((++token_sequence & UINT64_C(0xffffffff)) << 16) | extent;
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
 * Wait for the reply carrying `token` in this thread's completion slot, which
 * is what §4.6 means by a blocking get or fetching AMO being complete per
 * operation: the caller waits for *its* reply, not for a byte count. The token
 * is what makes "its" checkable rather than assumed. The wake word is loaded
 * **before** the flag it guards (§4.6), so a bump between the load and the
 * wait makes the wait return at once rather than sleep on a value that has
 * already moved.
 */
static void await_reply(struct osoe_thread_record *slot, uint64_t token, uint32_t peer, int pe,
                        const char *routine)
{
    bool waiting = false;
    struct am_wait w;
    for (;;) {
        uint32_t seen = atomic_load_explicit(&slot->wake, memory_order_acquire);
        if (atomic_load_explicit(&slot->ready, memory_order_acquire) &&
            atomic_load_explicit(&slot->token, memory_order_acquire) == token)
            break;
        /* Only this thread's replies bump the slot's wake word, so it is also
         * the progress word: it stands still only while its reply is away. */
        if (!waiting) {
            am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_REPLY, "the thread's reply slot",
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

/* Arm this thread's slot for `token`: published before the request goes out,
 * whose send's reservation and commit order it before any reply can exist. */
static void arm(struct osoe_thread_record *slot, uint64_t token)
{
    atomic_store_explicit(&slot->ready, 0, memory_order_release);
    atomic_store_explicit(&slot->armed_token, token, memory_order_release);
}

/* Publish a piece's record, its fields before its state (node.h). */
static void issue_record(uint32_t extent, uint64_t token, uint32_t length)
{
    struct osoe_landing_record *r = &my_records()[extent];
    atomic_store_explicit(&r->token, token, memory_order_release);
    atomic_store_explicit(&r->length, length, memory_order_release);
    atomic_store_explicit(&r->state, OSOE_LANDING_ISSUED, memory_order_release);
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

/* The largest piece a get of this connection's may land at once: the zone's,
 * and the wire's (payload_limit). */
static uint32_t piece_limit(struct osoe_connection *connection, uint32_t peer)
{
    uint32_t limit = osoe.node.landing_data_bytes;
    uint32_t wire = payload_limit(connection, peer, (uint32_t)sizeof(struct osoe_am_get_reply));
    return limit < wire ? limit : wire;
}

/*
 * A blocking get, a piece at a time: each piece claims a run of extents,
 * arms this thread's slot and its record with the piece's token, goes out, and
 * waits in the slot for the lead's completion; then this thread copies the
 * bytes out, gives the extents back, and returns the credit. Under
 * THREAD_MULTIPLE every thread has its own slot, and every piece its own
 * extents, so blocking gets from several threads run at once (§4.11 item 5).
 */
static void remote_get(int pe, enum osoe_region region, uint64_t offset, void *dst,
                       size_t len)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    uint32_t limit = piece_limit(connection, peer), thread;
    struct osoe_thread_record *slot = my_thread(&thread);

    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        uint32_t count = extents_for(n);
        uint32_t extent = make_room(count, "shmem_get");
        uint64_t token = make_token(false, thread, extent);
        arm(slot, token);
        issue_record(extent, token, n);
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
        memcpy(at, osoe_node_landing_arena(&osoe.node, osoe.bootstrap.local_id) +
                       (size_t)extent * OSOE_LANDING_EXTENT_BYTES,
               n);
        struct osoe_landing_record *r = &my_records()[extent];
        atomic_store_explicit(&r->token, 0, memory_order_release);
        atomic_store_explicit(&r->state, OSOE_LANDING_FREE, memory_order_release);
        release_run(extent, count);
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
 * Complete every nonblocking get context `c` has in flight: copy out each of
 * its pieces that has landed and that no other thread has claimed first
 * (copy_out), and wait until every byte the context asked for has been copied
 * out, by whichever of the PE's threads (§4.6's counters, in the context's
 * block, bytes_copied against bytes_issued; copy_out_claim_model.py). A wait on
 * the context calls it - a quiet, and all that completes as one. A piece of
 * `pending` another thread copied out has a record that no longer holds its
 * token, or none landed: copy_out's claim fails and the loop moves on, and the
 * count is what says it is done.
 */
static void complete_gets(struct osoe_transport_context *c, const char *routine)
{
    if (c->pending_count == 0)
        return;
    struct osoe_context_block *block = block_of(c);
    uint64_t issued = atomic_load_explicit(&block->bytes_issued, memory_order_acquire);
    struct am_wait w;
    bool waiting = false;
    uint32_t slept_on = 0;
    bool swept = false;
    osoe_wait_word_enter(&block->counter_wake);
    for (;;) {
        uint32_t seen = osoe_wait_word_load(&block->counter_wake);
        for (uint32_t i = 0; i < c->pending_count; i++) {
            const struct pending_piece *piece = &c->pending[i];
            if (atomic_load_explicit(&my_records()[piece->extent].token, memory_order_acquire) ==
                piece->token)
                copy_out(piece->extent);
        }
        uint64_t copied = atomic_load_explicit(&block->bytes_copied, memory_order_seq_cst);
        /* Past equality it can never return: a reply counted twice, which the
         * lead's checks exist to refuse (progress.c). */
        if (copied > issued)
            osoe_fatal("%s: %llu bytes of this context's nonblocking gets were copied out, "
                       "which asked for %llu", routine, (unsigned long long)copied,
                       (unsigned long long)issued);
        if (copied == issued) {
            if (swept)
                count_if_lost(&block->counter_wake.word, slept_on, routine);
            break;
        }
        if (!waiting) {
            /* Whatever this PE held back goes out before it waits (transport.h).
             * The gets may be toward any number of nodes, so the wait names
             * none: its record says how many bytes are still to come. */
            osoe_progress_host_wake_lead();
            am_wait_begin_detail(&w, routine, OSOE_BLOCKED_DETAIL_GETS_IN_FLIGHT,
                                 issued - copied, "the bytes its nonblocking gets have landed",
                                 UINT32_MAX, -1, copied);
            waiting = true;
        }
        slept_on = seen;
        swept = sleep_on(&block->counter_wake, seen);
        am_wait_check(&w, atomic_load_explicit(&block->bytes_copied, memory_order_seq_cst));
    }
    osoe_wait_word_leave(&block->counter_wake);
    if (waiting)
        am_wait_end();
    c->pending_count = 0;
}

static void pending_add(struct osoe_transport_context *c, uint32_t extent, uint64_t token)
{
    if (c->pending_count == c->pending_capacity) {
        uint32_t capacity = c->pending_capacity ? 2 * c->pending_capacity : 16;
        struct pending_piece *grown = realloc(c->pending, capacity * sizeof *grown);
        if (!grown)
            osoe_fatal("out of memory for a context's nonblocking gets in flight");
        c->pending = grown;
        c->pending_capacity = capacity;
    }
    c->pending[c->pending_count++] = (struct pending_piece){ extent, token };
}

/*
 * A nonblocking get (§4.6, as built): ask, and return. Each piece claims a run
 * of extents (make_room, which copies out landed pieces of any context and
 * then waits when the zone is full), records where its reply goes, publishes
 * its record and its token, counts its bytes in the context's block, and goes
 * out; the lead lands the reply, marks the record landed, counts it and
 * returns the credit, and the context's next wait - or any issue that found
 * no room - copies it into `dst`.
 */
static void remote_get_nonblocking(struct osoe_transport_context *c, int pe,
                                   enum osoe_region region, uint64_t offset, void *dst,
                                   size_t len, unsigned flags)
{
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    uint32_t limit = piece_limit(connection, peer);
    struct osoe_context_block *block = block_of(c);
    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        uint32_t extent = make_room(extents_for(n), "shmem_get_nbi");
        uint64_t token = make_token(true, c->block, extent);
        copy_destination[extent] = at;
        pending_add(c, extent, token);
        atomic_fetch_add_explicit(&block->bytes_issued, n, memory_order_seq_cst);
        /* Published before the request goes out: the send's reservation and
         * commit order it before any reply can exist (as arm() for a slot). */
        issue_record(extent, token, n);

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
    struct osoe_thread_record *slot = NULL;
    uint64_t token = 0;

    if (fetching) {
        /* Its reply is a value, into this thread's slot, and no extent. */
        uint32_t thread;
        slot = my_thread(&thread);
        token = make_token(false, thread, OSOE_LANDING_NO_EXTENT);
        arm(slot, token);
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
    context_enter(context);
    remote_put(context, pe, region, offset, src, len, flags);
    context_leave(context);
}

void osoe_transport_get(struct osoe_transport_context *context, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe)) {
        /* Applied before it returns, nonblocking or not: complete early, which
         * a nonblocking get may be (transport.h). */
        osoe_transport_local_get(pe, region, offset, dst, len);
        return;
    }
    osoe_count(OSOE_C_TRANSPORT_GET_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_GET_BYTES, len);
    context_enter(context);
    if (flags & OSOE_TRANSPORT_NONBLOCKING)
        remote_get_nonblocking(context, pe, region, offset, dst, len, flags);
    else
        /* A blocking get is complete on return, so it never joins the dirty
         * set: §4.6 makes gets a quiet-only concern and there is nothing left
         * for a quiet to wait for once the bytes are in dst. */
        remote_get(pe, region, offset, dst, len);
    context_leave(context);
}

uint64_t osoe_transport_rmw(struct osoe_transport_context *context, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags)
{
    if (osoe_routes_in_process(&osoe.job, (uint32_t)pe))
        return osoe_transport_local_rmw(pe, region, offset, op, width, operand, compare,
                                        flags);
    osoe_count(OSOE_C_TRANSPORT_RMW_CALLS, 1);
    bool fetching = (flags & OSOE_TRANSPORT_FETCH) != 0;
    if (fetching)
        osoe_count(OSOE_C_TRANSPORT_RMW_FETCHING, 1);
    context_enter(context);
    uint64_t previous = remote_rmw(context, pe, region, offset, op, width, operand, compare,
                                   flags);
    if (fetching)
        context_leave(context);
    return previous;
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
static void fence_body(struct osoe_transport_context *context)
{
    osoe_count(OSOE_C_TRANSPORT_FENCE_CALLS, 1);
    /* Also on a NOSTORE context: spec §9.5 lets fence skip ordering the
     * application's own stores there, but on the intranode path this fence is
     * also what orders the context's puts, each a memcpy in this thread
     * (base/order.h), and those it must still order (transport.h). */
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
static void wait_body(struct osoe_transport_context *context, const int *pes, size_t npes,
                      const char *routine)
{
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TRANSPORT_WAIT_CALLS, 1);
    uint64_t t0 = osoe_now_ns();
    /* This context's nonblocking gets, every one of them, whatever PEs the
     * wait names: their completion is counted per context, not per target
     * (§4.6). First, so that the stores copying them into place come before
     * quiet's fence and are ordered as every other store before it is. */
    complete_gets(context, routine);
    /* On a NOSTORE context too, for fence's reason above. */
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

void osoe_transport_fence(struct osoe_transport_context *context)
{
    context_enter(context);
    fence_body(context);
    context_leave(context);
}

void osoe_transport_wait(struct osoe_transport_context *context, const int *pes, size_t npes,
                         const char *routine)
{
    context_enter(context);
    wait_body(context, pes, npes, routine);
    context_leave(context);
}

void osoe_transport_wait_gets(struct osoe_transport_context *context, const char *routine)
{
    context_enter(context);
    complete_gets(context, routine);
    context_leave(context);
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
