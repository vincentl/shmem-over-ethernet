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
#include <unistd.h>

#include <linux/sockios.h>

#include "base/environment.h"
#include "fatal.h"
#include "listener.h"
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

void osoe_wire_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t count,
                         const struct osoe_bootstrap *b)
{
    struct osoe_wire_settings s;
    osoe_wire_settings_read(&s, b, true);
    /* With the zero-copy send on, each wire's notifications still owed are
     * completions the queue must have room for (§3.3, ring.h) - refused by name
     * where that passes io_uring's own most, rather than failing to create a
     * ring for a reason the setting does not name. */
    uint64_t completions = (uint64_t)s.buffer_count +
                           (2 + (uint64_t)OSOE_WIRE_ZERO_COPY_OWED_MOST) * count + 1;
    if (s.zero_copy && completions > OSOE_RING_COMPLETIONS_MOST)
        osoe_fatal("OSOE_WIRE_ZERO_COPY=on: %u peer nodes and %u receive buffers need a "
                   "completion queue of %llu, more than io_uring's %u; turn the zero-copy "
                   "send off, or lower OSOE_WIRE_BUFFERS", count, s.buffer_count,
                   (unsigned long long)completions, OSOE_RING_COMPLETIONS_MOST);
    osoe_ring_open(r, wires, count, s.buffer_count, s.buffer_bytes,
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
     * peer's *kernel* is there, which contention does not stop: a stopped
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

void osoe_wires_open(struct osoe_wire *wires, struct osoe_ring *const *rings, uint32_t helpers,
                     const struct osoe_bootstrap *b, uint32_t session,
                     const struct osoe_mesh *mesh)
{
    /*
     * §6.2: the lower node index connects to the higher, so each pair has one
     * stream (§3.1). Every lower node first, then every higher one: a lead is
     * answering its callers the whole time it waits for them, so no lead waits
     * on one that is itself waiting to be answered, and establishing the whole
     * mesh takes about N connection times rather than a chain of them.
     */
    uint32_t peers = b->node_count - 1;
    for (uint32_t i = 0; i < 2 * peers; i++)
        wires[i] = (struct osoe_wire){ .fd = -1 };
    struct osoe_listener own, *l = mesh ? mesh->listener : &own;
    if (!mesh)
        osoe_listener_open(&own, b, session, NULL);
    const enum osoe_hello_purpose purposes[2] = { OSOE_HELLO_CONTROL_WIRE,
                                                  OSOE_HELLO_DATA_WIRE };
    for (uint32_t accepted = 0; accepted < 2 * b->node; accepted++) {
        /* For the notice of a wait that lasts: the lower nodes still to call,
         * the first few of them and how many in all. */
        char expecting[160];
        uint32_t missing = 0, named = 0;
        bool lacks[b->node ? b->node : 1];
        for (uint32_t peer = 0; peer < b->node; peer++) {
            struct osoe_ring *ring;
            uint32_t ring_index;
            lacks[peer] = false;
            for (uint32_t k = 0; k < 2; k++)
                if (wire_of(wires, rings, peer, purposes[k], peers, helpers, &ring,
                            &ring_index)->fd < 0)
                    lacks[peer] = true;
            missing += lacks[peer];
        }
        int used = snprintf(expecting, sizeof expecting, "%u lower node%s of the mesh (",
                            missing, missing == 1 ? "" : "s");
        for (uint32_t peer = 0; peer < b->node && used > 0 &&
                                (size_t)used < sizeof expecting - 24; peer++)
            if (lacks[peer] && named++ < 8)
                used += snprintf(expecting + used, sizeof expecting - (size_t)used, "%s%u",
                                 named == 1 ? "node " : ", ", peer);
        if (used > 0 && (size_t)used < sizeof expecting - 8)
            snprintf(expecting + used, sizeof expecting - (size_t)used, "%s)",
                     named > 8 ? ", ..." : "");
        uint32_t peer;
        enum osoe_hello_purpose purpose;
        int fd = osoe_listener_accept(l, OSOE_HELLO_PURPOSE(OSOE_HELLO_CONTROL_WIRE) |
                                             OSOE_HELLO_PURPOSE(OSOE_HELLO_DATA_WIRE),
                                      &peer, &purpose, expecting);
        if (peer >= b->node)
            osoe_fatal("node %u was connected to by node %u, which is not a lower node",
                       b->node, peer);
        struct osoe_ring *ring;
        uint32_t ring_index;
        struct osoe_wire *w = wire_of(wires, rings, peer, purpose, peers, helpers, &ring,
                                      &ring_index);
        if (w->fd >= 0)
            osoe_fatal("node %u was connected to twice by node %u for its %s wire", b->node,
                       peer, wire_name(purpose));
        osoe_wire_attach(w, ring, ring_index, fd, peer, b);
    }
    osoe_listener_close(l);
    /* Without a rendezvous's mesh - the tests below the library - each peer is
     * found at its host-list address on its candidate ports, as the rendezvous
     * finds its parent. Not at loopback: a node in a network namespace of its
     * own (tier 3, §8.2) reaches only itself there. */
    for (uint32_t peer = b->node + 1; peer < b->node_count; peer++)
        for (uint32_t k = 0; k < 2; k++) {
            int fd;
            if (mesh) {
                fd = osoe_listener_connect_port(b, session, &mesh->peers[peer],
                                                ntohs(mesh->peers[peer].sin_port), peer,
                                                purposes[k]);
            } else {
                struct sockaddr_in at;
                osoe_listener_node_address(b, peer, &at);
                fd = osoe_listener_connect(b, session, &at, peer, purposes[k]);
            }
            struct osoe_ring *ring;
            uint32_t ring_index;
            struct osoe_wire *w = wire_of(wires, rings, peer - 1, purposes[k], peers, helpers,
                                          &ring, &ring_index);
            osoe_wire_attach(w, ring, ring_index, fd, peer, b);
        }
    for (uint32_t o = 0; o <= helpers; o++)
        osoe_wire_ring_open(rings[o], &wires[osoe_ring_first(o, peers, helpers)],
                            osoe_ring_wires(o, peers, helpers), b);
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
