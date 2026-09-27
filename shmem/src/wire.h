/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node-pair wire (§3.1, §3.3; phase 1 step 3).
 *
 * One TCP connection per node pair, carried by the progress thread's io_uring
 * (ring.h). §7.1 step 2 ran the same interface over a shared-memory stand-in,
 * so that every internode semantic was validated before a socket existed; this
 * replaces the stand-in, and the engine above it did not change: send and recv
 * mean what they meant.
 *
 * **It is a byte stream, not a message queue.** osoe_wire_send accepts a prefix
 * of what it was offered and says how much; osoe_wire_recv delivers whatever has
 * arrived, which may be a fraction of a segment or several segments' worth.
 * §3.3's parser is written against exactly that - a parse cursor per connection,
 * headers accumulated in a small per-connection area, payload copied straight to
 * its destination as it arrives - and §8.2 warns that loopback "will never
 * produce a short write or partial read", which is where the most common defect
 * in hand-rolled TCP framing lives. So this wire produces them on purpose:
 * OSOE_WIRE_SPLIT truncates sends and receives, deterministically (§3.14's
 * hash-derived injection, so the same seed gives the same splits and a failing
 * case is a permanent regression case), and a small OSOE_WIRE_BYTES makes the
 * kernel split a segment across provided buffers besides.
 *
 * **Sending is asynchronous, and that is the one change of contract.** Offered
 * bytes go to the kernel by reference - the sealed connection buffer itself, with
 * no copy - so they must stay unchanged until they are reported accepted, and
 * the caller offers the rest again from where the report ended, exactly as §3.3
 * says of a short send ("advance past a short send and resubmit the
 * remainder"). The first offer starts the send and returns 0; an offer after
 * the completion has been reaped returns what the kernel took. The engine
 * already behaved this way - a buffer is released only once every byte of it
 * has been accepted - so nothing above had to change; the wire checks it.
 *
 * Only the node's lead touches a wire (§3.12, invariant 19), on the thread that
 * drives the ring.
 */
#ifndef OSOE_WIRE_H
#define OSOE_WIRE_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/deadline.h"
#include "bootstrap.h"
#include "listener.h"
#include "ring.h"

/*
 * Provided buffers: OSOE_WIRE_BUFFERS of OSOE_WIRE_BYTES each, shared by every
 * wire of the ring, so the memory a node holds for receiving does not grow with
 * its peers. The count is what bounds the completion queue (§3.3); the size
 * only how finely a segment is split. A small size is how the tests make the
 * kernel split what a sender wrote whole.
 */
#define OSOE_WIRE_DEFAULT_BYTES    (UINT32_C(16) << 10)
#define OSOE_WIRE_MIN_BYTES        UINT32_C(64)
#define OSOE_WIRE_MAX_BYTES        (UINT32_C(1) << 20)
#define OSOE_WIRE_DEFAULT_BUFFERS  UINT32_C(64)
#define OSOE_WIRE_MAX_BUFFERS      UINT32_C(32768)

struct osoe_wire {
    int fd;
    uint32_t peer;              /* the node at the other end */
    uint32_t slot;              /* its place in the ring's wires */
    struct osoe_ring *ring;
    /* Receiving: provided buffers the reap queued here, oldest first. */
    uint32_t first, last;       /* buffer numbers, or OSOE_RING_NONE */
    uint32_t offset;            /* bytes of `first` already delivered */
    uint64_t queued;            /* bytes in all of them, not yet delivered */
    bool receiving;             /* its multishot recv is armed */
    bool ended;                 /* the peer's end of stream has arrived */
    int error;                  /* a receive failed, or a send for any reason but
                                 * the peer's reset: -errno, or 0 */
    /* A send met the peer's reset - its process ended with input unread:
     * -EPIPE or -ECONNRESET, or 0. Nothing more is sent, and the receive side
     * goes on, since it may hold what the peer sent ahead of the reset, its
     * goodbye among it; the stream's end decides (progress.c). */
    int send_error;
    /* Sending: one in flight (§3.3, invariant 2). */
    const unsigned char *send_at;
    uint32_t send_bytes;        /* what was submitted */
    bool sending;               /* submitted, and not yet reported to the caller */
    int32_t sent;               /* its completion's result, or -1 until it is reaped */
    bool shut;                  /* this side's end of stream has been sent */
    /* §3.14's fault injection, stateless and hash-derived. */
    uint64_t seed;
    uint32_t split;             /* truncations per myriad; 0 disables */
    uint64_t sends, recvs;      /* this wire's transfers, for the hash */
};

/*
 * Where every node's lead listens for its node-pair connections, as the
 * rendezvous taught it (§6.2): `peers[n]` is node n's address and port, and
 * `listener` is this lead's own, already listening since it advertised its port.
 */
struct osoe_mesh {
    struct osoe_listener *listener;
    const struct sockaddr_in *peers;
};

/*
 * Establish this lead's node-pair connections, one per peer node, and open the
 * ring that carries them (disabled until the driving thread enables it). §6.2's
 * rule: the lower node index connects to the higher, so each pair has exactly
 * one stream. With a mesh, each connection goes to the port its peer advertised;
 * without one (NULL), each lead is found at its host-list address by the port
 * rule (listener.h), which is how the tests below the library meet.
 * `wires` has node_count - 1 entries, in peer-slot order. Fatal on failure
 * (§3.15).
 */
void osoe_wires_open(struct osoe_wire *wires, struct osoe_ring *ring,
                     const struct osoe_bootstrap *b, uint32_t session,
                     const struct osoe_mesh *mesh);

/*
 * The two halves of osoe_wires_open, for a caller that makes its own
 * connections - a test holding both ends of one in a single process. The ring
 * for `count` wires, sized from OSOE_WIRE_BYTES and OSOE_WIRE_BUFFERS and
 * disabled until enabled; and a connected socket made wire `slot` of it, toward
 * node `peer`.
 */
void osoe_wire_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t count,
                         const struct osoe_bootstrap *b);
void osoe_wire_attach(struct osoe_wire *w, struct osoe_ring *r, uint32_t slot, int fd,
                      uint32_t peer, const struct osoe_bootstrap *b);

/*
 * Write up to len bytes. Returns how many the kernel has accepted, which may be
 * 0 and is often less than len; the caller keeps the rest and offers it again,
 * unchanged, from where the count ended. See above for why the bytes offered
 * must stay put until then.
 */
size_t osoe_wire_send(struct osoe_wire *w, const void *buffer, size_t len);

/* Read up to len bytes. Returns how many were delivered, which may be 0. */
size_t osoe_wire_recv(struct osoe_wire *w, void *buffer, size_t len);

/* Send this side's end of stream, once it has nothing more to send. */
void osoe_wire_shutdown(struct osoe_wire *w);

/* The peer's end of stream has arrived and every byte before it has been
 * delivered. */
bool osoe_wire_ended(const struct osoe_wire *w);

/* Whether the peer's kernel has yet to acknowledge anything sent on this wire:
 * SIOCOUTQ, bytes sent or queued and not acknowledged. False with no socket,
 * and once the connection has closed - a reset - since nothing will be. */
bool osoe_wire_unacknowledged(const struct osoe_wire *w);

/* Close the socket; the caller has sent its end of stream and seen the peer's. */
void osoe_wire_close(struct osoe_wire *w);

#endif /* OSOE_WIRE_H */
