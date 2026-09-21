#include <stdio.h>
#include <stdlib.h>

#include "base/environment.h"
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
    b->gtids = malloc(sizeof *b->gtids * b->world_size);
    if (!b->gtids)
        osoe_fatal("out of memory");
    size_t n;
    if (osoe_parse_u32_list(gtids, b->gtids, b->world_size, &n) != 0)
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


    const char *size = getenv("SHMEM_SYMMETRIC_SIZE");
    b->symmetric_size = OSOE_DEFAULT_SYMMETRIC_SIZE;
    if (size && osoe_parse_size(size, &b->symmetric_size) != 0)
        osoe_fatal("SHMEM_SYMMETRIC_SIZE='%s' is invalid: expected a non-negative "
                   "decimal number with an optional k, m, g or t multiplier", size);
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
