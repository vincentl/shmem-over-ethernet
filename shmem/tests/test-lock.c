/*
 * The distributed lock (spec §9.13; lock.c): mutual exclusion over a counter
 * on PE 0 that every PE increments by a get and a put - a lost update, from two
 * PEs inside at once or from a clear that let the next in before its put had
 * landed, shows as a short count. The first rounds put a mebibyte to PE 0
 * ahead of the count, which a clear without its quiet leaves in flight while
 * the grant and the next holder's get, two small messages on other paths,
 * overtake it: without the bulk the put lands first and such a clear passes.
 * shmem_test_lock's answer while another PE
 * holds the lock and once it is free, and the lock taken by testing alone; an
 * array of locks, whose homes spread over the PEs, each guarding a counter on
 * a different PE; rounds enough that the queue forms.
 *
 * First come, first served, the reason the lock is a queue: PE 0 holds the
 * lock while each PE k joins the queue only once PE k-1 is asleep in
 * shmem_set_lock - its "blocked in" record (§3.14), published after its swap
 * and its link - and each takes a ticket as it enters, which must be its place
 * in the queue. A test-and-set lock passes every other test here and fails
 * this one. Twelve locks held at once, cleared in another order than taken,
 * past the held list's first growth. The counter again with the lock and
 * the counter both statics, as the spec's examples declare them. And a lock
 * held across the last
 * shmem_finalize, which warns, and whose offset is free to set again in the
 * next session.
 *
 * With OSOE_TEST_LOCK_MISUSE=clear a PE clears a lock it does not hold, with
 * =twice it sets one it holds, and with =garbage it sets a lock that was never
 * zeroed: each must end the job by name (shmem.lock.misuse.*).
 */
#define _POSIX_C_SOURCE 200809L       /* nanosleep, under -std=c11 too */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 50, BULK_ROUNDS = 10, BULK = (1 << 20) / sizeof(long) };

static int me, npes;

/* The spec's own examples' lock: a static, zero from the start (§4.3). */
static long static_lock;
static long static_count;

static void counter(long *lock, long *count)
{
    long *bulk = shmem_calloc(BULK, sizeof *bulk);
    long *mine = malloc(BULK * sizeof *mine);
    CHECK_FATAL(bulk && mine);
    for (int r = 0; r < ROUNDS; r++) {
        shmem_set_lock(lock);
        long v = shmem_long_g(count, 0);
        if (r < BULK_ROUNDS) {
            /* The last holder's bulk, which it put before the count, is all
             * here, or the count this PE read is not the last holder's. */
            long last = shmem_long_g(&bulk[BULK - 1], 0);
            if (last > v)
                fprintf(stderr, "PE %d: the bulk says %ld, the count %ld\n", me, last, v);
            CHECK(last <= v);
            for (size_t i = 0; i < BULK; i++)
                mine[i] = v + 1;
            shmem_long_put(bulk, mine, BULK, 0);
        }
        shmem_long_p(count, v + 1, 0);
        shmem_clear_lock(lock);
    }
    shmem_barrier_all();
    free(mine);
    shmem_free(bulk);
    long total = shmem_long_g(count, 0);
    if (total != (long)ROUNDS * npes)
        fprintf(stderr, "PE %d: the counter reads %ld, not %ld\n", me, total,
                (long)ROUNDS * npes);
    CHECK(total == (long)ROUNDS * npes);
    shmem_barrier_all();
}

static void testing(long *lock, long *count)
{
    if (me == 0)
        CHECK(shmem_test_lock(lock) == 0);          /* free: PE 0 takes it */
    shmem_barrier_all();
    if (me != 0)
        CHECK(shmem_test_lock(lock) == 1);          /* held by PE 0 */
    shmem_barrier_all();
    if (me == 0)
        shmem_clear_lock(lock);
    shmem_barrier_all();

    /* Every PE takes it by testing alone, and counts once. */
    while (shmem_test_lock(lock) != 0)
        ;
    long v = shmem_long_g(count, 0);
    shmem_long_p(count, v + 1, 0);
    shmem_clear_lock(lock);
    shmem_barrier_all();
    CHECK(shmem_long_g(count, 0) == npes);
    shmem_barrier_all();
}

static void array_of_locks(void)
{
    long *locks = shmem_calloc((size_t)npes, sizeof *locks);
    long *counts = shmem_calloc((size_t)npes, sizeof *counts);
    CHECK_FATAL(locks && counts);
    for (int r = 0; r < ROUNDS / 5; r++)
        for (int i = 0; i < npes; i++) {
            int k = (me + i) % npes;
            shmem_set_lock(&locks[k]);
            long v = shmem_long_g(&counts[k], k);
            shmem_long_p(&counts[k], v + 1, k);
            shmem_clear_lock(&locks[k]);
        }
    shmem_barrier_all();
    CHECK(counts[me] == (long)(ROUNDS / 5) * npes);
    shmem_free(locks);
    shmem_free(counts);
}

/* Each PE's logical node and local id, so that any PE can read any other's
 * "blocked in" record from that node's control segment. */
static long *where;

static void publish_where(void)
{
    where = shmem_calloc((size_t)npes, sizeof *where);
    CHECK_FATAL(where);
    const char *node = osoe_test_launcher("NODEID"), *local = osoe_test_launcher("LOCALID");
    long mine = (node ? atol(node) : 0) << 32 | (local ? atol(local) : 0);
    for (int pe = 0; pe < npes; pe++)
        shmem_long_p(&where[me], mine, pe);
    shmem_barrier_all();
}

/* Until `pe` is asleep in shmem_set_lock: past its swap and its link, queued. */
static void wait_queued(int pe)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    const char *job = osoe_test_launcher("JOB_ID"), *step = osoe_test_launcher("STEP_ID");
    snprintf(name, sizeof name, "/osoe.%s.%s.%ld.0", job ? job : "0", step ? step : "0",
             where[pe] >> 32);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "PE %d: %s\n", me, err);
        CHECK(0);
        return;
    }
    uint32_t local = (uint32_t)(where[pe] & 0xffffffff);
    struct osoe_segment_blocked b = { 0 };
    int waited = 0;
    for (; waited < 30000; waited++) {
        if (osoe_segment_blocked(&s, local, &b) && b.valid && b.blocked &&
            strcmp(b.routine, "shmem_set_lock") == 0)
            break;
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (waited == 30000)
        fprintf(stderr, "PE %d: PE %d never queued for the lock\n", me, pe);
    CHECK(waited < 30000);
    osoe_segment_close(&s);
}

static void first_come_first_served(long *lock, long *tickets)
{
    if (npes < 3)
        return;                         /* one waiter is in order by itself */
    publish_where();
    if (me == 0)
        shmem_set_lock(lock);
    shmem_barrier_all();
    if (me == 0) {
        wait_queued(npes - 1);          /* the whole queue has formed */
        shmem_clear_lock(lock);
    } else {
        if (me > 1)
            wait_queued(me - 1);
        shmem_set_lock(lock);
        long ticket = shmem_long_atomic_fetch_inc(tickets, 0);
        if (ticket != me - 1)
            fprintf(stderr, "PE %d entered %ld-th, not %d-th\n", me, ticket, me - 1);
        CHECK(ticket == me - 1);
        shmem_clear_lock(lock);
    }
    shmem_barrier_all();
    shmem_free(where);
}

/* Twelve locks at once, taken in address order - so no two PEs deadlock - and
 * cleared odd ones first: the held list grows past its first eight, and
 * forgets each lock by name, not by position. */
static void several_held(void)
{
    enum { HELD = 12 };
    long *locks = shmem_calloc(HELD, sizeof *locks);
    long *counts = shmem_calloc(HELD, sizeof *counts);
    CHECK_FATAL(locks && counts);
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < HELD; i++)
            shmem_set_lock(&locks[i]);
        for (int i = 0; i < HELD; i++)
            shmem_long_atomic_inc(&counts[i], 0);
        for (int i = 1; i < HELD; i += 2)
            shmem_clear_lock(&locks[i]);
        for (int i = 0; i < HELD; i += 2)
            shmem_clear_lock(&locks[i]);
    }
    shmem_barrier_all();
    if (me == 0)
        for (int i = 0; i < HELD; i++)
            CHECK(counts[i] == 4L * npes);
    shmem_free(locks);
    shmem_free(counts);
}

/* PE 0 finalizes holding a lock, which warns; in the next session the same
 * offset - the allocator starts afresh - is a lock like any other. */
static void held_across_sessions(void)
{
    long *lock = shmem_calloc(1, sizeof *lock);
    CHECK_FATAL(lock);
    if (me == 0)
        shmem_set_lock(lock);
    shmem_finalize();

    shmem_init();
    long *again = shmem_calloc(1, sizeof *again);
    CHECK_FATAL(again);
    CHECK(again == lock);
    shmem_set_lock(again);
    shmem_clear_lock(again);
    shmem_barrier_all();
    shmem_free(again);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    long *lock = shmem_calloc(1, sizeof *lock);
    long *count = shmem_calloc(1, sizeof *count);
    CHECK_FATAL(lock && count);

    const char *misuse = getenv("OSOE_TEST_LOCK_MISUSE");
    if (misuse && strcmp(misuse, "clear") == 0) {
        shmem_clear_lock(lock);
        return 1;
    }
    if (misuse && strcmp(misuse, "twice") == 0) {
        shmem_set_lock(lock);
        shmem_set_lock(lock);
        return 1;
    }
    if (misuse && strcmp(misuse, "garbage") == 0) {
        *lock = -1;                     /* never zeroed: the tail names no PE */
        shmem_barrier_all();
        shmem_set_lock(lock);
        return 1;
    }

    counter(lock, count);
    *count = 0;
    shmem_barrier_all();
    testing(lock, count);
    array_of_locks();
    *count = 0;
    shmem_barrier_all();
    first_come_first_served(lock, count);
    several_held();
    counter(&static_lock, &static_count);   /* the lock and its counter statics */

    shmem_free(lock);
    shmem_free(count);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    held_across_sessions();
    CHECK_INVARIANTS_SESSION(1);
    shmem_finalize();
    return CHECK_DONE();
}
