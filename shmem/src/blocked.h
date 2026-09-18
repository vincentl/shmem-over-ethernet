/*
 * The "blocked in" record (§3.14): where a PE is waiting, readable from another
 * process while it waits.
 *
 * Published only when a wait is about to sleep. A wait whose predicate is
 * already true touches none of this - no locked instruction and no clock -
 * which is what §8.6's U1 and U3 measure as "wait with nothing pending".
 *
 * The protocol is a generation word bumped with two seq_cst read-modify-writes
 * around the field writes, model-checked in docs/design/evidence/blocked-record/.
 */
#ifndef OSOE_BLOCKED_H
#define OSOE_BLOCKED_H

#include <stdint.h>

/* `routine` is an API routine name; `detail` is routine-specific - a target PE,
 * an epoch, a symmetric offset - and is whatever most helps say *what* the PE
 * is waiting for. */
void osoe_blocked_enter(const char *routine, uint64_t detail);
void osoe_blocked_leave(void);

/* CLOCK_MONOTONIC nanoseconds, for the record and for the wait histograms.
 * Taken only on paths that are already about to sleep (§3.14 cost rules). */
uint64_t osoe_now_ns(void);

#endif /* OSOE_BLOCKED_H */
