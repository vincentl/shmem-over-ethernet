/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * futex wrappers: absolute deadlines and wakes (§3.10, invariant 18) - the
 * contract base/futex.h states, which this pins for whichever file a build
 * compiles: futex-futex2.c in the full build, futex-classic.c in the
 * single-node build (OSOE_SINGLE_NODE). A deadline honoured within the
 * scheduler's slack; a wait on a word that already differs returning at once;
 * a private wait woken from another thread; a shared one woken from another
 * process, on a shared mapping; a bitset wait woken only by a wake that shares
 * a bit with it; a wait under a program's timer signals ending at its deadline,
 * with SA_RESTART and without; a wake of none waking no one; and the start-up
 * check passing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "check.h"

static double elapsed_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

static _Atomic uint32_t word;

static void *waker(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    /* Single-writer publication, then the wake: the waiter's futex compare
     * catches a store it did not see. */
    atomic_store_explicit(&word, 1, memory_order_release);
    osoe_futex_wake(&word, INT_MAX, OSOE_FUTEX_PRIVATE);
    return NULL;
}

/* A bitset waiter: sleeps with bitset 2 until woken, and says when it
 * returned and with what. */
static _Atomic uint32_t bit_word;
static _Atomic int bit_returned;
static int bit_result;

static void *bit_waiter(void *arg)
{
    (void)arg;
    struct osoe_deadline d = osoe_deadline_in(3 * 1000 * 1000 * 1000ull);
    bit_result = osoe_futex_wait_bitset(&bit_word, 0, 2, &d, OSOE_FUTEX_PRIVATE);
    atomic_store_explicit(&bit_returned, 1, memory_order_release);
    return NULL;
}

/* A program's own timer, as a profiler's SIGPROF or an alarm is: counted. */
static volatile sig_atomic_t ticks;

static void on_tick(int signal_number)
{
    (void)signal_number;
    ticks++;
}

/* A wait of 300 ms on a word nobody changes, under a 10 ms timer whose handler
 * has `flags`: it must end at its deadline, not at the first signal. */
static void wait_under_signals(int flags)
{
    struct sigaction action, before;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_tick;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGALRM, &action, &before) == 0);
    struct itimerval every = { { 0, 10000 }, { 0, 10000 } }, off = { { 0, 0 }, { 0, 0 } };
    ticks = 0;
    CHECK(setitimer(ITIMER_REAL, &every, NULL) == 0);
    _Atomic uint32_t quiet = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct osoe_deadline d = osoe_deadline_in(300 * 1000 * 1000);
    int r = osoe_futex_wait(&quiet, 0, &d, OSOE_FUTEX_PRIVATE);
    double e = elapsed_since(&t0);
    CHECK(setitimer(ITIMER_REAL, &off, NULL) == 0);
    CHECK(sigaction(SIGALRM, &before, NULL) == 0);
    CHECK(r == ETIMEDOUT);
    CHECK(e >= 0.295);
    CHECK(ticks >= 5);          /* the signals did come */
}

/* A waiter for the wake of none: one wait, and when it returned and with
 * what - one call, not a loop, so that a wake it should not have had shows. */
static _Atomic uint32_t none_word;
static _Atomic int none_returned;
static int none_result;

static void *none_waiter(void *arg)
{
    (void)arg;
    struct osoe_deadline d = osoe_deadline_in(3 * 1000 * 1000 * 1000ull);
    none_result = osoe_futex_wait(&none_word, 0, &d, OSOE_FUTEX_PRIVATE);
    atomic_store_explicit(&none_returned, 1, memory_order_release);
    return NULL;
}

static void pause_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

int main(void)
{
    struct timespec t0;

    /* The kernel has what this build's futex file calls. */
    CHECK(osoe_futex_check() == 0);
    CHECK(osoe_futex_requirement() != NULL && strlen(osoe_futex_requirement()) > 0);

    /* A deadline 100 ms out must wait about 100 ms: a relative value passed
     * as absolute would return at once (§3.10). */
    atomic_store_explicit(&word, 0, memory_order_release);
    uint64_t before = osoe_futex_timeouts();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct osoe_deadline d = osoe_deadline_in(100 * 1000 * 1000);
    CHECK(!osoe_deadline_expired(&d));
    int r = osoe_futex_wait(&word, 0, &d, OSOE_FUTEX_PRIVATE);
    double e = elapsed_since(&t0);
    CHECK(r == ETIMEDOUT);
    CHECK(e >= 0.095 && e < 2.0);
    CHECK(osoe_deadline_expired(&d));
    CHECK(osoe_futex_timeouts() == before + 1);

    /* A stale expected value returns at once, not as a timeout. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    d = osoe_deadline_in(2 * 1000 * 1000 * 1000ull);
    r = osoe_futex_wait(&word, 7, &d, OSOE_FUTEX_SHARED);
    CHECK(r == 0);
    CHECK(elapsed_since(&t0) < 1.0);

    /* A wake from another thread ends the wait well before the deadline. */
    pthread_t t;
    CHECK(pthread_create(&t, NULL, waker, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    d = osoe_deadline_in(5 * 1000 * 1000 * 1000ull);
    while (atomic_load_explicit(&word, memory_order_acquire) == 0)
        CHECK(osoe_futex_wait(&word, 0, &d, OSOE_FUTEX_PRIVATE) == 0);
    CHECK(elapsed_since(&t0) < 4.0);
    pthread_join(t, NULL);

    /* A wake from another process, on a word in a shared mapping, ends a
     * shared wait there: the scope's flag is what lets the two meet (§3.10,
     * §4.9 - every word a node's PEs wait on is in a shared mapping). */
    _Atomic uint32_t *shared = mmap(NULL, sizeof *shared, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    atomic_store_explicit(shared, 0, memory_order_release);
    pid_t child = fork();
    if (child == 0) {
        struct timespec c0;
        clock_gettime(CLOCK_MONOTONIC, &c0);
        struct osoe_deadline cd = osoe_deadline_in(5 * 1000 * 1000 * 1000ull);
        while (atomic_load_explicit(shared, memory_order_acquire) == 0)
            if (osoe_futex_wait(shared, 0, &cd, OSOE_FUTEX_SHARED) == ETIMEDOUT)
                _exit(2);
        _exit(elapsed_since(&c0) < 4.0 ? 0 : 3);
    }
    CHECK(child > 0);
    pause_ms(100);
    atomic_store_explicit(shared, 1, memory_order_release);
    osoe_futex_wake(shared, INT_MAX, OSOE_FUTEX_SHARED);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    munmap((void *)shared, sizeof *shared);

    /* A bitset wait is woken only by a wake whose bitset shares a bit with it
     * (base/epoch.c keeps one epoch's wakes from the next's this way): a wake
     * with bit 1 leaves a waiter on bit 2 asleep, one with bit 2 wakes it. */
    atomic_store_explicit(&bit_word, 0, memory_order_release);
    CHECK(pthread_create(&t, NULL, bit_waiter, NULL) == 0);
    pause_ms(100);
    osoe_futex_wake_bitset(&bit_word, INT_MAX, 1, OSOE_FUTEX_PRIVATE);
    pause_ms(200);
    CHECK(atomic_load_explicit(&bit_returned, memory_order_acquire) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    osoe_futex_wake_bitset(&bit_word, INT_MAX, 2, OSOE_FUTEX_PRIVATE);
    pthread_join(t, NULL);
    CHECK(bit_result == 0);
    CHECK(elapsed_since(&t0) < 1.0);

    /* A signal does not end a wait, whatever its handler's SA_RESTART: the
     * classic timed wait comes back with EINTR from every handler, and a
     * profiler's timer ended every wait early, so a waiter that makes a fresh
     * sweep deadline each pass never swept (the adversarial read of v0.2 step
     * 1, parts 2 and 3). */
    wait_under_signals(SA_RESTART);
    wait_under_signals(0);

    /* A wake of none wakes no one - the classic call woke one - and a wake of
     * one then does. The word stays 0, so only a wake ends the wait before its
     * deadline. */
    atomic_store_explicit(&none_word, 0, memory_order_release);
    CHECK(pthread_create(&t, NULL, none_waiter, NULL) == 0);
    pause_ms(100);
    osoe_futex_wake(&none_word, 0, OSOE_FUTEX_PRIVATE);
    pause_ms(200);
    CHECK(atomic_load_explicit(&none_returned, memory_order_acquire) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    osoe_futex_wake(&none_word, 1, OSOE_FUTEX_PRIVATE);
    pthread_join(t, NULL);
    CHECK(none_result == 0);
    CHECK(elapsed_since(&t0) < 1.0);

    /* Deadline arithmetic carries nanoseconds into seconds. */
    d = osoe_deadline_in(1999999999ull);
    CHECK(d.at.tv_nsec >= 0 && d.at.tv_nsec < 1000000000L);
    return CHECK_DONE();
}
