/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* OSOE_TEST_PERTURB (perturb.h). */
#define _GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "base/environment.h"
#include "fatal.h"
#include "perturb.h"

static bool perturbing;

void osoe_perturb_init(void)
{
    uint32_t v = 0;
    if (osoe_environment_u32("OSOE_TEST_PERTURB", &v) == OSOE_ENVIRONMENT_INVALID || v > 1)
        osoe_fatal("OSOE_TEST_PERTURB='%s' is not 0 or 1", getenv("OSOE_TEST_PERTURB"));
    perturbing = v == 1;
}

void osoe_perturb(void)
{
    if (!perturbing)
        return;
    static _Thread_local uint64_t x;
    if (!x)
        x = (uint64_t)(uintptr_t)&x * UINT64_C(0x9E3779B97F4A7C15) | 1;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    if ((x & 7) == 0) {
        struct timespec t = { 0, (long)((x >> 40) % 200000) };
        nanosleep(&t, NULL);
    } else if ((x & 1) == 0) {
        sched_yield();
    }
}
