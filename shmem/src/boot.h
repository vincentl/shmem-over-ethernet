/*
 * Bootstrap environment (§6.1, §6.2).
 *
 * Phase 1 step 1 reads only the OSOE_ variables, and only a one-node job:
 * Slurm's SLURM_ variables arrive with the environment fixtures of §8.2.
 */
#ifndef OSOE_BOOT_H
#define OSOE_BOOT_H

#include <stdbool.h>
#include <stdint.h>

struct osoe_boot {
    bool have_rank;             /* rank is valid, for messages */
    uint32_t rank;              /* OSOE_PROCID */
    uint32_t world_size;        /* OSOE_STEP_NUM_TASKS */
    uint32_t node;              /* OSOE_NODEID */
    uint32_t local_id;          /* OSOE_LOCALID; the lead is 0 */
    uint32_t local_count;       /* L: the length of OSOE_GTIDS */
    uint32_t *gtids;            /* world rank of each local id */
    uint64_t job;               /* OSOE_JOB_ID */
    uint32_t step;              /* OSOE_STEP_ID */
    uint64_t symmetric_size;    /* SHMEM_SYMMETRIC_SIZE, or the default */
    uint64_t timeout_ns;        /* OSOE_BOOTSTRAP_TIMEOUT, seconds in the environment */
    uint64_t heap_base;         /* OSOE_HEAP_BASE, 0 if unset */
    bool watchdog;              /* OSOE_WATCHDOG, or the build default (§3.15) */

    /* The specification's own variables (§8). Each is "Any": the value does
     * not matter, only that it is set. SMA_-prefixed spellings are deprecated
     * and still honoured, with the SHMEM_ one controlling when both are set. */
    bool print_version;         /* SHMEM_VERSION */
    bool print_info;            /* SHMEM_INFO */
    bool debug;                 /* SHMEM_DEBUG */
};

/* The default SHMEM_SYMMETRIC_SIZE: 256 MiB (§4.4). */
#define OSOE_DEFAULT_SYMMETRIC_SIZE (UINT64_C(256) << 20)
#define OSOE_DEFAULT_BOOTSTRAP_TIMEOUT_S 60

/* Read and validate the environment; fatal on anything unusable. */
void osoe_boot_read(struct osoe_boot *b);
void osoe_boot_free(struct osoe_boot *b);

#endif /* OSOE_BOOT_H */
