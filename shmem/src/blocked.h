/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The "blocked in" record (§3.14): where a PE is waiting, readable from another
 * process while it waits.
 *
 * Published only when a wait is about to sleep. A wait whose predicate is
 * already true touches none of this - no locked instruction and no clock -
 * which is what §8.6's U1 and U3 measure as "wait with nothing pending".
 *
 * The protocol (blocked.c) is a generation word bumped with two seq_cst
 * read-modify-writes around the field writes, every field an atomic word,
 * model-checked in tools/modelcheck/blocked_record_model.py.
 */
#ifndef OSOE_BLOCKED_H
#define OSOE_BLOCKED_H

#include <stdbool.h>
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

/* ---- The record's protocol, both sides (blocked.c) ----------------------- */

/* Publish `slot`'s record: the generation odd, the fields, the generation
 * even. Only the slot's own PE writes it, one thread at a time (metrics.c's
 * owner rule). A NULL routine publishes "waiting in nothing". */
void osoe_blocked_publish(struct osoe_pe_slot *slot, const char *routine,
                          enum osoe_blocked_detail kind, uint64_t detail, uint64_t since_ns);

/* A reader's copy of one publication, whole. */
struct osoe_blocked_copy {
    char routine[OSOE_BLOCKED_ROUTINE_MAX];
    uint32_t kind;                       /* enum osoe_blocked_detail */
    uint64_t detail, since_ns;
};

/* Copy `slot`'s record, from any process. False when every attempt met a
 * publication in flight: a PE publishing waits faster than this reads is busy,
 * not the hung PE the record is for, so it is not retried for ever. */
bool osoe_blocked_read(const struct osoe_pe_slot *slot, struct osoe_blocked_copy *copy);

#endif /* OSOE_BLOCKED_H */
