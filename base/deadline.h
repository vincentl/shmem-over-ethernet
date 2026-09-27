/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Absolute deadlines (§3.10). futex_wait takes an absolute instant on a named
 * clock; a relative value passed there names an instant just after boot, so the
 * wait returns ETIMEDOUT at once and the waiter becomes a busy loop. Deadlines
 * are therefore only ever made here, from a duration, on CLOCK_MONOTONIC.
 */
#ifndef OSOE_BASE_DEADLINE_H
#define OSOE_BASE_DEADLINE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* The safety-sweep interval for library waits (§3.10, §4.9). */
#define OSOE_SWEEP_NS UINT64_C(100000000)

struct osoe_deadline {
    struct timespec at;         /* absolute, CLOCK_MONOTONIC */
};

/* A deadline ns nanoseconds from now. */
struct osoe_deadline osoe_deadline_in(uint64_t ns);

bool osoe_deadline_expired(const struct osoe_deadline *d);

#endif /* OSOE_BASE_DEADLINE_H */
