/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The single-node build's half of the probe (probe.c has the checks both builds
 * make): the classic futex(2) calls the library's base/futex-classic.c makes,
 * FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET, which every Linux since 2.6.25 has.
 * This build has no io_uring and no network, so there is nothing of either to
 * check, and no liburing to compile against; the full build compiles
 * probe-full-build.c in this file's place.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"

void probe_build_info(void)
{
    printf("info single-node build: no io_uring and no network to check\n");
}

/*
 * The two calls the library makes, with the bitset every plain wait and wake
 * passes. FUTEX_WAIT_BITSET takes an absolute deadline, on CLOCK_MONOTONIC
 * unless FUTEX_CLOCK_REALTIME is set, which it never is here. `flags` is
 * FUTEX_PRIVATE_FLAG for a word in this process alone, and 0 for a shared one.
 */
static long classic_wait(_Atomic uint32_t *word, uint32_t value, int flags,
                         const struct timespec *absolute_deadline)
{
    return syscall(SYS_futex, (void *)word, FUTEX_WAIT_BITSET | flags, (unsigned long)value,
                   absolute_deadline, NULL, (unsigned long)FUTEX_BITSET_MATCH_ANY);
}

static long classic_wake(_Atomic uint32_t *word, int n, int flags)
{
    return syscall(SYS_futex, (void *)word, FUTEX_WAKE_BITSET | flags, n, NULL, NULL,
                   (unsigned long)FUTEX_BITSET_MATCH_ANY);
}

/* The two calls probe.c's cross-process futex check is written against, over
 * the classic ones: no FUTEX_PRIVATE_FLAG, an absolute CLOCK_MONOTONIC deadline. */
long probe_futex_wait_shared(_Atomic uint32_t *word, uint32_t value,
                             const struct timespec *absolute_deadline)
{
    return classic_wait(word, value, 0, absolute_deadline);
}

long probe_futex_wake_shared(_Atomic uint32_t *word, int n)
{
    return classic_wake(word, n, 0);
}

void probe_build_checks(void)
{
    _Atomic uint32_t word = 7;

    /* Expecting 8 where the word holds 7: the kernel compares and refuses with
     * EAGAIN, rather than sleep to the deadline, 1 ms ahead, and say ETIMEDOUT. */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 1000000;
    if (deadline.tv_nsec >= 1000000000L) {          /* a valid timespec, or EINVAL */
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    long r = classic_wait(&word, 8, FUTEX_PRIVATE_FLAG, &deadline);
    int e = errno;
    CHECK("futex U32 wait (value mismatch)", r == -1 && e == EAGAIN,
          "r=%ld errno=%s", r, strerror(e));

    /* Nobody waits on the word, so a wake finds no one to wake. */
    r = classic_wake(&word, 1, FUTEX_PRIVATE_FLAG);
    e = errno;
    CHECK("futex wake", r == 0, "woke=%ld%s%s", r, r < 0 ? " errno=" : "",
          r < 0 ? strerror(e) : "");
}
