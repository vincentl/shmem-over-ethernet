/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * What probe.c shares with the file of the build it is compiled with
 * (probe-full-build.c or probe-single-node.c, as the build chooses): the check
 * macro, the count of failures, and the calls each build supplies.
 */
#ifndef OSOE_PROBE_PROBE_H
#define OSOE_PROBE_PROBE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* The checks that have failed so far; the probe exits nonzero if there are any. */
extern int probe_failures;

/*
 * One line a check: "ok" or "FAIL", the check's name, and its detail, a printf
 * format with its arguments. `ok` is evaluated once, ahead of the printing, so
 * that printf cannot change an errno that `ok` reads.
 */
#define CHECK(name, ok, fmt, ...) do { \
    bool check_passed = (ok); \
    printf("%-4s %-44s " fmt "\n", check_passed ? "ok" : "FAIL", name, ##__VA_ARGS__); \
    if (!check_passed) probe_failures++; } while (0)

/*
 * What the file of the build supplies.
 */

/* One info line saying which build this is, and what it was built with. */
void probe_build_info(void);

/* The checks only this build makes, run after the ones in probe.c. */
void probe_build_checks(void);

/*
 * The build's futex calls, on a word in a shared mapping - without the private
 * flag, since the waiter and the waker are different processes. The wait takes
 * an absolute deadline on CLOCK_MONOTONIC, and sleeps only while *word == value.
 * Each returns what the system call returned, with errno set when that is -1:
 * the wait 0 when it was woken, -1 with EAGAIN when the word differed, EINTR on
 * a signal, ETIMEDOUT at the deadline; the wake how many waiters it woke.
 */
long probe_futex_wait_shared(_Atomic uint32_t *word, uint32_t value,
                             const struct timespec *absolute_deadline);
long probe_futex_wake_shared(_Atomic uint32_t *word, int n);

#endif /* OSOE_PROBE_PROBE_H */
