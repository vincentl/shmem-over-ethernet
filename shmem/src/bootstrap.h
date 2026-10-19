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
    uint32_t progress_helpers;  /* OSOE_PROGRESS_HELPERS: the threads beside the lead's agent
                                 * that own the data wires (owner-map.h; v0.2 step 4) */
    uint32_t collective_pipeline;   /* OSOE_COLLECTIVE_PIPELINE: D, a data collective's
                                     * pieces in flight per level of the tree (§5.8) */
    uint32_t small_collective_bytes;    /* OSOE_SMALL_COLLECTIVE_BYTES: the largest data
                                         * collective through a team's own slots (§5.8) */
    uint32_t collective_shape;  /* OSOE_COLLECTIVE_SHAPE: OSOE_COLLECTIVE_SHAPE_LINK or
                                 * _FIXED (v0.2 step 2, collective-shape.h) */
    uint32_t collective_places; /* OSOE_COLLECTIVE_PLACES: how collect finds its places,
                                 * OSOE_COLLECTIVE_PLACES_* (P30, P31) */
    /* OSOE_LINK_HOP_NS, OSOE_LINK_MESSAGE_NS, OSOE_LINK_BYTES_PER_SECOND (v0.2
     * step 2): the link a large collective's shape is chosen from, set rather
     * than measured - all three or none; 0 when none is set. */
    uint64_t link_hop_ns, link_message_ns, link_bytes_per_second;
    uint16_t port_low, port_high;   /* OSOE_PORT_RANGE: a lead's listener (§6.2) */
    /* OSOE_KEEPALIVE (§3.15): TCP keepalive on every node-pair connection, as
     * seconds idle, seconds between probes, and probes. Zero means the default,
     * so that a bootstrap built by hand, zeroed, keeps it; `keepalive_off` is
     * OSOE_KEEPALIVE=0, or, where it is not set, a job whose every node has a
     * loopback address (progress-host.c), and `keepalive_set` says it was. */
    uint32_t keepalive_idle_s, keepalive_interval_s, keepalive_count;
    bool keepalive_off, keepalive_set;
    const char *node_list;      /* STEP_NODELIST (§6.1), when the job spans nodes */
    bool secured;               /* OSOE_JOB_SECRET is set (§6.2, §10) */
    unsigned char secret[16];   /* its 128 bits, the key every hello's proof is made with */

    /* The specification's own variables (§8). Each is "Any": the value does
     * not matter, only that it is set. SMA_-prefixed spellings are deprecated
     * and still honoured, with the SHMEM_ one controlling when both are set. */
    bool print_version;         /* SHMEM_VERSION */
    bool print_info;            /* SHMEM_INFO */
    bool debug;                 /* SHMEM_DEBUG */

    /* OSOE_TEST_PRINT_LINK=1, a testing hook (T19): rank 0 prints the link it
     * measured at the end of shmem_init, as the OSOE_LINK_* that pin it. */
    bool print_link;
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
 * 36 MiB of address space at the other settings' defaults and 500 nodes
 * (internal-region.h: since the team redesign's step E a number holds counters
 * and small slots, about 27 KiB, and the large slots are each thread's lane). 256 by default
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
 * slots of a header and 16 KiB, and a PE has one per thread record,
 * OSOE_MAX_THREADS + 1 of them: 9.6 MiB at the defaults (fan-out 8, D = 4, 16
 * threads) - and at the extremes the ranges allow (1,024 teams, fan-out 32,
 * D = 8, 1,023 threads, small collectives of 16 KiB) a PE's region is about
 * 5.2 GiB, which the shared-memory capacity check refuses at init on a machine
 * without it (node.c). Since v0.2 step 2 it is the window's capacity: each
 * large collective's window is chosen up to it (collective-shape.h). Its
 * default 4 is the author's (docs/decisions/2026-10-06.md item 5): the Pi's
 * 1 MiB reduction at k = 2 took 33.5 ms at D = 2 and 23.0 at D = 4.
 */
#define OSOE_COLLECTIVE_PIPELINE_DEFAULT 4u
#define OSOE_COLLECTIVE_PIPELINE_LOWEST  1u
#define OSOE_COLLECTIVE_PIPELINE_HIGHEST 8u

/*
 * OSOE_SMALL_COLLECTIVE_BYTES (§5.8; the team redesign's step E, E1 and E2):
 * the largest data collective that moves through a team number's own small
 * slots, in one piece with no grant and no lane (internal-region.h); a larger
 * one goes through the lanes of the threads that lead it. Its default, and the
 * range it must lie in: at 1 KiB a team number costs a PE about 27 KiB, at 128
 * bytes about 11, and at 16 KiB - a lane's piece, past which nothing is small -
 * its small rows hold as much as a lane's row of one slot.
 */
#define OSOE_SMALL_COLLECTIVE_BYTES_DEFAULT 1024u
#define OSOE_SMALL_COLLECTIVE_BYTES_LOWEST  128u
#define OSOE_SMALL_COLLECTIVE_BYTES_HIGHEST 16384u

/*
 * OSOE_COLLECTIVE_SHAPE (v0.2 step 2; `tree` since step 3): `link`, the
 * default, chooses each large reduction's and broadcast's shape from the link
 * (collective-shape.h) - a tree's fan-out and window, or for a reduction
 * Träff's circulant reduce-scatter and allgather where it costs less; `tree`
 * chooses the same way among the trees alone, Träff's turned off by name;
 * `fixed` runs every one at the fan-out and the pipeline as set, the 0.1.0
 * tag's shape.
 */
#define OSOE_COLLECTIVE_SHAPE_LINK  0u
#define OSOE_COLLECTIVE_SHAPE_FIXED 1u
#define OSOE_COLLECTIVE_SHAPE_TREE  2u

/* A collective shape as the setting spells it. */
static inline const char *osoe_collective_shape_name(uint32_t shape)
{
    return shape == OSOE_COLLECTIVE_SHAPE_FIXED  ? "fixed"
           : shape == OSOE_COLLECTIVE_SHAPE_TREE ? "tree"
                                                 : "link";
}

/*
 * OSOE_COLLECTIVE_PLACES (P30, P31 of docs/prompts/v0.2-open-items.md): how a
 * collect of differing blocks on a team spanning nodes finds its places
 * (collectives.c, collect_varying). `link`, the default, takes the way the link
 * makes cheapest among those the team allows (collective-shape.h,
 * osoe_places_choose) - each node's total through the tree, its exchange, on no
 * more than 16 nodes (P35), or its scan where every node holds one run of the
 * team's ranks, the members' vector
 * elsewhere - and each node's total through the tree where no link is known;
 * the others force one way, for the tests and the measurements, where the team
 * allows it: `members` the members' vector,
 * `totals` a reduction of each node's total (P29), `scan` a scan of the nodes'
 * totals among the leads and a gather by ranges (P30), `exchange` a direct
 * exchange of the nodes' totals among the leads (P33). A way the team does not
 * allow falls back - `scan` to `totals` where the runs meet the node set's
 * positions in neither order, `exchange` to `totals` where the team spans more
 * nodes than the exchange has room for, `totals` to `members` where some node's
 * members are not one run - as `link` does.
 */
#define OSOE_COLLECTIVE_PLACES_LINK     0u
#define OSOE_COLLECTIVE_PLACES_MEMBERS  1u
#define OSOE_COLLECTIVE_PLACES_TOTALS   2u
#define OSOE_COLLECTIVE_PLACES_SCAN     3u
#define OSOE_COLLECTIVE_PLACES_EXCHANGE 4u

/* collect's way to its places as the setting spells it. */
static inline const char *osoe_collective_places_name(uint32_t places)
{
    return places == OSOE_COLLECTIVE_PLACES_MEMBERS    ? "members"
           : places == OSOE_COLLECTIVE_PLACES_TOTALS   ? "totals"
           : places == OSOE_COLLECTIVE_PLACES_SCAN     ? "scan"
           : places == OSOE_COLLECTIVE_PLACES_EXCHANGE ? "exchange"
                                                       : "link";
}

/*
 * OSOE_LINK_* (v0.2 step 2): the link's three numbers set by hand, for tests
 * and for a user who knows their fabric, in place of the measurement. The
 * ranges keep the chooser's integer arithmetic inside 64 bits: a hop of up to
 * ten seconds, a message cost of up to one, from one byte a second to ten
 * terabytes.
 */
#define OSOE_LINK_HOP_NS_HIGHEST           UINT64_C(10000000000)
#define OSOE_LINK_MESSAGE_NS_HIGHEST       UINT64_C(1000000000)
#define OSOE_LINK_BYTES_PER_SECOND_HIGHEST UINT64_C(10000000000000)

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

/*
 * OSOE_PROGRESS_HELPERS (docs/design/progress-helpers-proposal.md §3.8): how
 * many threads of the node lead's process own the data wires beside its agent,
 * which then keeps the control wires alone (owner-map.h). 0, the default, is
 * one thread a node, as before helpers existed. At most 64: an owner is a bit
 * of a PE's mask of owners to wake (progress-host.c). It shapes the connection
 * area and the node segment's owner records, so a node's PEs must agree, and
 * the node's own rendezvous checks that they do (node.c); nodes need not.
 */
#define OSOE_PROGRESS_HELPERS_HIGHEST 64u

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
