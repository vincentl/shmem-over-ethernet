/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
    uint64_t launch;            /* which launch of these ids: SLURM_RESTART_COUNT under
                                 * Slurm, OSOE_LAUNCH otherwise; 0 when not given */
    uint64_t symmetric_size;    /* SHMEM_SYMMETRIC_SIZE, or the default */
    uint64_t no_progress_ns;    /* OSOE_NO_PROGRESS_TIMEOUT, seconds; 0 = never (§3.15) */
    uint64_t heap_base;         /* OSOE_HEAP_BASE, 0 if unset */
    uint64_t heap_shift;        /* OSOE_HEAP_SHIFT: own heap at B + rank x this (§8.2) */
    bool keep_control_segment;  /* OSOE_KEEP_CONTROL_SEGMENT: name kept until finalize */
    bool shm_capacity_warning_only; /* OSOE_SHM_CAPACITY_CHECK=0: report, do not refuse */
    uint32_t collective_fanout; /* OSOE_COLLECTIVE_FANOUT: k of the internode tree (§5.3) */
    uint32_t max_teams;         /* OSOE_MAX_TEAMS: teams one PE may be in at once (§5.7) */
    uint32_t max_contexts;      /* OSOE_MAX_CONTEXTS: contexts a PE may have at once, the
                                 * default one included (§4.11; phase 1 step 8) */
    uint32_t team_blocks;       /* OSOE_TEAM_BLOCKS: the node's pool of team blocks, the
                                 * three predefined teams' included (§5.7; step D) */
    uint32_t max_threads;       /* OSOE_MAX_THREADS: live threads of a PE that have called
                                 * the library, the main one included (§3.14) */
    uint32_t collective_pipeline;   /* OSOE_COLLECTIVE_PIPELINE: D, a data collective's
                                     * pieces in flight per level of the tree (§5.8) */
    uint32_t small_collective_bytes;    /* OSOE_SMALL_COLLECTIVE_BYTES: the largest data
                                         * collective through a team's own slots (§5.8) */
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
 * OSOE_MAX_TEAMS (§5.7): its default, and the range it must lie in - the
 * numbers one PE may hold at once, room for at least one team beyond the three
 * predefined, and at most 1,024, where each PE's internal region comes to about
 * 18 MiB of address space at the other settings' defaults (internal-region.h:
 * since the team redesign's step E a number holds counters and small slots,
 * about 15 KiB, and the large slots are each thread's lane). 256 by default
 * since step E made a number that cheap (the author, 2026-10-02: "256 max teams
 * is more than sufficient"); 64 before it.
 * A node's team blocks are OSOE_TEAM_BLOCKS's (below), since the team
 * redesign's step D.
 */
#define OSOE_MAX_TEAMS_DEFAULT 256u
#define OSOE_MAX_TEAMS_LOWEST  4u
#define OSOE_MAX_TEAMS_HIGHEST 1024u

/*
 * OSOE_MAX_CONTEXTS (§4.11; phase 1 step 8): how many contexts a PE may have
 * at once, SHMEM_CTX_DEFAULT counted, and so the size of each PE's pool of
 * context blocks in the node segment (node.h). Its default, and the range it
 * must lie in: at least the default context, and at most what a nonblocking
 * get's token can name (OSOE_LANDING_CONTEXT_MASK) - a block is a cache line,
 * so the ceiling costs a PE half a megabyte of segment.
 */
#define OSOE_MAX_CONTEXTS_DEFAULT 64u
#define OSOE_MAX_CONTEXTS_LOWEST  1u
#define OSOE_MAX_CONTEXTS_HIGHEST 4096u

/*
 * OSOE_TEAM_BLOCKS (§5.7; the team redesign's step D,
 * docs/design/team-resources-proposal.md D3): how many teams a node can hold
 * at once - its pool of team blocks in the control segment, the three
 * predefined teams' blocks among them. Its default, and the range it must lie
 * in: at 1,024 a pool is about a megabyte of a node's segment, and the
 * ceiling, 65,536, what a block index of the map can name with room to spare.
 */
#define OSOE_TEAM_BLOCKS_DEFAULT 1024u
#define OSOE_TEAM_BLOCKS_LOWEST  8u
#define OSOE_TEAM_BLOCKS_HIGHEST 65536u
/* The summary words of the pool's posted bitmap (node.h), a bit each for 64
 * blocks: as many as the largest pool needs. */
#define OSOE_TEAM_POSTED_SUMMARY_WORDS (OSOE_TEAM_BLOCKS_HIGHEST / 64 / 64)

/*
 * OSOE_COLLECTIVE_PIPELINE (§5.8, "Pipelined pieces"): D, how many pieces of a
 * data collective each lead of the internode tree keeps in flight between
 * sending them up and receiving their release. Its default, and the range it
 * must lie in: 1 is one piece at a time, each a whole pass up and down, as
 * before phase 1 step 5 slice 5; each step of D costs every lane's slots in
 * the internal region again (internal-region.h). A lane is (fan-out + 1) x D
 * slots of 16 KiB, and a PE has one per thread record, OSOE_MAX_THREADS + 1 of
 * them: 2.7 MiB at the defaults (fan-out 4, D = 2, 16 threads) - and at the
 * extremes the ranges allow (1,024 teams, fan-out 32, D = 8, 1,023 threads,
 * small collectives of 16 KiB) a PE's region is about 5.2 GiB, which the
 * shared-memory capacity check refuses at init on a machine without it
 * (node.c).
 */
#define OSOE_COLLECTIVE_PIPELINE_DEFAULT 2u
#define OSOE_COLLECTIVE_PIPELINE_LOWEST  1u
#define OSOE_COLLECTIVE_PIPELINE_HIGHEST 8u

/*
 * OSOE_SMALL_COLLECTIVE_BYTES (§5.8; the team redesign's step E, E1 and E2):
 * the largest data collective that moves through a team number's own small
 * slots, in one piece with no grant and no lane (internal-region.h); a larger
 * one goes through the lanes of the threads that lead it. Its default, and the
 * range it must lie in: at 1 KiB a team number costs a PE about 15 KiB, at 128
 * bytes about 6, and at 16 KiB - a lane's piece, past which nothing is small -
 * its small rows hold as much as a lane's row of one slot.
 */
#define OSOE_SMALL_COLLECTIVE_BYTES_DEFAULT 1024u
#define OSOE_SMALL_COLLECTIVE_BYTES_LOWEST  128u
#define OSOE_SMALL_COLLECTIVE_BYTES_HIGHEST 16384u

/*
 * OSOE_MAX_THREADS: the application threads a PE may have alive that have
 * called the library, the main one included - not threads in it at once. A
 * thread takes a counter block and a thread record of the node segment at its
 * first counted call and gives them back only when it exits (metrics.c,
 * osoe_metrics_block and release_block), so a worker that made one call and
 * lives on holds its block: the main thread and 15 such workers, calling
 * strictly one after another, fill the 16 (the release review, C3). The
 * agent's block is one more. 16 by default: at 8 with the agent inside, SOS's
 * eight-thread programs ended the job (phase 1 step 8 slice 2, B1); and 16
 * rather than the node's CPU count, which a four-core Pi
 * would make 4 - threads oversubscribe cores freely, and a default that moves
 * with the machine is one a program cannot rely on. Each costs about 768 bytes
 * of the node segment a PE (decisions, 2026-09-26-evening). At most
 * OSOE_MAX_THREADS_HIGHEST - 1, node.h's bound on the active bitmap less the
 * agent. Read and checked by every PE, and compared between a node's PEs, since
 * the lead lays out every PE's blocks by its own (node.c; the round-three
 * review's R8).
 */
#define OSOE_MAX_THREADS_DEFAULT 16u

/* The default SHMEM_SYMMETRIC_SIZE: 256 MiB (§4.4). */
#define OSOE_DEFAULT_SYMMETRIC_SIZE (UINT64_C(256) << 20)

/* Read and validate the environment; fatal on anything unusable. */
void osoe_bootstrap_read(struct osoe_bootstrap *b);

/*
 * Called by osoe_bootstrap_read, if set, as soon as `b` holds the PE's
 * identity - its job, step, node and local id - and before anything else of the
 * environment is read: init arms the PE's refusal there (node.h), so that from
 * then on a PE refusing its own environment tells its node. Unset in the tests
 * below the library.
 */
extern void (*osoe_bootstrap_identified)(const struct osoe_bootstrap *b);
void osoe_bootstrap_free(struct osoe_bootstrap *b);

#endif /* OSOE_BOOTSTRAP_H */
