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
#include "base/epoch.h"
#include "boot.h"

#define OSOE_NODE_MAGIC  UINT64_C(0x65646f6e656f736f)  /* "osoenode", little-endian */
#define OSOE_NODE_LAYOUT 3

enum osoe_phase {
    OSOE_PHASE_EMPTY = 0,       /* slot unused */
    OSOE_PHASE_JOINED,          /* slot fields written; own heap mapped at B */
    OSOE_PHASE_MAPPED,          /* every sibling heap mapped */
    OSOE_PHASE_FINALIZED,       /* last finalize has run */
};

enum { OSOE_HEAP_NAME_MAX = 96 };

struct osoe_pe_slot {
    _Alignas(OSOE_CACHELINE)
    _Atomic uint32_t phase;         /* enum osoe_phase; this PE only */
    _Atomic uint32_t agent_wake;    /* shared futex word: wakes this PE's agent */
    /* The §4.9 watch slot: the block this PE waits on, and its futex word.
     * Written by this PE, read by every writer that touches its heap. On their
     * own cache line, so a notifying writer does not share one with the words
     * this PE's own agent and rendezvous use. */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t watch_block;
    _Atomic uint32_t watch_seq;
    uint32_t rank;
    int32_t pid;                    /* this PE's process id, for the watchdog (§3.15) */
    uint64_t symmetric_size;        /* SHMEM_SYMMETRIC_SIZE as this PE parsed it */
    uint64_t heap_bytes;            /* mapped length of the heap object */
    char heap_name[OSOE_HEAP_NAME_MAX];
};

struct osoe_node_header {
    /* Written by the lead before `ready`; constant afterwards. */
    uint64_t magic;
    uint32_t layout;
    uint32_t header_bytes;
    uint64_t segment_bytes;
    uint64_t job;
    uint32_t step;
    uint32_t node;
    uint32_t session;
    uint32_t local_count;
    uint32_t world_size;
    uint32_t slot_bytes;
    uint64_t slots_offset;
    uint64_t heap_base;             /* B, where every PE maps its own heap (§4.1) */

    /* The §3.14 metric table, described here once it exists. Zero in layout 1;
     * a reader treats metrics_count == 0 as "no table". */
    uint64_t metrics_offset;
    uint64_t metrics_bytes;
    uint32_t metrics_count;
    uint32_t metrics_checksum;

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

    /* SHMEM_TEAM_WORLD's epoch counters (§5.4), each on its own line. */
    struct osoe_epoch world_arrived;
    struct osoe_epoch world_released;
    struct osoe_epoch world_copied;
};

/* Process-private view of the node. */
struct osoe_node {
    char control_name[80];
    struct osoe_node_header *hdr;
    struct osoe_pe_slot *slots;
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
