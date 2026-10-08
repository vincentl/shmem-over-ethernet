/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node's progress engine (§3.3, §3.7, §3.8, §3.9, §3.12).
 *
 * One pass over an owner's connections: receive and apply what arrived, drain
 * deferred replies into outbound buffers, and move sealed buffers onto the
 * wire. An *owner* is a thread with a ring and the wires attached to it
 * (owner-map.h; docs/design/progress-helpers-proposal.md §3.2): the local
 * lead's agent is owner 0, and with no helpers it holds every wire. The same
 * pass runs on every owner, and no thread but an owner touches what it owns,
 * so everything here except the connection buffers themselves is private to
 * one thread: an owner's to its owner, and the engine's own - the control
 * plane, the internode tree's state - to owner 0, the lead's agent (§3.12).
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

struct osoe_deadlock_message;

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
    /*
     * A local PE's get `token`, blocking or not, in flight (§4.6; the extents
     * of layout 23): where its reply lands, and in `length` the bytes it asked
     * for - or NULL if the PE is not local or has no such get in flight, which
     * is a peer's bug (§3.4's trust boundary).
     */
    unsigned char *(*landing_request)(void *context, uint32_t pe, uint64_t token,
                                      uint32_t *length);
    /* A nonblocking get's reply has landed whole: mark its record landed, so
     * that a second reply naming it is refused and any of the PE's threads may
     * copy it out, then count it toward the context's wait (§4.6's counters)
     * and wake the PE's threads waiting there. */
    void (*landed)(void *context, uint32_t pe, uint64_t token, uint32_t bytes);
    /* A blocking get's reply has landed whole, `length` bytes, or a fetching
     * AMO's has arrived with `value`: complete it in the thread the token
     * names (§4.11 item 5) and wake that thread. False if no thread of that PE
     * waits for that token, a peer's bug. */
    bool (*completed)(void *context, uint32_t pe, uint64_t token, uint32_t length,
                      uint64_t value);
    /* A deadlock detector message from `node`'s lead arrived (§3.15). NULL
     * where there is no detector: the engine's own tests. */
    void (*deadlock)(void *context, uint32_t node, const struct osoe_deadlock_message *m);
    /*
     * Whether this node's abort is declared (§3.15): the job is ending, and a
     * peer's connection failing, or its stream ending without its goodbye, is
     * a peer ending too. Asked only of a wire that has failed or ended. With
     * helpers the peer's abortive goodbye may have arrived on a wire another
     * owner holds, which declared the node's abort (the progress helpers
     * proposal's Part 3). NULL where there is no node: the engine's own tests.
     */
    bool (*aborting)(void *context);
};

/*
 * How a host tells the engine a team it meets for the first time - its own
 * members' arrival, a child's, or a release (§5.3): the team's place in the
 * internode tree, in node numbers; where its state is published for osoe-stat
 * (§3.14); its name on its neighbours' nodes, which its messages carry
 * (`number`, `parent_lead`, `child_lead`); and the generation of its block
 * (`generation`) - into `learnt`, or false if the team has no internode phase
 * on this node. `team` is the team's block on this node (internode.h). A host
 * in a job reads it from the team's record in that block (node.h), published
 * before the team's first arrival anywhere; the engine never learns a team any
 * other way there, since the process it runs in may hold no descriptor for it.
 * Asked again whenever the block shows a generation other than the one the
 * engine learnt (the team redesign's step D, D4): the block holds another team.
 * Called with the host's context, and given with the per-team arrays it fills
 * (osoe_progress_attach_teams).
 */
struct osoe_progress_team;
typedef bool osoe_progress_team_source(void *context, uint32_t team,
                                       struct osoe_progress_team *learnt);

/*
 * How a host resolves the name a team's message carries (am.h) - the local id
 * of the team's local lead on this node, and its number - to the team's block
 * here, and that block's generation into `*generation`; or UINT32_MAX if this
 * node maps no block under the name (the team redesign's step D). A host in a
 * job reads the node's map and the block's name (node.h).
 */
typedef uint32_t osoe_progress_team_finder(void *context, uint32_t lead, uint32_t number,
                                           uint32_t *generation);

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
    /* The team's name on its neighbours' nodes (the team redesign's step D):
     * its number, and the local id of its local lead on the parent's node and
     * on each child's, in the order of `tree.child` - what a message to that
     * node names it by (am.h), the node's map resolving the name to its own
     * block. Learnt with the tree; for a team registered by hand, the engine's
     * own tests, the number is the team's index and every lead local id 0. */
    uint32_t number;
    uint32_t parent_lead;
    uint32_t child_lead[OSOE_COLLECTIVE_FANOUT_MAX];
    /* The generation of the block this was learnt at (node.h, osoe_team_name):
     * a handoff or a message showing another has the engine learn the block
     * afresh. 0 for a team registered by hand. */
    uint32_t generation;
    /* Where the state below is published for osoe-stat (§3.14): the team's
     * block in the node header, or NULL where there is none (the engine's own
     * tests), and then nothing is published. */
    struct osoe_collective_state *published;
    uint32_t open;              /* the epoch in progress, or 0 */
    uint32_t closed;            /* the last epoch closed */
    bool own;                   /* this node's members have arrived for `open` */
    uint32_t arrived;           /* children arrived for `open`, one bit per child */
    /*
     * An epoch whose release has been forwarded to the children but not yet
     * applied to this node's members, or 0. Applied at the end of the pass,
     * after the children's control wires have been flushed, so the release
     * leaves for the next level before this level's local wake (§5.3).
     */
    uint32_t release_pending;
    /*
     * The RELEASEs of this team still in a child's control queue (§3.8,
     * argument 2): the local release waits until none is (release_local), so
     * that the team's members here leave its block only once its forwards
     * have left the reply pool, and a block given back and claimed again finds
     * none of the old team's there - which the pool's bound of a degree's worth
     * a block counts on.
     */
    uint32_t releases_queued;
};

/*
 * Where a wire is in its close (§6.4, close_model.py): this node has sealed its
 * goodbye toward the peer - the last segment it sends there, header-only and
 * flagged - then sent it, then ended its stream. Each wire has its own (the
 * progress helpers proposal's §3.1; close_model.py's two_wires).
 */
enum osoe_goodbye { OSOE_GOODBYE_NONE, OSOE_GOODBYE_SEALED, OSOE_GOODBYE_SENT };

/* An owner's private bookkeeping for one data connection. Writers see only the
 * seal bit in the buffer's head (§3.5); none of this is shared. */
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
     * What this node's writers want is in the connection (ack_wanted_through);
     * `wanted` is it as flush last loaded it, the detector's want beside it, so
     * that busy() can tell a request still to be sealed without a shared load.
     */
    bool owe_ack;
    uint64_t flagged;
    uint64_t wanted;
    /*
     * The deadlock detector's (§3.15): the last segment this node sealed that
     * carried application work - any message but the detector's own - and how
     * far the detector wants the peer to acknowledge. Quiet asks that the peer
     * has applied every such segment; where it has not said so, the detector
     * raises `detector_wanted`, which flush reads beside the writers' want, so
     * a header-only segment asks the peer and its answer says.
     */
    uint64_t last_application_seq;
    uint64_t detector_wanted;
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
    enum osoe_goodbye goodbye;
    bool peer_goodbye;
    /*
     * The abort (osoe_progress_abort, abort_model.py). `abort_done`: this
     * connection has sent its abortive goodbye and ended its stream, or has
     * nobody to tell - its normal goodbye already went, the peer died, or the
     * peer's own abort arrived (`peer_aborted`), naming the rank
     * `peer_abort_origin`. `abort_sent`: bytes of the abortive goodbye the
     * wire has taken.
     */
    bool abort_done;
    bool peer_aborted;
    uint32_t peer_abort_origin;
    uint32_t abort_sent;
    struct osoe_am_parser parser;
    struct osoe_reply_queues queues;
};

/* A control wire's private send buffer: the most one segment of the control
 * plane's messages takes, header included. */
#define OSOE_PROGRESS_CONTROL_BYTES 4096u

/*
 * A node pair's control wire, as owner 0 keeps it (the progress helpers
 * proposal's §3.1): the lead's alone, and nothing with a payload on it - the
 * internode tree's ARRIVE and RELEASE, the deadlock detector's messages, the
 * goodbye and the abortive goodbye. No PE writes a control message, so its
 * send buffer is private and needs no reservation: the queue's messages are
 * sealed into it a segment at a time, one send in flight (§3.3), and the next
 * is sealed once that one has gone whole, so that a burst aggregates behind a
 * send as §3.9's buffers do. Its stream is parsed as a data wire's, under the
 * same segment header, and acknowledged as a data wire's is (§3.4, invariant
 * 22): the tree's two messages are application work their receiver counts in
 * its activity, so the deadlock detector's proof needs a node whose message is
 * not yet applied at its peer to look busy (§3.15). As first built nothing on
 * a control wire was acknowledged, and a barrier's message delayed in the
 * network past the detector's two collections could have a job that would
 * finish declared deadlocked (the adversarial read of Part 2, F1;
 * deadlock_model.py's control_wire). Only the detector asks; an
 * acknowledgement rides whatever segment goes back, and a header-only one
 * answers a request.
 */
struct osoe_progress_control {
    struct osoe_reply_queue queue;  /* the control plane's messages toward the peer */
    uint32_t bytes;                 /* the sealed segment's length; 0 while none is */
    uint32_t sent;                  /* bytes of it the wire has taken */
    uint64_t next_seq;              /* the next segment's sequence */
    /* Acknowledgements, as a data connection's (§3.4, asks_for_ack): the
     * peer's segments applied here, this side's the peer has applied, this
     * side's last that carried application work, what the detector wants
     * acknowledged, the last of this side's that asked, and the peer's ask. */
    uint64_t applied;
    uint64_t acked;
    uint64_t last_application_seq;
    uint64_t detector_wanted;
    uint64_t flagged;
    bool owe_ack;
    enum osoe_goodbye goodbye;
    bool peer_goodbye;
    /* The abort, as a data connection's (osoe_progress_abort). */
    bool abort_done;
    bool peer_aborted;
    uint32_t peer_abort_origin;
    uint32_t abort_sent;
    struct osoe_am_parser parser;
    unsigned char buffer[OSOE_PROGRESS_CONTROL_BYTES];
};

/*
 * One owner's engine (owner-map.h): its ring and the wires it carries, the
 * connections over them, and everything a pass over them writes. Private to
 * the owner's thread, like the rest of this file's state.
 */
struct osoe_progress_owner {
    struct osoe_progress *engine;               /* the node's, whose control plane is owner 0's */
    uint32_t index;                             /* 0, the lead's agent, or helper 1 to H */
    uint32_t count;                             /* the data wires it holds */
    struct osoe_ring *ring;                     /* carries them (ring.h) */
    struct osoe_wire *wire;                     /* `count`, by index (owner-map.h) */
    struct osoe_progress_connection *connection;/* `count`, by the same index */
    /*
     * Owner 0's alone: every peer's control wire, by peer slot - on the same
     * ring, after the data wires (owner-map.h) - their bookkeeping, and a busy
     * bit each, as the data connections' below. None for a helper.
     */
    uint32_t controls;
    struct osoe_wire *control_wire;
    struct osoe_progress_control *control;
    uint64_t *control_busy;
    /*
     * One bit a connection by index: those with send-side work the owner
     * already knows about (busy()), kept current as that work appears and
     * goes, so that a pass finds them in count/64 words instead of asking every
     * connection. Its dirty bitmap, the writers' half, is the area's
     * (connection-area.h).
     */
    uint64_t *busy;
    /* Application messages applied - any but the detector's - cumulative: part
     * of this node's activity for the deadlock detector (§3.15), which with
     * helpers the lead's agent sums over the owners, so each owner publishes
     * its own, a single-writer word (§3.16; deadlock_owners_model.py). */
    _Atomic uint64_t application_applied;
    bool closing;                               /* osoe_progress_close has begun */
    /* The job is ending (osoe_progress_abort): none of its own traffic goes out
     * any more, and every connection ends with the abortive goodbye below - the
     * same bytes for every peer, built once. */
    bool aborting;
    bool abort_keeps_stream;                    /* a global exit's (osoe_progress_abort) */
    unsigned char abort_segment[64];
    uint32_t abort_segment_bytes;
    /* The replies its connections' queues hold (§3.8): the owner's own pool,
     * sized over its connections - and, owner 0's, the control plane's forwards
     * and the detector's messages besides. */
    struct osoe_reply_pool pool;
    /*
     * §3.14 counters, read by osoe-stat and by the tests: owner 0's are the
     * node header's `am` record when running inside a job (progress-host.c
     * binds it), and `own_metrics` until then, so that the loop counts the same
     * way whether or not there is a control segment to count into.
     */
    struct osoe_am_owner_metrics *metrics;
    struct osoe_am_owner_metrics own_metrics;
};

/*
 * The node's engine: what its owners share - read-only once they run - and the
 * control plane, which is owner 0's (the progress helpers proposal's §3.1).
 */
struct osoe_progress {
    struct osoe_connection_area *area;
    uint32_t node, nodes;
    uint32_t helpers;                           /* the area's (owner-map.h) */
    struct osoe_progress_owner *owner;          /* helpers + 1, by owner */
    struct osoe_progress_host host;

    /*
     * The internode tree's state, per team block (§5.3; the team redesign's
     * step D, where it was per team number until then): owner 0's, like the
     * rest of the control plane, and O(k) per team - the topology - plus O(1)
     * for the epoch in progress. An entry is filled the first time the engine
     * meets its team (`team_source`). `team_count` entries: OSOE_TEAM_BLOCKS
     * once the host attaches its arrays (osoe_progress_attach_teams), and until
     * then `own_team`'s, room for the predefined teams, which is all an engine
     * without a job registers.
     *
     * `pending` lists the teams whose release this pass has forwarded and not
     * yet applied (release_pending), so that the pass's end costs what it has
     * to do and not a look at every team number.
     */
    struct osoe_progress_team *team;
    uint32_t team_count;
    uint32_t *pending;
    uint32_t pending_count;
    struct osoe_progress_team own_team[OSOE_PREDEFINED_TEAMS];
    uint32_t own_pending[OSOE_PREDEFINED_TEAMS];
    /* Where a team met for the first time is learnt, and how a message's name
     * for a team is resolved to its block, or NULL where teams are registered
     * by hand (osoe_progress_team): the engine's own tests, where a name's
     * number is its index and its lead local id 0. */
    osoe_progress_team_source *team_source;
    osoe_progress_team_finder *team_find;

    /*
     * §3.14's connection scope, by peer slot, in the control segment where
     * osoe-stat and the tests read it (progress-host.c binds it): a record a
     * connection, each written by the connection's owner alone. Each team's
     * tree state is published into its own block (`published` above). NULL
     * outside a job - the engine's own tests run without a control segment -
     * and every write to them is guarded, because a diagnostic must not decide
     * whether the loop can run.
     */
    struct osoe_am_connection_metrics *connection_metrics;
};

/*
 * Set up the engine of a node of the area's nodes and helpers: `owners` is the
 * caller's array of helpers + 1, which osoe_progress_attach fills an owner at a
 * time.
 */
void osoe_progress_init(struct osoe_progress *p, struct osoe_connection_area *area,
                        struct osoe_progress_owner *owners,
                        const struct osoe_progress_host *host);

/*
 * Bind owner `owner`'s arrays, and put every connection of it into its
 * starting state: its data wires, as many as owner-map.h gives it and in index
 * order, all carried by `ring`; as many connections; its busy bitmap, a bit a
 * wire; and `descriptors`, the pool backing its connections' queues (§3.8) -
 * owner 0's, the control wires' too. Separate from init because the caller
 * owns the arrays: the loop never allocates.
 */
void osoe_progress_attach(struct osoe_progress *p, uint32_t owner, struct osoe_ring *ring,
                          struct osoe_wire *wires, struct osoe_progress_connection *connections,
                          uint64_t *busy, struct osoe_reply *descriptors,
                          uint32_t descriptor_count);

/* Bind the per-team state, `count` entries - one per block of the node's pool
 * of teams (OSOE_TEAM_BLOCKS) - and a list of as many block indices, both
 * zeroed; where each team is learnt from the first time the engine meets it;
 * and how a message's name for a team finds its block. Until this, the engine
 * has room for the predefined teams alone, registered by hand. */
void osoe_progress_attach_teams(struct osoe_progress *p, struct osoe_progress_team *teams,
                                uint32_t *pending, uint32_t count,
                                osoe_progress_team_source *source,
                                osoe_progress_team_finder *find);

/*
 * One pass of owner `o`, on its thread. Returns true if anything moved, which
 * is what tells the idle loop whether to sleep (§3.10): a pass that moved
 * nothing is the only safe moment to wait, and the wait still carries a sweep
 * deadline so that a lost wake costs latency rather than the job. Owner 0's
 * pass runs the control plane's work besides: the releases the tree has to
 * apply on this node.
 */
bool osoe_progress_poll(struct osoe_progress_owner *o);

/*
 * Bind owner 0's control wires, one a peer by peer slot, on its ring after its
 * data wires (owner-map.h), their bookkeeping, and a busy bitmap of
 * (nodes - 1 + 63) / 64 words; and put each into its starting state. After
 * osoe_progress_attach of owner 0.
 */
void osoe_progress_attach_control(struct osoe_progress *p, struct osoe_wire *wires,
                                  struct osoe_progress_control *controls, uint64_t *busy);

/*
 * Meet every team in a block below `count` whose record is published now, as
 * if each had been used: the predefined teams, the pool's first three blocks,
 * whose records are written before the engine's thread starts, so that
 * osoe-stat shows each one's place in the tree from the first pass (§3.14), as
 * it did when they were registered at init - a node none of whose members has
 * reached the first collective still says where it hangs from. O(count) once;
 * a team published later is met at its first use.
 */
void osoe_progress_meet_teams(struct osoe_progress *p, uint32_t count);

/*
 * Does owner `o` owe the network nothing? True when every connection of it has
 * an empty reply queue, nothing sealed, nothing in flight, and no committed
 * bytes waiting in an open buffer - and, owner 0's, no release of the tree's
 * waiting to be applied.
 *
 * It exists for teardown. §5.3's root releases its own node the instant the
 * last node arrives and only *enqueues* the releases that free the others, so
 * a node can leave a collective while messages other nodes are waiting for are
 * still in a buffer that only its engine sends. Stopping the engine there
 * hangs every other node (§6.4).
 */
bool osoe_progress_idle(struct osoe_progress_owner *o);

/*
 * End the job naming any peer whose kernel has acknowledged nothing sent to it
 * for `budget_ms` while data waits for acknowledgement (§3.15, wire.h's
 * osoe_wire_silent_ms). Keepalive probes only an idle connection, so a peer
 * that vanished with data in flight toward it was otherwise found only when
 * the kernel gave up retransmitting, about fifteen minutes by default; with the
 * budget keepalive itself is given, this says what keepalive would have. A
 * peer merely stopped is not found: its kernel acknowledges what arrives, and
 * one whose buffer is full is probed through a zero window with nothing
 * outstanding.
 */
void osoe_progress_check_silent(struct osoe_progress_owner *o, uint32_t budget_ms);

/*
 * Close every connection of owner `o` (§6.4, close_model.py): each one, once
 * it owes the peer nothing, sends its goodbye - a header-only segment flagged
 * OSOE_AM_SEGMENT_GOODBYE, the last it will send - and then ends its stream;
 * the passes that follow keep receiving until every peer's stream has ended
 * too. Called once the engine is idle (osoe_progress_idle). An end of stream
 * that arrives before its goodbye is a peer that died, and ends the job then
 * as at any other time.
 */
void osoe_progress_close(struct osoe_progress_owner *o);

/* Every connection of owner `o` has sent its goodbye and ended its stream, and
 * seen the peer's goodbye and the end of the peer's. Its sockets may then be
 * closed. */
bool osoe_progress_closed(const struct osoe_progress_owner *o);

/*
 * The job is ending (§3.15; abort_model.py). From here owner `o` sends none of
 * the job's own traffic. Each connection of it finishes the segment already on its
 * wire, so the peer's stream stays whole, then sends its abortive goodbye - a
 * segment flagged OSOE_AM_SEGMENT_GOODBYE carrying one OSOE_AM_ABORT - and ends
 * its stream; unless it has nobody to tell: its normal goodbye already went, the
 * peer died, or the peer's own abort arrived. With `keep_stream` - a global exit
 * (abort.h) - the stream is left to the process's end, which ends it: the node
 * whose PE carries the status takes another node's end of stream for that
 * node's PEs having ended, which its lead waits for before its own end, and a
 * stream ended at the goodbye would say so too soon (Part 2's second
 * adversarial read: with three nodes, a node told by a third ended its stream
 * to the carrier's at once, and srun -K1 killed its PEs). O(N) once, what a normal goodbye
 * costs. A connection that fails from here is a peer that is ending too, and is
 * not a fatal; nor is a peer's end of stream without its goodbye.
 */
void osoe_progress_abort(struct osoe_progress_owner *o, uint32_t pe, int32_t status,
                         uint32_t disposition, bool keep_stream);

/* Every connection of owner `o` has had its abortive goodbye acknowledged by
 * the peer's kernel, or has nobody to tell. O(N) calls to the kernel. */
bool osoe_progress_aborted(const struct osoe_progress_owner *o);

/* The stream of every peer of owner `o`'s has ended, by its end of stream or
 * an error, but a peer whose own abort named a rank other than `carrier`: a
 * node carrying its own caller of shmem_global_exit (abort.h). O(N). */
bool osoe_progress_peers_ended(const struct osoe_progress_owner *o, uint32_t carrier);

/* The peer-slot index for a peer node, or OSOE_CONNECTION_NONE for this node. */
uint32_t osoe_progress_slot(const struct osoe_progress *p, uint32_t peer);

/*
 * The deadlock detector's view (§3.15, transport.h's node_quiet), of owner `o`'s
 * connections: idle, and every connection's peer has applied the last segment
 * that carried application work; where one has not said so, ask it.
 * `applied`: the application messages it has applied, cumulative.
 */
bool osoe_progress_quiet(struct osoe_progress_owner *o, uint64_t *applied);

/* Queue a deadlock detector message toward `node`'s lead, on owner 0's control
 * wire to it: the control plane's (the progress helpers proposal's §3.1). */
void osoe_progress_deadlock_send(struct osoe_progress *p, uint32_t node,
                                 const struct osoe_deadlock_message *m);

/* The highest sequence number `peer` has applied (§4.6's completion evidence). */
uint64_t osoe_progress_acked(const struct osoe_progress *p, uint32_t peer);

/*
 * Register a team's internode tree by hand (collective-tree.h), for an engine
 * whose host has no records to learn it from: the engine's own tests. In a job
 * the engine learns each team from its host's source the first time it meets
 * it (osoe_progress_attach_teams). Called before the engine's thread starts;
 * the engine keeps the tree, O(k), and never the node set, O(N).
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
 * root, starts the release. `generation` is the one the handoff carried, the
 * block's while its poster held it (node.h): the engine learns the block
 * afresh if it is not the one it learnt.
 */
void osoe_progress_arrive(struct osoe_progress *p, uint32_t team, uint32_t generation,
                          uint32_t epoch);

#endif /* OSOE_PROGRESS_H */
