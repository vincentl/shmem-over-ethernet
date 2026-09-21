/* futex wrappers: absolute deadlines and wakes (§3.10, invariant 18). */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

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

int main(void)
{
    struct timespec t0;

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

    /* Deadline arithmetic carries nanoseconds into seconds. */
    d = osoe_deadline_in(1999999999ull);
    CHECK(d.at.tv_nsec >= 0 && d.at.tv_nsec < 1000000000L);
    return CHECK_DONE();
}
