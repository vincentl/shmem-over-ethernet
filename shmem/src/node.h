/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node control segment and node-local rendezvous (§3.14, §6.2).
 *
 * One named shared-memory object per node per session,
 * /osoe.<job>.<step>.<node>.<session>, mode 0600, created by the local lead.
 * It holds a versioned header ending in one block per team number, one slot
 * per local PE, and each PE's published epochs. Each PE's heap is its own
 * named object, /osoe.<job>.<step>.<node>.<session>.heap.<local id>, whose
 * name the lead removes once every local PE has mapped it. The control
 * segment keeps its name until the last finalize, so it can be read while the
 * job runs.
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
#include "collective-tree.h"

#define OSOE_NODE_MAGIC  UINT64_C(0x65646f6e656f736f)  /* "osoenode", little-endian */
#define OSOE_NODE_LAYOUT 30

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
    /* Layout 17: a point-to-point wait on the program's statics (§4.3), the
     * detail being the offset into them, as OSOE_BLOCKED_DETAIL_OFFSET's is
     * into the heap. */
    OSOE_BLOCKED_DETAIL_STATICS_OFFSET,
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
 * A team's number indexes a PE's state for the team - its published epoch in
 * the PE's row, its block of the PE's internal region, its descriptor - and,
 * with the local id of the team's local lead, names the team's block in the
 * node's pool (struct osoe_team_name). Since the team redesign's step D a
 * number is each PE's own: no PE is in two live teams under one number, and two
 * teams with no PE in common may hold one. The predefined teams take the first
 * three, everywhere; teams created by shmem_team_split_* take 3 to
 * OSOE_MAX_TEAMS - 1, agreed by the split among the new team's members and
 * given back at its destroy (§5.7, team.c).
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
    OSOE_PREDEFINED_TEAMS,
};

/* The routine name in a "blocked in" record (§3.14). Fixed width: it is read
 * from another process, so it cannot be a pointer. */
enum { OSOE_BLOCKED_ROUTINE_MAX = 32 };

/* The most live threads of a PE that may have called the library
 * (OSOE_MAX_THREADS: a thread keeps its place until it exits), and so the
 * words of its `active` bitmap. */
enum { OSOE_WATCH_ACTIVE_WORDS = 16, OSOE_MAX_THREADS_HIGHEST = OSOE_WATCH_ACTIVE_WORDS * 64 };

/* The CPUs a PE's task may run on, a bit each, as many as glibc's cpu_set_t
 * holds (CPU_SETSIZE, 1024). */
enum { OSOE_TASK_CPU_WORDS = 16 };

struct osoe_pe_slot {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t phase;         /* enum osoe_phase; this PE only */
    _Atomic uint32_t agent_wake;    /* shared futex word: wakes this PE's agent */
    /* §4.11 item 3's `active` bitmap (layout 23; phase 1 step 8 slice 2): a
     * bit per thread record of this PE (struct osoe_thread_record) holding a
     * published watch. Set and cleared by this PE's waiting threads with
     * locked instructions, loaded by every writer that touches this PE's
     * memory, which then reads only the records whose bits are set - one load
     * when nobody waits, the phase-1 slot's cost. On its own cache line, so a
     * notifying writer shares none with the words this PE's own agent and
     * rendezvous use. Until layout 23 the slot held the one watch a PE could
     * have, which is a record with at most one bit set. */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t watch_active[OSOE_WATCH_ACTIVE_WORDS];
    uint32_t rank;
    int32_t pid;                    /* this PE's process id, for liveness (§3.15) */
    int32_t tid;                    /* the initializing thread, for /proc/<pid>/task/<tid> */
    uint64_t symmetric_size;        /* SHMEM_SYMMETRIC_SIZE as this PE parsed it */
    uint64_t heap_bytes;            /* mapped length of the heap object */
    char heap_name[OSOE_HEAP_NAME_MAX];
    /* Layout 17: this PE's symmetric statics (§4.3) - the object holding the
     * executable's data, which oshcc's linker script gathers, and which is
     * mapped over it - its length, 0 for a program built without the script,
     * and its name, as for the heap. */
    uint64_t statics_bytes;
    char statics_name[OSOE_HEAP_NAME_MAX];

    /*
     * The §3.14 "blocked in" record, so a hung job says where each PE is
     * waiting. Written by this PE only when a wait is about to sleep - a wait
     * that returns at once touches none of it - and read from another process,
     * by osoe-stat and by the deadlock detector's lead, while this PE may be
     * publishing its next wait.
     *
     * `blocked_gen` is odd while the fields are in flux: the writer bumps it,
     * writes, and bumps again, both bumps seq_cst; a reader loads it, reads the
     * fields, and loads it again, accepting only an unchanged even value. Two
     * locked read-modify-writes rather than a textbook seqlock, because that
     * needs relaxed stores and a release fence and §3.16 permits neither.
     * Every field is an atomic word, stored with release and loaded with
     * acquire; blocked.c has the protocol and the argument, and
     * tools/modelcheck/blocked_record_model.py the model.
     *
     * The routine's name is four such words holding its NUL-padded characters
     * (base/atomic-name.h). It was a plain character array until the
     * round-three review (K1): on aarch64 a reader's plain loads of it could be
     * satisfied after its second load of the generation, so a record could
     * pair one wait's name with another's detail. The bytes did not move - the
     * same 32 at the same offset, and a reader that copies them still finds
     * the name - so the layout number did not change for it.
     */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t blocked_gen;
    _Atomic uint64_t blocked_detail;     /* routine-specific: a PE, an epoch, an offset */
    _Atomic uint64_t blocked_since_ns;   /* CLOCK_MONOTONIC when the wait began */
    _Atomic uint64_t blocked_routine[OSOE_BLOCKED_ROUTINE_MAX / 8];
    _Atomic uint32_t blocked_detail_kind;   /* enum osoe_blocked_detail; layout 9 */

    /* Liveness and progress, published by this PE (§3.14 question 1). Its
     * epoch count k for each team is published too - since layout 18 in a row
     * of its own outside this slot, a word per team number, and since layout
     * 30 with the team's block beside it (osoe_node_published_epoch). */
    _Atomic uint64_t sweeps;             /* safety-sweep expiries: a blocked PE still moves */

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

    /*
     * Layout 25 (the scan's pieces in flight, 2026-09-27): the most pieces
     * of a scan this PE had in flight at once - sent up and their down not yet
     * taken - in its last scan over a team spanning nodes (§5.8, "Prefix
     * scans"). min(OSOE_COLLECTIVE_PIPELINE, pieces) at a leaf of the scan's
     * tree, which fills its window before it looks for a down. Written by the
     * scanning thread before its scan returns, so a count pins the overlap
     * where a time could only be compared, as the reductions' tree state does
     * (team.tree.most_pieces_in_flight).
     */
    _Atomic uint32_t scan_most_in_flight;

    /*
     * Layout 26 (the deadlock detector, 2026-09-28; §3.15, deadlock.h): this
     * PE's threads asleep in library waits, in the low half, and how many
     * times that number has changed, in the high half - one word, so the
     * node's lead reads both at one instant. Moved by whichever of this PE's
     * threads waits, one locked add as a wait first sleeps and one as it
     * ends (base/sweep.h), and read by the lead's agent only when it samples
     * its node.
     */
    _Atomic uint64_t waits;
    /* This PE's agent's thread id, which the detector leaves out when it
     * counts the PE's threads in /proc: published by the agent as it starts. */
    _Atomic int32_t agent_tid;

    /*
     * Layout 27 (the agents' CPUs, 2026-09-28; the author's question of
     * 2026-09-27): the CPUs this PE's task may run on as its launcher left
     * them - Slurm binds a node's tasks one core each when they fill it - a
     * bit each, written with the fields above before this PE joins. With
     * OSOE_AGENT_CPUS=node every agent may run on any CPU of its node's PEs
     * (agent.c), so the node's progress thread need not wait behind its PE on
     * one core while the job's other cores idle, and never runs on a CPU the
     * job was not given.
     */
    uint64_t task_cpus[OSOE_TASK_CPU_WORDS];
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

    /*
     * Appended at layout 18 (§5.7; phase 1 step 7): one block per team number,
     * OSOE_MAX_TEAMS of them - the node header's own tail, so `header_bytes`
     * covers them and a node-scope metric can name a predefined team's field -
     * and each local PE's published epochs, a row per PE of one 32-bit word
     * per team number (OSOE_MSCOPE_EPOCHS), each row padded to a cache line;
     * since layout 30 a 64-bit word, the team's block beside the epoch
     * (osoe_node_published_epoch).
     */
    uint64_t teams_offset;
    uint64_t teams_bytes;
    uint32_t team_bytes;             /* one block */
    uint32_t team_count;             /* OSOE_TEAM_BLOCKS */
    uint64_t epochs_offset;
    uint64_t epochs_bytes;
    uint32_t epoch_row_bytes;        /* one PE's row */
    uint32_t epoch_row_count;        /* local_count */

    /*
     * Appended at layout 21 (§4.6, §4.11; phase 1 step 8): each local PE's
     * pool of context blocks, contexts_per_pe (OSOE_MAX_CONTEXTS) of them in
     * local-id order, after every other section so that nothing before them
     * moved. A PE's block 0 is SHMEM_CTX_DEFAULT's.
     */
    uint64_t contexts_offset;
    uint64_t contexts_bytes;
    uint32_t context_bytes;          /* one block */
    uint32_t contexts_per_pe;        /* OSOE_MAX_CONTEXTS */

    /*
     * Appended at layout 23 (§4.11 item 3; phase 1 step 8 slice 2): each local
     * PE's thread records, threads_per_pe (OSOE_MAX_THREADS) of them in
     * local-id order, record i being the thread that holds counter block i.
     */
    uint64_t threads_offset;
    uint64_t threads_bytes;
    uint32_t thread_record_bytes;    /* one record */
    uint32_t threads_per_pe;         /* OSOE_MAX_THREADS */

    /*
     * Appended at layout 29 (§5.7; the team redesign's step D,
     * docs/design/team-resources-proposal.md D3): since this layout the team
     * blocks above, `team_count` of them, are a pool of OSOE_TEAM_BLOCKS, found
     * by a team's *name* - the local id of its local lead, and its number -
     * through the map: `team_map_row` (OSOE_MAX_TEAMS) words for each local
     * PE, name to block index plus one, 0 for none. Which blocks are free is a
     * bitmap of `team_free_words` 64-bit words, a bit set for a free block.
     * After the threads' records, so that nothing before them moved.
     */
    uint64_t team_map_offset;
    uint64_t team_map_bytes;
    uint64_t team_free_offset;
    uint32_t team_map_row;           /* OSOE_MAX_TEAMS */
    uint32_t team_free_words;        /* ceil(OSOE_TEAM_BLOCKS / 64) */
};

/*
 * The internode arrival handoff, one per team block (§5.3, §3.12).
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
 * A team with no internode phase never touches its entry: SHMEM_TEAM_SHARED is
 * one node by definition, and so is every team split from it. It is per team
 * because the world, the finalize team and the teams a split creates across
 * nodes each have one, and sharing one word between two teams would make a
 * sync on one forward the arrival of the other.
 */
struct osoe_arrival_handoff {
    /*
     * The posted epoch, k+1, in the low half, and in the high half the
     * generation of the block the poster holds (osoe_team_name), or 0 before
     * the first post. A single-writer publication store (§3.16): one member
     * posts at a time, and the release of the previous epoch orders it after
     * the last one. The generation since layout 30 (the team redesign's step
     * D, D4): a block given back and claimed for another team starts its
     * epochs again, and a word of the old team's could equal the new team's
     * first post if it held the epoch alone; with the generation beside it no
     * two teams' posts are one value, and the agent, comparing the whole word
     * with the one it consumed last, learns the block's team afresh when the
     * generation has moved (internode-am.c). Nothing resets it when the block
     * is claimed: the word stays the old team's last post, already consumed,
     * until the new team's first.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t word;
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
    /* Layout 30: the times a team's local release was held back because a
     * RELEASE it forwarded was still in a child's control queue (§3.8,
     * progress.c, release_local, which a pass runs twice). Zero but when a
     * connection is full at the release. */
    _Atomic uint64_t releases_held;
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
 * of single-writer stores per barrier (§3.16) - all but the last three fields,
 * which the generic internode phase writes on the lead's application thread.
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
    /*
     * Layout 19 (phase 1 step 5 slice 5), in padding the line already had: the
     * most pieces of a data collective this node's lead had in flight at once -
     * sent up and their release not yet held - in its last one on the team
     * (§5.8, "Pipelined pieces"). min(OSOE_COLLECTIVE_PIPELINE, pieces) at
     * every lead but the root, which sends nothing up and says 0. Written as
     * the generic internode phase ends (internode-generic.c), before the lead
     * releases its members, so a count pins the overlap where a time could
     * only be compared.
     */
    _Atomic uint32_t most_pieces_in_flight;
    /*
     * Layout 20 (phase 1 step 5 slice 6), in the same padding: the steps this
     * node's lead made in its last gather on the team, and the (node, step)
     * pairs its lists of what to send hold - a share for each node a step
     * sends, counted as the lists were filled rather than computed -
     * ceil(log2 N) and N - 1 for a team over N nodes (§5.8, "An allgather for
     * fcollect"). Written as the gather ends (internode-generic.c), before the
     * lead releases its members, so counts pin the algorithm where a time
     * could only be compared.
     */
    _Atomic uint32_t gather_steps;
    _Atomic uint32_t gather_shares;
};

/*
 * A team's record on this node (layout 18; phase 1 step 7): what this node's
 * progress thread and internode backend need of a team they may hold no
 * descriptor for. The node lead's process - where the progress thread runs -
 * need not belong to a team a split creates at all, so it learns the team
 * here, by number, and never from a private descriptor.
 *
 * Its content is §5.2's accounting as the node sees it: the team's members on
 * this node, and this node's place in the team's internode tree - O(k), the
 * part of the node set the tree keeps. Each neighbour node is named by its
 * lead's rank - for the predefined teams the rank at its local id 0, for a
 * created team its member with the lowest team rank there - which the generic
 * internode phase puts to, and from which the progress thread finds the node
 * numbers its messages go to.
 *
 * Written once for a team, fields first and then `published` with a release
 * store; a reader loads `published` with acquire and reads the rest only once
 * it is 1 (§3.16's single-writer publication). Since layout 30 (the team
 * redesign's step D, D4) a block is given back when the last of the team's
 * members here leaves it, after the destroy's sync: that member retracts the
 * record - `published` back to 0 - before anything else of the block's life
 * moves (osoe_team_name), and the next team the block is claimed for writes
 * its own. Until then nobody reads the old team's: the team's members have
 * left it, and nothing of it is still to arrive (the proposal's "Why giving a
 * number back is safe"). SHMEM_TEAM_SHARED under force-remote has none: it is
 * a different team of one on each PE of the node under the one number, and
 * touches nothing of the node's. A created team of one has one, its name being
 * its own. More than one cache line: the children's ranks alone are 128 bytes
 * at the largest fan-out.
 *
 * **Who sees it, and why** (§3.16). The predefined teams' records are written
 * by this node's lead in osoe_team_start (team.c), in every session, before
 * two things: its agent's start, which orders them before everything that
 * thread does; and its release store to `lead_agent_running`, which every other
 * local PE loads with acquire before it returns from shmem_init (init.c). So
 * every PE finds them published, in any process and on any thread that runs
 * after its init, whether it reads its team's record on the arrival path
 * (osoe_internode_arrive) or as a lead in a data collective
 * (internode-generic.c). The arrival counter carries them as well: the lead is
 * a member of every predefined team, and the member that posts an arrival
 * reads, with its own seq_cst add, a value in the release sequence of the
 * lead's add, which the lead made after writing them.
 *
 * **A created team's record** (phase 1 step 7 slice 2) is written by the
 * team's local lead on this node - its member of lowest team rank here, in
 * whatever process that is - while every process runs, and before that PE
 * arrives at the split's sync over the parent team (team.c). Every reader sees
 * it by orders that exist already, the sync carrying it:
 *
 * - **The team's members on this node**, on the arrival path and in the data
 *   half. The local lead publishes the record and stores the map entry (layout
 *   29), before its own arrival at the parent's sync, a seq_cst add to the parent's
 *   `arrived`. The parent's epoch is released
 *   here only after the last local arrival, a later add to the same counter,
 *   and every add to it is a read-modify-write, so the last one reads a value
 *   in the release sequence of the local lead's: the last arriver's release
 *   bump of `released` - or, for a parent spanning nodes, the progress
 *   thread's, below - follows the record, and each member's acquire of that
 *   release precedes its return from the split, and so its first arrival at
 *   the new team. (The data half reads this node's record on the local lead,
 *   which wrote it.)
 * - **This node's progress thread**, whose process need not hold a member.
 *   For a parent spanning nodes the thread performs this node's arrival at the
 *   parent's sync by acquiring the parent's arrival handoff, which the last
 *   local arriver posted after its add, and so after the local lead's:
 *   everything the thread does after that follows the record. It meets the
 *   new team at one of two things. A local member's first post, which comes
 *   after that member's return from the split, and sets the block's posted
 *   bits before its bump of the lead's wake word, so the thread's exchanges of
 *   the summary and the bitmap after its load of that word find the block
 *   (internode-am.c, `team_posted_summary`). Or a
 *   message about the new team from another node, which can only have been
 *   sent once the parent's sync completed, which needed this node's arrival,
 *   which the thread sent after its acquire of the handoff: the message's
 *   receipt is later in the thread's own program order.
 * - **A parent on one node** has children on one node, which send no messages
 *   and have no internode phase for the thread to learn.
 *
 * osoe-stat reads a record from another process at any time, loading
 * `published` with acquire.
 */
struct osoe_team_record {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t published;     /* 0 until the record is written, then 1 */
    uint32_t local_members;         /* the team's members on this node (§5.2) */
    uint32_t nodes;                 /* the nodes it spans: 1 is no internode phase */
    uint32_t index;                 /* this node's position in its sorted node set */
    /* The parent node's lead's rank - for the predefined teams the rank at its
     * local id 0 - or UINT32_MAX at the root. */
    uint32_t parent;
    uint32_t position;              /* this node among its parent's children */
    uint32_t children;              /* how many of `child` there are */
    char name[16];                  /* in metric names: world, shared, finalize, a number */
    /* Each child node's lead's rank - for the predefined teams the rank at its
     * local id 0 - in the order of the children's positions. */
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];
    /*
     * Layout 22 (the author's decision E of 2026-09-26; phase 1 step 10 slice
     * 1): the team's ranks as world ranks, so a backend maps any member from
     * the record alone - team rank r is world rank `world_start` + r x
     * `world_stride`, for r below `members`. Every team descending from
     * SHMEM_TEAM_WORLD is such a progression (spec §9.4.6: a split of a
     * progression by a progression is one), and only SHMEM_TEAM_SHARED's
     * family is not; that family never leaves a node, so it has no internode
     * phase, and its record says `strided` 0 and nothing below asks. Until
     * layout 22 a backend asked the SHMEM layer, through
     * osoe_internode_team_world_ranks, which is gone.
     */
    uint32_t members;
    uint32_t world_start;
    int32_t world_stride;
    uint32_t strided;               /* 1 for a progression, 0 for a table */
};

/*
 * What the pool needs of a block (layout 29; the team redesign's step D,
 * docs/design/team-resources-proposal.md D3 and D4): the team's *name* on this
 * node - the local id of its local lead and its number, under which the map
 * holds the block's index - the local members holding it, and its generation.
 *
 * **A block's life** (layout 30; tools/modelcheck/team_block_model.py). The
 * local lead of a team a split creates claims a free block from the bitmap
 * before the split's reduction (osoe_node_team_claim), and once the number is
 * agreed names it (osoe_node_team_name): its generation raised, to an odd
 * value, its counters and tree state zeroed, the name and the holders - the
 * team's members on this node - written; then the record, and last the map
 * entry, with release, before the split's sync (team.c). Members find the block
 * through the map after the sync. At destroy, after the sync over the team,
 * each member leaves it (osoe_node_team_leave): the team's local lead first
 * clears the map entry under its name, which is its own, and which nothing
 * resolves once any member here has left the team - no message for the team
 * reaches the node after that (the proposal's "A late message") - and then
 * every member brings holders down by one, and the member that brings them to
 * zero retracts the record, raises the generation again and returns the block
 * to the bitmap, in that order. The model's breakages show the order: the bit
 * before the raise, and another team can claim and write the block in between,
 * the raise landing on that team's generation; the map entry cleared by every
 * member as it leaves, and a slow member clears the entry the lead has made
 * again for its next team; the entry the last holder's to clear - the first
 * design - and a lead whose members are slow to leave finds its own name
 * taken, its number not free at its next split. A lead's free mask leaves a
 * number out while a block is mapped under the lead's name (team.c); and a
 * block is claimed only from the bitmap, so no block is written for a team
 * while a member of another still holds it.
 *
 * The generation is what the progress engine follows (D4): it keeps the one
 * it learnt a block's team at, and a handoff or a message showing another
 * makes it learn the block afresh (progress.c). It moves only while the block
 * has no holder, so a member's acquire load of its block's needs nothing more.
 *
 * Written by the team's local lead as it names the block, before the map
 * entry, whose release orders them before any member's acquire of it after the
 * split's sync (the record's argument, below); read by members, by the progress
 * engine as it learns the team, and by osoe-stat, which labels a block by it -
 * with acquire (§3.16), though the map entry and the record's `published` are
 * what order them.
 * `holders` moves by read-modify-writes, seq_cst as every one is (§3.16).
 */
struct osoe_team_name {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t lead;          /* local id of the team's local lead here */
    _Atomic uint32_t number;        /* the team's number */
    _Atomic uint32_t holders;       /* local members holding the block */
    _Atomic uint32_t generation;    /* odd while a team holds it; 0 never written */
    /* Layout 30: the holders it was named with, the team's members here - so
     * that fewer holders says its team is leaving it - kept once the last has
     * left, and zeroed when the block is claimed again: a block given back and
     * not yet in the bitmap is told by it (osoe_node_team_returning). */
    _Atomic uint32_t members;
};

/*
 * Everything a node keeps for a team (layout 18; §5.3, §5.4, §5.7): the header
 * ends with a pool of these, OSOE_TEAM_BLOCKS since layout 29 (step D), found
 * by the team's name through the map (osoe_node_team_find) and then by block
 * index (osoe_node_team), so a team created later needs no storage of its own
 * and no team's state shares a cache line with another's. The predefined teams'
 * are the first three, at the block index of their number; a created team's is
 * claimed from the free bitmap by its local lead here (osoe_node_team_claim),
 * and given back when the last of its members here leaves it (osoe_team_name).
 *
 * - **§5.4's three epoch counters**, each alone on its line. SHMEM_TEAM_SHARED
 *   has its own although on one node its membership is the world's: the spec
 *   makes them distinct predefined teams forming separate atomicity domains,
 *   and sharing the counters would make a sync on one team release a wait on
 *   the other. The finalize team never uses `copied`: it has no copy phase.
 * - **The arrival handoff** (§5.3), to the node lead's agent.
 * - **The tree's published state** (§3.14), which that agent writes.
 * - **The record** above.
 */
struct osoe_team_block {
    struct osoe_epoch arrived;
    struct osoe_epoch released;
    struct osoe_epoch copied;
    struct osoe_arrival_handoff arrival;
    struct osoe_collective_state tree;
    struct osoe_team_record record;
    struct osoe_team_name name;
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
    /* Layout 28: the launch these ids belong to (bootstrap.h), so that a
     * node-mate tells a leftover of another launch by the same names from
     * this one's segment (open_control). */
    uint64_t launch;
    /* Layout 19: the lead's OSOE_COLLECTIVE_FANOUT and OSOE_COLLECTIVE_PIPELINE.
     * They size every PE's internal region (internal-region.h), and a team's
     * local lead - any of the node's PEs once teams are created (§5.2) - runs
     * the tree and keeps the window they shape, so open_control holds every
     * other PE of the node to them, as it does to OSOE_MAX_TEAMS. */
    uint32_t collective_fanout;
    uint32_t collective_pipeline;

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

    /* Appended at layout 8; layout 11 appended passes and send_visits to it,
     * and layout 13 the ring's six counters, which moved nothing before it
     * (what follows moved, and is found by name). Layout 18 moved every
     * team's epoch counters and arrival handoff from before this record into
     * the team blocks at the header's end, which moved it; readers find it by
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

    /*
     * Appended at layout 16; their meaning since layout 23 (§4.11 item 7): a
     * PE's zone (struct osoe_landing_zone) is its header and bitmap, then
     * `landing_requests` extent records at `landing_requests_offset`, then
     * the extents' `landing_arena_bytes` at `landing_arena_offset`, one
     * record and OSOE_LANDING_EXTENT_BYTES an extent; `landing_data_bytes` is
     * the largest piece a get lands at once. Offsets are within a zone.
     */
    uint32_t landing_data_bytes;
    uint32_t landing_requests;
    uint32_t landing_requests_offset;
    uint32_t landing_arena_offset;
    uint32_t landing_arena_bytes;

    /*
     * Layout 30 (the team redesign's step D, D5;
     * tools/modelcheck/posted_bitmap_model.py): which team blocks have an
     * arrival posted. A bit a block in the posted bitmap (`team_posted`, after
     * the free bitmap), and here a bit for each 64-block word of it, so that the
     * node lead's agent finds the posts with one load when there are none,
     * however many teams the node holds - 1,024 blocks need the first word
     * alone, the 65,536 the setting allows all sixteen. A member that posts
     * sets its block's bit, then its word's bit here, between its store of the
     * handoff and its bump of the lead's wake word (osoe_internode_arrive); the
     * agent, after loading its wake word, takes each nonzero word here by
     * exchange, and each bitmap word it names by exchange, and reads those
     * blocks' handoffs. The model's breakages each lose a post: the bit here
     * before the block's, the wake before either, either before the handoff,
     * the agent clearing this word after reading the bitmap. On its own line:
     * every pass loads it, every post sets it. Until layout 30 the agent read
     * every block at and above a low-water mark of published blocks,
     * `team_blocks_published_from`, and until layout 29 every number below a
     * high-water mark, `team_numbers_published`.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t team_posted_summary[OSOE_TEAM_POSTED_SUMMARY_WORDS];

    /*
     * The pool of team blocks, `dir.team_count` of them (OSOE_TEAM_BLOCKS since
     * layout 29; one per team number, OSOE_MAX_TEAMS, from layout 18), which
     * end the header - `dir.header_bytes` counts them, so the node scope of the
     * metric table reaches the predefined teams' fields, the first three
     * blocks, by the names they had when the header held them (§3.14). Found
     * with osoe_node_team; a created team's through the map first.
     */
    struct osoe_team_block team[];
};

/*
 * A thread's record in the node segment (§4.11 items 3 and 5; layout 23):
 * its watch - the range it waits on, packed as watch-range.h says, 0 for none,
 * and the futex word a writer bumps - one per thread of the PE, so that two
 * threads waiting at once each have their own and neither's wake is lost
 * (tools/modelcheck/watch_list_model.py). Record i belongs to the thread that
 * holds the PE's counter block i (metrics.c), claimed at the thread's first
 * call and released at its exit; the PE's `active` bitmap (struct
 * osoe_pe_slot) says which records hold a published range.
 */
struct osoe_thread_record {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t watch_range;
    _Atomic uint32_t watch_sequence;
    uint32_t reserved;

    /*
     * The thread's completion slot (§4.11 item 5): a blocking get or fetching
     * AMO blocks its thread until its reply lands, so a thread has at most one
     * outstanding, and every thread its own slot. The thread stores
     * `armed_token` before its request goes out; the lead refuses a reply
     * carrying any other (§3.4's trust boundary), and otherwise stores the
     * value or the reply's length, then `ready`, then bumps `wake` and wakes -
     * §4.6's wake-word pattern with one waiter and one writer. A get's bytes
     * land in the extents its token names (struct osoe_landing_zone), not
     * here. Until layout 23 the PE had one such slot, with its data.
     */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t wake;
    _Atomic uint32_t ready;             /* 0, or 1 once the reply is complete */
    _Atomic uint32_t length;            /* bytes a get's reply carried */
    uint32_t reserved2;
    _Atomic uint64_t value;             /* a fetching AMO's result */
    _Atomic uint64_t token;             /* the reply's */
    _Atomic uint64_t armed_token;       /* the one the thread waits for */
};

/*
 * A context's block in the node segment (§4.6, §4.11; phase 1 step 8 slice
 * 1): the part of a context that the lead's agent writes and another process
 * reads. Each local PE has a pool of OSOE_MAX_CONTEXTS of them (layout 21,
 * osoe_node_context), block 0 being SHMEM_CTX_DEFAULT's, live from init to
 * finalize; shmem_ctx_create claims a free one by a compare-and-swap on its
 * state word and shmem_ctx_destroy releases it, so a destroyed context's
 * block serves a later creation. Fixed-size, one cache line: the pool is O(1)
 * per context whatever the job's size, which is the proposal's bound
 * (docs/prompts/done/phase1-step8-slice1-prompt.md, decision 2).
 *
 * The rest of a context - its dirty set, O(nodes) virtual - is process
 * memory of the PE that owns it (transport-am.c).
 */
struct osoe_context_block {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t state;             /* OSOE_CONTEXT_FREE or OSOE_CONTEXT_LIVE */
    uint32_t reserved;
    /*
     * §4.6's get completion, per context since layout 21 (until then the
     * landing slot held one pair per PE): two cumulative counters, never
     * reset, with the companion 32-bit wake word they need because a 64-bit
     * counter cannot be futexed (§4.9). The PE adds each nonblocking get's
     * bytes to `bytes_issued` as it asks; the progress thread adds them to
     * `bytes_landed` as the whole reply lands, then bumps the wake word and
     * wakes only while the PE waits on it (base/wait-word.h); a wait on the
     * context waits for the two to be equal and then copies its replies out.
     * The reply's token names the block (OSOE_LANDING_CONTEXT_SHIFT), so the
     * lead credits the right context without the wire changing.
     */
    _Atomic uint64_t bytes_issued[2];
    _Atomic uint64_t bytes_landed;
    struct osoe_wait_word counter_wake;
    /* Since layout 23 (§4.11 item 8): the bytes of its replies copied out to
     * their `dest`, by whichever of the PE's threads claimed each (node.h,
     * struct osoe_landing_record). A wait on the context is over when this
     * reaches `bytes_issued`; a copier adds, then bumps `counter_wake`. */
    _Atomic uint64_t bytes_copied[2];
    /*
     * Since layout 24 (§4.11 items 1 and 2; phase 1 step 8 slice 3): a shared
     * context's gets count in the pair `generation & 1` names - issued and
     * copied alike, the pair travelling in the token (OSOE_LANDING_PAIR) - so
     * that a quiet can close a pair while other threads issue into the other.
     * A quiet, holding the context's mutex, bumps `generation` and waits for
     * the pair it closed; `full` is the generation the last full quiet
     * opened, which a quiet begun before it may piggyback on. A serial
     * context uses pair 0 and never moves `generation`. Both are 64 bits:
     * a pe_quiet or a wait for gets moves `generation` without `full`, and
     * a 32-bit difference between them wraps after 2^31 of those, when a
     * quiet would piggyback with puts in flight (review of slice 3, B1).
     */
    _Atomic uint64_t generation;
    _Atomic uint64_t full;
};

enum { OSOE_CONTEXT_FREE = 0, OSOE_CONTEXT_LIVE = 1 };

/*
 * A PE's landing zone (§3.12, §4.6, §4.11 item 7; layout 23, phase 1 step 8
 * slice 2): where every reply to a get of the PE's, blocking or nonblocking,
 * lands before it is copied to its `dest`.
 *
 * A get's `dest` may be a stack or malloc buffer that the lead's process cannot
 * see, so a reply cannot be written where the caller asked for it. It lands
 * here, in the node segment, and the *origin* copies it out - the calling
 * thread for a blocking get, and for a nonblocking one whichever of the PE's
 * threads claims it first: its context's quiet, or an issue that found the
 * zone full. It belongs in the control segment rather than the AM connection
 * area because every backend needs it: the reason it exists is the address
 * space boundary, not the network.
 *
 * **The zone is `landing_extents` extents of OSOE_LANDING_EXTENT_BYTES**,
 * claimed by compare-and-swap as a run of clear bits within one word of
 * `bits`, so a piece of a get is at most 64 extents; a claimer that finds no
 * run copies out landed replies it can claim, and then sleeps on `freed`,
 * which every release bumps (tools/modelcheck/landing_extent_model.py). Every
 * extent has a record (struct osoe_landing_record); a piece's is its first
 * extent's. Until layout 23 the zone was one blocking slot of a chunk, then
 * OSOE_GETS_IN_FLIGHT records, then an arena bumped from one end - one caller
 * at a time.
 */
enum { OSOE_LANDING_EXTENT_BYTES = 1024, OSOE_LANDING_RUN_MAX = 64 };

struct osoe_landing_zone {
    _Alignas(OSOE_CACHELINE)
    struct osoe_wait_word freed;        /* bumped whenever extents are released */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint64_t bits[];            /* ceil(landing_extents / 64) words */
};

/*
 * A piece of a get in flight, at its first extent: its token, the bytes it
 * asked for, and its state - FREE, ISSUED by its PE, LANDED by the lead once
 * the whole reply is in, COPYING by the one thread whose compare-and-swap won
 * it (tools/modelcheck/copy_out_claim_model.py), and back to FREE once its
 * bytes are out and its extents released. A blocking get's record goes to
 * LANDED_BLOCKING instead, which no other thread claims: its own thread copies
 * it. The PE writes `token` and `length`, then `state` with release, before its
 * request goes out; the lead finds the record by the index in the reply's
 * token and refuses a reply whose token is not the record's, whose record is
 * not ISSUED, or whose bytes would fall outside the piece (§3.4's trust
 * boundary).
 */
enum osoe_landing_state {
    OSOE_LANDING_FREE = 0,
    OSOE_LANDING_ISSUED,
    OSOE_LANDING_LANDED,
    OSOE_LANDING_COPYING,
    OSOE_LANDING_LANDED_BLOCKING,
};

struct osoe_landing_record {
    _Atomic uint64_t token;
    _Atomic uint32_t length;
    _Atomic uint32_t state;             /* enum osoe_landing_state */
};

/*
 * A token (layout 23): the top bit names a nonblocking get; bits 48 to 59
 * the block its completion goes to - a nonblocking get's context block, or a
 * blocking operation's thread record; bit 47, since layout 24, the counter
 * pair a shared context's nonblocking get counts in (struct
 * osoe_context_block); bits 16 to 46 a sequence; and the low
 * 16 bits the piece's first extent, or OSOE_LANDING_NO_EXTENT for a fetching
 * AMO, whose reply is a value. The token is echoed by the target and read by
 * the origin alone, so the wire format is unchanged.
 */
#define OSOE_LANDING_NONBLOCKING (UINT64_C(1) << 63)
#define OSOE_LANDING_EXTENT_MASK UINT64_C(0xffff)
#define OSOE_LANDING_NO_EXTENT UINT64_C(0xffff)
#define OSOE_LANDING_OWNER_SHIFT 48
#define OSOE_LANDING_OWNER_MASK UINT64_C(0xfff)
#define OSOE_LANDING_PAIR (UINT64_C(1) << 47)
#define OSOE_LANDING_SEQUENCE_MASK UINT64_C(0x7fffffff)

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
    /*
     * OSOE_REGION_STATICS (§4.3): the executable's data as oshcc's linker
     * script gathers it, two page-aligned ranges - initialized data, then
     * zero-initialized - and the object holding them, the first range at
     * offset 0 and the second after it. Each local PE's object is mapped whole
     * at statics_of[local id]; this PE's own is also mapped over the two
     * ranges, which is where the program's code reads and writes it. For a
     * program built without the script the ranges are empty, statics_bytes is
     * 0 and statics_of is NULL.
     */
    unsigned char *statics_data;
    uint64_t statics_data_bytes;
    unsigned char *statics_bss;
    uint64_t statics_bss_bytes;
    uint64_t statics_bytes;
    void **statics_of;
    unsigned char *landing;         /* the landing zones; index with osoe_node_landing */
    uint32_t landing_slot_bytes;    /* a PE's whole zone */
    uint32_t landing_data_bytes;    /* the largest piece: min(an AM chunk, a run of extents) */
    uint32_t landing_requests;      /* extents, and so records, in a PE's zone */
    uint32_t landing_requests_offset;   /* its records, from the zone's start */
    uint32_t landing_arena_offset;      /* its extents' bytes, from the zone's start */
    uint32_t landing_arena_bytes;
    /* One record per peer connection, in peer-slot order, or NULL on a job of
     * one node (§3.14's connection scope). */
    struct osoe_am_connection_metrics *connection_metrics;
    uint32_t connection_count;
    /* The header's pool of team blocks (OSOE_TEAM_BLOCKS), the map from a
     * team's name to its block, `team_map_row` (OSOE_MAX_TEAMS) words a local
     * PE, the bitmap of free blocks, and each local PE's row of published
     * epochs, `epoch_row_words` long, by number. */
    uint32_t team_count;
    _Atomic uint32_t *team_map;
    uint32_t team_map_row;
    _Atomic uint64_t *team_free;
    uint32_t team_free_words;
    _Atomic uint64_t *team_posted;      /* team_free_words words: the posted blocks */
    _Atomic uint64_t *epochs;
    uint32_t epoch_row_words;
    /* Each local PE's pool of context blocks, `contexts_per_pe` each
     * (OSOE_MAX_CONTEXTS), indexed by osoe_node_context. */
    struct osoe_context_block *contexts;
    uint32_t contexts_per_pe;
    /* Each local PE's thread records, `threads_per_pe` each (OSOE_MAX_THREADS),
     * indexed by osoe_node_thread. */
    struct osoe_thread_record *threads;
    uint32_t threads_per_pe;
};

/* Local PE `local_id`'s thread record `index` (§4.11 item 3; layout 23). */
static inline struct osoe_thread_record *osoe_node_thread(const struct osoe_node *n,
                                                          uint32_t local_id, uint32_t index)
{
    return &n->threads[(size_t)local_id * n->threads_per_pe + index];
}

/* Local PE `local_id`'s context block `index` (§4.11; layout 21). */
static inline struct osoe_context_block *osoe_node_context(const struct osoe_node *n,
                                                           uint32_t local_id, uint32_t index)
{
    return &n->contexts[(size_t)local_id * n->contexts_per_pe + index];
}

/* The node's team block at `block_index` in the pool, or NULL past the last
 * (§5.7; step D). The predefined teams' are at the index of their number. */
static inline struct osoe_team_block *osoe_node_team(const struct osoe_node *n,
                                                     uint32_t block_index)
{
    return block_index < n->team_count ? &n->hdr->team[block_index] : NULL;
}

/* The map's word for the name (`lead`, `number`): its block index plus one, or
 * 0 for none (step D, D3). The team's local lead stores it with release once
 * the block is written, before the split's sync; a member loads it with acquire
 * after the sync (osoe_node_team_find). */
static inline _Atomic uint32_t *osoe_node_team_map_entry(const struct osoe_node *n,
                                                         uint32_t lead, uint32_t number)
{
    return &n->team_map[(size_t)lead * n->team_map_row + number];
}

/* The block index the name (`lead`, `number`) maps to on this node, or
 * UINT32_MAX for a name with no block - or out of range, as a message's name
 * from another node may be. */
static inline uint32_t osoe_node_team_find(const struct osoe_node *n, uint32_t lead,
                                           uint32_t number)
{
    if (lead >= n->local_count || number >= n->team_map_row)
        return UINT32_MAX;
    uint32_t v = atomic_load_explicit(osoe_node_team_map_entry(n, lead, number),
                                      memory_order_acquire);
    return v && v <= n->team_count ? v - 1 : UINT32_MAX;
}

/*
 * Claim a free block of the pool for a team created here, or UINT32_MAX if
 * none is free: a compare-and-swap on the free bitmap, so a failed claim takes
 * nothing, and then the block's members zeroed (osoe_team_name). From the top
 * of the pool down, so that a created team's block index is seldom its number
 * - a mix-up of the two shows at once rather than on some node of some map. A
 * block claimed and never named - its split failed - goes back with
 * osoe_node_team_return.
 */
uint32_t osoe_node_team_claim(struct osoe_node *n);

/* Give back a block claimed and not named (osoe_node_team_claim): one atomic
 * or of its bit into the free bitmap. */
void osoe_node_team_return(struct osoe_node *n, uint32_t block_index);

/*
 * Name the claimed block `block_index` for the team (`lead`, `number`) with
 * `holders` members on this node (osoe_team_name, a block's life): the
 * generation raised, the counters and the tree state zeroed, the name and the
 * holders written. By the team's local lead, before it writes the record and
 * the map entry. Returns the generation.
 */
uint32_t osoe_node_team_name(struct osoe_node *n, uint32_t block_index, uint32_t lead,
                             uint32_t number, uint32_t holders);

/*
 * Whether a block of the pool is on its way back: its team has begun to leave
 * it, or its last holder has given it back and it is not in the bitmap yet -
 * fewer holders than the members it was named with, which only a claim zeroes;
 * or it is in the bitmap, back since the caller found the pool empty. Each
 * lasts only as long as a member past the destroy's sync takes to leave. What
 * a lead that finds the pool empty asks, so that its split runs the round
 * again rather than failing for a block about to be free (team.c;
 * tools/modelcheck/pool_dry_model.py). A block claimed and not yet named is
 * not on its way back: its split will name it, or fail. O(OSOE_TEAM_BLOCKS),
 * on that path alone.
 */
bool osoe_node_team_returning(const struct osoe_node *n);

/*
 * One member of the team in block `block_index` leaves it, after the destroy's
 * sync (osoe_team_name, a block's life): the team's local lead here - `lead` -
 * first clearing the map entry under the block's name, its own; then holders
 * down by one, and if that was the last, the record retracted, the generation
 * raised and the block returned to the bitmap, in that order. True for the
 * member that gave the block back.
 */
bool osoe_node_team_leave(struct osoe_node *n, uint32_t block_index, bool lead);

/*
 * Where local PE `local_id` publishes its epoch count k for the team it holds
 * under number `team` (§5.4), which osoe-stat compares against the team's
 * counters to say which PE has not reached a collective (§3.14). One word of
 * that PE's row, which only it writes; the rows are padded to a cache line, so
 * no two PEs share one. Since layout 30 (step D) the word is the team's block
 * index plus one in the high half and k in the low (OSOE_EPOCH_WORD), so that
 * a reader pairs it with a block and not with a number: two PEs of one node
 * may hold different teams under one number (D1). 0 for none.
 */
static inline _Atomic uint64_t *osoe_node_published_epoch(const struct osoe_node *n,
                                                          uint32_t local_id, uint32_t team)
{
    return &n->epochs[(size_t)local_id * n->epoch_row_words + team];
}

/* The word of the posted bitmap that holds block `block_index`'s bit, and the
 * summary word that holds that word's (node.h, `team_posted_summary`). */
static inline _Atomic uint64_t *osoe_node_team_posted(const struct osoe_node *n,
                                                      uint32_t block_index)
{
    return &n->team_posted[block_index / 64];
}

static inline _Atomic uint64_t *osoe_node_team_posted_summary(const struct osoe_node *n,
                                                              uint32_t block_index)
{
    return &n->hdr->team_posted_summary[block_index / 4096];
}

/* A published epoch word: block `block_index` and epoch count `epoch`. */
#define OSOE_EPOCH_WORD(block_index, epoch)                                 \
    ((uint64_t)((block_index) + 1) << 32 | (uint32_t)(epoch))

/* A local PE's landing zone. Its size is set at run time, so zones are indexed
 * by bytes, never by array. */
static inline struct osoe_landing_zone *osoe_node_landing(const struct osoe_node *n,
                                                          uint32_t local_id)
{
    return (struct osoe_landing_zone *)(n->landing + (size_t)local_id * n->landing_slot_bytes);
}

/* A local PE's extent records, and its extents' bytes. */
static inline struct osoe_landing_record *osoe_node_landing_records(const struct osoe_node *n,
                                                                    uint32_t local_id)
{
    return (struct osoe_landing_record *)((unsigned char *)osoe_node_landing(n, local_id) +
                                          n->landing_requests_offset);
}

static inline unsigned char *osoe_node_landing_arena(const struct osoe_node *n,
                                                     uint32_t local_id)
{
    return (unsigned char *)osoe_node_landing(n, local_id) + n->landing_arena_offset;
}

/*
 * Local PE `local_id`'s statics at `offset` (§4.3), in this process: a
 * node-mate's through its object's mapping, and this PE's own - `own` - at
 * the program's own addresses, not the alias, so that a transfer between two
 * of its statics that overlap is one memmove over one set of addresses.
 */
static inline unsigned char *osoe_node_statics_at(const struct osoe_node *n, uint32_t local_id,
                                                  bool own, uint64_t offset)
{
    if (!own)
        return (unsigned char *)n->statics_of[local_id] + offset;
    return offset < n->statics_data_bytes ? n->statics_data + offset
                                          : n->statics_bss + (offset - n->statics_data_bytes);
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

/*
 * From the moment a PE knows who it is - its job, step, node and local id -
 * until its node has joined, a fatal of its own tells its node before it goes
 * (the round-three review's R8: a PE refusing its own environment, or the
 * kernel, left its node-mates waiting in silence). The tale is the `failed`
 * word every start-up wait of the node reads (§6.2): set in the control
 * segment once this PE has it; otherwise, from the node's lead, in a tombstone
 * - a control segment that is nothing but a header with `failed` set and
 * `ready` never - and from any other PE, in the lead's segment, once the lead
 * has made it, waiting at most OSOE_ABORT_GRACE_SECONDS for it. A PE that cannot
 * read its own identity tells nobody; the 30 s notices of the others name it.
 */
void osoe_node_refusal_arm(struct osoe_node *n, const struct osoe_bootstrap *b,
                           uint32_t session);
void osoe_node_refusal_disarm(void);

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
