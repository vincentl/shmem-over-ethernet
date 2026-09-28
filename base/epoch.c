/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>

#include "base/deadline.h"
#include "base/epoch.h"
#include "base/futex.h"
#include "base/sweep.h"

void osoe_epoch_init(struct osoe_epoch *e)
{
    atomic_store_explicit(&e->count_compare_only_by_equality, 0, memory_order_release);
    atomic_store_explicit(&e->released_at_ns, 0, memory_order_release);
}

bool osoe_epoch_bump_eq(struct osoe_epoch *e, uint32_t target)
{
    uint32_t prev = atomic_fetch_add_explicit(&e->count_compare_only_by_equality, 1,
                                              memory_order_seq_cst);
    return (uint32_t)(prev + 1) == target;
}

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

bool osoe_epoch_release_bump_eq(struct osoe_epoch *e, uint32_t target)
{
    atomic_store_explicit(&e->released_at_ns, monotonic_ns(), memory_order_release);
    return osoe_epoch_bump_eq(e, target);
}

/*
 * How long before a sweep's deadline the target must have been reached for
 * the wake to count as lost rather than late.
 *
 * A waiter whose sweep expires and then finds its target reached cannot see
 * whether the wake is lost or still on its way: the releaser bumps, then wakes,
 * and a release landing just as the timer fires looks exactly like a wake that
 * never came. Under load that coincidence is not rare - measured at 3 runs in
 * 20 of shmem.multinode.0-1-2-0-1-0 with 12 spinners on 4 cores, every one with
 * the bump 1-12 us before the waiter looked and the wake 2-3 us behind the bump.
 * So a wake is lost only if the target was reached well before the deadline.
 * 10 ms is a scheduler timeslice with margin, far above bump-to-wake, and far
 * below the sweep: a release whose wake is deleted outright still lands more
 * than 10 ms before the deadline nine times in ten, which is plenty for an
 * invariant every test checks after hundreds of barriers.
 */
#define LOST_WAKE_MARGIN_NS UINT64_C(10000000)

static bool wake_was_lost(const struct osoe_epoch *e, const struct osoe_deadline *expired)
{
    uint64_t deadline = (uint64_t)expired->at.tv_sec * UINT64_C(1000000000) +
                        (uint64_t)expired->at.tv_nsec;
    uint64_t reached = atomic_load_explicit(&e->released_at_ns, memory_order_acquire);
    return reached + LOST_WAKE_MARGIN_NS < deadline;
}

void osoe_epoch_wake_all(struct osoe_epoch *e)
{
    osoe_futex_wake(&e->count_compare_only_by_equality, INT_MAX, OSOE_FUTEX_SHARED);
}

/* The chain's width: each link wakes this many (§5.3). */
#define CHAIN_WIDTH 2

/* The futex bitset for waiters of `target`: its parity (epoch.h). */
static uint32_t parity(uint32_t target)
{
    return UINT32_C(1) << (target & 1);
}

void osoe_epoch_release_wake(struct osoe_epoch *e, uint32_t target)
{
    osoe_futex_wake_bitset(&e->count_compare_only_by_equality, CHAIN_WIDTH, parity(target),
                           OSOE_FUTEX_SHARED);
}

static void wait_eq(struct osoe_epoch *e, uint32_t target, const char *routine, bool chained)
{
    bool timed_out = false, slept = false;
    struct osoe_deadline sweep;
    for (;;) {
        uint32_t v = atomic_load_explicit(&e->count_compare_only_by_equality,
                                          memory_order_acquire);
        if (v == target) {
            if (slept)
                osoe_wait_ends();       /* the detector's count (sweep.h) */
            /* Reached, but only noticed because the sweep expired: if the
             * release landed well before the deadline, its wake never came. */
            if (timed_out && wake_was_lost(e, &sweep))
                osoe_missed_wake(routine);
            /*
             * Pass the chained wake on (epoch.h). Every wait that returned
             * does, whether a wake, a changed word or a sweep ended it: which
             * of those it was is not something the call reports, and an extra
             * two wakes on a word nobody is left waiting on cost one syscall.
             * A wait that found its target on the first load never slept, holds
             * no one's wake, and owes nothing.
             */
            if (chained && slept)
                osoe_epoch_release_wake(e, target);
            return;
        }
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        slept = true;
        osoe_wait_sleeps();
        timed_out = osoe_futex_wait_bitset(&e->count_compare_only_by_equality, v,
                                           parity(target), &sweep,
                                           OSOE_FUTEX_SHARED) == ETIMEDOUT;
        if (timed_out)
            osoe_sweep_expired(routine);
    }
}

void osoe_epoch_wait_eq(struct osoe_epoch *e, uint32_t target, const char *routine)
{
    wait_eq(e, target, routine, false);
}

void osoe_epoch_wait_eq_chained(struct osoe_epoch *e, uint32_t target, const char *routine)
{
    wait_eq(e, target, routine, true);
}
