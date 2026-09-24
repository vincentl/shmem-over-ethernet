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
    /*
     * CLOCK_MONOTONIC time of the last osoe_epoch_release_bump_eq, stored
     * before its bump, so that a waiter that sees the count reach its target
     * sees at least this stamp (§3.16: the bump is a seq_cst read-modify-write
     * and the waiter's load of the count is an acquire). It exists for one
     * question only - was a wake lost, or merely late? - and lives in the
     * padding the counter already owns, so it costs no line.
     */
    _Atomic uint64_t released_at_ns;
};

_Static_assert(sizeof(struct osoe_epoch) == OSOE_CACHELINE,
               "an epoch counter occupies exactly one cache line");

/* Set to zero before anyone else can see the counter. */
void osoe_epoch_init(struct osoe_epoch *e);

/* Add one (a locked instruction, §3.16); true if the result equals target. */
bool osoe_epoch_bump_eq(struct osoe_epoch *e, uint32_t target);

/*
 * bump_eq for a counter that waiters wait on with wait_eq: the same add,
 * preceded by the stamp that lets a waiter tell a lost wake from a late one.
 * One bumper per target, the releaser of an epoch - or several, when every one
 * of them stamps: a data collective's arrivals and copy-outs, which its lead
 * waits on (collectives.c). Each stamp precedes its own bump, so a waiter that
 * sees the target sees the last bumper's stamp or a later one, and a single
 * word still holds the time the target was reached. The barrier's arrivals,
 * which nobody waits on, use bump_eq and pay nothing.
 */
bool osoe_epoch_release_bump_eq(struct osoe_epoch *e, uint32_t target);

/*
 * Start the wake of every waiter for `target`, which the caller has just made
 * the counter's value: the **chained** release (§5.3). This wakes two; each
 * waiter that wait_eq wakes and that finds its target reached wakes two more
 * before it returns. Depth O(log L), and the caller - on a multi-node job the
 * node's progress thread - does O(1) instead of the O(L) of a wake-all.
 *
 * The wakes carry the target's parity as a futex bitset, and wait_eq sleeps on
 * its target's parity. That is what makes the chain safe, and it is needed:
 * a member released early can re-enter and sleep on this same counter waiting
 * for target + 1, and without the bitset a wake meant for `target` could pick
 * it - it would find nothing to do, pass nothing on, and a member still waiting
 * for `target` would be stranded until its sweep. Bounded skew (§5.4) allows
 * exactly those two targets on one counter at once, so parity tells them apart
 * (docs/design/evidence/am-backend/chained_wake_model.py).
 */
void osoe_epoch_release_wake(struct osoe_epoch *e, uint32_t target);

/*
 * Wake every waiter, whatever it waits for: one futex_wake(INT_MAX), O(L) in
 * the caller, which the kernel does in one pass. Measured on the 4-core VM it
 * beats the chain at every L to 32 (§5.3) - a chain only pays when the woken
 * threads can run in parallel - so it is the single-node release, where the
 * caller is the last arriving member and not the node's network.
 */
void osoe_epoch_wake_all(struct osoe_epoch *e);

/* Return once the counter equals target. Waits on the counter itself with a
 * safety-sweep deadline, so a lost wake costs latency, not a hang; `routine`
 * names the API routine for the diagnostic a sweep may print (base/sweep.h).
 * Sleeps on its target's parity (above), so it is woken by either kind of
 * release; for a counter released by osoe_epoch_wake_all. */
void osoe_epoch_wait_eq(struct osoe_epoch *e, uint32_t target, const char *routine);

/* The same wait for a counter released by osoe_epoch_release_wake: a wait
 * that slept and finds its target reached passes the chained wake on. Every
 * waiter on one counter must use the same one of the two, since the chain
 * depends on every woken waiter passing it on. */
void osoe_epoch_wait_eq_chained(struct osoe_epoch *e, uint32_t target, const char *routine);

#endif /* OSOE_BASE_EPOCH_H */
