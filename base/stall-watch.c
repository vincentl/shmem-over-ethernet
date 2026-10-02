/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "base/stall-watch.h"

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

void osoe_stall_watch_begin(struct osoe_stall_watch *w, uint64_t value, uint64_t limit_ns)
{
    w->value = value;
    w->limit_ns = limit_ns;
    w->since_ns = limit_ns ? monotonic_ns() : 0;
    w->begun_ns = 0;
    w->notice_ns = 0;
}

/* The first notice's delay: 30 s, or the test hook's (stall-watch.h). Read once. */
static uint64_t first_notice_ns(void)
{
    static _Atomic uint64_t first;          /* 0: not read yet */
    uint64_t f = atomic_load_explicit(&first, memory_order_acquire);
    if (f)
        return f;
    f = UINT64_C(30) * UINT64_C(1000000000);
    const char *hook = getenv("OSOE_TEST_NOTICE_SECONDS");
    if (hook) {
        char *end;
        unsigned long s = strtoul(hook, &end, 10);
        if (end != hook && !*end && s > 0 && s <= 3600)
            f = (uint64_t)s * UINT64_C(1000000000);
    }
    atomic_store_explicit(&first, f, memory_order_release);
    return f;
}

void osoe_stall_watch_begin_noticed(struct osoe_stall_watch *w, uint64_t value,
                                    uint64_t limit_ns)
{
    osoe_stall_watch_begin(w, value, limit_ns);
    w->begun_ns = monotonic_ns();
    w->notice_ns = w->begun_ns + first_notice_ns();
}

bool osoe_stall_watch_notice_due(struct osoe_stall_watch *w)
{
    if (w->begun_ns == 0)
        return false;
    uint64_t now = monotonic_ns();
    if (now < w->notice_ns)
        return false;
    /* Every minute after the first, or twice the hook's: the next from now, so
     * a wait that asked late is not owed a burst of them. */
    w->notice_ns = now + 2 * first_notice_ns();
    return true;
}

uint64_t osoe_stall_watch_waited_seconds(const struct osoe_stall_watch *w)
{
    return w->begun_ns ? (monotonic_ns() - w->begun_ns) / UINT64_C(1000000000) : 0;
}

bool osoe_stall_watch_stalled(struct osoe_stall_watch *w, uint64_t value)
{
    if (w->limit_ns == 0)
        return false;
    uint64_t now = monotonic_ns();
    if (value != w->value) {
        w->value = value;
        w->since_ns = now;
        return false;
    }
    return now - w->since_ns >= w->limit_ns;
}

uint64_t osoe_stall_watch_seconds(const struct osoe_stall_watch *w)
{
    return w->limit_ns ? (monotonic_ns() - w->since_ns) / UINT64_C(1000000000) : 0;
}
