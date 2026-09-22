/*
 * Bootstrap environment (§6.1, §6.2).
 *
 * Phase 1 step 1 reads only the OSOE_ variables, and only a one-node job:
 * Slurm's SLURM_ variables arrive with the environment fixtures of §8.2.
 */
#ifndef OSOE_BOOTSTRAP_H
#define OSOE_BOOTSTRAP_H

#include <stdbool.h>
#include <stdint.h>

struct osoe_bootstrap {
    bool have_rank;             /* rank is valid, for messages */
    uint32_t rank;              /* OSOE_PROCID */
    uint32_t world_size;        /* OSOE_STEP_NUM_TASKS */
    uint32_t node;              /* OSOE_NODEID */
    uint32_t node_count;        /* OSOE_STEP_NUM_NODES */
    uint32_t local_id;          /* OSOE_LOCALID; the lead is 0 */
    uint32_t local_count;       /* L: the length of OSOE_GTIDS */
    uint32_t *gtids;            /* world rank of each local id */
    uint64_t job;               /* OSOE_JOB_ID */
    uint32_t step;              /* OSOE_STEP_ID */
    uint64_t symmetric_size;    /* SHMEM_SYMMETRIC_SIZE, or the default */
    uint64_t no_progress_ns;    /* OSOE_NO_PROGRESS_TIMEOUT, seconds; 0 = never (§3.15) */
    uint64_t heap_base;         /* OSOE_HEAP_BASE, 0 if unset */
    bool watchdog;              /* OSOE_WATCHDOG, or the build default (§3.15) */
    bool keep_control_segment;  /* OSOE_KEEP_CONTROL_SEGMENT: name kept until finalize */
    bool shm_capacity_warning_only; /* OSOE_SHM_CAPACITY_CHECK=0: report, do not refuse */
    uint32_t collective_fanout; /* OSOE_COLLECTIVE_FANOUT: k of the internode tree (§5.3) */

    /* The specification's own variables (§8). Each is "Any": the value does
     * not matter, only that it is set. SMA_-prefixed spellings are deprecated
     * and still honoured, with the SHMEM_ one controlling when both are set. */
    bool print_version;         /* SHMEM_VERSION */
    bool print_info;            /* SHMEM_INFO */
    bool debug;                 /* SHMEM_DEBUG */
};

/* The default SHMEM_SYMMETRIC_SIZE: 256 MiB (§4.4). */
#define OSOE_DEFAULT_SYMMETRIC_SIZE (UINT64_C(256) << 20)

/* Read and validate the environment; fatal on anything unusable. */
void osoe_bootstrap_read(struct osoe_bootstrap *b);
void osoe_bootstrap_free(struct osoe_bootstrap *b);

#endif /* OSOE_BOOTSTRAP_H */
