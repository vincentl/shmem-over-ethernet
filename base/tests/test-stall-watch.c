/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The no-progress rule (base/stall-watch.h): a watch reports a stall only when
 * its word has not changed for the whole limit, and never with a limit of 0. */
#define _GNU_SOURCE
#include <time.h>

#include "base/stall-watch.h"
#include "check.h"

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

int main(void)
{
    struct osoe_stall_watch w;

    /* Limit 0, the default: never, however long nothing moves. */
    osoe_stall_watch_begin(&w, 7, 0);
    sleep_ms(30);
    CHECK(!osoe_stall_watch_stalled(&w, 7));
    CHECK(osoe_stall_watch_seconds(&w) == 0);

    /* Unchanged for the whole limit: stalled. */
    osoe_stall_watch_begin(&w, 7, 20 * 1000000ull);
    CHECK(!osoe_stall_watch_stalled(&w, 7));
    sleep_ms(40);
    CHECK(osoe_stall_watch_stalled(&w, 7));

    /* Slow but moving is never stalled: each change restarts the interval, so
     * three steps of 15 ms under a 20 ms limit take 45 ms and never trip it. */
    osoe_stall_watch_begin(&w, 0, 20 * 1000000ull);
    for (uint64_t v = 1; v <= 3; v++) {
        sleep_ms(15);
        CHECK(!osoe_stall_watch_stalled(&w, v));
    }
    /* ... and once it stops, it stalls a limit after the last change. */
    sleep_ms(40);
    CHECK(osoe_stall_watch_stalled(&w, 3));
    return CHECK_DONE();
}
