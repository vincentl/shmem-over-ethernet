#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/environment.h"
#include "fatal.h"
#include "listener.h"
#include "wire.h"

/*
 * §3.14's fault injection, stateless and hash-derived, so the same seed gives
 * the same decision for the same transfer however the threads interleave. A
 * shared generator would make the split each site sees depend on arrival order
 * and destroy reproducibility, which is the whole value.
 */
static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

/*
 * How much of `n` this transfer moves: all of it, or - OSOE_WIRE_SPLIT times in
 * a myriad - a prefix of at least one byte. Never nothing: a send truncated to
 * nothing would submit nothing and leave the connection waiting for a
 * completion that is not coming, and a receive returning 0 with bytes queued
 * would end the pass's loop over a wire it must empty (§3.3).
 */
static size_t truncate_to(const struct osoe_wire *w, uint64_t site, uint64_t n)
{
    if (w->split == 0 || n <= 1)
        return (size_t)n;
    uint64_t h = hash64(w->seed ^ site ^ (n * UINT64_C(0x9E3779B97F4A7C15)));
    if (h % 10000 >= w->split)
        return (size_t)n;
    return (size_t)((h >> 20) % n + 1);
}

struct wire_settings {
    uint32_t buffer_bytes, buffer_count, split;
    uint64_t seed;
};

static void read_settings(struct wire_settings *s, const struct osoe_bootstrap *b,
                          bool announce)
{
    s->buffer_bytes = OSOE_WIRE_DEFAULT_BYTES;
    if (osoe_environment_u32("OSOE_WIRE_BYTES", &s->buffer_bytes) == OSOE_ENVIRONMENT_INVALID ||
        s->buffer_bytes < OSOE_WIRE_MIN_BYTES || s->buffer_bytes > OSOE_WIRE_MAX_BYTES)
        osoe_fatal("OSOE_WIRE_BYTES='%s' is not a byte count between %u and %u",
                   getenv("OSOE_WIRE_BYTES"), OSOE_WIRE_MIN_BYTES, OSOE_WIRE_MAX_BYTES);
    s->buffer_count = OSOE_WIRE_DEFAULT_BUFFERS;
    if (osoe_environment_u32("OSOE_WIRE_BUFFERS", &s->buffer_count) ==
            OSOE_ENVIRONMENT_INVALID || s->buffer_count < 2 ||
        s->buffer_count > OSOE_WIRE_MAX_BUFFERS ||
        (s->buffer_count & (s->buffer_count - 1)) != 0)
        osoe_fatal("OSOE_WIRE_BUFFERS='%s' is not a power of two between 2 and %u",
                   getenv("OSOE_WIRE_BUFFERS"), OSOE_WIRE_MAX_BUFFERS);
    s->split = 0;
    if (osoe_environment_u32("OSOE_WIRE_SPLIT", &s->split) == OSOE_ENVIRONMENT_INVALID ||
        s->split > 10000)
        osoe_fatal("OSOE_WIRE_SPLIT='%s' is not a rate per myriad (0 to 10000)",
                   getenv("OSOE_WIRE_SPLIT"));
    /*
     * §3.14: the seed must be overridable by environment, with a failing one
     * committed as a permanent regression case. Without the override it comes
     * from the job id, which is the launcher's pid, so every run would inject a
     * different pattern and a case that failed could not be run again.
     */
    s->seed = hash64(b->job ^ ((uint64_t)b->step << 32));
    uint64_t seed;
    switch (osoe_environment_u64("OSOE_WIRE_SEED", &seed)) {
    case OSOE_ENVIRONMENT_OK:
        s->seed = seed;
        break;
    case OSOE_ENVIRONMENT_UNSET:
        break;
    default:
        osoe_fatal("OSOE_WIRE_SEED='%s' is not a decimal number", getenv("OSOE_WIRE_SEED"));
    }
    /* Said whenever injection is on, so a failure names what to reproduce it
     * with; off by default, so a normal run says nothing. */
    if (announce && s->split != 0)
        fprintf(stderr, "osoe: wire split %u per myriad, OSOE_WIRE_SEED=%llu\n", s->split,
                (unsigned long long)s->seed);
}

void osoe_wire_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t count,
                         const struct osoe_bootstrap *b)
{
    struct wire_settings s;
    read_settings(&s, b, true);
    osoe_ring_open(r, wires, count, s.buffer_count, s.buffer_bytes);
}

void osoe_wire_attach(struct osoe_wire *w, struct osoe_ring *r, uint32_t slot, int fd,
                      uint32_t peer, const struct osoe_bootstrap *b)
{
    struct wire_settings settings;
    read_settings(&settings, b, false);
    const struct wire_settings *s = &settings;
    /* The seed differs per peer, so the two directions of a pair - and two
     * wires in one test process - split differently. */
    *w = (struct osoe_wire){ .fd = fd, .peer = peer, .slot = slot, .ring = r,
                             .first = OSOE_RING_NONE, .last = OSOE_RING_NONE, .sent = -1,
                             .seed = hash64(s->seed ^ ((uint64_t)b->node << 32) ^ peer),
                             .split = s->split };
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
     * after it, a fatal timeout. With data in flight the kernel's own
     * retransmission limit ends a vanished peer's connection instead
     * (tcp_retries2: about fifteen minutes by default).
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

void osoe_wires_open(struct osoe_wire *wires, struct osoe_ring *ring,
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
    for (uint32_t slot = 0; slot < peers; slot++)
        wires[slot] = (struct osoe_wire){ .fd = -1 };
    struct osoe_listener own, *l = mesh ? mesh->listener : &own;
    if (!mesh)
        osoe_listener_open(&own, b, session, NULL);
    for (uint32_t accepted = 0; accepted < b->node; accepted++) {
        uint32_t peer;
        int fd = osoe_listener_accept(l, OSOE_HELLO_MESH, &peer);
        if (peer >= b->node)
            osoe_fatal("node %u was connected to by node %u, which is not a lower node",
                       b->node, peer);
        uint32_t slot = peer;
        if (wires[slot].fd >= 0)
            osoe_fatal("node %u was connected to twice by node %u", b->node, peer);
        osoe_wire_attach(&wires[slot], ring, slot, fd, peer, b);
    }
    osoe_listener_close(l);
    /* Without a rendezvous's mesh - the tests below the library - each peer is
     * found at its host-list address on its candidate ports, as the rendezvous
     * finds its parent. Not at loopback: a node in a network namespace of its
     * own (tier 3, §8.2) reaches only itself there. */
    for (uint32_t peer = b->node + 1; peer < b->node_count; peer++) {
        int fd;
        if (mesh) {
            fd = osoe_listener_connect_port(b, session, &mesh->peers[peer],
                                            ntohs(mesh->peers[peer].sin_port), peer,
                                            OSOE_HELLO_MESH);
        } else {
            struct sockaddr_in at;
            osoe_listener_node_address(b, peer, &at);
            fd = osoe_listener_connect(b, session, &at, peer, OSOE_HELLO_MESH);
        }
        osoe_wire_attach(&wires[peer - 1], ring, peer - 1, fd, peer, b);
    }
    osoe_wire_ring_open(ring, wires, peers, b);
}

size_t osoe_wire_send(struct osoe_wire *w, const void *buffer, size_t len)
{
    if (w->error)
        osoe_fatal("the connection to node %u failed: %s", w->peer, strerror(-w->error));
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
    io_uring_prep_send(sqe, w->fd, buffer, n, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, ((uint64_t)w->slot << 8) | 2);
    w->send_at = buffer;
    w->send_bytes = n;
    w->sending = true;
    w->sent = -1;
    w->ring->counts.sends++;
    return 0;
}

size_t osoe_wire_recv(struct osoe_wire *w, void *buffer, size_t len)
{
    struct osoe_ring *r = w->ring;
    uint64_t have = w->queued < len ? w->queued : len;
    size_t n = truncate_to(w, w->recvs++, have);
    for (size_t done = 0; done < n; ) {
        uint32_t b = w->first;
        size_t take = r->filled[b] - w->offset;
        if (take > n - done)
            take = n - done;
        memcpy((unsigned char *)buffer + done,
               r->buffer_memory + (size_t)b * r->buffer_bytes + w->offset, take);
        done += take;
        w->offset += (uint32_t)take;
        if (w->offset == r->filled[b]) {        /* emptied: back to the kernel at once */
            w->first = r->next[b];
            if (w->first == OSOE_RING_NONE)
                w->last = OSOE_RING_NONE;
            w->offset = 0;
            osoe_ring_recycle(r, b);
        }
    }
    w->queued -= n;
    return n;
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

void osoe_wire_close(struct osoe_wire *w)
{
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
}
