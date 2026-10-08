/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The wire's settings from the environment (wire-settings.h). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "base/environment.h"
#include "fatal.h"
#include "wire-settings.h"

void osoe_wire_settings_read(struct osoe_wire_settings *s, const struct osoe_bootstrap *b,
                          bool announce)
{
    s->buffer_bytes = OSOE_WIRE_DEFAULT_BYTES;
    if (osoe_environment_u32("OSOE_WIRE_BYTES", &s->buffer_bytes) == OSOE_ENVIRONMENT_INVALID ||
        s->buffer_bytes < OSOE_WIRE_MIN_BYTES || s->buffer_bytes > OSOE_WIRE_MAX_BYTES)
        osoe_fatal("OSOE_WIRE_BYTES='%s' is not a byte count between %u and %u",
                   getenv("OSOE_WIRE_BYTES"), OSOE_WIRE_MIN_BYTES, OSOE_WIRE_MAX_BYTES);
    s->buffer_count = OSOE_WIRE_DEFAULT_BUFFERS;
    if (osoe_environment_u32("OSOE_WIRE_BUFFERS", &s->buffer_count) ==
            OSOE_ENVIRONMENT_INVALID || s->buffer_count < 2 ||
        s->buffer_count > OSOE_WIRE_MAX_BUFFERS ||
        (s->buffer_count & (s->buffer_count - 1)) != 0)
        osoe_fatal("OSOE_WIRE_BUFFERS='%s' is not a power of two between 2 and %u",
                   getenv("OSOE_WIRE_BUFFERS"), OSOE_WIRE_MAX_BUFFERS);
    s->split = 0;
    if (osoe_environment_u32("OSOE_WIRE_SPLIT", &s->split) == OSOE_ENVIRONMENT_INVALID ||
        s->split > 10000)
        osoe_fatal("OSOE_WIRE_SPLIT='%s' is not a rate per myriad (0 to 10000)",
                   getenv("OSOE_WIRE_SPLIT"));
    /*
     * §3.14: the seed must be overridable by environment, with a failing one
     * committed as a permanent regression case. Without the override it comes
     * from the job id, which is the launcher's pid, so every run would inject a
     * different pattern and a case that failed could not be run again.
     */
    s->seed = osoe_wire_hash(b->job ^ ((uint64_t)b->step << 32));
    uint64_t seed;
    switch (osoe_environment_u64("OSOE_WIRE_SEED", &seed)) {
    case OSOE_ENVIRONMENT_OK:
        s->seed = seed;
        break;
    case OSOE_ENVIRONMENT_UNSET:
        break;
    default:
        osoe_fatal("OSOE_WIRE_SEED='%s' is not a decimal number", getenv("OSOE_WIRE_SEED"));
    }
    s->unsent_set = false;
    s->unsent_bytes = 0;
    switch (osoe_environment_u32("OSOE_WIRE_UNSENT_BYTES", &s->unsent_bytes)) {
    case OSOE_ENVIRONMENT_OK:
        if (s->unsent_bytes > OSOE_WIRE_UNSENT_MAX)
            osoe_fatal("OSOE_WIRE_UNSENT_BYTES='%s' is more than %u bytes",
                       getenv("OSOE_WIRE_UNSENT_BYTES"), OSOE_WIRE_UNSENT_MAX);
        s->unsent_set = true;
        break;
    case OSOE_ENVIRONMENT_UNSET:
        break;
    default:
        osoe_fatal("OSOE_WIRE_UNSENT_BYTES='%s' is not a byte count: 0 for no cap, or up to %u",
                   getenv("OSOE_WIRE_UNSENT_BYTES"), OSOE_WIRE_UNSENT_MAX);
    }
    /* Said whenever injection is on, so a failure names what to reproduce it
     * with; off by default, so a normal run says nothing. */
    if (announce && s->split != 0)
        fprintf(stderr, "osoe: wire split %u per myriad, OSOE_WIRE_SEED=%llu\n", s->split,
                (unsigned long long)s->seed);
}

void osoe_wire_settings_check(const struct osoe_bootstrap *b)
{
    struct osoe_wire_settings s;
    osoe_wire_settings_read(&s, b, false);
}

uint32_t osoe_wire_unsent_bytes(const struct osoe_wire_settings *s, uint64_t bytes_per_second)
{
    if (s->unsent_set)
        return s->unsent_bytes;
    /* The link's highest bandwidth (bootstrap.h) times the interval fits in 64
     * bits: 10^13 x 1.28 x 10^5. */
    uint64_t bytes = bytes_per_second * OSOE_WIRE_UNSENT_NS / UINT64_C(1000000000);
    if (bytes < OSOE_WIRE_UNSENT_FLOOR)
        bytes = OSOE_WIRE_UNSENT_FLOOR;
    if (bytes > OSOE_WIRE_UNSENT_MAX)
        bytes = OSOE_WIRE_UNSENT_MAX;
    return (uint32_t)bytes;
}

