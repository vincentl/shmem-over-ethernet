/*
 * shmem_wait_until and shmem_test (spec §9.11.1, §9.11.8) over the §4.9 watch
 * slot: a PE blocks on its own variable, another PE updates it with an AMO, and
 * the waiter wakes. Both widths, since the 32-bit path is the one the kernel's
 * futex can watch directly and the 64-bit path is the one that forced the
 * sequence word (§4.9).
 */
#include <stdint.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 20, SLEEP_MS = 5 };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    int right = (me + 1) % n, left = (me + n - 1) % n;

    uint64_t *flag = shmem_malloc(sizeof *flag);
    uint32_t *flag32 = shmem_malloc(sizeof *flag32);
    CHECK(flag && flag32);
    *flag = 0;
    *flag32 = 0;
    shmem_barrier_all();

    /* Already satisfied: returns without blocking, and test agrees. */
    CHECK(shmem_uint64_test(flag, SHMEM_CMP_EQ, 0) == 1);
    CHECK(shmem_uint64_test(flag, SHMEM_CMP_NE, 0) == 0);
    shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 0);
    shmem_uint64_wait_until(flag, SHMEM_CMP_LE, 5);
    shmem_uint32_wait_until(flag32, SHMEM_CMP_LT, 1);

    /* A ring: each PE waits for its left neighbour to bump its flag, having
     * bumped its right neighbour's. Every wait must block on the first rounds,
     * since the neighbour sleeps before writing. */
    double t0 = now();
    for (int k = 1; k <= ROUNDS; k++) {
        struct timespec ts = { 0, SLEEP_MS * 1000 * 1000 };
        nanosleep(&ts, NULL);
        shmem_uint64_atomic_add(flag, 1, right);
        shmem_uint64_wait_until(flag, SHMEM_CMP_GE, (uint64_t)k);
        CHECK(shmem_uint64_test(flag, SHMEM_CMP_GE, (uint64_t)k) == 1);
    }
    double elapsed = now() - t0;
    CHECK(*flag == ROUNDS);

    /* Every wake arrived promptly. A lost wake costs a 100 ms safety sweep
     * (§3.10), so systematically losing them would take far longer than this. */
    CHECK(elapsed < ROUNDS * 0.05);

    /* The 32-bit path, with every comparison. Each comparison gets a fresh
     * value: a predicate such as LT against a counter every PE increments can
     * be made false forever by a neighbour one round ahead. */
    static const struct {
        int cmp;
        uint32_t start, set, against;
    } cases[] = {
        { SHMEM_CMP_EQ, 0, 7, 7 },
        { SHMEM_CMP_NE, 0, 1, 0 },
        { SHMEM_CMP_GT, 0, 5, 4 },
        { SHMEM_CMP_GE, 0, 5, 5 },
        { SHMEM_CMP_LT, 9, 2, 3 },
        { SHMEM_CMP_LE, 9, 2, 2 },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        *flag32 = cases[c].start;
        shmem_barrier_all();
        shmem_uint32_atomic_set(flag32, cases[c].set, right);
        shmem_uint32_wait_until(flag32, cases[c].cmp, cases[c].against);
        CHECK(shmem_uint32_test(flag32, cases[c].cmp, cases[c].against) == 1);
        CHECK(*flag32 == cases[c].set);
        shmem_barrier_all();
    }

    /* A set-style AMO wakes a waiter too: it is a seq_cst store (§4.9). */
    *flag = 0;
    shmem_barrier_all();
    shmem_uint64_atomic_set(flag, 42, right);
    shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 42);

    /* So does a compare-and-swap, and a write from the PE's own thread. */
    shmem_barrier_all();
    CHECK(shmem_uint64_atomic_compare_swap(flag, 42, 43, right) == 42);
    shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 43);
    shmem_uint64_atomic_set(flag, 44, me);
    shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 44);

    /* Waiting on one variable and then another reuses the slot. */
    shmem_barrier_all();
    shmem_uint32_atomic_set(flag32, 7, right);
    shmem_uint64_atomic_set(flag, 8, right);
    shmem_uint32_wait_until(flag32, SHMEM_CMP_EQ, 7);
    shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 8);

    (void)left;
    shmem_free(flag32);
    shmem_free(flag);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
