/*
 * futex2 wait and wake on 32-bit words (§3.10, §4.9, invariant 18).
 *
 * The kernel implements only 32-bit futexes, and futex_wait takes an absolute
 * deadline; both are fixed here, once. Every wait carries a deadline, and
 * expiries are counted: a timeout rate far above normal is a lost wake or a
 * busy loop in disguise.
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
 * Returns 0 when woken, when the word no longer held expected, or on a
 * signal - callers re-check their predicate in every case - and ETIMEDOUT
 * when the deadline passed. Any other kernel error aborts: it is a bug.
 */
int osoe_futex_wait(_Atomic uint32_t *word, uint32_t expected,
                    const struct osoe_deadline *deadline, enum osoe_futex_scope scope);

/* Wake up to n waiters on word; INT_MAX wakes all. */
void osoe_futex_wake(_Atomic uint32_t *word, int n, enum osoe_futex_scope scope);

/*
 * Does this kernel have the futex2 calls (Linux 6.7)? Returns 0, or the errno
 * of the call that failed: ENOSYS on an older kernel. Makes one wake and one
 * wait that cannot sleep, on a private word, so it is cheap enough for init
 * (§1: the kernel is checked at start-up, before anything is created).
 */
int osoe_futex_check(void);

/* Process-wide count of waits that ended at their deadline. */
uint64_t osoe_futex_timeouts(void);

#endif /* OSOE_BASE_FUTEX_H */
