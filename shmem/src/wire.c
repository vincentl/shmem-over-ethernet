/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/sockios.h>

#include "base/environment.h"
#include "base/print-line.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "fatal.h"
#include "listener.h"
#include "mesh-schedule.h"
#include "owner-map.h"
#include "wire.h"
#include "wire-settings.h"

/*
 * How much of `n` this transfer moves: all of it, or - OSOE_WIRE_SPLIT times in
 * a myriad - a prefix of at least one byte. Never nothing: a send truncated to
 * nothing would submit nothing and leave the connection waiting for a
 * completion that is not coming, and a peek returning 0 with bytes queued
 * would end the pass's loop over a wire it must empty (§3.3).
 */
static size_t truncate_to(const struct osoe_wire *w, uint64_t site, uint64_t n)
{
    if (w->split == 0 || n <= 1)
        return (size_t)n;
    uint64_t h = osoe_wire_hash(w->seed ^ site ^ (n * UINT64_C(0x9E3779B97F4A7C15)));
    if (h % 10000 >= w->split)
        return (size_t)n;
    return (size_t)((h >> 20) % n + 1);
}

/* osoe_wire_ring_open, saying the settings that are said (OSOE_WIRE_SPLIT's
 * line) only where `announce` asks: once a node, by owner 0's ring (the
 * adversarial read of Part 1, N7). */
static void ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t count,
                      uint32_t lending, const struct osoe_bootstrap *b, bool announce)
{
    struct osoe_wire_settings s;
    osoe_wire_settings_read(&s, b, announce);
    /* With the zero-copy send on, each data wire's notifications still owed are
     * completions the queue must have room for (§3.3, ring.h) - refused by name
     * where that passes io_uring's own most, rather than failing to create a
     * ring for a reason the setting does not name. A control wire never lends,
     * so it owes none (the adversarial read of Part 2, S2). */
    uint64_t completions = (uint64_t)s.buffer_count + 2 * (uint64_t)count +
                           (uint64_t)OSOE_WIRE_ZERO_COPY_OWED_MOST * lending + 1;
    if (s.zero_copy && completions > OSOE_RING_COMPLETIONS_MOST)
        osoe_fatal("OSOE_WIRE_ZERO_COPY=on: %u peer nodes' data wires and %u receive buffers "
                   "need a completion queue of %llu, more than io_uring's %u; turn the "
                   "zero-copy send off, or lower OSOE_WIRE_BUFFERS", lending, s.buffer_count,
                   (unsigned long long)completions, OSOE_RING_COMPLETIONS_MOST);
    osoe_ring_open(r, wires, count, lending, s.buffer_count, s.buffer_bytes,
                   s.zero_copy ? OSOE_WIRE_ZERO_COPY_OWED_MOST : 0);
    if (!s.zero_copy)
        return;
    /* Asked for by name, so refused by name where the kernel, or a filter in
     * front of it, has not the opcode (Linux 6.0; the full build needs 6.7). */
    struct io_uring_probe *probe = io_uring_get_probe_ring(&r->uring);
    bool supported = probe && io_uring_opcode_supported(probe, IORING_OP_SEND_ZC);
    if (probe)
        io_uring_free_probe(probe);
    if (!supported)
        osoe_fatal("OSOE_WIRE_ZERO_COPY=on: this kernel's io_uring offers no IORING_OP_SEND_ZC "
                   "(Linux 6.0, and 6.2 for the usage report it asks)");
}

void osoe_wire_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t count,
                         uint32_t lending, const struct osoe_bootstrap *b)
{
    ring_open(r, wires, count, lending, b, true);
}

void osoe_wire_attach(struct osoe_wire *w, struct osoe_ring *r, uint32_t index, int fd,
                      uint32_t peer, const struct osoe_bootstrap *b)
{
    struct osoe_wire_settings settings;
    osoe_wire_settings_read(&settings, b, false);
    const struct osoe_wire_settings *s = &settings;
    /* The seed differs per peer, so the two directions of a pair - and two
     * wires in one test process - split differently. */
    *w = (struct osoe_wire){ .fd = fd, .peer = peer, .index = index, .ring = r,
                             .first = OSOE_RING_NONE, .last = OSOE_RING_NONE, .sent = -1,
                             .seed = osoe_wire_hash(s->seed ^ ((uint64_t)b->node << 32) ^ peer),
                             .split = s->split, .zero_copy = s->zero_copy,
                             .zero_copy_bytes = s->zero_copy_bytes };
    /*
     * The application layer owns coalescing (§3.3, invariant 14). §3.9 is
     * already a coalescer, and a better-informed one: it knows message
     * boundaries, fence and quiet. Nagle is a second coalescer underneath that
     * knows none of them - it holds a small write while earlier data is
     * unacknowledged, and with the peer's delayed ACK can sit on a segment for
     * tens of milliseconds. The damage lands on quiet: the buffer was sealed and
     * submitted, so the safety sweep sees nothing wrong while the kernel holds
     * it, and because §3.4 piggybacks acknowledgements, reverse traffic usually
     * clocks it out - perfect under ping-pong benchmarks, stalled on one-way
     * patterns.
     */
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0)
        osoe_fatal("cannot set TCP_NODELAY toward node %u: %s", peer, strerror(errno));

    /*
     * TCP keepalive (§3.15). A peer host that vanishes without a FIN - power
     * lost, a cable cut, a partition - leaves an idle connection open forever,
     * and no wait gives up because time passed. Keepalive asks whether the
     * peer's *kernel* is there, which contention between nodes does not stop
     * (on one overloaded machine's loopback it can: the backlog drops probes
     * and answers alike - the review of v0.2 step 4's merge point, Q2-1 - so
     * a job whose every node has a loopback address runs without it unless
     * OSOE_KEEPALIVE is set, progress-host.c): a stopped
     * node's kernel still acknowledges, so a stopped node is waited for. After
     * IDLE seconds without traffic, COUNT unanswered probes INTERVAL seconds
     * apart end the connection with ETIMEDOUT, and the job with it
     * (progress.c). Not TCP_USER_TIMEOUT, which bounds how long sent data may
     * go unacknowledged and which Linux applies to zero-window probing too: a
     * peer merely stopped with a full receive buffer would be declared dead
     * after it, a fatal timeout. With data in flight, which keepalive does not
     * probe, the lead asks TCP_INFO the same question within the same budget
     * (osoe_wire_silent_ms, progress.c's osoe_progress_check_silent), where
     * the kernel's own retransmission limit would take about fifteen minutes
     * (tcp_retries2).
     */
    if (b->keepalive_off)
        return;
    int idle = (int)(b->keepalive_idle_s ? b->keepalive_idle_s : OSOE_DEFAULT_KEEPALIVE_IDLE_S);
    int interval = (int)(b->keepalive_interval_s ? b->keepalive_interval_s
                                                 : OSOE_DEFAULT_KEEPALIVE_INTERVAL_S);
    int count = (int)(b->keepalive_count ? b->keepalive_count : OSOE_DEFAULT_KEEPALIVE_COUNT);
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count) != 0)
        osoe_fatal("cannot set TCP keepalive toward node %u: %s", peer, strerror(errno));
}

/* Peer slot `slot`'s wire of `purpose` in an array grouped by ring
 * (owner-map.h), the ring that carries it, and its place there. */
static struct osoe_wire *wire_of(struct osoe_wire *wires, struct osoe_ring *const *rings,
                                 uint32_t slot, enum osoe_hello_purpose purpose,
                                 uint32_t peers, uint32_t helpers, struct osoe_ring **ring,
                                 uint32_t *ring_index)
{
    if (purpose == OSOE_HELLO_CONTROL_WIRE) {
        *ring = rings[0];
        *ring_index = osoe_control_ring_index(slot, peers, helpers);
        return &wires[*ring_index];
    }
    uint32_t owner = osoe_owner_of(slot, helpers);
    *ring = rings[owner];
    *ring_index = osoe_owner_index(slot, helpers);
    return &wires[osoe_ring_first(owner, peers, helpers) + *ring_index];
}

static const char *wire_name(enum osoe_hello_purpose purpose)
{
    return purpose == OSOE_HELLO_CONTROL_WIRE ? "control" : "data";
}

/* osoe_wire_attach, and a control wire never sends by reference: its ring
 * counts no notifications for it (osoe_ring_open). */
static void attach_purpose(struct osoe_wire *w, struct osoe_ring *r, uint32_t index, int fd,
                           uint32_t peer, enum osoe_hello_purpose purpose,
                           const struct osoe_bootstrap *b)
{
    osoe_wire_attach(w, r, index, fd, peer, b);
    if (purpose == OSOE_HELLO_CONTROL_WIRE)
        w->zero_copy = false;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* A peer's slot among this node's: the others in node order. */
static uint32_t peer_slot(uint32_t peer, uint32_t node)
{
    return peer < node ? peer : peer - 1;
}

/* One call of this lead's in the mesh's schedule: a peer, a wire, the slot. */
struct mesh_call {
    uint32_t peer;
    enum osoe_hello_purpose purpose;
    uint32_t slot;
};

/*
 * The mesh as this lead makes it: its calls in the schedule's order, those in
 * flight, and what its listener has taken - with what the notice of a wait
 * that lasts, or its no-progress fatal, says of it. Written only when one is
 * due (P16): written at every connection, the callers still to come cost two
 * lookups a peer, 2(n - 1) times.
 */
struct mesh {
    struct osoe_wire *wires;
    struct osoe_ring *const *rings;
    const struct osoe_bootstrap *b;
    struct osoe_listener *listener;
    uint32_t peers, helpers;
    struct mesh_call *calls;
    uint32_t call_count, begun, made;
    uint32_t expected, answered;            /* callers' wires, as the schedule gives */
    struct osoe_listener_call *flying[OSOE_MESH_CALLS_IN_FLIGHT];
    uint32_t flying_call[OSOE_MESH_CALLS_IN_FLIGHT];    /* which call; UINT32_MAX: none */
};

static bool lacks_a_wire(const struct mesh *m, uint32_t peer)
{
    static const enum osoe_hello_purpose purposes[2] = { OSOE_HELLO_CONTROL_WIRE,
                                                         OSOE_HELLO_DATA_WIRE };
    for (uint32_t k = 0; k < 2; k++) {
        struct osoe_ring *ring;
        uint32_t ring_index;
        if (wire_of(m->wires, m->rings, peer_slot(peer, m->b->node), purposes[k], m->peers,
                    m->helpers, &ring, &ring_index)->fd < 0)
            return true;
    }
    return false;
}

/* "3 nodes of the mesh to call it on port 40213 (node 1, 4, 6; 1 caller
 * part-way through the hello) and node 9 (pi9, at 10.0.0.10) to answer on port
 * 37011: the connection timed out, 0.4 s ago, and 5 more calls of its own" -
 * what a lead calling and called at once waits for, in one line. */
static void describe_mesh(const struct mesh *m, char *text, size_t len)
{
    const struct osoe_bootstrap *b = m->b;
    uint32_t n = b->node_count, missing = 0, named = 0, part = 0;
    int used = 0;
    text[0] = '\0';
    if (m->answered < m->expected) {
        for (uint32_t peer = 0; peer < n; peer++)
            missing += osoe_mesh_schedule_calls(peer, b->node, n) && lacks_a_wire(m, peer);
        used = snprintf(text, len, "%u node%s of the mesh to call it on port %u (", missing,
                        missing == 1 ? "" : "s", m->listener->port);
        for (uint32_t peer = 0; peer < n && used > 0 && (size_t)used < len - 80; peer++)
            if (osoe_mesh_schedule_calls(peer, b->node, n) && lacks_a_wire(m, peer) &&
                named++ < 8)
                used += snprintf(text + used, len - (size_t)used, "%s%u",
                                 named == 1 ? "node " : ", ", peer);
        part = osoe_listener_part_way(m->listener);
        if (used > 0 && (size_t)used < len - 64)
            used += snprintf(text + used, len - (size_t)used,
                             "%s; %u caller%s part-way through the hello)",
                             named > 8 ? ", ..." : "", part, part == 1 ? "" : "s");
    }
    if (m->made == m->call_count || used < 0 || (size_t)used >= len - 16)
        return;
    /* The first call in flight, whole, and how many more are to come. */
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++)
        if (m->flying_call[k] != UINT32_MAX) {
            if (used > 0)
                used += snprintf(text + used, len - (size_t)used, " and ");
            if (used >= 0 && (size_t)used < len) {
                osoe_listener_call_describe(m->flying[k], text + used, len - (size_t)used);
                used += (int)strlen(text + used);
            }
            break;
        }
    uint32_t more = m->call_count - m->made - (m->begun > m->made ? 1 : 0);
    if (more && used >= 0 && (size_t)used < len)
        snprintf(text + used, len - (size_t)used, ", and %u more call%s of its own", more,
                 more == 1 ? "" : "s");
}

/* The listener's caller from `peer` for `purpose`, confirmed: by the schedule's
 * rule, and once a wire. */
static void attach_called(struct mesh *m, int fd, uint32_t peer, enum osoe_hello_purpose purpose)
{
    const struct osoe_bootstrap *b = m->b;
    if (!osoe_mesh_schedule_calls(peer, b->node, b->node_count))
        osoe_fatal("node %u was called by node %u, which the mesh's schedule has node %u call "
                   "(§6.2)", b->node, peer, b->node);
    struct osoe_ring *ring;
    uint32_t ring_index;
    struct osoe_wire *w = wire_of(m->wires, m->rings, peer_slot(peer, b->node), purpose, m->peers,
                                  m->helpers, &ring, &ring_index);
    if (w->fd >= 0)
        osoe_fatal("node %u was connected to twice by node %u for its %s wire", b->node, peer,
                   wire_name(purpose));
    attach_purpose(w, ring, ring_index, fd, peer, purpose, b);
    m->answered++;
}

/* One of this lead's own calls, made. */
static void attach_made(struct mesh *m, int fd, const struct mesh_call *call)
{
    const struct osoe_bootstrap *b = m->b;
    struct osoe_ring *ring;
    uint32_t ring_index;
    struct osoe_wire *w = wire_of(m->wires, m->rings, peer_slot(call->peer, b->node),
                                  call->purpose, m->peers, m->helpers, &ring, &ring_index);
    if (w->fd >= 0)
        osoe_fatal("node %u connected to node %u twice for its %s wire", b->node, call->peer,
                   wire_name(call->purpose));
    attach_purpose(w, ring, ring_index, fd, call->peer, call->purpose, b);
    m->made++;
}

/* OSOE_TEST_MESH_HOLD: this lead reaches its mesh `ms` late, sweeping, its
 * callers waiting in its listener's backlog meanwhile (H23's late lead). */
static void hold(uint32_t ms)
{
    uint64_t until = now_ns() + (uint64_t)ms * 1000000;
    for (uint64_t now = now_ns(); now < until; now = now_ns()) {
        uint64_t wait = until - now < OSOE_SWEEP_NS ? until - now : OSOE_SWEEP_NS;
        struct timespec t = { (time_t)(wait / 1000000000), (long)(wait % 1000000000) };
        nanosleep(&t, NULL);
        osoe_sweep_expired("shmem_init");
    }
}

void osoe_wires_open(struct osoe_wire *wires, struct osoe_ring *const *rings, uint32_t helpers,
                     const struct osoe_bootstrap *b, uint32_t session,
                     const struct osoe_mesh *mesh)
{
    /*
     * §6.2, by the mesh's schedule (mesh-schedule.h; H23): each pair has one
     * caller, so one stream of each wire (§3.1), and each lead calls about
     * half its peers and answers the rest, one offset a slot, at most
     * OSOE_MESH_CALLS_IN_FLIGHT calls in flight. The calls are cycles - at the
     * offset 1 every lead calls the next - so a lead never waits in one call:
     * one loop answers its listener and takes each call a step on as its
     * socket allows (mesh_model.py: blocking calls wait for ever).
     */
    uint32_t n = b->node_count, peers = n - 1;
    for (uint32_t i = 0; i < 2 * peers; i++)
        wires[i] = (struct osoe_wire){ .fd = -1 };
    struct osoe_listener own, *l = mesh ? mesh->listener : &own;
    if (!mesh)
        osoe_listener_open(&own, b, session);
    if (b->mesh_hold_ms && b->mesh_hold_node == b->node)
        hold(b->mesh_hold_ms);
    /* The slots begin with the rendezvous's release, which reached this lead
     * just now, nothing between them (the proposal: a late start is one
     * waiting call a slot missed, at the late lead). */
    uint64_t begun_ns = now_ns();
    uint64_t slot_ns = (uint64_t)(b->mesh_slot_set ? b->mesh_slot_us : OSOE_DEFAULT_MESH_SLOT_US) *
                       1000;

    uint32_t slots = osoe_mesh_schedule_slots(n);
    uint32_t *offsets = malloc((slots ? slots : 1) * sizeof *offsets);
    struct mesh m = { wires, rings, b, l, peers, helpers,
                      malloc((slots ? 2 * slots : 1) * sizeof *m.calls), 0, 0, 0,
                      2 * osoe_mesh_schedule_callers(b->node, n), 0, { NULL }, { 0 } };
    if (!offsets || !m.calls)
        osoe_fatal("out of memory for the mesh's schedule of %u nodes", n);
    osoe_mesh_schedule_order(b->job, b->step, n, offsets);
    for (uint32_t s = 0; s < slots; s++) {
        uint32_t peer = osoe_mesh_schedule_callee(b->node, offsets[s], n);
        if (peer == n)
            continue;
        m.calls[m.call_count++] = (struct mesh_call){ peer, OSOE_HELLO_CONTROL_WIRE, s };
        m.calls[m.call_count++] = (struct mesh_call){ peer, OSOE_HELLO_DATA_WIRE, s };
    }
    free(offsets);
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
        m.flying[k] = osoe_listener_call_new();
        m.flying_call[k] = UINT32_MAX;
    }

    const uint32_t purposes = OSOE_HELLO_PURPOSE(OSOE_HELLO_CONTROL_WIRE) |
                              OSOE_HELLO_PURPOSE(OSOE_HELLO_DATA_WIRE);
    struct pollfd p[OSOE_LISTENER_POLLED + OSOE_MESH_CALLS_IN_FLIGHT];
    uint32_t polled_call[OSOE_MESH_CALLS_IN_FLIGHT];
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
    uint64_t progress = 0, sweep_ns = begun_ns + OSOE_SWEEP_NS;
    while (m.made < m.call_count || m.answered < m.expected) {
        uint64_t now = now_ns();
        if (now >= sweep_ns) {
            sweep_ns = now + OSOE_SWEEP_NS;
            osoe_sweep_expired("shmem_init");
            bool stalled = osoe_stall_watch_stalled(&watch, progress);
            bool notice = !stalled && osoe_stall_watch_notice_due(&watch);
            char what[640];
            if (stalled || notice)
                describe_mesh(&m, what, sizeof what);
            if (stalled)
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is "
                           "waiting for %s", (unsigned long long)osoe_stall_watch_seconds(&watch),
                           b->node, what);
            if (notice)
                osoe_notice("shmem_init on node %u has waited %llu s for %s; still waiting",
                            b->node, (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            what);
        }
        /* Begin each call whose slot has come while a place is free: in
         * order, and however late - a slot says not before (mesh_model.py's
         * skip_late). */
        uint64_t wake = sweep_ns;
        for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT && m.begun < m.call_count; k++) {
            if (m.flying_call[k] != UINT32_MAX)
                continue;
            const struct mesh_call *call = &m.calls[m.begun];
            uint64_t due = begun_ns + call->slot * slot_ns;
            if (due > now) {
                if (due < wake)
                    wake = due;
                break;
            }
            struct sockaddr_in at;
            uint16_t port = 0;
            if (mesh) {
                at = mesh->peers[call->peer];
                port = ntohs(at.sin_port);
            } else {
                /* Without a rendezvous - the tests below the library - each
                 * peer is found at its host-list address on its candidate
                 * ports, as the rendezvous finds its parent. Not at loopback:
                 * a node in a network namespace of its own (tier 3, §8.2)
                 * reaches only itself there. */
                osoe_listener_node_address(b, call->peer, &at);
            }
            osoe_listener_call_begin(m.flying[k], b, session, &at, port, call->peer,
                                     call->purpose);
            m.flying_call[k] = m.begun++;
        }
        uint32_t count = osoe_listener_poll(l, purposes, p, &wake);
        for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
            polled_call[k] = UINT32_MAX;
            if (m.flying_call[k] == UINT32_MAX)
                continue;
            short events = 0;
            int fd = osoe_listener_call_poll(m.flying[k], &events, &wake);
            if (fd < 0)
                continue;
            polled_call[k] = count;
            p[count++] = (struct pollfd){ .fd = fd, .events = events };
        }
        now = now_ns();
        uint64_t wait_ns = wake > now ? wake - now : 0;
        struct timespec t = { (time_t)(wait_ns / 1000000000), (long)(wait_ns % 1000000000) };
        int r = ppoll(p, count, &t, NULL);
        if (r < 0 && errno != EINTR)
            osoe_fatal("waiting for the mesh's connections: %s", strerror(errno));
        if (r < 0)
            continue;
        /* Every caller its listener has, confirmed. */
        for (;;) {
            uint32_t peer;
            enum osoe_hello_purpose purpose;
            int fd = osoe_listener_serve(l, purposes, p, &peer, &purpose);
            if (fd < 0)
                break;
            attach_called(&m, fd, peer, purpose);
        }
        /* Each call in flight a step on, polled or not: a dial's limit, a
         * back-off's end. */
        for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++) {
            if (m.flying_call[k] == UINT32_MAX)
                continue;
            short revents = polled_call[k] != UINT32_MAX ? p[polled_call[k]].revents : 0;
            int fd = osoe_listener_call_step(m.flying[k], revents);
            if (fd < 0)
                continue;
            attach_made(&m, fd, &m.calls[m.flying_call[k]]);
            m.flying_call[k] = UINT32_MAX;
        }
        /* A wait is for the next wire: each one that comes begins it again,
         * so the notice says how long none has. */
        if ((uint64_t)m.made + m.answered != progress) {
            progress = (uint64_t)m.made + m.answered;
            osoe_stall_watch_begin_noticed(&watch, progress,
                                           osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
        }
    }
    osoe_listener_close(l);
    for (uint32_t k = 0; k < OSOE_MESH_CALLS_IN_FLIGHT; k++)
        osoe_listener_call_free(m.flying[k]);
    free(m.calls);
    /* Under SHMEM_DEBUG, how long the mesh took this lead, from the
     * rendezvous's release to its last wire (H23's timing). */
    if (b->debug)
        osoe_print_line("osoe: node %u connected its mesh in %llu us: %u wires answered and %u "
                        "called, in %u slots of %llu us, %u calls in flight at most\n", b->node,
                        (unsigned long long)((now_ns() - begun_ns) / 1000), m.answered, m.made,
                        slots, (unsigned long long)(slot_ns / 1000), OSOE_MESH_CALLS_IN_FLIGHT);
    for (uint32_t o = 0; o <= helpers; o++)
        ring_open(rings[o], &wires[osoe_ring_first(o, peers, helpers)],
                  osoe_ring_wires(o, peers, helpers), osoe_owner_wires(o, peers, helpers), b,
                  o == 0);
}

/* osoe_wire_send and osoe_wire_send_lent: `lent` is the region the bytes are
 * lent from, or OSOE_WIRE_LENT_REGIONS for a send that copies. */
static size_t send_from(struct osoe_wire *w, const void *buffer, size_t len, uint32_t lent)
{
    if (w->error)
        osoe_fatal("the connection to node %u failed: %s", w->peer, strerror(-w->error));
    if (w->send_error)
        return 0;                       /* the peer has reset the connection (wire.h) */
    if (w->sending) {
        /* One send in flight (§3.3): the caller offers what it offered, until
         * the kernel says how much of it went. */
        if ((const unsigned char *)buffer != w->send_at)
            osoe_fatal("bytes toward node %u were offered while another send was in "
                       "flight (§3.3, invariant 2)", w->peer);
        if (w->sent < 0)
            return 0;
        size_t n = (size_t)w->sent;
        w->sending = false;
        w->sent = -1;
        return n;
    }
    if (len == 0)
        return 0;
    if (len > UINT32_MAX)
        len = UINT32_MAX;
    uint32_t n = (uint32_t)truncate_to(w, w->sends++ ^ UINT64_C(0x5e4d), len);
    struct io_uring_sqe *sqe = osoe_ring_entry(w->ring);
    /* MSG_NOSIGNAL: a peer that has gone is a completion with -EPIPE, not a signal. */
    /*
     * By reference only while the other region owes nothing, so that the
     * sender always has a region a copied send away from free (wire.h). On the
     * Pi, with both lent, an acknowledgement the peer was waiting for found no
     * room until a notification came, and the notification waited for the
     * peer's TCP to acknowledge, which it delays while it has nothing to send:
     * osoe-bench's 1 MiB lines took seconds (reservation_model.py, BOTH LENT).
     */
    if (lent < OSOE_WIRE_LENT_REGIONS && w->zero_copy && n >= w->zero_copy_bytes &&
        w->owed[lent ^ 1] == 0 && w->owed[lent] < OSOE_WIRE_ZERO_COPY_OWED_MOST) {
        /* By reference: the completion says how much the kernel took, and then a
         * notification - IORING_CQE_F_NOTIF on the same tag - when it has let go of
         * those pages, which over TCP is once the peer has acknowledged them. The
         * notification says too whether the kernel copied after all (ring.c). */
        io_uring_prep_send_zc(sqe, w->fd, buffer, n, MSG_NOSIGNAL, IORING_SEND_ZC_REPORT_USAGE);
        io_uring_sqe_set_data64(sqe, osoe_ring_send_tag(w->index, lent));
        w->ring->counts.zero_copy_sends++;
    } else {
        io_uring_prep_send(sqe, w->fd, buffer, n, MSG_NOSIGNAL);
        io_uring_sqe_set_data64(sqe, osoe_ring_send_tag(w->index, OSOE_WIRE_LENT_REGIONS));
    }
    w->send_at = buffer;
    w->send_bytes = n;
    w->sending = true;
    w->sent = -1;
    w->ring->counts.sends++;
    return 0;
}

size_t osoe_wire_send(struct osoe_wire *w, const void *buffer, size_t len)
{
    return send_from(w, buffer, len, OSOE_WIRE_LENT_REGIONS);
}

size_t osoe_wire_send_lent(struct osoe_wire *w, const void *buffer, size_t len,
                           uint32_t region)
{
    if (region >= OSOE_WIRE_LENT_REGIONS)
        osoe_fatal("bytes toward node %u were lent from region %u of %u", w->peer, region,
                   OSOE_WIRE_LENT_REGIONS);
    return send_from(w, buffer, len, region);
}

uint32_t osoe_wire_owed(const struct osoe_wire *w, uint32_t region)
{
    return region < OSOE_WIRE_LENT_REGIONS ? w->owed[region] : 0;
}

bool osoe_wire_limit_unsent(struct osoe_wire *w, uint32_t bytes)
{
    /*
     * With the kernel's unsent queue limited, a send it cannot take whole is a
     * short write (§3.3), offered again when the queue drains, and the stream
     * waits in the connection's own two buffers, where a small message behind
     * it is a turnover away rather than megabytes (§3.2).
     */
    if (w->fd < 0)
        return false;
    if (bytes == 0)
        return true;
    int v = (int)bytes;
    if (setsockopt(w->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, sizeof v) != 0)
        osoe_fatal("cannot limit the unsent bytes toward node %u to %u (TCP_NOTSENT_LOWAT): %s",
                   w->peer, bytes, strerror(errno));
    return true;
}

void osoe_wire_lift_unsent(struct osoe_wire *w)
{
    if (w->fd < 0)
        return;
    int v = INT_MAX;
    (void)setsockopt(w->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, sizeof v);
}

size_t osoe_wire_peek(struct osoe_wire *w, const unsigned char **bytes)
{
    *bytes = NULL;
    w->peeked = 0;
    if (w->queued == 0)
        return 0;
    struct osoe_ring *r = w->ring;
    uint32_t b = w->first;
    *bytes = r->buffer_memory + (size_t)b * r->buffer_bytes + w->offset;
    w->peeked = (uint32_t)truncate_to(w, w->recvs++, r->filled[b] - w->offset);
    return w->peeked;
}

void osoe_wire_consume(struct osoe_wire *w, size_t n)
{
    /* Against the last peek, not the buffer: under OSOE_WIRE_SPLIT a peek shows
     * less than its buffer holds, and a consume of the rest would skip bytes the
     * parse never saw (the third adversarial read, S1). A peek is consumed once. */
    if (n > w->peeked)
        osoe_fatal("%zu bytes from node %u were consumed where the last peek showed %u "
                   "(wire.h)", n, w->peer, w->peeked);
    w->peeked = 0;
    if (n == 0)
        return;
    struct osoe_ring *r = w->ring;
    uint32_t b = w->first;
    w->offset += (uint32_t)n;
    w->queued -= n;
    if (w->offset == r->filled[b]) {            /* emptied: back to the kernel at once */
        w->first = r->next[b];
        if (w->first == OSOE_RING_NONE)
            w->last = OSOE_RING_NONE;
        w->offset = 0;
        osoe_ring_recycle(r, b);
    }
}

void osoe_wire_shutdown(struct osoe_wire *w)
{
    if (w->shut || w->fd < 0)
        return;
    w->shut = true;
    if (shutdown(w->fd, SHUT_WR) != 0 && errno != ENOTCONN)
        osoe_fatal("cannot end the stream toward node %u: %s", w->peer, strerror(errno));
}

bool osoe_wire_ended(const struct osoe_wire *w)
{
    return w->ended && w->queued == 0;
}

uint32_t osoe_wire_silent_ms(const struct osoe_wire *w)
{
    struct tcp_info info;
    socklen_t len = sizeof info;
    memset(&info, 0, sizeof info);
    if (w->fd < 0 || getsockopt(w->fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
        len < offsetof(struct tcp_info, tcpi_last_ack_recv) + sizeof info.tcpi_last_ack_recv)
        return 0;
    return info.tcpi_unacked > 0 ? info.tcpi_last_ack_recv : 0;
}

bool osoe_wire_unacknowledged(const struct osoe_wire *w)
{
    int queued = 0;
    if (w->fd < 0 || ioctl(w->fd, SIOCOUTQ, &queued) != 0 || queued == 0)
        return false;
    /* A reset purges the send queue without moving what SIOCOUTQ subtracts,
     * so it never reads 0 after one: a closed connection has nothing more to
     * be acknowledged. */
    struct tcp_info info;
    socklen_t len = sizeof info;
    return getsockopt(w->fd, IPPROTO_TCP, TCP_INFO, &info, &len) != 0 ||
           info.tcpi_state != TCP_CLOSE;
}

void osoe_wire_close(struct osoe_wire *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
}
