/*
 * The node's progress engine (§3.3, §3.7, §3.8, §3.9, §3.12).
 *
 * One pass over every peer connection: receive and apply what arrived, drain
 * deferred replies into outbound buffers, and move sealed buffers onto the
 * wire. The local lead's agent runs it; no other thread does (§3.12), so
 * everything here except the connection buffers themselves is private to that
 * thread.
 *
 * **A handler may enqueue, but never reserve** (§3.7). That is the load-bearing
 * rule and it shapes this file: applying an incoming message may take a reply
 * *descriptor*, which is fallible and returns at once, but it never asks for
 * outbound buffer space. A handler that waited for space would be waiting on a
 * resource only this thread releases - a single node deadlocking on itself,
 * with no peer misbehaviour and no symmetric load required. Reserving is done
 * in the drain step below, where failing is a non-event.
 *
 * **Aggregation self-clocks off the send** (§3.9). There is one in-flight send
 * per connection (§3.3), so while a send is outstanding, operations accumulate
 * in the open buffer; when it completes, everything queued goes as one segment.
 * Batch size grows under load and falls to one when idle, with no timers and no
 * tuning parameters. Nothing here waits for a buffer to fill.
 *
 * The host supplies the three things this layer deliberately does not know:
 * how to turn (pe, region, offset) into an address, where a PE's landing zone
 * is, and how to wake that PE's waiters. Keeping them behind callbacks is what
 * lets the loop be tested without a job, and what keeps it from reaching into
 * global state that only exists inside a real PE.
 */
#ifndef OSOE_PROGRESS_H
#define OSOE_PROGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "am.h"
#include "connection-area.h"
#include "node.h"
#include "reply.h"
#include "wire.h"

/* What the progress loop asks of the PE it runs inside. */
struct osoe_progress_host {
    /*
     * The address of (pe, region, offset) in this process, or NULL if this node
     * does not hold that PE. All local heaps are mapped in every process of the
     * node (§4.1), so for a local PE this is ordinary pointer arithmetic.
     * `length` is passed so the host can bounds-check: a message naming an
     * offset outside the region is a peer disagreeing about the job, and the
     * host aborts rather than writing past a heap.
     */
    void *(*resolve)(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                     uint32_t length);
    /* A local PE's landing zone (§3.12), or NULL if the PE is not local. */
    struct osoe_landing_slot *(*landing)(void *context, uint32_t pe);
    /* Wake that PE's waiters whose watched range this write overlapped (§4.9). */
    void (*notify)(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length);
    void *context;
};

/* The progress thread's private bookkeeping for one connection. Writers see
 * only the seal bit in the buffer's head (§3.5); none of this is shared. */
struct osoe_progress_connection {
    uint8_t state[2];               /* enum osoe_buffer_state */
    uint32_t open;                  /* the reservable buffer, or OSOE_CONNECTION_NONE */
    uint32_t sealed;                /* sealed and waiting to go out */
    uint32_t sending;               /* on the wire now; at most one (§3.3) */
    uint32_t sent;                  /* bytes of it the wire has taken so far */
    uint32_t sending_bytes;         /* its total length */
    uint64_t next_seq;              /* assigned when a buffer becomes reservable */
    uint64_t applied;               /* highest sequence number applied from the peer */
    uint64_t acked;                 /* highest the peer says it has applied */
    bool owe_ack;                   /* the peer asked; a segment must carry one */
    struct osoe_am_parser parser;
    struct osoe_reply_queues queues;
};

struct osoe_progress {
    struct osoe_connection_area *area;
    struct osoe_wire *wire;                     /* one per peer, by peer slot */
    struct osoe_progress_connection *connection;/* one per peer, by peer slot */
    struct osoe_reply_pool pool;
    struct osoe_reply *descriptors;
    unsigned char *scratch;                     /* receive staging */
    uint32_t scratch_bytes;
    uint32_t node, nodes;
    struct osoe_progress_host host;

    /* §3.14 counters, read by osoe-stat and by the tests. */
    uint64_t segments_sent, segments_applied, messages_applied;
    uint64_t reply_reservation_failures;        /* the fallible path, taken */
    uint64_t acks_only;                         /* header-only segments sent */
};

/*
 * Set up for a node of `nodes` logical nodes. `wires` is one wire per peer in
 * peer-slot order, `descriptors` the node-wide pool backing every connection's
 * queues (§3.8), and `scratch` a staging buffer at least one AM chunk long.
 */
void osoe_progress_init(struct osoe_progress *p, struct osoe_connection_area *area,
                        struct osoe_wire *wires, struct osoe_reply *descriptors,
                        uint32_t descriptor_count, unsigned char *scratch,
                        uint32_t scratch_bytes, const struct osoe_progress_host *host);

/*
 * One pass. Returns true if anything moved, which is what tells the idle loop
 * whether to sleep (§3.10): a pass that moved nothing is the only safe moment
 * to wait, and the wait still carries a sweep deadline so that a lost wake
 * costs latency rather than the job.
 */
/* Bind the per-connection state, one entry per peer in peer-slot order, and
 * put every connection into its starting state. Separate from init because the
 * caller owns the array: the loop never allocates. */
void osoe_progress_attach(struct osoe_progress *p,
                          struct osoe_progress_connection *connections);

bool osoe_progress_poll(struct osoe_progress *p);

/* The peer-slot index for a peer node, or OSOE_CONNECTION_NONE for this node. */
uint32_t osoe_progress_slot(const struct osoe_progress *p, uint32_t peer);

/* Ask that the next segment toward `peer` carries an acknowledgement request,
 * which is what fence, quiet and the barriers set so that a one-way peer still
 * acknowledges (§3.4, §3.9). */
void osoe_progress_request_ack(struct osoe_progress *p, uint32_t peer);

/* The highest sequence number `peer` has applied (§4.6's completion evidence). */
uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer);

#endif /* OSOE_PROGRESS_H */
