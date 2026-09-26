/*
 * Bootstrap environment (§6.1, §6.2).
 *
 * The launcher's variables are Slurm's, as srun sets them, or osoe-launch's
 * mirror of them - the same names with OSOE_ in place of SLURM_ - which a test
 * or a job started by hand sets too. OSOE_PROCID decides which are read. The
 * comments below give the names without their prefix.
 */
#ifndef OSOE_BOOTSTRAP_H
#define OSOE_BOOTSTRAP_H

#include <stdbool.h>
#include <stdint.h>

struct osoe_bootstrap {
    const char *launcher;       /* "SLURM_" or "OSOE_": whose names were read; NULL
                                 * in a bootstrap built by hand, which means OSOE_ */
    bool have_rank;             /* rank is valid, for messages */
    uint32_t rank;              /* PROCID */
    uint32_t world_size;        /* STEP_NUM_TASKS */
    uint32_t node;              /* NODEID */
    uint32_t node_count;        /* STEP_NUM_NODES */
    uint32_t local_id;          /* LOCALID; the lead is 0 */
    uint32_t local_count;       /* L: the length of GTIDS */
    uint32_t *gtids;            /* world rank of each local id */
    uint64_t job;               /* JOB_ID */
    uint32_t step;              /* STEP_ID */
    uint64_t symmetric_size;    /* SHMEM_SYMMETRIC_SIZE, or the default */
    uint64_t no_progress_ns;    /* OSOE_NO_PROGRESS_TIMEOUT, seconds; 0 = never (§3.15) */
    uint64_t heap_base;         /* OSOE_HEAP_BASE, 0 if unset */
    uint64_t heap_shift;        /* OSOE_HEAP_SHIFT: own heap at B + rank x this (§8.2) */
    bool keep_control_segment;  /* OSOE_KEEP_CONTROL_SEGMENT: name kept until finalize */
    bool shm_capacity_warning_only; /* OSOE_SHM_CAPACITY_CHECK=0: report, do not refuse */
    uint32_t collective_fanout; /* OSOE_COLLECTIVE_FANOUT: k of the internode tree (§5.3) */
    uint32_t max_teams;         /* OSOE_MAX_TEAMS: teams in existence job-wide at once (§5.7) */
    uint32_t collective_pipeline;   /* OSOE_COLLECTIVE_PIPELINE: D, a data collective's
                                     * pieces in flight per level of the tree (§5.8) */
    uint16_t port_low, port_high;   /* OSOE_PORT_RANGE: a lead's listener (§6.2) */
    /* OSOE_KEEPALIVE (§3.15): TCP keepalive on every node-pair connection, as
     * seconds idle, seconds between probes, and probes. Zero means the default,
     * so that a bootstrap built by hand, zeroed, keeps it; `keepalive_off` is
     * OSOE_KEEPALIVE=0. */
    uint32_t keepalive_idle_s, keepalive_interval_s, keepalive_count;
    bool keepalive_off;
    const char *node_list;      /* STEP_NODELIST (§6.1), when the job spans nodes */
    bool secured;               /* OSOE_JOB_SECRET is set (§6.2, §10) */
    unsigned char secret[16];   /* its 128 bits, the key every hello's proof is made with */

    /* The specification's own variables (§8). Each is "Any": the value does
     * not matter, only that it is set. SMA_-prefixed spellings are deprecated
     * and still honoured, with the SHMEM_ one controlling when both are set. */
    bool print_version;         /* SHMEM_VERSION */
    bool print_info;            /* SHMEM_INFO */
    bool debug;                 /* SHMEM_DEBUG */
};

/* The default OSOE_KEEPALIVE (§3.15): a node that has vanished is found one to
 * two minutes after its connections go quiet - a minute idle, then six probes
 * ten seconds apart that nobody answers. */
#define OSOE_DEFAULT_KEEPALIVE_IDLE_S     60
#define OSOE_DEFAULT_KEEPALIVE_INTERVAL_S 10
#define OSOE_DEFAULT_KEEPALIVE_COUNT      6

/* The default OSOE_PORT_RANGE (§6.2): below the ephemeral ports. */
#define OSOE_DEFAULT_PORT_LOW  17000
#define OSOE_DEFAULT_PORT_HIGH 26999

/*
 * OSOE_MAX_TEAMS (§5.7): its default, and the range it must lie in - room for
 * at least one team beyond the three predefined, and at most 1,024, where a
 * node's team blocks come to about a megabyte of its control segment and each
 * PE's internal region to about 160 MiB of address space at the default
 * fan-out and OSOE_COLLECTIVE_PIPELINE.
 */
#define OSOE_MAX_TEAMS_DEFAULT 64u
#define OSOE_MAX_TEAMS_LOWEST  4u
#define OSOE_MAX_TEAMS_HIGHEST 1024u

/*
 * OSOE_COLLECTIVE_PIPELINE (§5.8, "Pipelined pieces"): D, how many pieces of a
 * data collective each lead of the internode tree keeps in flight between
 * sending them up and receiving their release. Its default, and the range it
 * must lie in: 1 is one piece at a time, each a whole pass up and down, as
 * before phase 1 step 5 slice 5; each step of D costs every team number's
 * slots in the internal region again (internal-region.h). A PE's region is
 * OSOE_MAX_TEAMS x (fan-out + 1) x D slots of 16 KiB, and a line per counter:
 * 10 MiB at the defaults (64 teams, fan-out 4, D = 2), and at the extremes the
 * ranges allow (1,024 teams, fan-out 32, D = 8) 4.1 GiB, which the shared-memory
 * capacity check refuses at init on a machine without it (node.c).
 */
#define OSOE_COLLECTIVE_PIPELINE_DEFAULT 2u
#define OSOE_COLLECTIVE_PIPELINE_LOWEST  1u
#define OSOE_COLLECTIVE_PIPELINE_HIGHEST 8u

/* The default SHMEM_SYMMETRIC_SIZE: 256 MiB (§4.4). */
#define OSOE_DEFAULT_SYMMETRIC_SIZE (UINT64_C(256) << 20)

/* Read and validate the environment; fatal on anything unusable. */
void osoe_bootstrap_read(struct osoe_bootstrap *b);
void osoe_bootstrap_free(struct osoe_bootstrap *b);

#endif /* OSOE_BOOTSTRAP_H */
