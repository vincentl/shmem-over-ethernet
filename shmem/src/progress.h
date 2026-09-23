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
#include "collective-tree.h"
#include "connection-area.h"
#include "node.h"
#include "reply.h"

/* The engine holds these by pointer; wire.h and ring.h bring liburing.h, whose
 * POSIX types a strict-C11 file would then have to ask for. */
struct osoe_wire;
struct osoe_ring;

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
    /* A local PE's landing zone (§3.12), or NULL if the PE is not local, and in
     * `capacity` the bytes its data area holds - which a get reply is checked
     * against before a byte of it is written (§3.4's trust boundary). */
    struct osoe_landing_slot *(*landing)(void *context, uint32_t pe, uint32_t *capacity);
    /* Wake that PE's waiters whose watched range this write overlapped (§4.9). */
    void (*notify)(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length);
    /*
     * Release a team's epoch on this node: advance its `released` counter and
     * wake every local waiter (§5.3). It is a host callback rather than
     * something this file does, because the counter is the *team's* (§5.4) and
     * the progress loop has no business knowing what a team is. The host
     * forwards it to internode.h's upcall, which is where that knowledge is.
     */
    void (*release)(void *context, uint32_t team, uint32_t epoch);
    void *context;
    /*
     * A peer's abortive goodbye arrived (OSOE_AM_ABORT): the job is ending, on
     * behalf of `pe`, with `status`, as `disposition` says (1 a fatal, 2 a
     * global exit). The host declares its node's abort - the first declarer
     * wins - which wakes its agent to tell its own peers and end (abort.h).
     * NULL where there is no node: the engine's own tests.
     */
    void (*abort)(void *context, uint32_t pe, int32_t status, uint32_t disposition);
};

/*
 * One team's place in the internode tree, and the epoch it is in (§5.3).
 *
 * A node *opens* an epoch at the first arrival booked for it - its own members'
 * or a child's - and *closes* it when both are complete: it forwards ARRIVE to
 * its parent, or at the root starts the release. Closing resets the counts
 * **before** anything is released, so that a child released by this node's
 * forward can come back with the next epoch at once and find it clean
 * (tree_barrier_model.py, `release_first` and `wrong_epoch`).
 */
struct osoe_progress_team {
    bool registered;
    struct osoe_collective_tree tree;
    uint32_t open;              /* the epoch in progress, or 0 */
    uint32_t closed;            /* the last epoch closed */
    bool own;                   /* this node's members have arrived for `open` */
    uint32_t arrived;           /* children arrived for `open`, one bit per child */
    /*
     * An epoch whose release has been forwarded to the children but not yet
     * applied to this node's members, or 0. Applied at the end of the pass,
     * after the children's connections have been flushed, so the release
     * leaves for the next level before this level's local wake (§5.3).
     */
    uint32_t release_pending;
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
    /*
     * §3.4's acknowledgement, split in two because it is two things (second
     * review, C1). `owe_ack`: the peer asked, and this node's next segment pays
     * - every segment carries the cumulative `ack = applied`, so finalizing any
     * segment clears it. `flagged`: the highest sequence this node has sent
     * carrying ACK_REQUESTED, so a request made after its segment was already
     * finalized is asked once, on one header-only segment, and not every pass.
     * What this node's writers want is in the connection (ack_wanted_through).
     */
    bool owe_ack;
    uint64_t flagged;
    /*
     * The lead itself left bytes in the open buffer: the pre-open drain wrote
     * replies into it just before it became reservable, after this pass's
     * chance to seal. No writer set a dirty bit for them, so this is what keeps
     * the connection visited until they are sealed (work discovery, §3.10).
     */
    bool carrying;
    /*
     * The close (§6.4, close_model.py). `goodbye`: this node has sealed its
     * goodbye toward the peer - the last segment it sends there, header-only
     * and flagged - then sent it, then ended its stream. `peer_goodbye`: the
     * peer's has arrived, so the end of its stream is a peer that finished and
     * not one that died.
     */
    enum { OSOE_GOODBYE_NONE, OSOE_GOODBYE_SEALED, OSOE_GOODBYE_SENT } goodbye;
    bool peer_goodbye;
    /*
     * The abort (osoe_progress_abort, abort_model.py). `abort_done`: this
     * connection has sent its abortive goodbye and ended its stream, or has
     * nobody to tell - its normal goodbye already went, the peer died, or the
     * peer's own abort arrived (`peer_aborted`). `abort_sent`: bytes of the
     * abortive goodbye the wire has taken.
     */
    bool abort_done;
    bool peer_aborted;
    uint32_t abort_sent;
    struct osoe_am_parser parser;
    struct osoe_reply_queues queues;
};

struct osoe_progress {
    struct osoe_connection_area *area;
    struct osoe_wire *wire;                     /* one per peer, by peer slot */
    struct osoe_ring *ring;                     /* carries every wire (ring.h) */
    /*
     * One bit per peer slot: the connections with send-side work the lead
     * already knows about (busy()), kept current as that work appears and
     * goes, so that a pass finds them in N/64 words instead of asking every
     * connection. Private, like the rest of this struct.
     */
    uint64_t *busy;
    bool closing;                               /* osoe_progress_close has begun */
    /* The job is ending (osoe_progress_abort): none of its own traffic goes out
     * any more, and every connection ends with the abortive goodbye below - the
     * same bytes for every peer, built once. */
    bool aborting;
    unsigned char abort_segment[64];
    uint32_t abort_segment_bytes;
    struct osoe_progress_connection *connection;/* one per peer, by peer slot */
    struct osoe_reply_pool pool;
    struct osoe_reply *descriptors;
    unsigned char *scratch;                     /* receive staging */
    uint32_t scratch_bytes;
    uint32_t node, nodes;
    struct osoe_progress_host host;

    /*
     * The internode tree's state, per team (§5.3): private to the progress
     * thread, like everything else here, and O(k) per team - the topology -
     * plus O(1) for the epoch in progress. Registered once per team before
     * this thread runs (osoe_progress_team).
     */
    struct osoe_progress_team team[OSOE_TEAMS];

    /*
     * §3.14 counters, read by osoe-stat and by the tests: the node header's
     * `am` record when running inside a job (progress-host.c binds it), and
     * `own_metrics` until then, so that the loop counts the same way whether
     * or not there is a control segment to count into.
     */
    struct osoe_am_node_metrics *metrics;
    struct osoe_am_node_metrics own_metrics;
    /*
     * §3.14's connection scope and the tree's per-team state, in the control
     * segment where osoe-stat and the tests read them (progress-host.c binds
     * them). NULL outside a job - the engine's own tests run without a control
     * segment - and every write to them is guarded, because a diagnostic must
     * not decide whether the loop can run.
     */
    struct osoe_am_connection_metrics *connection_metrics;
    struct osoe_collective_state *collective;
};

/*
 * Set up for a node of `nodes` logical nodes. `wires` is one wire per peer in
 * peer-slot order, all carried by `ring`; `descriptors` the node-wide pool
 * backing every connection's queues (§3.8), and `scratch` a staging buffer at
 * least one AM chunk long.
 */
void osoe_progress_init(struct osoe_progress *p, struct osoe_connection_area *area,
                        struct osoe_wire *wires, struct osoe_ring *ring,
                        struct osoe_reply *descriptors, uint32_t descriptor_count,
                        unsigned char *scratch, uint32_t scratch_bytes,
                        const struct osoe_progress_host *host);

/*
 * One pass. Returns true if anything moved, which is what tells the idle loop
 * whether to sleep (§3.10): a pass that moved nothing is the only safe moment
 * to wait, and the wait still carries a sweep deadline so that a lost wake
 * costs latency rather than the job.
 */
/* Bind the per-connection state, one entry per peer in peer-slot order, and
 * the busy bitmap, (nodes - 1 + 63) / 64 words, and put every connection into
 * its starting state. Separate from init because the caller owns the arrays:
 * the loop never allocates. */
void osoe_progress_attach(struct osoe_progress *p,
                          struct osoe_progress_connection *connections, uint64_t *busy);

bool osoe_progress_poll(struct osoe_progress *p);

/*
 * Does this node owe the network nothing? True when every connection has an
 * empty reply queue, nothing sealed, nothing in flight, and no committed bytes
 * waiting in an open buffer.
 *
 * It exists for teardown. §5.3's root releases its own node the instant the
 * last node arrives and only *enqueues* the releases that free the others, so
 * a node can leave a collective while messages other nodes are waiting for are
 * still in a buffer that only its engine sends. Stopping the engine there
 * hangs every other node (§6.4).
 */
bool osoe_progress_idle(struct osoe_progress *p);

/*
 * Close every connection (§6.4, close_model.py): each one, once it owes the
 * peer nothing, sends its goodbye - a header-only segment flagged
 * OSOE_AM_SEGMENT_GOODBYE, the last it will send - and then ends its stream;
 * the passes that follow keep receiving until every peer's stream has ended
 * too. Called once the engine is idle (osoe_progress_idle). An end of stream
 * that arrives before its goodbye is a peer that died, and ends the job then
 * as at any other time.
 */
void osoe_progress_close(struct osoe_progress *p);

/* Every connection has sent its goodbye and ended its stream, and seen the
 * peer's goodbye and the end of the peer's. Its sockets may then be closed. */
bool osoe_progress_closed(const struct osoe_progress *p);

/*
 * The job is ending (§3.15; abort_model.py). From here the engine sends none of
 * the job's own traffic. Each connection finishes the segment already on its
 * wire, so the peer's stream stays whole, then sends its abortive goodbye - a
 * segment flagged OSOE_AM_SEGMENT_GOODBYE carrying one OSOE_AM_ABORT - and ends
 * its stream; unless it has nobody to tell: its normal goodbye already went, the
 * peer died, or the peer's own abort arrived. O(N) once, what a normal goodbye
 * costs. A connection that fails from here is a peer that is ending too, and is
 * not a fatal; nor is a peer's end of stream without its goodbye.
 */
void osoe_progress_abort(struct osoe_progress *p, uint32_t pe, int32_t status,
                         uint32_t disposition);

/* Every connection has sent its abortive goodbye, or has nobody to tell. */
bool osoe_progress_aborted(const struct osoe_progress *p);

/* The peer-slot index for a peer node, or OSOE_CONNECTION_NONE for this node. */
uint32_t osoe_progress_slot(const struct osoe_progress *p, uint32_t peer);

/* The highest sequence number `peer` has applied (§4.6's completion evidence). */
uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer);

/*
 * A team's internode tree, computed from its node set (internode-am.c,
 * collective-tree.h). Called before the engine's thread starts; the engine keeps
 * the tree, O(k), and never the node set, O(N).
 */
void osoe_progress_team(struct osoe_progress *p, uint32_t team,
                        const struct osoe_collective_tree *tree);

/*
 * This node's own arrival at a team's internode phase (§5.3): every local
 * member is in.
 *
 * Called by the lead's agent, out of the handoff word that whichever local
 * member's increment reached the arrival target left for it (internode-am.c).
 * Not by that member: everything here is the progress thread's private state,
 * in the lead's process, and the arriver is an application thread of any local
 * PE.
 *
 * Booked like a child's arrival. If it completes the epoch, the node forwards
 * ARRIVE to its parent - queued, for this same thread's drain - or, at the
 * root, starts the release.
 */
void osoe_progress_arrive(struct osoe_progress *p, uint32_t team, uint32_t epoch);

#endif /* OSOE_PROGRESS_H */
