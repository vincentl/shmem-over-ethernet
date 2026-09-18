/*
 * Collective epoch counters (§5.4, invariant 10).
 *
 * A cumulative 32-bit counter, never reset, compared only for equality: the
 * type offers bump_eq and wait_eq and no way to read the value. Equality is
 * sound only while skew is bounded, which blocking collectives guarantee. A
 * contributor adding nonblocking collectives should meet that assumption here,
 * at compile time, rather than as a hang; the upgrade path is 64-bit counters
 * with >= and a separate wake word (§5.4).
 *
 * Each counter sits alone on its cache line (§5.3) and is its own shared futex
 * word, since it lives in the node segment. Wraparound is harmless: a counter
 * runs ahead of a value someone waits for by at most L - 1 (§5.4).
 */
#ifndef OSOE_BASE_EPOCH_H
#define OSOE_BASE_EPOCH_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/cacheline.h"

struct osoe_epoch {
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t count_compare_only_by_equality;
};

_Static_assert(sizeof(struct osoe_epoch) == OSOE_CACHELINE,
               "an epoch counter occupies exactly one cache line");

/* Set to zero before anyone else can see the counter. */
void osoe_epoch_init(struct osoe_epoch *e);

/* Add one (a locked instruction, §3.16); true if the result equals target. */
bool osoe_epoch_bump_eq(struct osoe_epoch *e, uint32_t target);

/* Wake every waiter. */
void osoe_epoch_wake_all(struct osoe_epoch *e);

/* Return once the counter equals target. Waits on the counter itself with a
 * safety-sweep deadline, so a lost wake costs latency, not a hang; `routine`
 * names the API routine for the diagnostic a sweep may print (base/sweep.h). */
void osoe_epoch_wait_eq(struct osoe_epoch *e, uint32_t target, const char *routine);

#endif /* OSOE_BASE_EPOCH_H */
