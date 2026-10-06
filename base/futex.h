/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Futex wait and wake on 32-bit words (§3.10, §4.9, invariant 18).
 *
 * The kernel implements only 32-bit futexes, and the wait takes an absolute
 * deadline; both are fixed here, once. Every wait carries a deadline, and
 * expiries are counted: a timeout rate far above normal is a lost wake or a
 * busy loop in disguise.
 *
 * Two files implement this header, one a build, chosen when configuring and
 * never at run time: futex-futex2.c, the futex2 calls of Linux 6.7, in the full
 * build, whose ring's IORING_OP_FUTEX_WAIT needs 6.7 anyway; and
 * futex-classic.c, futex(2)'s FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, in the
 * single-node build (OSOE_SINGLE_NODE), for kernels before 6.7. Every feature
 * of futex2 the library uses - the 32-bit word, the private flag, the absolute
 * CLOCK_MONOTONIC deadline and the bitset - the classic calls have too, so the
 * two share nothing but this header, and base/tests/test-futex.c holds either
 * to the same contract.
 */
#ifndef OSOE_BASE_FUTEX_H
#define OSOE_BASE_FUTEX_H

#include <stdatomic.h>
#include <stdint.h>

#include "base/deadline.h"

_Static_assert(sizeof(_Atomic uint32_t) == sizeof(uint32_t) &&
               _Alignof(_Atomic uint32_t) == _Alignof(uint32_t),
               "a futex word must have the size and alignment of uint32_t");

enum osoe_futex_scope {
    OSOE_FUTEX_PRIVATE,         /* the word is private to this process */
    OSOE_FUTEX_SHARED,          /* the word is in a shared mapping */
};

/*
 * Sleep while *word == expected, until woken or the deadline passes.
 * Returns 0 when woken or when the word no longer held expected - callers
 * re-check their predicate in either case - and ETIMEDOUT when the deadline
 * passed. A signal does not end the wait: it is made again to the same
 * deadline, so that a program's handlers cannot hold a caller's sweep off.
 * Any other kernel error aborts: it is a bug.
 */
int osoe_futex_wait(_Atomic uint32_t *word, uint32_t expected,
                    const struct osoe_deadline *deadline, enum osoe_futex_scope scope);

/* Wake up to n waiters on word; INT_MAX wakes all, and n of 0 or less none. */
void osoe_futex_wake(_Atomic uint32_t *word, int n, enum osoe_futex_scope scope);

/*
 * The same two with a bitset: a wait with bitset b is woken only by a wake
 * whose bitset shares a bit with b (osoe_futex_wake and osoe_futex_wait pass
 * all ones, so they meet everything). What the chained release uses to keep
 * one epoch's wakes away from the next epoch's waiters (base/epoch.c). A
 * bitset is never 0, which the kernel refuses.
 */
int osoe_futex_wait_bitset(_Atomic uint32_t *word, uint32_t expected, uint32_t bitset,
                           const struct osoe_deadline *deadline,
                           enum osoe_futex_scope scope);
void osoe_futex_wake_bitset(_Atomic uint32_t *word, int n, uint32_t bitset,
                            enum osoe_futex_scope scope);

/*
 * Does this kernel have the futex calls this build uses? Returns 0, or the
 * errno of the call that failed: in the full build ENOSYS on a kernel before
 * Linux 6.7, which has no futex2; in either, EPERM or ENOSYS where a sandbox
 * (seccomp) denies them. Makes one wake and one wait that cannot sleep in each
 * scope, private and shared, so it is cheap enough for init (§1: the kernel is
 * checked at start-up, before anything is created).
 */
int osoe_futex_check(void);

/* What osoe_futex_check checks for, as init's refusal names it: "osoe needs
 * <this> (they fail with ...)". */
const char *osoe_futex_requirement(void);

/* Process-wide count of waits that ended at their deadline. */
uint64_t osoe_futex_timeouts(void);

#endif /* OSOE_BASE_FUTEX_H */
