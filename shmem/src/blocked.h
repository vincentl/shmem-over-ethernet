/* SPDX-License-Identifier: LGPL-3.0-or-later */
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

#include "node.h"

/* `routine` is an API routine name; `detail` is whatever most helps say *what*
 * the PE is waiting for - a peer node, an epoch, a symmetric offset - and
 * `kind` says which, so a reader can print it as that. */
void osoe_blocked_enter(const char *routine, enum osoe_blocked_detail kind, uint64_t detail);
void osoe_blocked_leave(void);

/* The routine this process's thread is waiting in, NULL if none: for its agent,
 * which names it when an abort ends the process (abort.c). `routine` must
 * have static storage - every caller passes a routine's name. */
const char *osoe_blocked_routine(void);

/* CLOCK_MONOTONIC nanoseconds, for the record and for the wait histograms.
 * Taken only on paths that are already about to sleep (§3.14 cost rules). */
uint64_t osoe_now_ns(void);

#endif /* OSOE_BLOCKED_H */
