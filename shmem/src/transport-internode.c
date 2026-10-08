/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The AM layer's far half (§3, §4.10): the internode backend's operations
 * toward a PE on another node, and the fence and wait of a job of more than one
 * node (internode-backend.h).
 *
 * transport-am.c owns §4.10's entry points and the part of the transport every
 * job uses; it hands this file every operation toward a PE that does not route
 * in the calling process, and, in a job of more than one node, a context's
 * fence and waits and its agent's calls. A job of one node never comes here.
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
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>

#include "am.h"
#include "am-state.h"
#include "base/deadline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/order.h"
#include "base/ring-check.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "base/wait-word.h"
#include "blocked.h"
#include "connection-area.h"
#include "fatal.h"
#include "internode-backend.h"
#include "job.h"
#include "metrics.h"
#include "node.h"
#include "perturb.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"
#include "transport.h"
#include "transport-context.h"

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
 * A context's state here (§4.6), behind its `internode` (transport-context.h).
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
struct osoe_internode_context {
    /* Serial mode's dirty set (§4.6). */
    uint64_t *high_segment;     /* per peer slot: the highest segment written into */
    uint32_t *order;            /* the slots touched, in the order first touched */
    bool *touched;              /* membership, so marking twice is not quadratic */
    uint32_t count;
    /* Shared mode's (§4.11 item 2): the highest segment per peer slot, raised
     * by compare-and-swap, and a bit per slot set after the raise and cleared
     * only by a wait holding the context's mutex. Process memory, like the
     * serial set: O(peer nodes) a context, never in the node segment. */
    _Atomic uint64_t *shared_highest_segment;
    _Atomic uint64_t *dirty_bits;
    /* OSOE_CHECK_QUIET only: per slot, the highest segment of a write whose
     * mark has finished - what a quiet called afterwards owes. */
    _Atomic uint64_t *returned_highest;
    /* This context's nonblocking gets in flight, as (extent, token) pairs:
     * the pieces its wait copies out, if no other thread of the PE has
     * (copy_out). Touched only by calls on this context. */
    struct pending_piece *pending;
    uint32_t pending_count, pending_capacity;
};

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

/*
 * OSOE_CHECK_QUIET (phase 1 step 8 slice 3; docs/decisions/2026-09-26-evening.md
 * item 12), as transport-am.c read it: every wait on a shared context checks,
 * at its return, what it promised - every node it covers has acknowledged
 * through the highest segment written toward it when the call began, and no
 * nonblocking get of the context whose call returned before this one began is
 * still in the landing zone - and ends the job naming the first it finds
 * broken. A race in those protocols - a put a quiet lets go of, a get it closes
 * the wrong pair on - shows only when the reply or the acknowledgement is
 * still on its way at the quiet's return, and a thread reading its own write
 * back rides the same ordered connection and cannot see it.
 */
static bool check_quiet;

static _Atomic uint64_t issue_clock;      /* nonblocking get calls returned */
/*
 * Per extent, the stamp of the call whose get it holds: that piece's issue
 * number in the high 24 bits and the call's clock in the low 40, the clock all
 * ones until the call returns. The issue number is there because a call
 * stamps its extents only as it returns, and by then one may have been
 * recycled - its reply landed, another thread copied it out and freed it, and
 * a later get took the extent. So a stamp is a compare-and-swap from its own
 * issue's unstamped word, and a late one lands nowhere else. As a plain store
 * it stamped a stranger's get with an earlier call's clock, and a wait that
 * began between the two read that get as one it had to complete: the
 * OSOE_CHECK_QUIET firing of 2026-09-26, reproduced by osoe-pacer's threads
 * exercise on 2026-09-27 in 5 of 11 runs, and in 0 of 12 once stamps were
 * owned (docs/decisions/2026-09-27.md). A stamp 16 million issues late would
 * wrap onto a live one; none is.
 */
static _Atomic uint64_t *issue_stamp;
#define UNSTAMPED UINT64_MAX
#define STAMP_BITS 40
#define STAMP_MASK ((UINT64_C(1) << STAMP_BITS) - 1)
#define ISSUE_MASK ((UINT64_C(1) << (64 - STAMP_BITS)) - 1)
static _Atomic uint64_t issue_count;      /* nonblocking get pieces issued */

/* The dirty set's arrays, O(peer nodes) virtual, resident as touched (§4.6):
 * internode-backend.h's context open. */
static void context_state_free(struct osoe_internode_context *s)
{
    free(s->high_segment);
    free(s->order);
    free(s->touched);
    free(s->pending);
    free(s->shared_highest_segment);
    free(s->dirty_bits);
    free(s->returned_highest);
    free(s);
}

bool osoe_internode_backend_context_open(struct osoe_transport_context *c)
{
    uint32_t peers = peer_count();
    struct osoe_internode_context *s = calloc(1, sizeof *s);
    if (!s)
        return false;
    s->high_segment = calloc(peers, sizeof *s->high_segment);
    s->order = calloc(peers, sizeof *s->order);
    s->touched = calloc(peers, sizeof *s->touched);
    /* Both sets, whatever the mode: the default context's mode is set after
     * it is opened (osoe_transport_threads). */
    s->shared_highest_segment = calloc(peers, sizeof *s->shared_highest_segment);
    s->dirty_bits = calloc((peers + 63) / 64, sizeof *s->dirty_bits);
    if (check_quiet)
        s->returned_highest = calloc(peers, sizeof *s->returned_highest);
    if (!s->high_segment || !s->order || !s->touched || !s->shared_highest_segment ||
        !s->dirty_bits || (check_quiet && !s->returned_highest)) {
        context_state_free(s);
        return false;
    }
    c->internode = s;
    return true;
}

void osoe_internode_backend_context_close(struct osoe_transport_context *c)
{
    context_state_free(c->internode);
    c->internode = NULL;
}

/*
 * A shared context's mutex (§4.11 item 4; phase 1 step 8 slice 3): taken by
 * its waits - quiet, pe_quiet, a wait for its gets, and destroy's implicit
 * quiet - and by nothing that issues. Serializing the waits is what makes
 * closing a counter pair and clearing a dirty bit sound (the models' `nolock`
 * and `unlocked`); issue is lock-free, as the transport under it already is.
 * Held across nothing but the network. Slice 2 held it around every call, an
 * interim. Below THREAD_MULTIPLE nothing is shared and no mutex is taken.
 */
static void context_enter(struct osoe_transport_context *c)
{
    if (c->concurrent) {
        pthread_mutex_lock(&c->mutex);
        osoe_count(OSOE_C_AM_CONTEXT_LOCKS, 1);
    }
}

static void context_leave(struct osoe_transport_context *c)
{
    if (c->concurrent)
        pthread_mutex_unlock(&c->mutex);
}

/*
 * internode-backend.h's check: the ring, which a job of more than one node is
 * the one kind to open, and whose checks are the kernel's (§1: the library
 * checks what it uses, before anything is created) - every local PE's alike,
 * so the part every job uses refuses with what this says, telling no one.
 */
const char *osoe_internode_backend_check(const struct osoe_bootstrap *b, char *why,
                                         size_t len)
{
    const char *facility;
    int ring_error = b->node_count > 1 ? osoe_ring_check(&facility) : 0;
    if (!ring_error)
        return NULL;
    struct utsname u;
    const char *release = uname(&u) == 0 ? u.release : "unknown";
    snprintf(why, len,
             "osoe needs io_uring with %s for a job of %u nodes (it fails with \"%s\"%s); "
             "this kernel is %s", facility, b->node_count, strerror(ring_error),
             ring_error == EPERM && facility == osoe_ring_check_creating
                 ? ", which usually means the kernel.io_uring_disabled sysctl is set"
                 : "", release);
    return why;
}

/* internode-backend.h's start: the AM state afresh, the two facts about the job
 * the engine reads first, the landing zone's bookkeeping, then the engine
 * itself (progress-host.c), complete before this returns. */
void osoe_internode_backend_start(uint32_t session, bool check)
{
    osoe_am = (struct osoe_am_state){ 0 };
    osoe_am.internode = true;
    osoe_am.lead = osoe.bootstrap.local_id == 0;
    check_quiet = check;
    copy_destination = calloc(osoe.node.landing_requests, sizeof *copy_destination);
    if (!copy_destination)
        osoe_fatal("out of memory for %u landing extents", osoe.node.landing_requests);
    if (check_quiet) {
        issue_stamp = calloc(osoe.node.landing_requests, sizeof *issue_stamp);
        if (!issue_stamp)
            osoe_fatal("out of memory for OSOE_CHECK_QUIET's stamps");
        for (uint32_t e = 0; e < osoe.node.landing_requests; e++)
            atomic_store_explicit(&issue_stamp[e], UNSTAMPED, memory_order_seq_cst);
    }
    osoe_progress_host_start(session);
}

/* After the agent has been joined: the engine drained what this node owed the
 * network and closed its connections on that thread, in its exit; what is left
 * is memory. */
void osoe_internode_backend_stop(void)
{
    osoe_progress_host_stop();
    free(copy_destination);
    copy_destination = NULL;
    free(issue_stamp);
    issue_stamp = NULL;
    check_quiet = false;
    osoe_am = (struct osoe_am_state){ 0 };
}

/*
 * §6.2's blob, on AM: the port this lead's node-pair listener took. The
 * listener is opened here, before the rendezvous, so its port can ride in the
 * blob to every node; the connections are made at start, once the rendezvous
 * has said where every node is (osoe_wires_open).
 */
void osoe_internode_backend_advertise(void *blob)
{
    osoe_progress_host_advertise(blob);
}

/* The agent thread, lent to this backend (transport.h, §3.12). */
bool osoe_internode_backend_agent_pass(void)
{
    return osoe_progress_host_poll();
}

bool osoe_internode_backend_agent_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    return osoe_progress_host_wait(wake, seen);
}

void osoe_internode_backend_agent_exit(void)
{
    osoe_progress_host_exit();
}

void osoe_internode_backend_agent_abort(void)
{
    osoe_progress_host_abort();
}

bool osoe_internode_backend_node_idle(uint64_t *applied)
{
    return osoe_progress_host_idle(applied);
}

bool osoe_internode_backend_node_quiet(uint64_t *applied)
{
    return osoe_progress_host_quiet(applied);
}

void osoe_internode_backend_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m)
{
    osoe_progress_host_deadlock_send(node, m);
}

/* ---- The far half ------------------------------------------------------ */

/* Record that this context has written into `seq` toward `peer` (§4.6). */
static void mark(struct osoe_transport_context *c, uint32_t peer, uint64_t seq)
{
    uint32_t slot = peer_slot(peer);
    if (c->concurrent) {
        /*
         * §4.11 item 2, as dirty_set_model.py checks it: raise the slot's
         * highest segment by compare-and-swap (a load and a store lets a lower
         * number overwrite a higher, `plainmax`), *then* set its bit (the bit
         * first lets a quiet clear it against an old number, `bitfirst`). The
         * bit's fetch_or only when it is clear: once a segment a node, not
         * once an operation.
         */
        osoe_perturb();
        _Atomic uint64_t *high = &c->internode->shared_highest_segment[slot];
        uint64_t seen = atomic_load_explicit(high, memory_order_seq_cst);
        osoe_perturb();
        while (seq > seen && !atomic_compare_exchange_weak_explicit(
                                 high, &seen, seq, memory_order_seq_cst, memory_order_seq_cst))
            ;
        osoe_perturb();
        _Atomic uint64_t *word = &c->internode->dirty_bits[slot / 64];
        uint64_t bit = UINT64_C(1) << (slot % 64);
        if (!(atomic_load_explicit(word, memory_order_seq_cst) & bit))
            atomic_fetch_or_explicit(word, bit, memory_order_seq_cst);
        if (c->internode->returned_highest) {
            _Atomic uint64_t *done = &c->internode->returned_highest[slot];
            uint64_t was = atomic_load_explicit(done, memory_order_seq_cst);
            while (seq > was && !atomic_compare_exchange_weak_explicit(
                                    done, &was, seq, memory_order_seq_cst, memory_order_seq_cst))
                ;
        }
        return;
    }
    if (!c->internode->touched[slot]) {
        c->internode->touched[slot] = true;
        c->internode->order[c->internode->count++] = slot;
    }
    if (seq > c->internode->high_segment[slot])
        c->internode->high_segment[slot] = seq;
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
    osoe_wait_ends();               /* the deadlock detector's count (base/sweep.h) */
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
    osoe_wait_sleeps();
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
    osoe_wait_ends();               /* the wait is over; this sleep is no wait's */
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
 * has its own reservation to retry. A put's part takes the room it finds, so
 * writers of large puts each get some of a reopened buffer, where before v0.2
 * step 4 one 64 KiB message filled it and the rest slept again (the release
 * fixes' review, S1); a whole message larger than the room left still sleeps
 * again. It asks again before every sleep, since the buffer that just opened
 * may have filled before this writer got to it.
 *
 * Step 2 slept 20 us and looked again instead, which on the ring held a stream
 * to 3% of the floor (§8.4). am.reserve_stalls counts every wait, so a buffer
 * that is often full is visible rather than assumed rare.
 *
 * It takes up to `len` and at least `least`, leaving `keep` at the buffer's end
 * (osoe_connection_reserve_up_to): a put's part takes the room there is less a
 * spare, and every other message passes its whole length as both and keeps
 * nothing.
 */
static void reserve_waiting(struct osoe_connection *connection, uint32_t peer, int pe,
                            const char *routine, uint32_t len, uint32_t least, uint32_t keep,
                            struct osoe_reservation *r)
{
    if (osoe_connection_reserve_up_to(connection, len, least, keep, r) == OSOE_RESERVATION_OK)
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
        if (osoe_connection_reserve_up_to(connection, len, least, keep, r) == OSOE_RESERVATION_OK) {
            if (swept)
                count_if_lost(&word->word, slept_on, routine);
            break;
        }
        /* The nudge is the lead's to see, so not a waiting thread's to make
         * (base/sweep.h): this thread stops waiting for it, and sleep_on counts
         * it again. */
        osoe_wait_ends();
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
    uint32_t overhead = (uint32_t)(sizeof(struct osoe_message_header) + sizeof(struct osoe_am_put));
    /* The least payload a part takes when it fills the room a buffer has left -
     * a sixteenth of the largest, 4,094 bytes at the default chunk: below it the part
     * waits for the next buffer, since each part pays a message's header and
     * its parse at the peer, and a sliver of room is better left empty. */
    uint32_t smallest_part = limit / 16 ? limit / 16 : 1;
    /* What a part larger than it leaves at the end of a buffer it cuts: room for
     * the small messages - a fetch-add, a get's request, a signal - that a
     * stream filling every buffer would otherwise hold back for a turnover.
     * Measured on the Pi, a fetch-add behind a put stream waited 0.3 ms more at
     * the median and up to 1 ms more at the 99th percentile without it
     * (docs/decisions/2026-10-07-one-thread-profile.md). A 128th of the
     * buffer, 1 KiB at the default, and at least 256 bytes. */
    uint32_t spare = connection->capacity / 128 > 256 ? connection->capacity / 128 : 256;
    spare &= ~(OSOE_MESSAGE_ALIGN - 1);
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
        /*
         * Fill the room (v0.2 step 4's first rung): this part takes as much of
         * the open buffer as there is, less the spare, down to smallest_part of
         * payload, or the whole remainder if that is less - so a buffer carries what it holds,
         * where a chunk at a time left a buffer of twice the chunk carrying
         * one, and a stream waited for a turnover every chunk
         * (docs/decisions/2026-10-07-one-thread-profile.md). The parts of a put
         * are successive messages of this thread, as its chunks always were:
         * they reach the peer in order, and a signal's fenced rmw after the
         * last (reservation_model.py, the `fill` cases).
         */
        uint32_t least = osoe_message_padded((uint32_t)sizeof m +
                                             (n < smallest_part ? n : smallest_part));
        uint32_t keep = osoe_message_padded(body) <= spare ? 0 : spare;
        reserve_waiting(connection, peer, pe, "shmem_put", osoe_message_padded(body), least,
                        keep, &r);
        if (r.len < osoe_message_padded(body)) {
            /* A part: the reservation is 8-byte padded, so its payload is too. */
            n = r.len - overhead;
            body = (uint32_t)sizeof m + n;
            h.len = body;
        }
        unsigned char *to = osoe_reservation_bytes(connection, &r);
        memcpy(to, &h, sizeof h);
        memcpy(to + sizeof h, &m, sizeof m);
        memcpy(to + sizeof h + sizeof m, at, n);
        osoe_connection_commit(connection, &r);
        /* After every commit, not once per put: the next part may have to
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
    /* seq_cst, as the check of a wait-word wait (base/wait-word.h): make_room
     * sleeps on `freed` after this finds no room, and a release skips the wake
     * when no waiter is registered (review of slice 2, S1). */
    uint64_t seen = atomic_load_explicit(bits, memory_order_seq_cst);
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
                                                  memory_order_seq_cst, memory_order_seq_cst))
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
    /* A copy another thread may be waiting for, which a waiting thread makes
     * only once it has stopped waiting as the deadlock detector counts it. */
    osoe_wait_ends();
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
    atomic_fetch_add_explicit(&c->bytes_copied[(token & OSOE_LANDING_PAIR) ? 1 : 0], length,
                              memory_order_seq_cst);
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
        if (atomic_load_explicit(&my_records()[e].state, memory_order_seq_cst) ==
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
        osoe_wait_sleeps();
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
           ((++token_sequence & OSOE_LANDING_SEQUENCE_MASK) << 16) | extent;
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
        osoe_wait_sleeps();
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
        osoe_wait_sleeps();
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
    reserve_waiting(connection, peer, pe, routine, osoe_message_padded(body),
                    osoe_message_padded(body), 0, &r);
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
    if (c->internode->pending_count == 0)
        return;
    struct osoe_context_block *block = osoe_transport_context_block(c);
    uint64_t issued = atomic_load_explicit(&block->bytes_issued[0], memory_order_acquire);
    struct am_wait w;
    bool waiting = false;
    uint32_t slept_on = 0;
    bool swept = false;
    osoe_wait_word_enter(&block->counter_wake);
    for (;;) {
        uint32_t seen = osoe_wait_word_load(&block->counter_wake);
        for (uint32_t i = 0; i < c->internode->pending_count; i++) {
            const struct pending_piece *piece = &c->internode->pending[i];
            if (atomic_load_explicit(&my_records()[piece->extent].token, memory_order_acquire) ==
                piece->token)
                copy_out(piece->extent);
        }
        uint64_t copied = atomic_load_explicit(&block->bytes_copied[0], memory_order_seq_cst);
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
        am_wait_check(&w, atomic_load_explicit(&block->bytes_copied[0], memory_order_seq_cst));
    }
    osoe_wait_word_leave(&block->counter_wake);
    if (waiting)
        am_wait_end();
    c->internode->pending_count = 0;
}

/*
 * A shared context's gets in pair `pair`, which the caller has closed by
 * bumping the generation while holding the context's mutex (§4.11 item 1;
 * get_completion_model.py): wait until every byte counted in it has been
 * copied out, copying out meanwhile every landed piece of this context's that
 * no other thread has claimed first. The pieces are found by scanning the
 * PE's landing records for this context's owner bits (the slice 3 brief,
 * decision 5): the zone's extent count of loads, against a network round
 * trip, and no word of its own to keep. A piece of the other pair copied here
 * is an early completion, which the boundary allows. An issuer that added to
 * this pair and then saw the generation move takes its add back and wakes
 * the word, so a transient excess ends.
 */
static void complete_pair(struct osoe_transport_context *c, unsigned pair, const char *routine)
{
    struct osoe_context_block *block = osoe_transport_context_block(c);
    /* Nothing outstanding in the pair: no scan, no wait (review of slice 3). */
    if (atomic_load_explicit(&block->bytes_issued[pair], memory_order_seq_cst) ==
        atomic_load_explicit(&block->bytes_copied[pair], memory_order_seq_cst))
        return;
    struct am_wait w;
    bool waiting = false;
    uint32_t slept_on = 0;
    bool swept = false;
    osoe_wait_word_enter(&block->counter_wake);
    for (;;) {
        uint32_t seen = osoe_wait_word_load(&block->counter_wake);
        for (uint32_t e = 0; e < osoe.node.landing_requests; e++) {
            struct osoe_landing_record *r = &my_records()[e];
            if (atomic_load_explicit(&r->state, memory_order_seq_cst) != OSOE_LANDING_LANDED)
                continue;
            uint64_t token = atomic_load_explicit(&r->token, memory_order_acquire);
            if ((token & OSOE_LANDING_NONBLOCKING) &&
                ((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK) == c->block)
                copy_out(e);
        }
        uint64_t issued = atomic_load_explicit(&block->bytes_issued[pair], memory_order_seq_cst);
        uint64_t copied = atomic_load_explicit(&block->bytes_copied[pair], memory_order_seq_cst);
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
            osoe_progress_host_wake_lead();
            am_wait_begin_detail(&w, routine, OSOE_BLOCKED_DETAIL_GETS_IN_FLIGHT,
                                 issued - copied, "the bytes its nonblocking gets have landed",
                                 UINT32_MAX, -1, copied);
            waiting = true;
        }
        slept_on = seen;
        swept = sleep_on(&block->counter_wake, seen);
        am_wait_check(&w, atomic_load_explicit(&block->bytes_copied[pair], memory_order_seq_cst));
    }
    osoe_wait_word_leave(&block->counter_wake);
    if (waiting)
        am_wait_end();
}

static void pending_add(struct osoe_transport_context *c, uint32_t extent, uint64_t token)
{
    if (c->internode->pending_count == c->internode->pending_capacity) {
        uint32_t capacity =
            c->internode->pending_capacity ? 2 * c->internode->pending_capacity : 16;
        struct pending_piece *grown = realloc(c->internode->pending, capacity * sizeof *grown);
        if (!grown)
            osoe_fatal("out of memory for a context's nonblocking gets in flight");
        c->internode->pending = grown;
        c->internode->pending_capacity = capacity;
    }
    c->internode->pending[c->internode->pending_count++] = (struct pending_piece){ extent, token };
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
/* OSOE_CHECK_QUIET: the extents of the nonblocking get call in progress on
 * this thread, and each one's issue number, stamped together when the call
 * returns. */
static _Thread_local uint32_t *call_extents;
static _Thread_local uint64_t *call_issues;
static _Thread_local uint32_t call_extent_count, call_extent_capacity;

static void call_extent_add(uint32_t extent, uint64_t issue)
{
    if (call_extent_count == call_extent_capacity) {
        uint32_t capacity = call_extent_capacity ? 2 * call_extent_capacity : 16;
        uint32_t *grown = realloc(call_extents, capacity * sizeof *grown);
        uint64_t *issues = realloc(call_issues, capacity * sizeof *issues);
        if (!grown || !issues)
            osoe_fatal("out of memory for OSOE_CHECK_QUIET's stamps");
        call_extents = grown;
        call_issues = issues;
        call_extent_capacity = capacity;
    }
    call_issues[call_extent_count] = issue;
    call_extents[call_extent_count++] = extent;
}

static void remote_get_nonblocking(struct osoe_transport_context *c, int pe,
                                   enum osoe_region region, uint64_t offset, void *dst,
                                   size_t len, unsigned flags)
{
    call_extent_count = 0;
    uint32_t peer = osoe_node_of(&osoe.job, (uint32_t)pe);
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which holds PE %d", peer, pe);
    uint32_t limit = piece_limit(connection, peer);
    struct osoe_context_block *block = osoe_transport_context_block(c);
    unsigned char *at = dst;
    while (len > 0) {
        uint32_t n = len > limit ? limit : (uint32_t)len;
        uint32_t extent = make_room(extents_for(n), "shmem_get_nbi");
        if (issue_stamp) {
            uint64_t issue = (atomic_fetch_add_explicit(&issue_count, 1, memory_order_seq_cst) +
                              1) & ISSUE_MASK;
            atomic_store_explicit(&issue_stamp[extent], (issue << STAMP_BITS) | STAMP_MASK,
                                  memory_order_seq_cst);
            call_extent_add(extent, issue);
        }
        unsigned pair = 0;
        if (c->concurrent) {
            /*
             * §4.11 item 1: count in the pair of the generation loaded, then
             * load it again; if a quiet bumped it between, take the add back,
             * wake whoever waits on the pair, and count in the other. Without
             * the second load the next quiet misses this get (`noretag`).
             */
            for (;;) {
                uint64_t g = atomic_load_explicit(&block->generation, memory_order_seq_cst);
                osoe_perturb();
                atomic_fetch_add_explicit(&block->bytes_issued[g & 1], n, memory_order_seq_cst);
                osoe_perturb();
                if (atomic_load_explicit(&block->generation, memory_order_seq_cst) == g) {
                    pair = g & 1;
                    break;
                }
                atomic_fetch_sub_explicit(&block->bytes_issued[g & 1], n, memory_order_seq_cst);
                osoe_wait_word_wake_all(&block->counter_wake);
                osoe_count(OSOE_C_AM_ISSUE_RETRIES, 1);
            }
        } else {
            atomic_fetch_add_explicit(&block->bytes_issued[0], n, memory_order_seq_cst);
        }
        uint64_t token = make_token(true, c->block, extent) | (pair ? OSOE_LANDING_PAIR : 0);
        copy_destination[extent] = at;
        if (!c->concurrent)
            pending_add(c, extent, token);
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
    if (issue_stamp) {
        uint64_t stamp = atomic_fetch_add_explicit(&issue_clock, 1, memory_order_seq_cst) + 1;
        for (uint32_t i = 0; i < call_extent_count; i++) {
            uint64_t unstamped = (call_issues[i] << STAMP_BITS) | STAMP_MASK;
            atomic_compare_exchange_strong_explicit(
                &issue_stamp[call_extents[i]], &unstamped,
                (call_issues[i] << STAMP_BITS) | (stamp & STAMP_MASK), memory_order_seq_cst,
                memory_order_seq_cst);
        }
        free(call_issues);
        call_issues = NULL;
        /* Freed with the call, not kept per thread: a thread-local list would
         * outlive its thread (asan, the verification of slice 3). */
        free(call_extents);
        call_extents = NULL;
        call_extent_count = call_extent_capacity = 0;
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

/* ---- internode-backend.h's operations ---------------------------------- */

void osoe_internode_backend_put(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, const void *src,
                                size_t len, unsigned flags)
{
    remote_put(context, pe, region, offset, src, len, flags);
}

void osoe_internode_backend_get(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, void *dst, size_t len,
                                unsigned flags)
{
    if (flags & OSOE_TRANSPORT_NONBLOCKING)
        remote_get_nonblocking(context, pe, region, offset, dst, len, flags);
    else
        /* A blocking get is complete on return, so it never joins the dirty
         * set: §4.6 makes gets a quiet-only concern and there is nothing left
         * for a quiet to wait for once the bytes are in dst. */
        remote_get(pe, region, offset, dst, len);
}

uint64_t osoe_internode_backend_rmw(struct osoe_transport_context *context, int pe,
                                    enum osoe_region region, uint64_t offset,
                                    enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                    uint64_t compare, unsigned flags)
{
    return remote_rmw(context, pe, region, offset, op, width, operand, compare, flags);
}

/* ---- Ordering and completion (§4.6) ------------------------------------ */

/* ---- Ordering and completion (§4.6) ------------------------------------ */

/* Has `peer` acknowledged everything this context wrote toward it? Read out of
 * the lead's connection state, which only the lead writes (§3.12). */
static bool acknowledged(const struct osoe_transport_context *c, uint32_t slot)
{
    uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
    uint64_t high = c->concurrent
                        ? atomic_load_explicit(&c->internode->shared_highest_segment[slot],
                                               memory_order_seq_cst)
                        : c->internode->high_segment[slot];
    return osoe_progress_host_acked(peer) >= high;
}

/* Ask `slot`'s peer to acknowledge through the highest segment this context
 * wrote there, unless it already has: whether it asked. */
static bool ask_acknowledgement(const struct osoe_transport_context *c, uint32_t slot)
{
    if (acknowledged(c, slot))
        return false;
    uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        return false;
    uint64_t high = c->concurrent
                        ? atomic_load_explicit(&c->internode->shared_highest_segment[slot],
                                               memory_order_seq_cst)
                        : c->internode->high_segment[slot];
    osoe_connection_request_ack(connection, high);
    osoe_connection_area_mark(&osoe_am.area, peer);
    return true;
}

/* Ask every dirty peer that has not already acknowledged this context's writes
 * to acknowledge through them (§3.4), and wake the lead if any was asked. A
 * peer that already has acknowledged is skipped: fence used to ask it again on
 * every call until the next quiet (second review, algorithmic-scaling 10).
 * Shared by fence and wait, which differ only in what they do afterwards. */
static void flush_dirty(struct osoe_transport_context *c)
{
    bool asked = false;
    if (c->concurrent) {
        /* The bits, read and never cleared: a fence takes no lock (§4.11
         * item 2). */
        uint32_t words = (peer_count() + 63) / 64;
        for (uint32_t w = 0; w < words; w++) {
            uint64_t bits =
                atomic_load_explicit(&c->internode->dirty_bits[w], memory_order_seq_cst);
            while (bits) {
                uint32_t slot = w * 64 + (uint32_t)__builtin_ctzll(bits);
                bits &= bits - 1;
                if (ask_acknowledgement(c, slot))
                    asked = true;
            }
        }
        if (asked)
            osoe_progress_host_wake_lead();
        return;
    }
    for (uint32_t i = 0; i < c->internode->count; i++) {
        uint32_t slot = c->internode->order[i];
        if (acknowledged(c, slot))
            continue;
        uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
        struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
        /* "Acknowledge through the highest segment this context wrote into":
         * what quiet then waits for, and nothing more. */
        if (connection) {
            osoe_connection_request_ack(connection, c->internode->high_segment[slot]);
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
 * already answers "which peers has this context touched". The release fence
 * that puts the calling thread's own stores under the fence came first, in
 * transport-am.c's osoe_transport_fence, on every path.
 */
void osoe_internode_backend_fence(struct osoe_transport_context *context)
{
    flush_dirty(context);
}

/*
 * Wait until `slot`'s peer has acknowledged through segment `through`, which
 * the caller asked for (ask_acknowledgement) before waking the lead; the agent
 * bumps the connection's acknowledgement word when one lands and anyone is
 * registered (base/wait-word.h). Every waiter is woken, each checking its own
 * writes. A number, not the slot's current highest: on a shared context
 * another thread's put may raise that after it was asked for, and a wait for
 * it would wait for an acknowledgement nobody requested - the hang the first
 * build of slice 3 had. The shared wait loads the number, asks, waits for it,
 * then clears the bit and looks again, as dirty_set_model.py has it.
 */
static void wait_acknowledged(uint32_t slot, uint64_t through, const char *routine)
{
    uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
    if (osoe_progress_host_acked(peer) >= through)
        return;
    struct osoe_connection *connection = osoe_connection_to(&osoe_am.area, peer);
    if (!connection)
        osoe_fatal("no connection toward node %u, which this context wrote to", peer);
    struct osoe_wait_word *word = osoe_connection_acked_wait(connection);
    struct am_wait w;
    /* No single PE: a quiet waits for a whole node's queue, not one PE's
     * (D-6). */
    am_wait_begin(&w, routine, OSOE_BLOCKED_DETAIL_ACKNOWLEDGEMENT,
                  "the peer's acknowledged sequence", peer, -1, osoe_progress_host_acked(peer));
    osoe_wait_word_enter(word);
    uint32_t slept_on = 0;
    bool swept = false;
    for (;;) {
        uint32_t seen = osoe_wait_word_load(word);
        if (osoe_progress_host_acked(peer) >= through) {
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
    if (c->internode->count == 0) {
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
    for (uint32_t i = 0; i < c->internode->count; i++) {
        uint32_t slot = c->internode->order[i];
        if (wanted && !wanted[slot]) {
            c->internode->order[kept++] = slot;        /* not asked for: stays dirty */
            continue;
        }
        waited++;
        wait_acknowledged(slot, c->internode->high_segment[slot], routine);
        c->internode->touched[slot] = false;
        c->internode->high_segment[slot] = 0;
    }
    c->internode->count = kept;
    free(wanted);
    osoe_count(OSOE_C_TRANSPORT_WAIT_DIRTY_PEERS, waited);
    osoe_count(OSOE_C_TRANSPORT_WAIT_NS, osoe_now_ns() - t0);
}

/*
 * A shared context's dirty set, under its mutex (§4.11 item 2;
 * dirty_set_model.py): for each set bit the wait covers, load the slot's
 * highest segment, wait for the acknowledgement through it, clear the bit,
 * and look again - a put another thread issued meanwhile may have raised the
 * number and found the bit still set, and so not set it; if the peer is now
 * behind, the bit goes back (`naive`, without the re-check, loses that put)
 * and the wait moves on: the one wait covered every put that returned before
 * the call, and the re-set leaves the later one to the next wait. Waiting
 * again, as the design first had it, let a thread that kept putting to the
 * node hold the quiet, and the mutex, for ever (the review of slice 3, S1;
 * the model's `resetstop`, approved by the author 2026-09-26). Only a wait clears bits, and waits hold the mutex
 * (`unlocked`, without it, lets a second quiet read a bit mid-clear as done).
 */
static uint32_t wait_dirty_shared(struct osoe_transport_context *c, const bool *wanted,
                                  const char *routine)
{
    flush_dirty(c);
    uint32_t waited = 0, words = (peer_count() + 63) / 64;
    for (uint32_t w = 0; w < words; w++) {
        uint64_t bits = atomic_load_explicit(&c->internode->dirty_bits[w], memory_order_seq_cst);
        while (bits) {
            uint32_t slot = w * 64 + (uint32_t)__builtin_ctzll(bits);
            uint64_t bit = UINT64_C(1) << (slot % 64);
            bits &= bits - 1;
            if (wanted && !wanted[slot])
                continue;               /* not asked for: stays dirty */
            waited++;
            uint64_t through =
                atomic_load_explicit(&c->internode->shared_highest_segment[slot],
                                     memory_order_seq_cst);
            if (ask_acknowledgement(c, slot))
                osoe_progress_host_wake_lead();
            wait_acknowledged(slot, through, routine);
            osoe_perturb();
            atomic_fetch_and_explicit(&c->internode->dirty_bits[w], ~bit, memory_order_seq_cst);
            osoe_perturb();
            if (!acknowledged(c, slot))
                atomic_fetch_or_explicit(&c->internode->dirty_bits[w], bit, memory_order_seq_cst);
        }
    }
    return waited;
}

/* OSOE_CHECK_QUIET's snapshot at a wait's call: every slot's highest segment,
 * and the clock of nonblocking get calls returned. */
struct quiet_promise {
    uint64_t *highest;
    uint64_t clock;
};

static void promise_take(const struct osoe_transport_context *c, struct quiet_promise *p)
{
    p->highest = NULL;
    if (!check_quiet)
        return;
    p->clock = atomic_load_explicit(&issue_clock, memory_order_seq_cst);
    uint32_t peers = peer_count();
    p->highest = malloc(peers * sizeof *p->highest);
    if (!p->highest)
        osoe_fatal("out of memory for OSOE_CHECK_QUIET's snapshot");
    for (uint32_t slot = 0; slot < peers; slot++)
        p->highest[slot] =
            atomic_load_explicit(&c->internode->returned_highest[slot], memory_order_seq_cst);
}

static void promise_check(const struct osoe_transport_context *c, struct quiet_promise *p,
                          const bool *wanted, bool only_gets, const char *routine)
{
    if (!check_quiet)
        return;
    for (uint32_t e = 0; e < osoe.node.landing_requests; e++) {
        struct osoe_landing_record *r = &my_records()[e];
        if (atomic_load_explicit(&r->state, memory_order_seq_cst) == OSOE_LANDING_FREE)
            continue;
        uint64_t token = atomic_load_explicit(&r->token, memory_order_seq_cst);
        if (!(token & OSOE_LANDING_NONBLOCKING) ||
            ((token >> OSOE_LANDING_OWNER_SHIFT) & OSOE_LANDING_OWNER_MASK) != c->block)
            continue;
        uint64_t stamp = atomic_load_explicit(&issue_stamp[e], memory_order_seq_cst) & STAMP_MASK;
        if (stamp != STAMP_MASK && stamp <= p->clock)
            osoe_fatal("OSOE_CHECK_QUIET: %s returned with a nonblocking get in flight that "
                       "returned before it was called (extent %u, call %llu of %llu)",
                       routine, e, (unsigned long long)stamp, (unsigned long long)p->clock);
    }
    if (p->highest && !only_gets)
        for (uint32_t slot = 0; slot < peer_count(); slot++) {
            if (wanted && !wanted[slot])
                continue;
            uint32_t peer = slot < osoe.job.node ? slot : slot + 1;
            uint64_t acked = osoe_progress_host_acked(peer);
            if (acked < p->highest[slot])
                osoe_fatal("OSOE_CHECK_QUIET: %s returned with node %u acknowledged through "
                           "segment %llu, and this context had written through %llu before "
                           "the call", routine, peer, (unsigned long long)acked,
                           (unsigned long long)p->highest[slot]);
        }
    free(p->highest);
    p->highest = NULL;
}

/*
 * A shared context's wait (§4.11 items 1, 2 and 4; get_completion_model.py,
 * dirty_set_model.py, pe_quiet_model.py). The generation is loaded at the
 * call, then the mutex taken. A full quiet that opened a generation past the
 * one loaded, which it did after this call began and finished before letting
 * the mutex go, covered everything posted before this call: the wait
 * piggybacks, full quiet or pe_quiet alike - and only on a *full* quiet, which
 * `full` records, since a pe_quiet leaves other nodes' puts outstanding.
 * Otherwise it bumps the generation, closing the pair issuers counted in until
 * now, completes that pair's gets, and waits for the dirty set - every node
 * for a quiet, the named ones for a pe_quiet - and a full quiet stores the
 * generation it opened in `full`. A wait for gets alone (`only_gets`)
 * closes a pair too and leaves the dirty set be.
 */
static void wait_shared(struct osoe_transport_context *c, const int *pes, size_t npes,
                        bool only_gets, const char *routine)
{
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TRANSPORT_WAIT_CALLS, 1);
    uint64_t t0 = osoe_now_ns();
    struct quiet_promise promise;
    promise_take(c, &promise);
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
    osoe_serializing_fence();
    struct osoe_context_block *block = osoe_transport_context_block(c);
    uint64_t e = atomic_load_explicit(&block->generation, memory_order_seq_cst);
    osoe_perturb();
    context_enter(c);
    uint64_t full = atomic_load_explicit(&block->full, memory_order_seq_cst);
    if (full > e) {
        context_leave(c);
        osoe_count(OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING, 1);
        promise_check(c, &promise, wanted, only_gets, routine);
        free(wanted);
        return;
    }
    uint64_t g = atomic_load_explicit(&block->generation, memory_order_seq_cst);
    atomic_store_explicit(&block->generation, g + 1, memory_order_seq_cst);
    complete_pair(c, (unsigned)(g & 1), routine);
    /* The stores that copied the gets into place, ordered as every other store
     * before the wait is (§4.6). */
    osoe_serializing_fence();
    uint32_t waited = 0;
    if (!only_gets)
        waited = wait_dirty_shared(c, wanted, routine);
    if (!pes && !only_gets)
        atomic_store_explicit(&block->full, g + 1, memory_order_seq_cst);
    context_leave(c);
    promise_check(c, &promise, wanted, only_gets, routine);
    free(wanted);
    if (waited == 0)
        osoe_count(OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING, 1);
    osoe_count(OSOE_C_TRANSPORT_WAIT_DIRTY_PEERS, waited);
    osoe_count(OSOE_C_TRANSPORT_WAIT_NS, osoe_now_ns() - t0);
}

void osoe_internode_backend_wait(struct osoe_transport_context *context, const int *pes,
                                 size_t npes, const char *routine)
{
    if (context->concurrent)
        wait_shared(context, pes, npes, false, routine);
    else
        wait_body(context, pes, npes, routine);
}

void osoe_internode_backend_wait_gets(struct osoe_transport_context *context,
                                      const char *routine)
{
    if (context->concurrent)
        wait_shared(context, NULL, 0, true, routine);
    else
        complete_gets(context, routine);
}
