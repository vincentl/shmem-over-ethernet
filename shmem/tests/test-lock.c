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
 * With OSOE_TEST_LOCK_MISUSE=clear a PE clears a lock it does not hold, and with
 * =twice it sets one it holds: each must end the job by name
 * (shmem.lock.misuse.*).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 50, BULK_ROUNDS = 10, BULK = (1 << 20) / sizeof(long) };

static int me, npes;

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

    counter(lock, count);
    *count = 0;
    shmem_barrier_all();
    testing(lock, count);
    array_of_locks();

    shmem_free(lock);
    shmem_free(count);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
