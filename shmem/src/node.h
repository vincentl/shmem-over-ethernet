/*
 * The node control segment and node-local rendezvous (§3.14, §6.2).
 *
 * One named shared-memory object per node per session,
 * /osoe.<job>.<step>.<node>.<session>, mode 0600, created by the local lead.
 * It holds a versioned header, one slot per local PE, and the world team's
 * epoch counters. Each PE's heap is its own named object,
 * /osoe.<job>.<step>.<node>.<session>.heap.<local id>, whose name the lead
 * removes once every local PE has mapped it. The control segment keeps its
 * name until the last finalize, so it can be read while the job runs.
 *
 * Writers: the lead writes the header before publishing `ready`; each PE writes
 * only its own slot, publishing each step through `phase`. Everything a reader
 * needs is published with a release store (§3.16) before a counter bump that
 * wakes it.
 */
#ifndef OSOE_NODE_H
#define OSOE_NODE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/cacheline.h"
#include "base/counters.h"
#include "base/epoch.h"
#include "base/wait-word.h"
#include "bootstrap.h"

#define OSOE_NODE_MAGIC  UINT64_C(0x65646f6e656f736f)  /* "osoenode", little-endian */
#define OSOE_NODE_LAYOUT 16

enum osoe_phase {
    OSOE_PHASE_EMPTY = 0,       /* slot unused */
    OSOE_PHASE_JOINED,          /* slot fields written; own heap mapped at B */
    OSOE_PHASE_MAPPED,          /* every sibling heap mapped */
    OSOE_PHASE_FINALIZED,       /* last finalize has run */
};

enum { OSOE_HEAP_NAME_MAX = 96 };

/*
 * What a blocked record's `blocked_detail` is (§3.14), so that osoe-stat can
 * print "node 3" or "epoch 17" rather than "+0x3" (second review, D-7). The
 * values are part of the segment's encoding: changing one is a layout bump.
 */
enum osoe_blocked_detail {
    OSOE_BLOCKED_DETAIL_NONE = 0,    /* nothing to add, or a reader must show it raw */
    OSOE_BLOCKED_DETAIL_NODE,        /* a peer node, for an internode wait */
    OSOE_BLOCKED_DETAIL_EPOCH,       /* a collective epoch */
    OSOE_BLOCKED_DETAIL_OFFSET,      /* a symmetric-heap offset, for a point-to-point wait */
    OSOE_BLOCKED_DETAIL_PE,          /* a PE */
    /*
     * Layout 14: an internode wait says which of the connection's words it
     * sleeps on (§3.11), the detail being the peer node. A reader counts a
     * connection's waiters per word from the records it already reads - §3.14's
     * "waiters per wake word" - and the job pays nothing for it: the record is
     * written only when a wait is about to sleep, as before.
     */
    OSOE_BLOCKED_DETAIL_ACKNOWLEDGEMENT,  /* quiet: the peer's acknowledged sequence */
    OSOE_BLOCKED_DETAIL_TURNOVER,         /* a writer with no room: the buffer turnover */
    OSOE_BLOCKED_DETAIL_CREDIT,           /* a get or fetching AMO: a reply credit */
    OSOE_BLOCKED_DETAIL_REPLY,            /* the reply itself, on the PE's landing slot */
    /* Layout 16: a wait for the PE's own nonblocking gets, the detail being
     * the bytes of them still in flight when it began - not a connection's
     * word, so not counted among them. */
    OSOE_BLOCKED_DETAIL_GETS_IN_FLIGHT,
};

/*
 * How the job is ending (§3.15), as the abort word says it. The values are
 * part of the segment's encoding: layout 15 gave the word its dispositions,
 * where it had been 0 or 1.
 */
enum osoe_abort_disposition {
    OSOE_ABORT_NONE = 0,
    OSOE_ABORT_FATAL,           /* a fatal, or a death: every PE leaves with a failure */
    OSOE_ABORT_GLOBAL_EXIT,     /* shmem_global_exit: every PE exits with its status */
};

/*
 * The abort word's fields (layout 15): the world rank in the low 32 bits, then
 * the status as the environment will see it - exit keeps only its low eight
 * bits, and so does the word - then the disposition. One CAS from zero
 * installs all three, so the first declarer wins them together and no reader
 * ever sees a rank without its status.
 */
#define OSOE_ABORT_STATUS_SHIFT      32
#define OSOE_ABORT_DISPOSITION_SHIFT 40

/*
 * The predefined teams, numbered (§7.3, spec §9.4).
 *
 * Both the node header and the progress loop index per-team state by this, so
 * it is one enumeration rather than two agreeing constants. shmem_team_split_*
 * is not implemented, so these are all the teams there are; a split team
 * spanning several nodes is what grows this, and it grows the arrays with it.
 */
enum osoe_team_number {
    OSOE_TEAM_WORLD = 0,
    OSOE_TEAM_SHARED = 1,
    /*
     * The library's own team for shmem_finalize: the world's members, its own
     * epoch counters. A finalize must never complete together with an
     * application's shmem_barrier_all, which a shared world epoch made
     * possible - a PE leaving through start_pes's implicit finalize matched the
     * others' barrier and let them past a collective the application never
     * matched (§6.4; second review, D14). Not a team an application can name.
     */
    OSOE_TEAM_FINALIZE = 2,
    OSOE_TEAMS,
};

/* The routine name in a "blocked in" record (§3.14). Fixed width: it is read
 * from another process, so it cannot be a pointer. */
enum { OSOE_BLOCKED_ROUTINE_MAX = 32 };

struct osoe_pe_slot {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t phase;         /* enum osoe_phase; this PE only */
    _Atomic uint32_t agent_wake;    /* shared futex word: wakes this PE's agent */
    /* The §4.9 watch slot: the range this PE waits on, packed by watch_range.h,
     * and its futex word. Written by this PE, read by every writer that touches
     * its memory. On their own cache line, so a notifying writer does not share
     * one with the words this PE's own agent and rendezvous use. */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t watch_rng;
    _Atomic uint32_t watch_seq;
    uint32_t rank;
    int32_t pid;                    /* this PE's process id, for liveness (§3.15) */
    int32_t tid;                    /* the initializing thread, for /proc/<pid>/task/<tid> */
    uint64_t symmetric_size;        /* SHMEM_SYMMETRIC_SIZE as this PE parsed it */
    uint64_t heap_bytes;            /* mapped length of the heap object */
    char heap_name[OSOE_HEAP_NAME_MAX];

    /*
     * The §3.14 "blocked in" record, so a hung job says where each PE is
     * waiting. Written by this PE only when a wait is about to sleep - a wait
     * that returns at once touches none of it - and read by osoe-stat from
     * another process while this PE may be publishing its next wait.
     *
     * `blocked_gen` is odd while the fields are in flux: the writer bumps it,
     * writes, and bumps again, both bumps seq_cst; a reader loads it, reads the
     * fields, and loads it again, accepting only an unchanged even value. Two
     * locked read-modify-writes rather than a textbook seqlock, because that
     * needs relaxed stores and a release fence and §3.16 permits neither.
     * Model-checked in docs/design/evidence/blocked-record/.
     */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t blocked_gen;
    _Atomic uint64_t blocked_detail;     /* routine-specific: a PE, an epoch, an offset */
    _Atomic uint64_t blocked_since_ns;   /* CLOCK_MONOTONIC when the wait began */
    char blocked_routine[OSOE_BLOCKED_ROUTINE_MAX];
    _Atomic uint32_t blocked_detail_kind;   /* enum osoe_blocked_detail; layout 9 */

    /* Liveness and progress, published by this PE (§3.14 question 1). */
    _Atomic uint64_t sweeps;             /* safety-sweep expiries: a blocked PE still moves */
    _Atomic uint32_t world_epoch;        /* this PE's k for SHMEM_TEAM_WORLD (§5.4) */
    _Atomic uint32_t shared_epoch;       /* and for SHMEM_TEAM_SHARED */
    _Atomic uint32_t finalize_epoch;     /* and for the finalize team (layout 10) */

    /*
     * Threads of this PE that found no free counter block (§3.14). It belongs
     * here and not in a counter block, which is where it started: a thread that
     * fails to claim a block writes to a process-private one that nobody can
     * read, so the one counter whose job is to say "the metrics are
     * under-reporting" was itself unreadable. It is written by whichever thread
     * ran out, so it is a seq_cst read-modify-write rather than this PE's
     * single-writer publication - on a path taken once per thread, at most.
     */
    _Atomic uint32_t blocks_exhausted;

    /* Heap levels. Per PE, not per thread: a gauge cannot be summed. */
    _Atomic uint64_t heap_in_use;
    _Atomic uint64_t heap_high_water;
    _Atomic uint64_t heap_largest_free;
};

/*
 * The frozen prefix of the control segment (§3.14).
 *
 * This is the one part of the segment a reader may depend on by layout, and it
 * exists so that nothing else has to be: it says where the three arrays are and
 * how big their elements are, and the metric table then describes every field
 * of every one of them by name. A tool built from a different commit reads a
 * job correctly because it reads this, then the table, and then everything else
 * by name.
 *
 * So: **fields are never reordered, resized or removed here, only appended**,
 * and `directory_bytes` says how much of it the writer filled in. Changing
 * anything above that line is what `layout` is for, and a reader that finds a
 * layout it does not know can still print the directory and say so.
 */
struct osoe_segment_directory {
    uint64_t magic;
    uint32_t layout;
    uint32_t directory_bytes;        /* how much of this structure is valid */
    uint32_t header_bytes;
    uint32_t reserved;
    uint64_t segment_bytes;

    uint64_t slots_offset;
    uint32_t slot_bytes;
    uint32_t local_count;

    /* metrics_count == 0 means "no table": a reader shows nothing rather than
     * guessing at a layout. */
    uint64_t metrics_offset;
    uint64_t metrics_bytes;
    uint32_t metrics_count;
    uint32_t metrics_checksum;       /* FNV-1a over the table's bytes */
    uint32_t metric_descriptor_bytes;

    uint64_t blocks_offset;
    uint64_t blocks_bytes;
    uint32_t block_count;            /* local_count * blocks_per_pe */
    uint32_t block_bytes;
    uint32_t blocks_per_pe;          /* OSOE_MAX_THREADS */

    /* Appended at layout 6: the §3.12 landing zones, one slot per local PE. */
    uint64_t landing_offset;
    uint64_t landing_bytes;
    uint32_t landing_slot_bytes;     /* a PE's zone: header plus one AM chunk, and since
                                      * layout 16 its nonblocking gets' records and arena */
    uint32_t landing_count;          /* local_count */

    /*
     * Appended at layout 12: one record per connection this node has toward a
     * peer node, in peer-slot order (§3.4). The fourth metric scope
     * (OSOE_MSCOPE_CONNECTION), which §3.14's catalogue needs to answer "which
     * peer is this node's traffic with" without a per-PE dimension.
     * connection_count is nodes - 1, and zero on a job of one node.
     */
    uint64_t connections_offset;
    uint64_t connections_bytes;
    uint32_t connection_bytes;       /* one record */
    uint32_t connection_count;       /* nodes - 1 */
};

/*
 * The internode arrival handoff, one per predefined team (§5.3, §3.12).
 *
 * Whichever local member's increment reaches the team's arrival target leaves
 * the epoch here and bumps the local lead's `agent_wake`; that agent consumes
 * it and performs the node's arrival. It cannot perform it itself: the progress
 * state the arrival moves - the root's arrival counts, the participating-node
 * count, the per-connection queues - is private to the lead's agent thread, in
 * the lead's process, and the arriver is an application thread of any local PE,
 * usually another process. This is §3.12's "a flag plus a futex wake", and it
 * is the whole of what crosses.
 *
 * **One word is enough, and the reason is §5.4's bounded skew.** Epoch k+2's
 * arrival cannot be posted until every local member has been released from
 * k+1, and that release cannot happen until the lead has already consumed and
 * forwarded k+1 - so no post can overwrite one the agent has not read, and this
 * needs no queue, no sequence number and no second slot. Model-checked in
 * docs/design/evidence/am-backend/arrival_model.py, where the `$posted` and
 * `$consumed` counters are that argument rather than an assertion of it.
 *
 * Two rules keep it true, and the model breaks each one. The agent loads
 * `agent_wake` **before** this word (§4.9), or a post landing between the two
 * loads leaves it asleep on a value that already counts the bump. And it
 * records the epoch consumed **from the value it forwarded**, never by
 * re-reading this word afterwards: on the root node the forward can release the
 * team locally at once (§5.3), so a member can re-enter and post the next epoch
 * here before a re-read would happen, and that arrival is then lost - a hang on
 * every other node, which is the worst place to debug it.
 *
 * A team with no internode phase never touches its entry. SHMEM_TEAM_SHARED is
 * one node by definition, so today only the world team's is ever written; it is
 * per team anyway, because a split team (§7.3) spanning several nodes is the
 * next thing to arrive and sharing one word between two teams would make a sync
 * on one forward the arrival of the other.
 */
struct osoe_arrival_handoff {
    /*
     * The posted epoch, k+1, or 0 before the first. A single-writer publication
     * store (§3.16): one member posts at a time, and the release of the
     * previous epoch orders it after the last one.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t epoch;
};

_Static_assert(sizeof(struct osoe_arrival_handoff) == OSOE_CACHELINE,
               "each team's handoff word sits alone on its cache line, as its "
               "epoch counters do (§5.3)");

/*
 * The AM layer's node-wide counters (§3.14), exported under `transport.am.*`.
 *
 * Everything they count - segments, the reply pool and queues - is private to
 * the lead's progress thread (§3.12), and until layout 8 so were the counts: a
 * test could not see the acknowledgement protocol echo, or the reply pool run
 * dry, except by the job misbehaving. This is the smallest export that lets a
 * test see both. Node scope only: one record per node, written by that one
 * thread, so every field is a single-writer publication (§3.16) and a reader
 * accepts a value a moment stale. A per-connection scope is the next step.
 *
 * Zero on a node whose job never goes internode, and in any build whose
 * backend is not AM - the fields are a backend's private counters, which §3.14
 * gives the `transport.<backend>.*` names so they cannot be mistaken for the
 * boundary's own.
 */
struct osoe_am_node_metrics {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t segments_sent;
    _Atomic uint64_t header_only_sent;        /* §3.4's ack-only segments, among those */
    _Atomic uint64_t segments_applied;
    _Atomic uint64_t messages_applied;
    /* §3.8's fallible path, taken: a request found the descriptor pool empty. */
    _Atomic uint64_t reply_reservation_failures;
    /* The deepest any one connection's queue has been, and the most
     * descriptors the node-wide pool has had out at once. */
    _Atomic uint64_t control_queue_high_water;
    _Atomic uint64_t bulk_queue_high_water;
    _Atomic uint64_t pool_high_water;
    /* Work discovery (layout 11): engine passes, and the connections a pass
     * drained and flushed. Their ratio is the send side's cost per pass, which
     * must follow the active connections and not the job's size (§3.10). */
    _Atomic uint64_t passes;
    _Atomic uint64_t send_visits;
    /*
     * The ring (layout 13; §3.3, ring.h): completions reaped, receive buffers
     * filled, multishot receives ended by -ENOBUFS and re-armed after any end,
     * sends submitted and the ones the kernel took only part of. -ENOBUFS and
     * short sends are normal under load; a rate far above the traffic's is a
     * receive-buffer ring too small for the peers sending at once (§3.3).
     */
    _Atomic uint64_t completions;
    _Atomic uint64_t receive_buffers;
    _Atomic uint64_t receive_enobufs;
    _Atomic uint64_t receive_rearms;
    _Atomic uint64_t sends;
    _Atomic uint64_t short_sends;
};

/*
 * One connection's counters (§3.14's fourth scope; phase 1 step 2 slice 3).
 *
 * Node scope answers "how much has this node sent" and PE scope "what is this
 * PE doing"; neither answers "which peer", which is the first question about a
 * job whose traffic is uneven or whose one slow link is the problem. Written
 * only by the lead's progress thread, which is this data's single writer
 * (§3.12), so every field is a single-writer publication (§3.16); a reader
 * takes a value a moment stale. Zero on a node whose job never goes internode,
 * and in any build whose backend is not AM.
 *
 * The gauges - queue depths, acknowledged and wanted sequences - are refreshed
 * when the lead visits the connection, which since slice 3 is only when it has
 * work (§3.10), so an idle connection's gauges are as stale as the connection
 * is idle. That is the right trade for a diagnostic: what a hung job needs is
 * the last state of a connection that stopped moving.
 */
struct osoe_am_connection_metrics {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t peer;                  /* the peer node this is toward */
    _Atomic uint32_t control_queue_depth;   /* replies queued now */
    _Atomic uint32_t bulk_queue_depth;
    _Atomic uint32_t control_queue_high_water;
    _Atomic uint32_t bulk_queue_high_water;
    _Atomic uint64_t segments_sent;
    _Atomic uint64_t bytes_sent;
    _Atomic uint64_t header_only_sent;      /* §3.4's acknowledgement-only segments */
    _Atomic uint64_t segments_applied;      /* from this peer */
    _Atomic uint64_t bytes_applied;
    _Atomic uint64_t messages_applied;
    _Atomic uint64_t acked_through;         /* the peer has applied through this */
    _Atomic uint64_t wanted_through;        /* this node's writers want this acknowledged */
    _Atomic uint64_t sent_through;          /* the highest sequence this node has sent */
};

/*
 * A team's place in the internode tree and the epoch it is in (§5.3), for the
 * one question a hung collective asks: which node has not arrived?
 *
 * With the tree that is answered by walking *down* from the root in O(log N)
 * visits - each node names the lowest child index that has not arrived, and
 * `hostname` below says where to look next - rather than by visiting every
 * node. Written by the lead's progress thread as the state changes, a handful
 * of single-writer stores per barrier (§3.16).
 */
struct osoe_collective_state {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t epoch_open;         /* the epoch in progress, or 0 */
    _Atomic uint32_t epoch_closed;       /* the last epoch this node closed */
    _Atomic uint32_t own_arrived;        /* this node's own members are in */
    _Atomic uint32_t children;           /* how many children this node has */
    _Atomic uint32_t children_arrived;   /* how many of them have arrived */
    _Atomic uint32_t lowest_missing;     /* the lowest child node not arrived, or
                                          * UINT32_MAX when none is missing */
    _Atomic uint32_t parent;             /* this node's parent, or UINT32_MAX at the root */
    _Atomic uint32_t fanout;
};

struct osoe_node_header {
    /* Written by the lead before `ready`; constant afterwards. */
    struct osoe_segment_directory dir;

    uint64_t job;
    uint32_t step;
    uint32_t node;
    uint32_t session;
    uint32_t world_size;
    uint64_t heap_base;             /* B, where every PE maps its own heap (§4.1) */
    int32_t creator_pid;            /* the lead, for reclaiming a leftover (shm-object.h) */

    /* Rendezvous words, each a shared futex. */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t ready;         /* lead: header complete */
    _Atomic uint32_t joined;        /* PEs in OSOE_PHASE_JOINED or later */
    _Atomic uint32_t mapped;        /* PEs in OSOE_PHASE_MAPPED or later */
    _Atomic uint32_t finalized;     /* PEs whose last finalize has run */
    _Atomic uint32_t failed;        /* nonzero once any PE has failed rendezvous */
    _Atomic uint32_t lead_agent_running;    /* lead: its agent runs, so the node can speak */

    /* The abort word (§3.15). Nonzero means the job is over and no wait can
     * finish; every library wait reads it at its next safety-sweep deadline
     * (base/sweep.h), and every agent at each turn of its loop (agent.c), so a
     * PE ends whatever it is doing. It holds how the job ends, the status and
     * the rank that ended it (OSOE_ABORT_*_SHIFT, layout 15); the first
     * declarer wins it (osoe_node_declare_abort). Not a futex word: nothing
     * sleeps on it, the declarer wakes each agent on the agent's own. */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t aborting;

    /*
     * Each predefined team's epoch counters (§5.4), each on its own line.
     * SHMEM_TEAM_SHARED has its own set although on one node its membership is
     * the world's: the spec makes them distinct predefined teams forming
     * separate atomicity domains, and sharing the counters would make a sync on
     * one team release a wait on the other.
     */
    struct osoe_epoch world_arrived;
    struct osoe_epoch world_released;
    struct osoe_epoch world_copied;
    struct osoe_epoch shared_arrived;
    struct osoe_epoch shared_released;
    struct osoe_epoch shared_copied;
    /* The finalize team's (layout 10). It has no copy phase. */
    struct osoe_epoch finalize_arrived;
    struct osoe_epoch finalize_released;

    /* Appended at layout 7 (§5.3). */
    struct osoe_arrival_handoff arrival[OSOE_TEAMS];

    /* Appended at layout 8; layout 11 appended passes and send_visits to it,
     * and layout 13 the ring's six counters, which moved nothing before it
     * (what follows moved, and is found by name). Layout 10 added the finalize team's counters above
     * and grew `arrival` by one, which moved this record; readers find it by
     * name. Layout 9 added blocked_detail_kind to the PE slot,
     * which moved no header field. Layout 8 also gave each epoch counter's line a
     * second word, released_at_ns (base/epoch.h), in padding it already owned:
     * no offset moved, but the line's contents changed meaning. */
    struct osoe_am_node_metrics am;

    /*
     * Appended at layout 12. `hostname` is what makes a walk down the tree
     * actionable: the node numbers a parent names are logical, and this says
     * which machine to run osoe-stat on next (§3.14's diagnosability walk).
     * Written by the lead before `ready`, constant afterwards.
     */
    char hostname[64];
    struct osoe_collective_state collective[OSOE_TEAMS];

    /*
     * Appended at layout 16: each PE's landing zone also holds its nonblocking
     * gets in flight (§4.6, as built). A zone is `dir.landing_slot_bytes`: the
     * blocking slot, whose data area is `landing_data_bytes` - one AM chunk -
     * then `landing_requests` records at `landing_requests_offset`, then an
     * arena of `landing_arena_bytes` at `landing_arena_offset` for their
     * replies. Offsets are within a zone.
     */
    uint32_t landing_data_bytes;
    uint32_t landing_requests;
    uint32_t landing_requests_offset;
    uint32_t landing_arena_offset;
    uint32_t landing_arena_bytes;
};

/*
 * A PE's landing zone (§3.12, §4.6): this slot, for its blocking gets and
 * fetching AMOs, then - since layout 16 - the records and the arena of its
 * nonblocking gets in flight (struct osoe_landing_request).
 *
 * A get's `dest` may be a stack or malloc buffer that the lead's process cannot
 * see, so a reply cannot be written where the caller asked for it. It lands
 * here, in the node segment, and the *origin* copies it out - the calling
 * thread for a blocking get or fetching AMO, that PE's agent or the application
 * thread at quiet for a nonblocking one. This is the correction to the earlier
 * claim that the origin never needs a helper: it never needs one for progress,
 * but it does need its private memory written by a thread inside it.
 *
 * It belongs in the control segment rather than the AM connection area because
 * it is needed by *every* backend: the reason it exists is the address space
 * boundary, not the network. An RDMA backend writing a reply straight into the
 * origin's memory still cannot write a private `dest`.
 *
 * One slot is enough for the blocking operations: under SERIALIZED a PE has at
 * most one outstanding (§4.6), so it is sized by L alone and not by node count.
 * Its nonblocking gets complete by the counters below, §4.6's shape for them:
 * the PE adds each one's bytes to `bytes_issued` as it asks, the lead adds
 * them to `bytes_landed` as the whole reply lands, and a quiet waits for the
 * two to be equal and then copies every reply out.
 */
struct osoe_landing_slot {
    /*
     * §4.6's get completion: two cumulative counters, never reset, with the
     * companion 32-bit wake word they need because a 64-bit counter cannot be
     * futexed (§4.9). The progress thread adds landed bytes, bumps the wake
     * word, then wakes; the waiter loads the wake word, then compares the
     * counters, then waits on the wake word.
     */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t bytes_issued;      /* this PE adds expected bytes on issue */
    _Atomic uint64_t bytes_landed;      /* the progress thread adds as replies land */
    /* Bumped after `bytes_landed` moves, and a wake only while this PE waits
     * on it (base/wait-word.h): a burst of small replies costs the lead no
     * syscall unless a quiet is waiting for them. */
    struct osoe_wait_word counter_wake;

    /*
     * Per-operation completion, which the counters cannot serve: a blocking
     * caller waits for *its* reply, not for a byte count (§4.6).
     */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t wake;
    _Atomic uint32_t ready;             /* 0, or 1 once `data` and `value` are complete */
    _Atomic uint32_t length;            /* bytes in `data` */
    _Atomic uint64_t value;             /* a fetching AMO's result */
    /*
     * Which operation this reply belongs to. Under SERIALIZED a PE has one
     * outstanding, so the slot could do without it - but a reply for an
     * operation that has already been abandoned is then indistinguishable from
     * the one being waited for, and §4.6 says the caller waits for *its* reply.
     * Recording it makes "its" checkable rather than assumed.
     */
    _Atomic uint64_t token;
    /*
     * The token of the operation this slot's owner is waiting for, stored by
     * the owner before its request is sent (layout 9). The lead refuses a reply
     * carrying any other before writing a byte of it - an unsolicited or stale
     * reply is a peer's bug, and landing it would corrupt this zone or, past
     * its end, a sibling's (§3.4's trust boundary; second review, D4).
     */
    _Atomic uint64_t armed_token;

    /* The reply bytes. One AM chunk is enough: a larger get is chunked on the
     * wire (§3.2) and the origin copies each chunk out before the next lands. */
    _Alignas(OSOE_CACHELINE)
    unsigned char data[];
};

/*
 * The top bit of a token names a nonblocking get's reply; its low 16 bits are
 * the index of its record among the PE's landing requests. A blocking
 * operation's token never has the bit (transport-am.c counts them from 1).
 */
#define OSOE_LANDING_NONBLOCKING (UINT64_C(1) << 63)
#define OSOE_LANDING_REQUEST_MASK UINT64_C(0xffff)

/*
 * A nonblocking get in flight (§4.6, as built): where in its PE's landing arena
 * the reply lands, and how long it is. The PE writes `offset` and `length`, then
 * `token` with release, before its request goes out - the send's reservation
 * and commit order it before any reply can exist - and clears `token` once the
 * reply is copied out. The lead finds the record by the index in the reply's
 * token and refuses a reply whose token is not the record's, or whose bytes
 * would fall outside the record's range (§3.4's trust boundary, as for the
 * blocking slot's `armed_token`). Atomics throughout, because a stale or bogus
 * reply may read a record while its owner rewrites it; relaxed for the two
 * numbers, which the token's acquire orders when it matches.
 */
struct osoe_landing_request {
    _Atomic uint64_t token;
    _Atomic uint32_t offset;            /* into the PE's landing arena */
    _Atomic uint32_t length;            /* bytes its reply carries */
};

/* Process-private view of the node. */
struct osoe_node {
    char control_name[80];
    /*
     * This PE's descriptor for the control segment, held for the whole session
     * (§3.14). The name is removed during init, and this is what keeps the
     * object findable afterwards: osoe-stat opens /proc/<pid>/fd/<n> of any
     * live PE of the node. One descriptor per PE; -1 outside a session.
     */
    int control_fd;
    bool keep_name;                 /* OSOE_KEEP_CONTROL_SEGMENT */
    struct osoe_node_header *hdr;
    struct osoe_pe_slot *slots;
    struct osoe_counter_block *blocks;   /* this node's whole pool (§3.14) */
    uint64_t control_bytes;
    void **heap_of;                 /* base_of[local id][OSOE_REGION_HEAP] */
    /*
     * "Is world rank r on this node, and which local id is it?" in O(L)
     * memory (osoe_node_local_id). Until slice 3 this was a world-sized array
     * in every PE: 4 x P bytes per PE, 1 GiB per node at 4,096 x 256 (second
     * review, D7). When the node's ranks are one contiguous block, in local-id
     * order - Slurm's block distribution, and every map in the suite but the
     * interleaved ones - it is a subtraction; otherwise a binary search over
     * the node's ranks sorted, O(log L).
     */
    uint32_t local_count;
    bool local_block;               /* gtids[i] == first_rank + i for every i */
    uint32_t first_rank;
    uint32_t *sorted_rank;          /* the L ranks ascending ... */
    uint32_t *sorted_local;         /* ... and each one's local id */
    uint64_t heap_bytes;            /* mapped: the heap, then the internal region */
    uint64_t heap_size;             /* the agreed SHMEM_SYMMETRIC_SIZE */
    /* OSOE_REGION_INTERNAL (internal-region.h): where in each heap mapping it
     * starts - the page-rounded heap size - and how long it is. */
    uint64_t internal_offset;
    uint64_t internal_bytes;
    unsigned char *landing;         /* the landing zones; index with osoe_node_landing */
    uint32_t landing_slot_bytes;    /* a PE's whole zone, since layout 16 */
    uint32_t landing_data_bytes;    /* the blocking slot's data: one AM chunk */
    uint32_t landing_requests;      /* nonblocking gets a PE may have in flight */
    uint32_t landing_requests_offset;
    uint32_t landing_arena_offset;
    uint32_t landing_arena_bytes;
    /* One record per peer connection, in peer-slot order, or NULL on a job of
     * one node (§3.14's connection scope). */
    struct osoe_am_connection_metrics *connection_metrics;
    uint32_t connection_count;
};

/* This node's landing slot for a local PE: the start of its zone. The zone is
 * not a fixed-size type - its payloads are sized at run time - so it is indexed
 * by bytes, never by array. */
static inline struct osoe_landing_slot *osoe_node_landing(const struct osoe_node *n,
                                                          uint32_t local_id)
{
    return (struct osoe_landing_slot *)(n->landing +
                                        (size_t)local_id * n->landing_slot_bytes);
}

/* A local PE's nonblocking gets in flight: its records, and their arena. */
static inline struct osoe_landing_request *osoe_node_landing_requests(const struct osoe_node *n,
                                                                      uint32_t local_id)
{
    return (struct osoe_landing_request *)((unsigned char *)osoe_node_landing(n, local_id) +
                                           n->landing_requests_offset);
}

static inline unsigned char *osoe_node_landing_arena(const struct osoe_node *n,
                                                     uint32_t local_id)
{
    return (unsigned char *)osoe_node_landing(n, local_id) + n->landing_arena_offset;
}

/* The non-block case of osoe_node_local_id. */
int32_t osoe_node_local_search(const struct osoe_node *n, uint32_t rank);

/*
 * The local id of world rank `rank` on this node, or -1 if it is on another.
 * On the put path (transport-am.c's osoe_transport_direct, and notify), so the
 * common case is inline and branch-predictable.
 */
static inline int32_t osoe_node_local_id(const struct osoe_node *n, uint32_t rank)
{
    if (n->local_block) {
        uint32_t i = rank - n->first_rank;      /* wraps for rank < first_rank */
        return i < n->local_count ? (int32_t)i : -1;
    }
    return osoe_node_local_search(n, rank);
}

/* Rendezvous with the other local PEs; map every heap. Fatal on failure. */
void osoe_node_join(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session);

/* Unmap everything; the PE whose finalize is the node's last removes the
 * control segment's name. */
void osoe_node_leave(struct osoe_node *n, const struct osoe_bootstrap *b);

/* Declare the job over: publish the abort word, and wake every local agent,
 * so every other local PE ends - at its agent's next turn, or at its next
 * sweep if it is waiting (abort.h). Does not itself terminate the caller. The
 * first declarer wins and true says it was this one; a later one changes
 * nothing, since the rank, the status and the disposition must stay the
 * first's. */
bool osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status,
                             enum osoe_abort_disposition how);

/* Wake every local agent, the lead's first, so each finds the abort word at
 * once rather than at its next sweep. O(L). */
void osoe_node_wake_agents(struct osoe_node *n);

/* The abort word's disposition, with the rank and status it names when it is
 * not OSOE_ABORT_NONE. One load. */
enum osoe_abort_disposition osoe_node_abort_read(const struct osoe_node *n, uint32_t *pe,
                                                 int *status);

#endif /* OSOE_NODE_H */
