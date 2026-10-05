/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "collective-tree.h"
#include "internode.h"
#include "base/environment.h"
#include "base/hostlist.h"
#include "base/size.h"
#include "bootstrap.h"
#include "node.h"
#include "fatal.h"
#include "watch-range.h"

/*
 * A specification environment variable (§8), whose deprecated SMA_ spelling is
 * still honoured: "If both SHMEM_- and SMA_-prefixed environment variables are
 * set, then the value in the SHMEM_-prefixed environment variable establishes
 * the controlling value." These four are all "Any", so being set is the whole
 * value and the rule reduces to checking SHMEM_ first - but it is written as
 * the rule rather than as the shortcut, because SHMEM_SYMMETRIC_SIZE has a
 * value and will need it.
 */
static const char *spec_env(const char *suffix)
{
    char name[64];
    snprintf(name, sizeof name, "SHMEM_%s", suffix);
    const char *v = getenv(name);
    if (v)
        return v;
    snprintf(name, sizeof name, "SMA_%s", suffix);
    return getenv(name);
}

/*
 * The launcher's names (§6.1). srun sets Slurm's; osoe-launch sets the same
 * names with OSOE_ in place of SLURM_, and so can a test or a job started by
 * hand. OSOE_PROCID decides which are read: set, every one with OSOE_; unset
 * inside a step of a Slurm job, every one with SLURM_ - never a mixture, which
 * could pair one launcher's rank with the other's node.
 */
static const char *launcher = "OSOE_";

static const char *launcher_name(const char *suffix)
{
    static char name[40];
    snprintf(name, sizeof name, "%s%s", launcher, suffix);
    return name;
}

static uint32_t need_u32(const char *name)
{
    uint32_t v;
    switch (osoe_environment_u32(name, &v)) {
    case OSOE_ENVIRONMENT_OK:
        return v;
    case OSOE_ENVIRONMENT_UNSET:
        osoe_fatal("%s is not set", name);
    case OSOE_ENVIRONMENT_INVALID:
    default:
        osoe_fatal("%s='%s' is not a decimal number", name, getenv(name));
    }
}

void (*osoe_bootstrap_identified)(const struct osoe_bootstrap *b);

void osoe_bootstrap_read(struct osoe_bootstrap *b)
{
    *b = (struct osoe_bootstrap){ 0 };

    launcher = "OSOE_";
    if (!getenv("OSOE_PROCID")) {
        if (getenv("SLURM_PROCID") && getenv("SLURM_STEP_ID"))
            launcher = "SLURM_";
        else if (getenv("SLURM_JOB_ID"))
            osoe_fatal("inside Slurm job %s but not in a step of it: start the program "
                       "with srun (design §6.1)", getenv("SLURM_JOB_ID"));
        else
            osoe_fatal("no launcher environment: neither OSOE_PROCID nor SLURM_PROCID is "
                       "set. Start programs with srun or osoe-launch, or set the OSOE_ "
                       "bootstrap variables of design §6.2");
    }
    const char *prefix = launcher;
    b->launcher = launcher;

    b->rank = need_u32(launcher_name("PROCID"));
    b->have_rank = true;
    b->world_size = need_u32(launcher_name("STEP_NUM_TASKS"));
    b->node = need_u32(launcher_name("NODEID"));
    b->node_count = need_u32(launcher_name("STEP_NUM_NODES"));
    b->local_id = need_u32(launcher_name("LOCALID"));
    b->step = need_u32(launcher_name("STEP_ID"));
    /* Steps Slurm runs for itself - a batch script's, an interactive
     * allocation's, the extern step - have ids of 0xfffffff0 and up, and each
     * component of a heterogeneous job reads as a job of its own. Neither has
     * been captured (§8.2), and a program run in one would be a job of the
     * wrong shape, so both are refused until they are. */
    if (strcmp(prefix, "SLURM_") == 0 && b->step >= UINT32_C(0xfffffff0))
        osoe_fatal("SLURM_STEP_ID=%u is one of Slurm's own steps, not one srun started: "
                   "start the program with srun (design §6.1)", b->step);
    if (strcmp(prefix, "SLURM_") == 0 && getenv("SLURM_HET_SIZE"))
        osoe_fatal("a heterogeneous Slurm job (SLURM_HET_SIZE=%s) is not supported yet "
                   "(design §6.1)", getenv("SLURM_HET_SIZE"));
    switch (osoe_environment_u64(launcher_name("JOB_ID"), &b->job)) {
    case OSOE_ENVIRONMENT_OK: break;
    case OSOE_ENVIRONMENT_UNSET: osoe_fatal("%sJOB_ID is not set", prefix);
    default: osoe_fatal("%sJOB_ID='%s' is not a decimal number", prefix,
                        getenv(launcher_name("JOB_ID")));
    }
    /*
     * Which launch of these ids this is. A requeued Slurm job keeps its job id
     * and starts its steps from 0 again, and an osoe-launch job's id is its
     * launcher's pid, so a failed start's leftover can carry this start's very
     * names: a node-mate reading its `failed` or its `ready` would end, or
     * join a dead node, before this start's lead reclaimed it (the adversarial
     * read of 2026-10-03). The segment records its launch, and a node-mate
     * takes one from another launch for the leftover it is (node.c). Slurm
     * counts its requeues in SLURM_RESTART_COUNT, set from the first; the
     * launcher sets OSOE_LAUNCH; either may be unset, and reads as 0.
     */
    const char *launch_name = strcmp(prefix, "SLURM_") == 0 ? "SLURM_RESTART_COUNT"
                                                            : "OSOE_LAUNCH";
    if (osoe_environment_u64(launch_name, &b->launch) == OSOE_ENVIRONMENT_INVALID)
        osoe_fatal("%s='%s' is not a decimal number", launch_name, getenv(launch_name));
    /* Who this PE is, so that every refusal below can tell its node (node.h).
     * Everything before is the identity itself, and nothing can be told. */
    if (osoe_bootstrap_identified)
        osoe_bootstrap_identified(b);

    if (b->node_count == 0 || b->node >= b->node_count)
        osoe_fatal("%sNODEID=%u is not below %sSTEP_NUM_NODES=%u", prefix, b->node, prefix,
                   b->node_count);
    if (b->world_size == 0 || b->rank >= b->world_size)
        osoe_fatal("%sPROCID=%u is not below %sSTEP_NUM_TASKS=%u", prefix, b->rank, prefix,
                   b->world_size);

    const char *gtids = getenv(launcher_name("GTIDS"));
    if (!gtids)
        osoe_fatal("%sGTIDS is not set", prefix);
    /*
     * Sized by the list, which is this node's L ranks, and not by the job: at
     * 4,096 x 256 a world-sized array is 4 MiB of address space per PE for 1 KiB
     * of data (second review, D7). One pass counts the commas; the parser then
     * refuses anything that is not a list of that many numbers.
     */
    size_t entries = 1;
    for (const char *c = gtids; *c; c++)
        entries += *c == ',';
    if (entries > b->world_size)
        osoe_fatal("%sGTIDS='%s' names %zu ranks in a job of %u", prefix, gtids, entries,
                   b->world_size);
    /* The bound §6.2 set when the job map held each rank's local index in 16
     * bits. The map has been runs of 32-bit fields since (rank-map.h), and the
     * bound stays - far above any node, and what the README states - rather
     * than let a node meet a size nothing has run at. */
    if (entries > UINT16_MAX)
        osoe_fatal("%sGTIDS names %zu ranks on one node; osoe supports at most %u", prefix,
                   entries, (unsigned)UINT16_MAX);
    b->gtids = malloc(sizeof *b->gtids * entries);
    if (!b->gtids)
        osoe_fatal("out of memory");
    size_t n;
    if (osoe_parse_u32_list(gtids, b->gtids, entries, &n) != 0)
        osoe_fatal("%sGTIDS='%s' is not a list of at most %u ranks", prefix, gtids,
                   b->world_size);
    b->local_count = (uint32_t)n;

    if (b->local_id >= b->local_count || b->gtids[b->local_id] != b->rank)
        osoe_fatal("%sLOCALID=%u does not name %sPROCID=%u in %sGTIDS='%s'", prefix,
                   b->local_id, prefix, b->rank, prefix, gtids);
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (b->gtids[i] >= b->world_size)
            osoe_fatal("%sGTIDS='%s' names rank %u, not below %u", prefix, gtids, b->gtids[i],
                       b->world_size);
        for (uint32_t j = 0; j < i; j++)
            if (b->gtids[j] == b->gtids[i])
                osoe_fatal("%sGTIDS='%s' names rank %u twice", prefix, gtids, b->gtids[i]);
    }


    const char *size = spec_env("SYMMETRIC_SIZE");
    b->symmetric_size = OSOE_DEFAULT_SYMMETRIC_SIZE;
    if (size && osoe_parse_size(size, &b->symmetric_size) != 0)
        osoe_fatal("SHMEM_SYMMETRIC_SIZE='%s' is invalid: expected a non-negative "
                   "decimal number with an optional k, m, g or t multiplier", size);
    /* A zero heap is syntactically valid (§8 does not say otherwise) but never
     * useful: every shmem_malloc then fails on every PE, and the first
     * dereference of the NULL it returns segfaults far from the cause
     * (conformance review F9 / omissions 11). Usually it is a script's
     * unset-variable arithmetic gone wrong, so say so here instead. */
    if (size && b->symmetric_size == 0)
        osoe_fatal("SHMEM_SYMMETRIC_SIZE='%s' is 0; a job with no symmetric heap cannot "
                   "run any OpenSHMEM program", size);
    /* The §4.9 watch word names an offset within a region, and stops filtering
     * above what it can address: a watch in a larger heap would saturate to the
     * whole region and wake on every write. Refuse the size rather than lose
     * that silently. No real job is near this - it is 4 TiB per PE. */
    if (b->symmetric_size > OSOE_WATCH_MAX_REGION)
        osoe_fatal("SHMEM_SYMMETRIC_SIZE='%s' is %llu bytes; the largest supported "
                   "symmetric heap is %llu bytes per PE (design §4.9)", size,
                   (unsigned long long)b->symmetric_size,
                   (unsigned long long)OSOE_WATCH_MAX_REGION);

    /*
     * The no-progress rule (§3.15, base/stall-watch.h): a wait gives up only
     * when what it watches has not moved for this long. 0, the default, means
     * never. It replaced OSOE_BOOTSTRAP_TIMEOUT, a bound on waiting at all,
     * which also bounded every internode wait and so failed a healthy quiet
     * that simply took long (second review, D3). A job that still sets the old
     * name is told, once, rather than left believing it is bounded.
     */
    uint32_t no_progress_s = 0;
    if (osoe_environment_u32("OSOE_NO_PROGRESS_TIMEOUT", &no_progress_s) ==
        OSOE_ENVIRONMENT_INVALID)
        osoe_fatal("OSOE_NO_PROGRESS_TIMEOUT='%s' is not a number of seconds",
                   getenv("OSOE_NO_PROGRESS_TIMEOUT"));
    b->no_progress_ns = (uint64_t)no_progress_s * UINT64_C(1000000000);
    if (getenv("OSOE_BOOTSTRAP_TIMEOUT") && b->local_id == 0 && b->rank == 0)
        fprintf(stderr, "osoe: OSOE_BOOTSTRAP_TIMEOUT is retired and ignored; "
                        "OSOE_NO_PROGRESS_TIMEOUT bounds a wait that stops making "
                        "progress, and by default nothing does\n");
    if ((getenv("OSOE_WATCHDOG") || getenv("OSOE_LIVENESS")) && b->local_id == 0 &&
        b->rank == 0)
        fprintf(stderr, "osoe: OSOE_WATCHDOG and OSOE_LIVENESS are retired and ignored; "
                        "a lead's agent watches the node's other PEs with pidfds, and each "
                        "other agent its lead, in every build (design §3.15)\n");

    /*
     * §3.14: the control segment's name is removed as soon as every local PE
     * has it open, like every other object, so that no death can strand it.
     * This keeps the name until finalize instead, so a crashed job's segment
     * survives for post-mortem reading - and is then the one thing a crash can
     * leave behind, for osoe-stat --gc to remove.
     */
    uint32_t keep = 0;
    if (osoe_environment_u32("OSOE_KEEP_CONTROL_SEGMENT", &keep) == OSOE_ENVIRONMENT_INVALID ||
        keep > 1)
        osoe_fatal("OSOE_KEEP_CONTROL_SEGMENT='%s' is not 0 or 1",
                   getenv("OSOE_KEEP_CONTROL_SEGMENT"));
    b->keep_control_segment = keep != 0;

    /*
     * §6.2's /dev/shm capacity check refuses a request larger than the free
     * space. Heaps are demand-paged, so a test machine with a small /dev/shm
     * can run a job that asks for more than it touches; 0 turns the refusal
     * into a report on stderr, for that and nothing else. Stored as "warning
     * only" so that a bootstrap built by hand, zeroed, keeps the default.
     */
    uint32_t capacity_check = 1;
    if (osoe_environment_u32("OSOE_SHM_CAPACITY_CHECK", &capacity_check) ==
            OSOE_ENVIRONMENT_INVALID || capacity_check > 1)
        osoe_fatal("OSOE_SHM_CAPACITY_CHECK='%s' is not 0 or 1",
                   getenv("OSOE_SHM_CAPACITY_CHECK"));
    b->shm_capacity_warning_only = capacity_check == 0;

    /*
     * The internode tree's fan-out (§5.3, collective-tree.h). Every node lead
     * must agree, and the job rendezvous checks that they do (§6.2): a node
     * with a different k computes different children, waits for arrivals
     * that are sent elsewhere, and hangs the job without a word.
     */
    b->collective_fanout = osoe_internode_fanout_default();
    if (osoe_environment_u32("OSOE_COLLECTIVE_FANOUT", &b->collective_fanout) ==
            OSOE_ENVIRONMENT_INVALID ||
        b->collective_fanout < 2 || b->collective_fanout > OSOE_COLLECTIVE_FANOUT_MAX)
        osoe_fatal("OSOE_COLLECTIVE_FANOUT='%s' is not between 2 and %u",
                   getenv("OSOE_COLLECTIVE_FANOUT"), (unsigned)OSOE_COLLECTIVE_FANOUT_MAX);

    /*
     * How many teams one PE may be in at once, the predefined ones included
     * (§5.7; the team redesign's step D, D8): a team's number is each PE's own
     * and below it. It sizes each PE's published epochs, each local PE's row of
     * the node's map of team names, and the internal region's per-number
     * blocks - so every node must agree, and the job rendezvous checks that
     * they do beside the fan-out (§6.2). A node with a different count would
     * lay its internal region out differently, and a put of one team's piece
     * would land in another's slot.
     */
    b->max_teams = OSOE_MAX_TEAMS_DEFAULT;
    if (osoe_environment_u32("OSOE_MAX_TEAMS", &b->max_teams) == OSOE_ENVIRONMENT_INVALID ||
        b->max_teams < OSOE_MAX_TEAMS_LOWEST || b->max_teams > OSOE_MAX_TEAMS_HIGHEST)
        osoe_fatal("OSOE_MAX_TEAMS='%s' is not between %u and %u", getenv("OSOE_MAX_TEAMS"),
                   OSOE_MAX_TEAMS_LOWEST, OSOE_MAX_TEAMS_HIGHEST);

    /*
     * How many contexts a PE may have at once (§4.11; phase 1 step 8): it
     * sizes each PE's pool of context blocks in the node segment, which the
     * node's lead lays out, so a node's PEs must agree, and the node's own
     * rendezvous checks that they do (node.c). Not between nodes: no other
     * node indexes the pool.
     */
    b->max_contexts = OSOE_MAX_CONTEXTS_DEFAULT;
    if (osoe_environment_u32("OSOE_MAX_CONTEXTS", &b->max_contexts) == OSOE_ENVIRONMENT_INVALID ||
        b->max_contexts < OSOE_MAX_CONTEXTS_LOWEST || b->max_contexts > OSOE_MAX_CONTEXTS_HIGHEST)
        osoe_fatal("OSOE_MAX_CONTEXTS='%s' is not between %u and %u", getenv("OSOE_MAX_CONTEXTS"),
                   OSOE_MAX_CONTEXTS_LOWEST, OSOE_MAX_CONTEXTS_HIGHEST);

    /*
     * How many teams a node can hold in a session (§5.7; step D; at once since
     * its second part gives a destroyed team's block back): it sizes the
     * node's pool of team blocks, which the node's lead lays out, so a node's
     * PEs must agree, and the node's own rendezvous checks that they do
     * (node.c). Not between nodes: a block index is its node's own, and the
     * wire names a team's block by its name (am.h).
     */
    b->team_blocks = OSOE_TEAM_BLOCKS_DEFAULT;
    if (osoe_environment_u32("OSOE_TEAM_BLOCKS", &b->team_blocks) == OSOE_ENVIRONMENT_INVALID ||
        b->team_blocks < OSOE_TEAM_BLOCKS_LOWEST || b->team_blocks > OSOE_TEAM_BLOCKS_HIGHEST)
        osoe_fatal("OSOE_TEAM_BLOCKS='%s' is not between %u and %u", getenv("OSOE_TEAM_BLOCKS"),
                   OSOE_TEAM_BLOCKS_LOWEST, OSOE_TEAM_BLOCKS_HIGHEST);

    /* OSOE_MAX_THREADS (bootstrap.h): here, by every PE, where until the
     * round-three review only the lead read it, as it laid out the segment - a
     * bad value given to every PE then ended the lead by name and left the
     * others waiting, and one given to a single non-lead was never read. A
     * malformed value is an error, never a default (base/environment.h). */
    b->max_threads = OSOE_MAX_THREADS_DEFAULT;
    if (osoe_environment_u32("OSOE_MAX_THREADS", &b->max_threads) == OSOE_ENVIRONMENT_INVALID ||
        b->max_threads == 0 || b->max_threads > OSOE_MAX_THREADS_HIGHEST - 1)
        osoe_fatal("OSOE_MAX_THREADS='%s' is not a number of threads from 1 to %u",
                   getenv("OSOE_MAX_THREADS"), (unsigned)OSOE_MAX_THREADS_HIGHEST - 1);

    /*
     * How many pieces of a data collective each lead of the internode tree
     * keeps in flight (§5.8, "Pipelined pieces"). It fixes the internal
     * region's offsets - D slots per child position and D release slots in
     * every team number's block (internal-region.h) - so every node must
     * agree, and the job rendezvous checks that they do beside the fan-out
     * (§6.2): a node with another D would put a piece where its parent keeps
     * another child's, or another piece.
     */
    b->collective_pipeline = OSOE_COLLECTIVE_PIPELINE_DEFAULT;
    if (osoe_environment_u32("OSOE_COLLECTIVE_PIPELINE", &b->collective_pipeline) ==
            OSOE_ENVIRONMENT_INVALID ||
        b->collective_pipeline < OSOE_COLLECTIVE_PIPELINE_LOWEST ||
        b->collective_pipeline > OSOE_COLLECTIVE_PIPELINE_HIGHEST)
        osoe_fatal("OSOE_COLLECTIVE_PIPELINE='%s' is not between %u and %u",
                   getenv("OSOE_COLLECTIVE_PIPELINE"), OSOE_COLLECTIVE_PIPELINE_LOWEST,
                   OSOE_COLLECTIVE_PIPELINE_HIGHEST);

    /*
     * The largest data collective that moves through a team number's own small
     * slots, in one piece (the team redesign's step E, E1 and E2): a larger one
     * goes through the lanes of the threads that lead it. It sizes every team
     * number's small rows (internal-region.h) and decides every data
     * collective's class, which every lead must decide alike, so every node
     * must agree, and the job rendezvous checks that they do beside the
     * pipeline's depth.
     */
    b->small_collective_bytes = OSOE_SMALL_COLLECTIVE_BYTES_DEFAULT;
    if (osoe_environment_u32("OSOE_SMALL_COLLECTIVE_BYTES", &b->small_collective_bytes) ==
            OSOE_ENVIRONMENT_INVALID ||
        b->small_collective_bytes < OSOE_SMALL_COLLECTIVE_BYTES_LOWEST ||
        b->small_collective_bytes > OSOE_SMALL_COLLECTIVE_BYTES_HIGHEST)
        osoe_fatal("OSOE_SMALL_COLLECTIVE_BYTES='%s' is not between %u and %u",
                   getenv("OSOE_SMALL_COLLECTIVE_BYTES"), OSOE_SMALL_COLLECTIVE_BYTES_LOWEST,
                   OSOE_SMALL_COLLECTIVE_BYTES_HIGHEST);

    /*
     * The ports a node lead may listen on (§6.2's "configurable range"). Each
     * lead binds the first free port of a candidate sequence hashed from (job,
     * step, node) inside it, and another lead finds it by trying the same
     * sequence (listener.h). The default sits below Linux's ephemeral range
     * (32768-60999), so an outgoing connection's local port never takes a
     * candidate, and above the privileged ports.
     */
    b->port_low = OSOE_DEFAULT_PORT_LOW;
    b->port_high = OSOE_DEFAULT_PORT_HIGH;
    const char *range = getenv("OSOE_PORT_RANGE");
    if (range) {
        unsigned low, high;
        int used = 0;
        if (sscanf(range, "%u-%u%n", &low, &high, &used) != 2 || range[used] != '\0' ||
            low < 1024 || high > 65535 || low > high)
            osoe_fatal("OSOE_PORT_RANGE='%s' is not LOW-HIGH with 1024 <= LOW <= HIGH <= 65535",
                       range);
        b->port_low = (uint16_t)low;
        b->port_high = (uint16_t)high;
    }

    /*
     * TCP keepalive on the node-pair connections (§3.15, wire.c): "IDLE,
     * INTERVAL,COUNT" - seconds without traffic before the first probe,
     * seconds between probes, and how many go unanswered before the peer is
     * dead - or 0 for none. Tests make it short; nothing else should need to.
     */
    const char *keepalive = getenv("OSOE_KEEPALIVE");
    if (keepalive && strcmp(keepalive, "0") == 0) {
        b->keepalive_off = true;
    } else if (keepalive) {
        unsigned idle, interval, count;
        int used = 0;
        if (sscanf(keepalive, "%u,%u,%u%n", &idle, &interval, &count, &used) != 3 ||
            keepalive[used] != '\0' || idle < 1 || interval < 1 || count < 1 ||
            idle > 32767 || interval > 32767 || count > 127)
            osoe_fatal("OSOE_KEEPALIVE='%s' is not IDLE,INTERVAL,COUNT (seconds, seconds, "
                       "probes: 1 to 32767, 1 to 32767, 1 to 127) or 0", keepalive);
        b->keepalive_idle_s = idle;
        b->keepalive_interval_s = interval;
        b->keepalive_count = count;
    }

    /*
     * The step's hosts (§6.1), which SLURM_NODEID indexes and so the node map
     * does too: where each node's lead listens, for the rendezvous (§6.2).
     * Needed only when the job spans nodes; osoe-launch gives §8.1's logical
     * nodes one loopback address each, 127.0.0.[1-N].
     */
    b->node_list = getenv(launcher_name("STEP_NODELIST"));
    if (b->node_count > 1) {
        uint32_t hosts = 0;
        char why[128];
        if (!b->node_list)
            osoe_fatal("%sSTEP_NODELIST is not set, and a job of %u nodes needs it: it "
                       "names each node's host (§6.1)", prefix, b->node_count);
        if (!osoe_hostlist_count(b->node_list, &hosts, why, sizeof why))
            osoe_fatal("%sSTEP_NODELIST='%s' is not a host list: %s", prefix, b->node_list, why);
        if (hosts != b->node_count)
            osoe_fatal("%sSTEP_NODELIST='%s' names %u hosts for a job of %u nodes", prefix,
                       b->node_list, hosts, b->node_count);
    }

    /*
     * A secret every lead proves it knows in each hello (§6.2; §10's
     * "authenticating the step-3 handshake", decided with step 3): optional,
     * since the network is trusted as Slurm's own traffic is (§3.4), and cheap
     * when a cluster's is not - a wrapper sets it before srun and Slurm carries
     * it to every task. 128 bits as 32 hex digits.
     */
    const char *secret = getenv("OSOE_JOB_SECRET");
    b->secured = false;
    if (secret && *secret) {            /* set but empty is unset */
        bool hex = strlen(secret) == 32;
        for (size_t i = 0; hex && i < 32; i++) {
            char c = secret[i];
            hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
        if (!hex)
            osoe_fatal("OSOE_JOB_SECRET is not 32 hexadecimal digits (128 bits); make one "
                       "with: head -c16 /dev/urandom | od -An -tx1 | tr -d ' \\n'");
        for (size_t i = 0; i < 16; i++) {
            unsigned byte;
            sscanf(secret + 2 * i, "%2x", &byte);
            b->secret[i] = (unsigned char)byte;
        }
        b->secured = true;
    }

    b->print_version = spec_env("VERSION") != NULL;
    b->print_info = spec_env("INFO") != NULL;
    b->debug = spec_env("DEBUG") != NULL;

    /* The heap is mapped at the base with MAP_FIXED_NOREPLACE, which needs a
     * page-aligned address: one that is not is refused here by name, not left
     * to fail as a base that is not free. A Raspberry Pi 5's kernel uses
     * 16 KiB pages, so an address that suits a 4 KiB machine may not suit it. */
    long page = sysconf(_SC_PAGESIZE);
    const char *base = getenv("OSOE_HEAP_BASE");
    if (base) {
        char *end;
        unsigned long long v = strtoull(base, &end, 0);
        if (*base == '\0' || *end != '\0' || v == 0)
            osoe_fatal("OSOE_HEAP_BASE='%s' is not a nonzero address", base);
        if (page <= 0 || v % (unsigned long long)page != 0)
            osoe_fatal("OSOE_HEAP_BASE='%s' is not a multiple of the %ld-byte page", base, page);
        b->heap_base = v;
    }

    /*
     * §8.2's heap-base shift, a test mode: each PE maps its own heap at
     * B + rank x OSOE_HEAP_SHIFT instead of at B, so that no two PEs of the job
     * hold a symmetric object at the same address, and code that uses an
     * address where it should use an offset fails (invariant 7). A size, as
     * SHMEM_SYMMETRIC_SIZE takes one, and a whole number of pages.
     */
    const char *shift = getenv("OSOE_HEAP_SHIFT");
    if (shift && (osoe_parse_size(shift, &b->heap_shift) != 0 || page <= 0 ||
                  b->heap_shift % (uint64_t)page != 0))
        osoe_fatal("OSOE_HEAP_SHIFT='%s' is not a whole number of %ld-byte pages", shift, page);
}

void osoe_bootstrap_free(struct osoe_bootstrap *b)
{
    free(b->gtids);
    b->gtids = NULL;
}
