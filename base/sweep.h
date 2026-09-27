/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The safety-sweep hook (§3.10, §3.15).
 *
 * Every library wait carries an absolute safety-sweep deadline so that a lost
 * wake costs latency rather than the job (§3.10). This is what a wait does when
 * one of those deadlines expires, and nothing else: it is never on the wake
 * path, so a healthy wait pays nothing for it.
 *
 * base declares it and provides a do-nothing definition, so base's own waits -
 * the epoch counters - can call it without depending on the layer above. The
 * SHMEM layer overrides that definition with its sweep (shmem/src/sweep.c),
 * which checks the abort word and, before the agent runs, the local PEs'
 * liveness. `routine` names the API routine the caller is inside, for the
 * diagnostic.
 */
#ifndef OSOE_BASE_SWEEP_H
#define OSOE_BASE_SWEEP_H

void osoe_sweep_expired(const char *routine);

/*
 * A sweep expired on a condition that was already true: the thing waited for
 * happened while the waiter slept, and no wake came (§3.14, invariant.missed_wake).
 *
 * A lost wake in a wait that carries a sweep costs latency and nothing else, so
 * no test can see it through a result - a barrier whose release wake is deleted
 * still completes, one sweep late each time. This is what makes it visible: the
 * SHMEM layer overrides it to count the invariant that every tier-2 test checks
 * at quiescence, as wait.c already does for its own waits. Same weak-default
 * arrangement as osoe_sweep_expired, for the same reason.
 *
 * A caller must not report a wake that is merely late. A release landing as the
 * sweep timer fires looks exactly like a lost wake to the waiter, and under
 * load that is not rare; base/epoch.c's waits therefore report only a target
 * reached well before the expired deadline (LOST_WAKE_MARGIN_NS). wait.c's
 * waits have the same window and do not yet make that distinction.
 */
void osoe_missed_wake(const char *routine);

#endif /* OSOE_BASE_SWEEP_H */
