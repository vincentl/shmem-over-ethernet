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
#include <stdint.h>

#include "base/cacheline.h"
#include "base/counters.h"
#include "base/epoch.h"
#include "boot.h"

#define OSOE_NODE_MAGIC  UINT64_C(0x65646f6e656f736f)  /* "osoenode", little-endian */
#define OSOE_NODE_LAYOUT 5

enum osoe_phase {
    OSOE_PHASE_EMPTY = 0,       /* slot unused */
    OSOE_PHASE_JOINED,          /* slot fields written; own heap mapped at B */
    OSOE_PHASE_MAPPED,          /* every sibling heap mapped */
    OSOE_PHASE_FINALIZED,       /* last finalize has run */
};

enum { OSOE_HEAP_NAME_MAX = 96 };

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
    int32_t pid;                    /* this PE's process id, for the watchdog (§3.15) */
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

    /* Liveness and progress, published by this PE (§3.14 question 1). */
    _Atomic uint64_t sweeps;             /* safety-sweep expiries: a blocked PE still moves */
    _Atomic uint32_t world_epoch;        /* this PE's k for SHMEM_TEAM_WORLD (§5.4) */
    _Atomic uint32_t shared_epoch;       /* and for SHMEM_TEAM_SHARED */

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
    uint32_t metric_desc_bytes;

    uint64_t blocks_offset;
    uint64_t blocks_bytes;
    uint32_t block_count;            /* local_count * blocks_per_pe */
    uint32_t block_bytes;
    uint32_t blocks_per_pe;          /* OSOE_MAX_THREADS */
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

    /* Rendezvous words, each a shared futex. */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t ready;         /* lead: header complete */
    _Atomic uint32_t joined;        /* PEs in OSOE_PHASE_JOINED or later */
    _Atomic uint32_t mapped;        /* PEs in OSOE_PHASE_MAPPED or later */
    _Atomic uint32_t finalized;     /* PEs whose last finalize has run */
    _Atomic uint32_t failed;        /* nonzero once any PE has failed rendezvous */

    /* The abort word (§3.15). Nonzero means the job is over and no wait can
     * finish; every library wait reads it at its next safety-sweep deadline
     * (base/sweep.h). Phase 1 step 1 sets it only from the development
     * watchdog; shmem_global_exit and the AM abort flood set it in step 4. */
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t aborting;      /* 0, or 1 once any PE has declared the job over */
    _Atomic uint32_t abort_pe;      /* the world rank the abort names */
    _Atomic int32_t abort_status;   /* the status to exit with (§9.1.6) */

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
};

/* Process-private view of the node. */
struct osoe_node {
    char control_name[80];
    struct osoe_node_header *hdr;
    struct osoe_pe_slot *slots;
    struct osoe_counter_block *blocks;   /* this node's whole pool (§3.14) */
    uint64_t control_bytes;
    void **heap_of;                 /* base_of[local id][OSOE_REGION_HEAP] */
    int32_t *local_of_rank;         /* world rank -> local id, or -1 */
    uint64_t heap_bytes;
    uint64_t heap_size;             /* the agreed SHMEM_SYMMETRIC_SIZE */
};

/* Rendezvous with the other local PEs; map every heap. Fatal on failure. */
void osoe_node_join(struct osoe_node *n, const struct osoe_boot *b, uint32_t session);

/* Unmap everything; the PE whose finalize is the node's last removes the
 * control segment's name. */
void osoe_node_leave(struct osoe_node *n, const struct osoe_boot *b);

/* Declare the job over: publish the abort word so every other local PE stops at
 * its next safety sweep. Does not itself terminate the caller. */
void osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status);

#endif /* OSOE_NODE_H */
