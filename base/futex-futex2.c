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
    long r = syscall(__NR_futex_wait, (void *)word, (unsigned long)expected,
                     (unsigned long)bitset, flags_for(scope),
                     &deadline->at, CLOCK_MONOTONIC);
    if (r == 0)
        return 0;
    switch (errno) {
    case EAGAIN:
    case EINTR:
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
    long r = syscall(__NR_futex_wake, (void *)word, (unsigned long)bitset,
                     n, flags_for(scope));
    if (r < 0) {
        fprintf(stderr, "osoe: futex_wake failed: %s\n", strerror(errno));
        abort();
    }
}

int osoe_futex_check(void)
{
    _Atomic uint32_t word = 0;
    if (syscall(__NR_futex_wake, (void *)&word, (unsigned long)FUTEX_BITSET_MATCH_ANY, 1,
                flags_for(OSOE_FUTEX_PRIVATE)) < 0)
        return errno;
    /* Expecting 1 where the word holds 0: the kernel compares and refuses. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (syscall(__NR_futex_wait, (void *)&word, 1UL, (unsigned long)FUTEX_BITSET_MATCH_ANY,
                flags_for(OSOE_FUTEX_PRIVATE), &now, CLOCK_MONOTONIC) == 0 || errno != EAGAIN)
        return errno ? errno : EINVAL;
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
