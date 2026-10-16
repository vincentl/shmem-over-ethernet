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
 * futex.h over the futex2 calls, futex_wait and futex_wake (Linux 6.7): the
 * full build's. The single-node build's is futex-classic.c.
 */

/* futex2 system calls, Linux 6.7. Older headers lack the numbers; they are
 * the same on every architecture using the generic syscall table. */
#ifndef __NR_futex_wake
#define __NR_futex_wake 454
#endif
#ifndef __NR_futex_wait
#define __NR_futex_wait 455
#endif

static _Atomic uint64_t timeouts;

static unsigned flags_for(enum osoe_futex_scope scope)
{
    return FUTEX2_SIZE_U32 | (scope == OSOE_FUTEX_PRIVATE ? FUTEX2_PRIVATE : 0);
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
    /* A signal whose handler lacks SA_RESTART ends the wait with EINTR: it is
     * made again to the same absolute deadline, as futex-classic.c does for
     * every signal, so that a program's handlers cannot hold a caller's sweep
     * off (that file's wait says why). */
    long r;
    do
        r = syscall(__NR_futex_wait, (void *)word, (unsigned long)expected,
                    (unsigned long)bitset, flags_for(scope), &deadline->at, CLOCK_MONOTONIC);
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
        fprintf(stderr, "osoe: futex_wait failed: %s\n", strerror(errno));
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
    if (n <= 0)
        return;                 /* none, as in futex-classic.c */
    long r = syscall(__NR_futex_wake, (void *)word, (unsigned long)bitset,
                     n, flags_for(scope));
    if (r < 0) {
        fprintf(stderr, "osoe: futex_wake failed: %s\n", strerror(errno));
        abort();
    }
}

int osoe_futex_check(void)
{
    /* Both scopes, as in futex-classic.c: a sandbox may allow the private
     * calls alone. */
    for (int shared = 0; shared <= 1; shared++) {
        enum osoe_futex_scope scope = shared ? OSOE_FUTEX_SHARED : OSOE_FUTEX_PRIVATE;
        _Atomic uint32_t word = 0;
        if (syscall(__NR_futex_wake, (void *)&word, (unsigned long)FUTEX_BITSET_MATCH_ANY, 1,
                    flags_for(scope)) < 0)
            return errno;
        /* Expecting 1 where the word holds 0: the kernel compares and refuses. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (syscall(__NR_futex_wait, (void *)&word, 1UL, (unsigned long)FUTEX_BITSET_MATCH_ANY,
                    flags_for(scope), &now, CLOCK_MONOTONIC) == 0 || errno != EAGAIN)
            return errno ? errno : EINVAL;
    }
    return 0;
}

uint64_t osoe_futex_timeouts(void)
{
    return atomic_load_explicit(&timeouts, memory_order_acquire);
}

const char *osoe_futex_requirement(void)
{
    return "Linux 6.7 or later, for the futex2 calls";
}

int osoe_futex_kernel_has_calls(const char *release)
{
    unsigned major = 0, minor = 0;
    if (!release || sscanf(release, "%u.%u", &major, &minor) != 2)
        return -1;
    return major > 6 || (major == 6 && minor >= 7);
}
