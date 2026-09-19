#include <stdio.h>
#include <stdlib.h>

#include "base/env.h"
#include "base/size.h"
#include "boot.h"
#include "fatal.h"
#include "watch_range.h"
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
    switch (osoe_env_u32(name, &v)) {
    case OSOE_ENV_OK:
        return v;
    case OSOE_ENV_UNSET:
        osoe_fatal("%s is not set", name);
    case OSOE_ENV_INVALID:
    default:
        osoe_fatal("%s='%s' is not a decimal number", name, getenv(name));
    }
}

void osoe_boot_read(struct osoe_boot *b)
{
    *b = (struct osoe_boot){ 0 };

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
    b->local_id = need_u32("OSOE_LOCALID");
    b->step = need_u32("OSOE_STEP_ID");
    switch (osoe_env_u64("OSOE_JOB_ID", &b->job)) {
    case OSOE_ENV_OK: break;
    case OSOE_ENV_UNSET: osoe_fatal("OSOE_JOB_ID is not set");
    default: osoe_fatal("OSOE_JOB_ID='%s' is not a decimal number", getenv("OSOE_JOB_ID"));
    }

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

    /*
     * One logical node still, until the AM backend carries internode traffic
     * (§7.1 step 2). What is missing is the backend, not the bootstrap: the
     * variables checked above are already a multi-node environment, and
     * osoe-launch derives them from §8.1's node map exactly as Slurm derives
     * them from a distribution. So this rejection moves rather than loosens
     * when the backend lands.
     */
    if (b->node != 0 || b->local_count != b->world_size)
        osoe_fatal("this job spans more than one node (OSOE_NODEID=%u, %u of %u ranks "
                   "local); internode traffic needs the AM backend of §7.1 step 2",
                   b->node, b->local_count, b->world_size);

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

    uint32_t timeout_s = OSOE_DEFAULT_BOOTSTRAP_TIMEOUT_S;
    if (osoe_env_u32("OSOE_BOOTSTRAP_TIMEOUT", &timeout_s) == OSOE_ENV_INVALID ||
        timeout_s == 0)
        osoe_fatal("OSOE_BOOTSTRAP_TIMEOUT='%s' is not a positive number of seconds",
                   getenv("OSOE_BOOTSTRAP_TIMEOUT"));
    b->timeout_ns = (uint64_t)timeout_s * UINT64_C(1000000000);

    uint32_t watchdog = OSOE_WATCHDOG_DEFAULT;
    if (osoe_env_u32("OSOE_WATCHDOG", &watchdog) == OSOE_ENV_INVALID || watchdog > 1)
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

void osoe_boot_free(struct osoe_boot *b)
{
    free(b->gtids);
    b->gtids = NULL;
}
