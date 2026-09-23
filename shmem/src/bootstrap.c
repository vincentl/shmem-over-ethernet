#include <stdio.h>
#include <stdlib.h>

#include "collective-tree.h"
#include "internode.h"
#include "base/environment.h"
#include "base/hostlist.h"
#include "base/size.h"
#include "bootstrap.h"
#include "fatal.h"
#include "watch-range.h"
#include "watchdog.h"

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

void osoe_bootstrap_read(struct osoe_bootstrap *b)
{
    *b = (struct osoe_bootstrap){ 0 };

    if (!getenv("OSOE_PROCID")) {
        if (getenv("SLURM_JOB_ID"))
            osoe_fatal("launch under Slurm is not yet supported; set the OSOE_ "
                       "bootstrap variables (design §6.2) or use osoe-launch");
        osoe_fatal("no launcher environment: OSOE_PROCID is not set. Start "
                   "programs with osoe-launch, or set the OSOE_ bootstrap "
                   "variables of design §6.2");
    }

    b->rank = need_u32("OSOE_PROCID");
    b->have_rank = true;
    b->world_size = need_u32("OSOE_STEP_NUM_TASKS");
    b->node = need_u32("OSOE_NODEID");
    b->node_count = need_u32("OSOE_STEP_NUM_NODES");
    b->local_id = need_u32("OSOE_LOCALID");
    b->step = need_u32("OSOE_STEP_ID");
    switch (osoe_environment_u64("OSOE_JOB_ID", &b->job)) {
    case OSOE_ENVIRONMENT_OK: break;
    case OSOE_ENVIRONMENT_UNSET: osoe_fatal("OSOE_JOB_ID is not set");
    default: osoe_fatal("OSOE_JOB_ID='%s' is not a decimal number", getenv("OSOE_JOB_ID"));
    }

    if (b->node_count == 0 || b->node >= b->node_count)
        osoe_fatal("OSOE_NODEID=%u is not below OSOE_STEP_NUM_NODES=%u", b->node,
                   b->node_count);
    if (b->world_size == 0 || b->rank >= b->world_size)
        osoe_fatal("OSOE_PROCID=%u is not below OSOE_STEP_NUM_TASKS=%u", b->rank,
                   b->world_size);

    const char *gtids = getenv("OSOE_GTIDS");
    if (!gtids)
        osoe_fatal("OSOE_GTIDS is not set");
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
        osoe_fatal("OSOE_GTIDS='%s' names %zu ranks in a job of %u", gtids, entries,
                   b->world_size);
    /* §6.2's per-rank local index in the job map is 16 bits (job.h). */
    if (entries > UINT16_MAX)
        osoe_fatal("OSOE_GTIDS names %zu ranks on one node; osoe supports at most %u",
                   entries, (unsigned)UINT16_MAX);
    b->gtids = malloc(sizeof *b->gtids * entries);
    if (!b->gtids)
        osoe_fatal("out of memory");
    size_t n;
    if (osoe_parse_u32_list(gtids, b->gtids, entries, &n) != 0)
        osoe_fatal("OSOE_GTIDS='%s' is not a list of at most %u ranks", gtids,
                   b->world_size);
    b->local_count = (uint32_t)n;

    if (b->local_id >= b->local_count || b->gtids[b->local_id] != b->rank)
        osoe_fatal("OSOE_LOCALID=%u does not name OSOE_PROCID=%u in OSOE_GTIDS='%s'",
                   b->local_id, b->rank, gtids);
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (b->gtids[i] >= b->world_size)
            osoe_fatal("OSOE_GTIDS='%s' names rank %u, not below %u", gtids, b->gtids[i],
                       b->world_size);
        for (uint32_t j = 0; j < i; j++)
            if (b->gtids[j] == b->gtids[i])
                osoe_fatal("OSOE_GTIDS='%s' names rank %u twice", gtids, b->gtids[i]);
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
     * The step's hosts (§6.1), which SLURM_NODEID indexes and so the node map
     * does too: where each node's lead listens, for the rendezvous (§6.2).
     * Needed only when the job spans nodes; osoe-launch gives §8.1's logical
     * nodes one loopback address each, 127.0.0.[1-N].
     */
    b->node_list = getenv("OSOE_STEP_NODELIST");
    if (b->node_count > 1) {
        uint32_t hosts = 0;
        char why[128];
        if (!b->node_list)
            osoe_fatal("OSOE_STEP_NODELIST is not set, and a job of %u nodes needs it: it "
                       "names each node's host (§6.1)", b->node_count);
        if (!osoe_hostlist_count(b->node_list, &hosts, why, sizeof why))
            osoe_fatal("OSOE_STEP_NODELIST='%s' is not a host list: %s", b->node_list, why);
        if (hosts != b->node_count)
            osoe_fatal("OSOE_STEP_NODELIST='%s' names %u hosts for a job of %u nodes",
                       b->node_list, hosts, b->node_count);
    }

    uint32_t watchdog = OSOE_WATCHDOG_DEFAULT;
    if (osoe_environment_u32("OSOE_WATCHDOG", &watchdog) == OSOE_ENVIRONMENT_INVALID || watchdog > 1)
        osoe_fatal("OSOE_WATCHDOG='%s' is not 0 or 1", getenv("OSOE_WATCHDOG"));
    b->watchdog = watchdog != 0;

    b->print_version = spec_env("VERSION") != NULL;
    b->print_info = spec_env("INFO") != NULL;
    b->debug = spec_env("DEBUG") != NULL;

    const char *base = getenv("OSOE_HEAP_BASE");
    if (base) {
        char *end;
        unsigned long long v = strtoull(base, &end, 0);
        if (*base == '\0' || *end != '\0' || v == 0)
            osoe_fatal("OSOE_HEAP_BASE='%s' is not a nonzero address", base);
        b->heap_base = v;
    }
}

void osoe_bootstrap_free(struct osoe_bootstrap *b)
{
    free(b->gtids);
    b->gtids = NULL;
}
