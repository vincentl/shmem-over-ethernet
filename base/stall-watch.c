#define _GNU_SOURCE
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
