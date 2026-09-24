/*
 * Waiting for a word of this PE's own region (region-wait.h; §4.9). Moved out
 * of wait.c, which is the SHMEM layer's, so that the generic internode phase can
 * wait on OSOE_REGION_INTERNAL without reaching above the API (phase 1 step 5
 * slice 2); wait.c's scalar and multiple-element forms are built on it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "base/deadline.h"
#include "base/sweep.h"
#include "blocked.h"
#include "metrics.h"
#include "region-wait.h"
#include "transport.h"

uint64_t osoe_region_extend(uint64_t value, unsigned width, bool is_signed)
{
    switch (width) {
    case 8: return value;
    case 4:
        return is_signed ? (uint64_t)(int64_t)(int32_t)(uint32_t)value
                         : (uint64_t)(uint32_t)value;
    default:
        return is_signed ? (uint64_t)(int64_t)(int16_t)(uint16_t)value
                         : (uint64_t)(uint16_t)value;
    }
}

bool osoe_region_satisfied(enum osoe_comparison comparison, uint64_t have, uint64_t want,
                           bool is_signed)
{
    if (is_signed) {
        int64_t h = (int64_t)have, w = (int64_t)want;
        switch (comparison) {
        case OSOE_COMPARISON_EQUAL: return h == w;
        case OSOE_COMPARISON_NOT_EQUAL: return h != w;
        case OSOE_COMPARISON_GREATER: return h > w;
        case OSOE_COMPARISON_GREATER_OR_EQUAL: return h >= w;
        case OSOE_COMPARISON_LESS: return h < w;
        case OSOE_COMPARISON_LESS_OR_EQUAL: return h <= w;
        }
    } else {
        switch (comparison) {
        case OSOE_COMPARISON_EQUAL: return have == want;
        case OSOE_COMPARISON_NOT_EQUAL: return have != want;
        case OSOE_COMPARISON_GREATER: return have > want;
        case OSOE_COMPARISON_GREATER_OR_EQUAL: return have >= want;
        case OSOE_COMPARISON_LESS: return have < want;
        case OSOE_COMPARISON_LESS_OR_EQUAL: return have <= want;
        }
    }
    return false;       /* not reached: every comparison is handled above */
}

/*
 * Which counter a late wake belongs in.
 *
 * Spec 1.5 change #17 requires a wait variable to be updated with an AMO, and
 * every AMO type is four or eight bytes wide. So at those widths a sweep that
 * expires on an already-true predicate means a wake was lost by the mechanism
 * that must not lose one, and the counter is an invariant (§8.2). The internal
 * region's words are four bytes and only ever updated by rmw, so the same holds
 * for them.
 *
 * At two bytes there is no AMO type - the deprecated short waits of Table 12
 * predate the rule - so the only remote update is a put, for which notification
 * is best effort by design (§4.10) and a sweep is how the wait is meant to
 * finish. Counting that as an invariant violation would make the oracle
 * unusable for the one case it is known to be wrong about.
 */
enum osoe_counter osoe_region_late_wake_counter(unsigned width)
{
    return width >= 4 ? OSOE_C_INVARIANT_MISSED_WAKE : OSOE_C_TRANSPORT_WATCH_LATE_WAKE;
}

uint64_t osoe_region_wait(const char *routine, enum osoe_region region, uint64_t offset,
                          unsigned width, bool is_signed, enum osoe_comparison comparison,
                          uint64_t want, enum osoe_blocked_detail kind, uint64_t detail)
{
    want = osoe_region_extend(want, width, is_signed);
    struct osoe_transport_watch *w = osoe_transport_watch(region, offset, width);
    uint64_t have, slept_since = 0;
    bool slept = false, woken = false, timed_out = false;

    for (;;) {
        uint32_t seq = osoe_transport_watch_seq(w);
        have = osoe_transport_load(region, offset, width);
        if (osoe_region_satisfied(comparison, osoe_region_extend(have, width, is_signed), want,
                                  is_signed)) {
            /* The condition was already true when the sweep expired, so the
             * update landed while this PE slept and no wake arrived (§4.9).
             * Which counter that is depends on the width: see above. */
            if (timed_out)
                osoe_count(osoe_region_late_wake_counter(width), 1);
            break;
        }
        if (woken)
            osoe_count(OSOE_C_TRANSPORT_WATCH_SPURIOUS, 1);

        if (!slept) {
            /* About to block. Everything on this path is paid only by a wait
             * that really sleeps (§3.14 cost rules). */
            slept = true;
            slept_since = osoe_now_ns();
            osoe_blocked_enter(routine, kind, detail);
            osoe_count(OSOE_C_TRANSPORT_WATCH_BLOCKED, 1);
        }

        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        woken = osoe_transport_watch_wait(w, seq, &sweep);
        timed_out = !woken;
        if (woken) {
            osoe_count(OSOE_C_TRANSPORT_WATCH_WAKES, 1);
        } else {
            osoe_count(OSOE_C_TRANSPORT_WATCH_SWEEPS, 1);
            osoe_sweep_expired(routine);        /* only here, never on a wake */
        }
    }

    osoe_transport_unwatch(w);
    if (slept) {
        osoe_count(OSOE_C_TRANSPORT_WATCH_BLOCKED_NS, osoe_now_ns() - slept_since);
        osoe_blocked_leave();
    }
    return have;
}
