/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/sockios.h>

#include "am-state.h"
#include "base/deadline.h"
#include "base/hostlist.h"
#include "base/print-line.h"
#include "base/stall-watch.h"
#include "connection-area.h"
#include "data-wires.h"
#include "fatal.h"
#include "mesh-schedule.h"
#include "owner-map.h"
#include "progress-host.h"
#include "progress.h"

/* What the agent has done about a peer slot's data wire (data-wires.h). */
enum slot_state {
    SLOT_NONE,                  /* nothing yet */
    SLOT_QUEUED,                /* to call, once a place among the calls in flight is free */
    SLOT_CALLING,               /* a call in flight */
    SLOT_ASKED,                 /* the peer, the pair's caller, asked to dial; its call not in */
    SLOT_MADE,                  /* made, and handed over - or, the node aborting, told */
};

/* The watched set: the listener's entries (osoe_listener_poll), then the calls'. */
enum { WATCHED_MOST = OSOE_LISTENER_POLLED + OSOE_MESH_CALLS_IN_FLIGHT };

static struct {
    bool on;
    const struct osoe_bootstrap *b;
    uint32_t session, node, nodes, peers;
    struct osoe_listener *listener;
    struct sockaddr_in *address;            /* every node's, as the rendezvous carried it */
    int epoll;                              /* the watched set, which the ring's watch watches */
    /* The owners' needs, a bit a peer slot, set by any owner and taken by the
     * agent; and a slot's socket, made by the agent and taken by its owner. */
    _Atomic uint64_t *wanted;
    uint32_t wanted_words;
    _Atomic int32_t *handed;
    /* The agent's own. */
    uint8_t *state;                         /* a peer slot's enum slot_state */
    uint64_t *needed_ns;                    /* when the slot was first needed, for SHMEM_DEBUG */
    struct osoe_stall_watch *asked_watch;   /* a slot's request, while SLOT_ASKED */
    uint32_t asked;                         /* slots in SLOT_ASKED */
    uint32_t *queue;                        /* slots to call, in order: a ring of `peers` */
    uint32_t queue_first, queue_count;
    struct osoe_listener_call *call[OSOE_MESH_CALLS_IN_FLIGHT];
    uint32_t call_slot[OSOE_MESH_CALLS_IN_FLIGHT];      /* the slot it makes, or UINT32_MAX */
    uint32_t call_entry[OSOE_MESH_CALLS_IN_FLIGHT];     /* its place in `watched`, or UINT32_MAX */
    uint64_t call_due_ns[OSOE_MESH_CALLS_IN_FLIGHT];    /* taken on by then, polled or not */
    struct osoe_stall_watch call_watch[OSOE_MESH_CALLS_IN_FLIGHT];
    struct pollfd watched[WATCHED_MOST];
    uint32_t watched_count;
    bool stir;                              /* a call begun or made, a need taken: turn again */
    uint64_t due_ns, sweep_ns;
    uint64_t hold_until_ns;                 /* OSOE_TEST_HOLD_DATA_WIRES's end, or 0 */
    /* The abort's (osoe_data_wires_abort). */
    bool aborting;
    _Atomic uint32_t handover_closed;
    unsigned char goodbye[OSOE_PROGRESS_ABORT_SEGMENT_BYTES];
    uint32_t goodbye_bytes;
    bool keep_stream;
    int told[OSOE_LISTENER_CALLERS];        /* the sockets made since, each told */
    uint32_t told_count;
} wires = { .epoll = -1 };

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static uint32_t peer_of(uint32_t slot)
{
    return slot < wires.node ? slot : slot + 1;
}

/* "node 3 (pi3, at 10.0.0.3)", as start-up's notices name a node. */
static void describe_peer(uint32_t peer, char *text, size_t len)
{
    char entry[128] = "?", ip[INET_ADDRSTRLEN] = "?";
    if (wires.b->node_list)
        osoe_hostlist_entry(wires.b->node_list, peer, entry, sizeof entry);
    inet_ntop(AF_INET, &wires.address[peer].sin_addr, ip, sizeof ip);
    snprintf(text, len, "node %u (%s, at %s)", peer, entry, ip);
}

void osoe_data_wires_start(const struct osoe_bootstrap *b, uint32_t session,
                           struct osoe_listener *listener, const struct sockaddr_in *peers)
{
    uint32_t nodes = b->node_count, slots = nodes - 1;
    wires.b = b;
    wires.session = session;
    wires.node = b->node;
    wires.nodes = nodes;
    wires.peers = slots;
    wires.listener = listener;
    wires.address = malloc(nodes * sizeof *wires.address);
    wires.wanted_words = (slots + 63) / 64;
    wires.wanted = calloc(wires.wanted_words, sizeof *wires.wanted);
    wires.handed = malloc(slots * sizeof *wires.handed);
    wires.state = calloc(slots, sizeof *wires.state);
    wires.asked_watch = calloc(slots, sizeof *wires.asked_watch);
    wires.queue = malloc(slots * sizeof *wires.queue);
    wires.needed_ns = calloc(slots, sizeof *wires.needed_ns);
    if (!wires.address || !wires.wanted || !wires.handed || !wires.state || !wires.asked_watch ||
        !wires.queue || !wires.needed_ns)
        osoe_fatal("out of memory for the data wires of %u peer nodes", slots);
    memcpy(wires.address, peers, nodes * sizeof *wires.address);
    /* Before any helper starts, which is after this node's engine is set up:
     * the stores need no reader to order against yet. */
    for (uint32_t slot = 0; slot < slots; slot++)
        atomic_store_explicit(&wires.handed[slot], -1, memory_order_release);
    atomic_store_explicit(&wires.handover_closed, 0, memory_order_release);
    wires.asked = wires.queue_first = wires.queue_count = wires.told_count = 0;
    wires.watched_count = 0;
    wires.aborting = false;
    wires.epoll = epoll_create1(EPOLL_CLOEXEC);
    if (wires.epoll < 0)
        osoe_fatal("cannot create the set of the data wires' connections (epoll): %s",
                   strerror(errno));
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        wires.call[k] = osoe_listener_call_new();
        wires.call_slot[k] = wires.call_entry[k] = UINT32_MAX;
    }
    uint64_t now = now_ns();
    wires.hold_until_ns = b->data_wire_hold_ms && b->data_wire_hold_node == b->node
                              ? now + (uint64_t)b->data_wire_hold_ms * 1000000
                              : 0;
    wires.stir = true;
    wires.due_ns = 0;
    wires.sweep_ns = now + OSOE_SWEEP_NS;
    wires.on = true;
}

bool osoe_data_wires_on(void)
{
    return wires.on;
}

void osoe_data_wires_want(uint32_t slot)
{
    if (wires.on)
        atomic_fetch_or_explicit(&wires.wanted[slot / 64], UINT64_C(1) << (slot % 64),
                                 memory_order_seq_cst);
}

bool osoe_data_wires_wanted(void)
{
    for (uint32_t w = 0; wires.on && w < wires.wanted_words; w++)
        if (atomic_load_explicit(&wires.wanted[w], memory_order_seq_cst) != 0)
            return true;
    return false;
}

/* A need of peer slot `slot`'s data wire - a writer's, or the peer's request:
 * acted on once, a call where the schedule has this node call the peer and a
 * request to the peer where it does not (data_wire_model.py). */
static void need(uint32_t slot)
{
    if (wires.state[slot] != SLOT_NONE || wires.aborting)
        return;
    uint32_t peer = peer_of(slot);
    wires.needed_ns[slot] = now_ns();
    if (osoe_mesh_schedule_calls(wires.node, peer, wires.nodes)) {
        wires.state[slot] = SLOT_QUEUED;
        wires.queue[(wires.queue_first + wires.queue_count++) % wires.peers] = slot;
    } else {
        wires.state[slot] = SLOT_ASKED;
        wires.asked++;
        osoe_stall_watch_begin_noticed(&wires.asked_watch[slot], 0, wires.b->no_progress_ns);
        osoe_progress_ask_data_wire(&osoe_am.progress, peer);
    }
    wires.stir = true;
}

void osoe_data_wires_requested(uint32_t node)
{
    if (!osoe_mesh_schedule_calls(wires.node, node, wires.nodes))
        osoe_fatal("node %u was asked by node %u to dial their data wire, which the mesh's "
                   "schedule has node %u dial (§6.2)", wires.node, node, node);
    need(osoe_peer_slot(wires.node, node));
}

/* Whether the peer's kernel has yet to acknowledge what was sent on `fd`, as
 * wire.c's osoe_wire_unacknowledged asks it of a wire. */
static bool unacknowledged(int fd)
{
    int queued = 0;
    if (ioctl(fd, SIOCOUTQ, &queued) != 0 || queued == 0)
        return false;
    struct tcp_info info;
    socklen_t len = sizeof info;
    return getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
           info.tcpi_state != TCP_CLOSE;
}

/* A socket made while the node aborts: its abortive goodbye, which fits an
 * empty socket's buffer in one send, and its end of stream - but a global
 * exit's, which ends with the process (progress.h's osoe_progress_abort). */
static void tell(int fd)
{
    (void)send(fd, wires.goodbye, wires.goodbye_bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (!wires.keep_stream)
        (void)shutdown(fd, SHUT_WR);
    if (wires.told_count < OSOE_LISTENER_CALLERS)
        wires.told[wires.told_count++] = fd;
    else
        close(fd);
}

/* Peer slot `slot`'s data wire made: handed to its connection's owner - the
 * socket stored, then the connection marked and its owner woken, as a writer
 * marks and wakes it (connection-area.h; lead_wake_model.py) - or, the node
 * aborting, told. `called`: it came in through the listener. */
static void made(uint32_t slot, int fd, bool called)
{
    uint32_t peer = peer_of(slot);
    if (wires.state[slot] == SLOT_MADE) {
        if (called)
            osoe_fatal("node %u was connected to twice by node %u for its data wire", wires.node,
                       peer);
        osoe_fatal("node %u connected to node %u twice for its data wire", wires.node, peer);
    }
    bool asked = wires.state[slot] == SLOT_ASKED;
    if (asked)
        wires.asked--;
    /* Under SHMEM_DEBUG, each wire as it is made, and how: which node dialled
     * - the schedule's caller, always - and how long the need waited for it. */
    if (wires.b->debug) {
        char waited[64] = "";
        if (wires.state[slot] != SLOT_NONE)
            snprintf(waited, sizeof waited, ", %llu us after its first need",
                     (unsigned long long)((now_ns() - wires.needed_ns[slot]) / 1000));
        osoe_print_line("osoe: node %u made its data wire toward node %u on demand: %s%s\n",
                        wires.node, peer,
                        !called ? "it dialled"
                        : asked ? "the peer dialled, as asked"
                                : "the peer dialled for its own need",
                        waited);
    }
    wires.state[slot] = SLOT_MADE;
    wires.stir = true;
    if (wires.aborting) {
        tell(fd);
        return;
    }
    atomic_store_explicit(&wires.handed[slot], fd, memory_order_release);
    osoe_connection_area_mark(&osoe_am.area, peer);
    osoe_progress_host_wake(peer);
}

/* A caller the listener has confirmed: by the schedule's rule, as start-up's
 * callers are judged (wire.c's attach_called), and once. */
static void caller_made(uint32_t peer, int fd)
{
    if (osoe_mesh_schedule_calls(wires.node, peer, wires.nodes))
        osoe_fatal("node %u was called by node %u for their data wire, which the mesh's "
                   "schedule has node %u call (§6.2)", wires.node, peer, wires.node);
    made(osoe_peer_slot(wires.node, peer), fd, true);
}

/* What the set's last wait said of each entry of `watched`. */
static void read_ready(void)
{
    for (uint32_t i = 0; i < wires.watched_count; i++)
        wires.watched[i].revents = 0;
    struct epoll_event events[WATCHED_MOST];
    int n = epoll_wait(wires.epoll, events, WATCHED_MOST, 0);
    if (n < 0 && errno != EINTR)
        osoe_fatal("looking at the data wires' connections (epoll_wait): %s", strerror(errno));
    for (int e = 0; e < n; e++) {
        uint32_t got = events[e].events;
        short revents = (short)((got & EPOLLIN ? POLLIN : 0) | (got & EPOLLOUT ? POLLOUT : 0) |
                                (got & EPOLLERR ? POLLERR : 0) | (got & EPOLLHUP ? POLLHUP : 0));
        for (uint32_t i = 0; i < wires.watched_count; i++)
            if (wires.watched[i].fd == events[e].data.fd)
                wires.watched[i].revents = revents;
    }
}

/* Every caller the listener has confirmed since its last poll. */
static bool serve(void)
{
    bool moved = false;
    for (;;) {
        uint32_t peer;
        enum osoe_hello_purpose purpose;
        int fd = osoe_listener_serve(wires.listener, OSOE_HELLO_PURPOSE(OSOE_HELLO_DATA_WIRE),
                                     wires.watched, &peer, &purpose);
        if (fd < 0)
            return moved;
        caller_made(peer, fd);
        moved = true;
    }
}

/* Each call in flight a step on: its socket moved, or its deadline came - a
 * dial's limit, a back-off's end - as the mesh's loop takes them (wire.c). */
static bool step_calls(uint64_t now)
{
    bool moved = false;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        uint32_t slot = wires.call_slot[k];
        if (slot == UINT32_MAX)
            continue;
        short revents = wires.call_entry[k] != UINT32_MAX ? wires.watched[wires.call_entry[k]].revents
                                                          : 0;
        if (!revents && now < wires.call_due_ns[k])
            continue;
        int fd = osoe_listener_call_step(wires.call[k], revents);
        if (fd < 0)
            continue;
        wires.call_slot[k] = UINT32_MAX;
        made(slot, fd, false);
        moved = true;
    }
    return moved;
}

/* The slots to call, in order, while a place is free: dialled at the next
 * turn's step, as a call begun with no slot to wait for. */
static bool begin_calls(void)
{
    bool begun = false;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT && wires.queue_count; k++) {
        if (wires.call_slot[k] != UINT32_MAX)
            continue;
        uint32_t slot = wires.queue[wires.queue_first];
        wires.queue_first = (wires.queue_first + 1) % wires.peers;
        wires.queue_count--;
        uint32_t peer = peer_of(slot);
        osoe_listener_call_begin(wires.call[k], wires.b, wires.session, &wires.address[peer],
                                 ntohs(wires.address[peer].sin_port), peer, OSOE_HELLO_DATA_WIRE);
        wires.call_slot[k] = slot;
        wires.call_due_ns[k] = 0;
        wires.state[slot] = SLOT_CALLING;
        osoe_stall_watch_begin_noticed(&wires.call_watch[k], 0, wires.b->no_progress_ns);
        wires.stir = true;
        begun = true;
    }
    return begun;
}

/*
 * A stalled data wire told as any wait is (§3.15; the author, 2026-10-25): a
 * call in flight, or a request the caller has not answered with a call, says
 * what it waits for after 30 s and every minute after - the node, its address
 * and the last call's error, as start-up's calls say - and ends the job under
 * OSOE_NO_PROGRESS_TIMEOUT. At the sweep's cadence, O(calls in flight) and
 * O(slots) only while a request is out.
 */
static void tell_stalls(void)
{
    const struct osoe_bootstrap *b = wires.b;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        uint32_t slot = wires.call_slot[k];
        if (slot == UINT32_MAX)
            continue;
        struct osoe_stall_watch *w = &wires.call_watch[k];
        bool stalled = osoe_stall_watch_stalled(w, 0);
        if (!stalled && !osoe_stall_watch_notice_due(w))
            continue;
        char what[512];
        osoe_listener_call_describe(wires.call[k], what, sizeof what);
        if (stalled)
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u's data wire "
                       "toward node %u is not made: it waits for %s",
                       (unsigned long long)osoe_stall_watch_seconds(w), b->node, peer_of(slot),
                       what);
        osoe_notice("node %u has waited %llu s to make its data wire toward node %u, for %s; "
                    "still waiting", b->node,
                    (unsigned long long)osoe_stall_watch_waited_seconds(w), peer_of(slot), what);
    }
    for (uint32_t slot = 0, seen = 0; slot < wires.peers && seen < wires.asked; slot++) {
        if (wires.state[slot] != SLOT_ASKED)
            continue;
        seen++;
        struct osoe_stall_watch *w = &wires.asked_watch[slot];
        bool stalled = osoe_stall_watch_stalled(w, 0);
        if (!stalled && !osoe_stall_watch_notice_due(w))
            continue;
        char who[256];
        describe_peer(peer_of(slot), who, sizeof who);
        if (stalled)
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u asked %s to "
                       "dial their data wire, which has not called",
                       (unsigned long long)osoe_stall_watch_seconds(w), b->node, who);
        osoe_notice("node %u has waited %llu s for %s to dial their data wire, which it asked "
                    "for on their control wire; still waiting", b->node,
                    (unsigned long long)osoe_stall_watch_waited_seconds(w), who);
    }
}

/*
 * The set watched again, from what the listener and each call ask now, and
 * when to turn at the latest. The last set unlisted whole first: a socket
 * closed since has left it by itself, and its number may be another's now.
 * O(entries) calls to the kernel, only at a turn something brought.
 */
static void rewatch(uint64_t now)
{
    for (uint32_t i = 0; i < wires.watched_count; i++)
        (void)epoll_ctl(wires.epoll, EPOLL_CTL_DEL, wires.watched[i].fd, NULL);
    wires.watched_count = 0;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        wires.call_entry[k] = UINT32_MAX;
        wires.call_due_ns[k] = UINT64_MAX;
    }
    if (now < wires.hold_until_ns) {
        wires.due_ns = wires.hold_until_ns;    /* OSOE_TEST_HOLD_DATA_WIRES: none watched */
        return;
    }
    uint64_t due = UINT64_MAX;
    uint32_t count = osoe_listener_poll(wires.listener, OSOE_HELLO_PURPOSE(OSOE_HELLO_DATA_WIRE),
                                        wires.watched, &due);
    bool calling = false;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        if (wires.call_slot[k] == UINT32_MAX)
            continue;
        calling = true;
        short events = 0;
        uint64_t by = UINT64_MAX;
        int fd = osoe_listener_call_poll(wires.call[k], &events, &by);
        wires.call_due_ns[k] = by;
        if (by < due)
            due = by;
        if (fd >= 0) {
            wires.call_entry[k] = count;
            wires.watched[count++] = (struct pollfd){ .fd = fd, .events = events };
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        struct epoll_event e = {
            .events = (wires.watched[i].events & POLLIN ? EPOLLIN : 0) |
                      (wires.watched[i].events & POLLOUT ? EPOLLOUT : 0),
            .data.fd = wires.watched[i].fd,
        };
        if (epoll_ctl(wires.epoll, EPOLL_CTL_ADD, wires.watched[i].fd, &e) != 0 &&
            errno != EEXIST)
            osoe_fatal("watching a data wire's connection (epoll_ctl): %s", strerror(errno));
    }
    wires.watched_count = count;
    if ((calling || wires.asked) && wires.sweep_ns < due)
        due = wires.sweep_ns;
    wires.due_ns = due;
}

bool osoe_data_wires_poll(struct osoe_ring *ring)
{
    if (!wires.on)
        return false;
    bool moved = false;
    /* The owners' needs: a word looked at first, taken only with bits in it. */
    for (uint32_t w = 0; w < wires.wanted_words; w++) {
        if (atomic_load_explicit(&wires.wanted[w], memory_order_acquire) == 0)
            continue;
        uint64_t bits = atomic_exchange_explicit(&wires.wanted[w], 0, memory_order_seq_cst);
        for (; bits != 0; bits &= bits - 1) {
            need(w * 64 + (uint32_t)__builtin_ctzll(bits));
            moved = true;
        }
    }
    uint64_t now = now_ns();
    bool fired = osoe_ring_take_watch(ring);
    if (fired || wires.stir || now >= wires.due_ns) {
        wires.stir = false;
        if (fired)
            read_ready();
        if (now >= wires.hold_until_ns) {
            moved |= serve();
            moved |= step_calls(now);
            moved |= begin_calls();
        }
        if (now >= wires.sweep_ns) {
            wires.sweep_ns = now + OSOE_SWEEP_NS;
            tell_stalls();
        }
        rewatch(now);
    }
    if (wires.watched_count)
        osoe_ring_watch(ring, wires.epoll);
    return moved;
}

uint64_t osoe_data_wires_due_ns(void)
{
    return wires.on ? wires.due_ns : UINT64_MAX;
}

bool osoe_data_wires_making(void)
{
    if (!wires.on)
        return false;
    if (wires.queue_count || wires.asked)
        return true;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++)
        if (wires.call_slot[k] != UINT32_MAX)
            return true;
    return false;
}

int osoe_data_wires_take(uint32_t slot)
{
    if (!wires.on || atomic_load_explicit(&wires.handed[slot], memory_order_acquire) < 0)
        return -1;
    return atomic_exchange_explicit(&wires.handed[slot], -1, memory_order_seq_cst);
}

bool osoe_data_wires_handover_closed(void)
{
    return !wires.on || atomic_load_explicit(&wires.handover_closed, memory_order_acquire) != 0;
}

void osoe_data_wires_abort(uint32_t pe, int32_t status, uint32_t disposition, bool keep_stream)
{
    if (!wires.on || wires.aborting)
        return;
    wires.aborting = true;
    wires.goodbye_bytes = osoe_progress_abort_segment(wires.goodbye, pe, status, disposition);
    wires.keep_stream = keep_stream;
    /* After every hand-over of this thread's, which a helper's pass after its
     * acquire load of the word then finds marked (data-wires.h). */
    atomic_store_explicit(&wires.handover_closed, 1, memory_order_release);
    osoe_listener_stop_answering(wires.listener);
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++)
        if (wires.call_slot[k] != UINT32_MAX) {
            osoe_listener_call_abandon(wires.call[k]);
            wires.call_slot[k] = UINT32_MAX;
        }
    wires.queue_count = 0;
    wires.hold_until_ns = 0;            /* answered callers are served whatever a test holds */
    wires.stir = true;
}

bool osoe_data_wires_aborted(void)
{
    if (!wires.on)
        return true;
    if (osoe_listener_answered(wires.listener) > 0)
        return false;
    for (uint32_t i = 0; i < wires.told_count; i++)
        if (unacknowledged(wires.told[i]))
            return false;
    return true;
}

void osoe_data_wires_stop(void)
{
    if (!wires.on)
        return;
    wires.on = false;
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        osoe_listener_call_free(wires.call[k]);
        wires.call[k] = NULL;
    }
    for (uint32_t slot = 0; slot < wires.peers; slot++) {
        int fd = atomic_load_explicit(&wires.handed[slot], memory_order_acquire);
        if (fd >= 0)
            close(fd);                  /* made, and never taken */
    }
    for (uint32_t i = 0; i < wires.told_count; i++)
        close(wires.told[i]);
    wires.told_count = 0;
    osoe_listener_close(wires.listener);
    if (wires.epoll >= 0)
        close(wires.epoll);
    wires.epoll = -1;
    free(wires.address);
    free(wires.wanted);
    free(wires.handed);
    free(wires.state);
    free(wires.asked_watch);
    free(wires.queue);
    free(wires.needed_ns);
    wires.needed_ns = NULL;
    wires.address = NULL;
    wires.wanted = NULL;
    wires.handed = NULL;
    wires.state = NULL;
    wires.asked_watch = NULL;
    wires.queue = NULL;
}
