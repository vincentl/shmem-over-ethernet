/*
 * Point-to-point synchronization: shmem_wait_until and shmem_test
 * (spec §9.11.1, §9.11.8; design §4.9).
 *
 * A wait is always on the calling PE's own variable, updated by an AMO from
 * any PE or thread (spec §9.11.1). The loop follows §4.9 exactly: publish the
 * watch before the first check, load the sequence word before the variable,
 * and wait on the sequence word with a safety-sweep deadline, so a lost wake
 * costs latency rather than the job.
 *
 * shmem_test never touches the slot: it is one coherent load and a compare.
 */
#include <stdbool.h>
#include <stdint.h>

#include <shmem.h>

#include "base/deadline.h"
#include "base/sweep.h"
#include "blocked.h"
#include "metrics.h"
#include "export.h"
#include "fatal.h"
#include "sym.h"
#include "transport.h"
#include "wait.h"

static bool satisfied(int cmp, uint64_t have, uint64_t want, const char *routine)
{
    switch (cmp) {
    case SHMEM_CMP_EQ: return have == want;
    case SHMEM_CMP_NE: return have != want;
    case SHMEM_CMP_GT: return have > want;
    case SHMEM_CMP_GE: return have >= want;
    case SHMEM_CMP_LT: return have < want;
    case SHMEM_CMP_LE: return have <= want;
    default:
        osoe_fatal("%s: comparison %d is not one of SHMEM_CMP_EQ, NE, GT, GE, LT, LE",
                   routine, cmp);
    }
}

/* Returns the value that satisfied the condition, which shmem_signal_wait_until
 * needs (spec §9.11.15) and the other waits discard. */
uint64_t osoe_wait_until(const char *routine, const void *ivar, unsigned width, int cmp,
                         uint64_t want)
{
    osoe_count(OSOE_C_SHMEM_WAIT_UNTIL_CALLS, 1);
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    struct osoe_tp_watch *w = osoe_tp_watch(OSOE_REGION_HEAP, offset, width);
    uint64_t have, slept_since = 0;
    bool slept = false, woken = false, timed_out = false;

    for (;;) {
        uint32_t seq = osoe_tp_watch_seq(w);
        have = osoe_tp_load(OSOE_REGION_HEAP, offset, width);
        if (satisfied(cmp, have, want, routine)) {
            /* The condition was already true when the sweep expired, so the
             * update landed while this PE slept and no wake arrived: a missed
             * wake (§4.9). Zero at quiescence for a conforming program, and
             * legitimately nonzero for an ivar written with a put, where notify
             * is best effort by design (§4.10). */
            if (timed_out)
                osoe_count(OSOE_C_INVARIANT_MISSED_WAKE, 1);
            break;
        }
        if (woken)
            osoe_count(OSOE_C_TP_WATCH_SPURIOUS, 1);

        if (!slept) {
            /* About to block. Everything on this path is paid only by a wait
             * that really sleeps (§3.14 cost rules). */
            slept = true;
            slept_since = osoe_now_ns();
            osoe_blocked_enter(routine, offset);
            osoe_count(OSOE_C_TP_WATCH_BLOCKED, 1);
        }

        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        woken = osoe_tp_watch_wait(w, seq, &sweep);
        timed_out = !woken;
        if (woken) {
            osoe_count(OSOE_C_TP_WATCH_WAKES, 1);
        } else {
            osoe_count(OSOE_C_TP_WATCH_SWEEPS, 1);
            osoe_sweep_expired(routine);        /* only here, never on a wake */
        }
    }

    osoe_tp_unwatch(w);
    if (slept) {
        osoe_count(OSOE_C_TP_WATCH_BLOCKED_NS, osoe_now_ns() - slept_since);
        osoe_blocked_leave();
    }
    return have;
}

int osoe_test(const char *routine, const void *ivar, unsigned width, int cmp,
              uint64_t want)
{
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    osoe_count(OSOE_C_SHMEM_TEST_CALLS, 1);
    return satisfied(cmp, osoe_tp_load(OSOE_REGION_HEAP, offset, width), want, routine);
}

OSOE_API void shmem_uint64_wait_until(uint64_t *ivar, int cmp, uint64_t cmp_value)
{
    osoe_wait_until("shmem_uint64_wait_until", ivar, 8, cmp, cmp_value);
}

OSOE_API void shmem_uint32_wait_until(uint32_t *ivar, int cmp, uint32_t cmp_value)
{
    osoe_wait_until("shmem_uint32_wait_until", ivar, 4, cmp, cmp_value);
}

OSOE_API int shmem_uint64_test(uint64_t *ivar, int cmp, uint64_t cmp_value)
{
    return osoe_test("shmem_uint64_test", ivar, 8, cmp, cmp_value);
}

OSOE_API int shmem_uint32_test(uint32_t *ivar, int cmp, uint32_t cmp_value)
{
    return osoe_test("shmem_uint32_test", ivar, 4, cmp, cmp_value);
}
