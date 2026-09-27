/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include "base/deadline.h"

struct osoe_deadline osoe_deadline_in(uint64_t ns)
{
    struct osoe_deadline d;
    clock_gettime(CLOCK_MONOTONIC, &d.at);
    uint64_t nsec = (uint64_t)d.at.tv_nsec + ns % UINT64_C(1000000000);
    d.at.tv_sec += (time_t)(ns / UINT64_C(1000000000) + nsec / UINT64_C(1000000000));
    d.at.tv_nsec = (long)(nsec % UINT64_C(1000000000));
    return d;
}

bool osoe_deadline_expired(const struct osoe_deadline *d)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > d->at.tv_sec ||
           (now.tv_sec == d->at.tv_sec && now.tv_nsec >= d->at.tv_nsec);
}
