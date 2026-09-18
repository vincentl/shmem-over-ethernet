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
 * SHMEM layer overrides that definition with the development watchdog
 * (shmem/src/watchdog.c), which checks the abort word and the local PEs'
 * liveness. `routine` names the API routine the caller is inside, for the
 * diagnostic.
 */
#ifndef OSOE_BASE_SWEEP_H
#define OSOE_BASE_SWEEP_H

void osoe_sweep_expired(const char *routine);

#endif /* OSOE_BASE_SWEEP_H */
