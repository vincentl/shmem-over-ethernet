/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The AM layer's sizes from the environment (am-configuration.h). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "am-configuration.h"
#include "base/environment.h"
#include "fatal.h"
#include "node.h"

static uint64_t env_bytes(const char *name, uint64_t dflt, uint64_t lo, uint64_t hi,
                          bool power_of_two)
{
    uint64_t v = dflt;
    switch (osoe_environment_u64(name, &v)) {
    case OSOE_ENVIRONMENT_UNSET:
        return dflt;
    case OSOE_ENVIRONMENT_OK:
        break;
    default:
        osoe_fatal("%s='%s' is not a decimal byte count", name, getenv(name));
    }
    if (v < lo || v > hi)
        osoe_fatal("%s=%llu is outside %llu..%llu", name, (unsigned long long)v,
                   (unsigned long long)lo, (unsigned long long)hi);
    if (power_of_two && (v & (v - 1)) != 0)
        osoe_fatal("%s=%llu is not a power of two", name, (unsigned long long)v);
    return v;
}

uint64_t osoe_get_landing_default_bytes(uint32_t local_count)
{
    uint64_t share = OSOE_GET_LANDING_NODE_BUDGET_BYTES / (local_count ? local_count : 1);
    share -= share % OSOE_LANDING_EXTENT_BYTES;
    if (share < OSOE_GET_LANDING_LEAST_DEFAULT_BYTES)
        return OSOE_GET_LANDING_LEAST_DEFAULT_BYTES;
    if (share > OSOE_GET_LANDING_MOST_DEFAULT_BYTES)
        return OSOE_GET_LANDING_MOST_DEFAULT_BYTES;
    return share;
}

void osoe_am_configuration_read(struct osoe_am_configuration *c, uint32_t local_count)
{
    c->chunk = env_bytes("OSOE_AM_CHUNK", OSOE_AM_DEFAULT_CHUNK, OSOE_AM_MIN_CHUNK,
                         OSOE_AM_MAX_CHUNK, true);
    /*
     * §3.4's floor, refused rather than silently raised: a buffer below twice
     * the chunk means one message per segment forever, and a user who asked for
     * it has misunderstood something that will not show up until a benchmark.
     * The default is a number of chunks, not of bytes, so that it stays above
     * the floor whatever the chunk.
     */
    c->buffer = env_bytes("OSOE_AM_BUFFER", OSOE_AM_DEFAULT_BUFFER_CHUNKS * c->chunk,
                          2 * c->chunk, UINT64_C(1) << 30, true);
    c->budget = env_bytes("OSOE_AM_BUFFER_BUDGET", OSOE_AM_DEFAULT_BUDGET,
                          2 * c->chunk, UINT64_C(64) << 30, false);

    /*
     * §3.8's credit C. Tunable because the only way to exercise exhaustion in a
     * test is to make it small: at the default a test would have to issue tens
     * of thousands of gets before the bound bound anything, and a credit that is
     * never reached is a credit nothing checks.
     */
    uint32_t credits = OSOE_AM_DEFAULT_CREDITS;
    if (osoe_environment_u32("OSOE_AM_CREDITS", &credits) == OSOE_ENVIRONMENT_INVALID ||
        credits < OSOE_AM_MIN_CREDITS || credits > OSOE_AM_MAX_CREDITS)
        osoe_fatal("OSOE_AM_CREDITS='%s' is not between %u and %u",
                   getenv("OSOE_AM_CREDITS"), OSOE_AM_MIN_CREDITS, OSOE_AM_MAX_CREDITS);
    c->credits = credits;

    uint32_t huge = 0;
    if (osoe_environment_u32("OSOE_AM_HUGEPAGES", &huge) == OSOE_ENVIRONMENT_INVALID || huge > 1)
        osoe_fatal("OSOE_AM_HUGEPAGES='%s' is not 0 or 1", getenv("OSOE_AM_HUGEPAGES"));
    c->hugepages = huge != 0;

    /* Tunable for the same reason as the credits: a test reaches a full landing
     * zone only by making it small. Its extents count what is in flight since
     * phase 1 step 8 slice 2, so OSOE_GETS_IN_FLIGHT, which counted it apart,
     * is retired and ignored. Unset, the node's budget split among its PEs,
     * which every PE of the node counts alike (P32). */
    c->get_landing_bytes = env_bytes("OSOE_GET_LANDING_BYTES",
                                     osoe_get_landing_default_bytes(local_count),
                                     OSOE_MIN_GET_LANDING_BYTES, OSOE_MAX_GET_LANDING_BYTES,
                                     false);
    /* Whole extents or nothing: a size that is not one is refused by name
     * rather than quietly rounded down (review of step 8 slice 2) - here, where
     * every PE reads it, and not only the lead that lays the zones out. */
    if (c->get_landing_bytes % OSOE_LANDING_EXTENT_BYTES != 0)
        osoe_fatal("OSOE_GET_LANDING_BYTES=%llu is not a multiple of the landing zone's "
                   "%u-byte extents", (unsigned long long)c->get_landing_bytes,
                   (unsigned)OSOE_LANDING_EXTENT_BYTES);
}
