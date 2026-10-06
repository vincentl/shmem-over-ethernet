/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "base/futex.h"

/*
 * futex.h over the classic futex(2): FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET,
 * in every Linux since 2.6.25. The single-node build's (OSOE_SINGLE_NODE),
 * whose kernels may be older than futex2's 6.7 - Red Hat 9's 5.14, Debian 12's
 * 6.1. The full build's is futex-futex2.c.
 *
 * Call for call it is futex-futex2.c's contract. The word is 32 bits, which is
 * all the classic calls know; the private scope is FUTEX_PRIVATE_FLAG, which
 * futex2's FUTEX2_PRIVATE is the same flag as; the bitset is the classic
 * calls' val3, FUTEX_BITSET_MATCH_ANY for the plain wait and wake; and the
 * deadline is absolute on CLOCK_MONOTONIC, which is what FUTEX_WAIT_BITSET
 * takes when FUTEX_CLOCK_REALTIME is not set - osoe_deadline's form
 * (base/deadline.h), so a relative value passed for one names an instant just
 * after boot here as there, and the wait becomes the busy loop §3.10 warns of.
 * FUTEX_WAIT, whose timeout is relative, is never used. The errors are
 * futex2's too: EAGAIN when the word no longer holds the value, ETIMEDOUT at
 * the deadline, and EINTR on a signal, after which the wait is made again (the
 * wait below says why).
 */

static _Atomic uint64_t timeouts;

static int operation(int op, enum osoe_futex_scope scope)
{
    return op | (scope == OSOE_FUTEX_PRIVATE ? FUTEX_PRIVATE_FLAG : 0);
}

int osoe_futex_wait(_Atomic uint32_t *word, uint32_t expected,
                    const struct osoe_deadline *deadline, enum osoe_futex_scope scope)
{
    return osoe_futex_wait_bitset(word, expected, FUTEX_BITSET_MATCH_ANY, deadline, scope);
}

int osoe_futex_wait_bitset(_Atomic uint32_t *word, uint32_t expected, uint32_t bitset,
                           const struct osoe_deadline *deadline,
                           enum osoe_futex_scope scope)
{
    /*
     * A signal ends a timed FUTEX_WAIT_BITSET with EINTR whatever the handler's
     * SA_RESTART: the kernel restarts it through a restart block, which a
     * handler turns into EINTR (futex2's wait restarts under SA_RESTART). So it
     * is made again to the same absolute deadline, which is what the kernel's
     * own restart does. Returned instead, a profiler's SIGPROF every 10 ms ended
     * every wait early, and a caller that makes a fresh sweep deadline each pass
     * never swept: a blocked PE's sweeps stood still, which osoe-stat reads as a
     * dead one (the adversarial read of v0.2 step 1, parts 2 and 3).
     *
     * The kernel arms this timer with the thread's timer slack, where futex2's
     * wait takes none: a session that raises the slack for power - a prctl, a
     * service's TimerSlackNSec= - makes these waits that much later than
     * futex2's, 41 ms on average at a slack of 50 ms (the v0.2 step 1 review,
     * correctness K4). futex.h says why it does not matter.
     */
    long r;
    do
        r = syscall(SYS_futex, (void *)word, operation(FUTEX_WAIT_BITSET, scope),
                    (unsigned long)expected, &deadline->at, NULL, (unsigned long)bitset);
    while (r < 0 && errno == EINTR);
    if (r == 0)
        return 0;
    switch (errno) {
    case EAGAIN:
        return 0;
    case ETIMEDOUT:
        atomic_fetch_add_explicit(&timeouts, 1, memory_order_seq_cst);
        return ETIMEDOUT;
    default:
        fprintf(stderr, "osoe: futex wait failed: %s\n", strerror(errno));
        abort();
    }
}

void osoe_futex_wake(_Atomic uint32_t *word, int n, enum osoe_futex_scope scope)
{
    osoe_futex_wake_bitset(word, n, FUTEX_BITSET_MATCH_ANY, scope);
}

void osoe_futex_wake_bitset(_Atomic uint32_t *word, int n, uint32_t bitset,
                            enum osoe_futex_scope scope)
{
    /* The classic wake of 0 wakes one, and futex2's none: none, in both files. */
    if (n <= 0)
        return;
    long r = syscall(SYS_futex, (void *)word, operation(FUTEX_WAKE_BITSET, scope), n, NULL, NULL,
                     (unsigned long)bitset);
    if (r < 0) {
        fprintf(stderr, "osoe: futex wake failed: %s\n", strerror(errno));
        abort();
    }
}

int osoe_futex_check(void)
{
    /* Both scopes: a sandbox may allow the private calls alone, which glibc's
     * own locks make, and refuse the shared ones every wait between a node's
     * PEs needs - and refused only at its first shared wait, a job would have
     * made its objects first. */
    for (int shared = 0; shared <= 1; shared++) {
        enum osoe_futex_scope scope = shared ? OSOE_FUTEX_SHARED : OSOE_FUTEX_PRIVATE;
        _Atomic uint32_t word = 0;
        if (syscall(SYS_futex, (void *)&word, operation(FUTEX_WAKE_BITSET, scope), 1, NULL,
                    NULL, (unsigned long)FUTEX_BITSET_MATCH_ANY) < 0)
            return errno;
        /* Expecting 1 where the word holds 0: the kernel compares and refuses. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (syscall(SYS_futex, (void *)&word, operation(FUTEX_WAIT_BITSET, scope), 1UL, &now,
                    NULL, (unsigned long)FUTEX_BITSET_MATCH_ANY) == 0 ||
            errno != EAGAIN)
            return errno ? errno : EINVAL;
    }
    return 0;
}

/* Every kernel since 2.6.25 has them, so a refusal here is a sandbox's:
 * "osoe needs the futex calls ..., which a sandbox (seccomp) must allow (they
 * fail with "Operation not permitted"); this kernel is ...". */
const char *osoe_futex_requirement(void)
{
    return "the futex calls FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, which a sandbox "
           "(seccomp) must allow";
}

uint64_t osoe_futex_timeouts(void)
{
    return atomic_load_explicit(&timeouts, memory_order_acquire);
}
