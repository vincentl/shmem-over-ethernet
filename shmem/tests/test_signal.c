/*
 * Signalling operations (spec §9.8, §9.11.15).
 *
 * The producer-consumer case first: a PE fills a payload, sends it with
 * shmem_putmem_signal, and its neighbour blocks in shmem_signal_wait_until and
 * then checks the payload. That the payload is always complete when the signal
 * arrives is invariant 5, which put-with-signal buys with OSOE_TP_FENCED
 * (§4.10). The payload is large enough that a signal overtaking it would be
 * seen: a memcpy of 64 KiB that had not finished would leave the tail unwritten.
 *
 * Then signal_add and signal_set racing a waiter, signal_fetch, and the return
 * value of signal_wait_until.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"

enum { PAYLOAD = 8192, ROUNDS = 40, ADDERS_PER_ROUND = 3 };

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    int right = (me + 1) % n, left = (me + n - 1) % n;

    uint64_t *data = shmem_malloc(PAYLOAD * sizeof *data);
    uint64_t *sig = shmem_malloc(sizeof *sig);
    uint64_t *mine = shmem_malloc(PAYLOAD * sizeof *mine);
    CHECK(data && sig && mine);
    *sig = 0;
    memset(data, 0, PAYLOAD * sizeof *data);
    shmem_barrier_all();

    /* Producer-consumer around the ring, SHMEM_SIGNAL_ADD. Round k's signal
     * value is k, so a waiter cannot mistake round k - 1's arrival for it. */
    for (int k = 1; k <= ROUNDS; k++) {
        for (int i = 0; i < PAYLOAD; i++)
            mine[i] = (uint64_t)(me * 1000003 + k * 7919 + i);
        shmem_putmem_signal(data, mine, PAYLOAD * sizeof *mine, sig, 1, SHMEM_SIGNAL_ADD,
                            right);
        uint64_t got = shmem_signal_wait_until(sig, SHMEM_CMP_GE, (uint64_t)k);
        CHECK(got >= (uint64_t)k);
        for (int i = 0; i < PAYLOAD; i++)
            if (data[i] != (uint64_t)(left * 1000003 + k * 7919 + i)) {
                fprintf(stderr, "PE %d round %d: data[%d] = %llu, want %llu\n", me, k, i,
                        (unsigned long long)data[i],
                        (unsigned long long)(left * 1000003 + k * 7919 + i));
                CHECK(0);
                break;
            }
        /* The next round overwrites `data`, so nobody may still be reading it. */
        shmem_barrier_all();
    }
    CHECK(shmem_signal_fetch(sig) == ROUNDS);

    /* SHMEM_SIGNAL_SET through put-with-signal, and nelems 0, which still
     * signals. */
    *sig = 0;
    shmem_barrier_all();
    shmem_putmem_signal(data, mine, 0, sig, 0x5eed, SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 0x5eed) == 0x5eed);
    CHECK(shmem_signal_fetch(sig) == 0x5eed);

    /* signal_add racing a waiter: every PE adds to its right neighbour several
     * times per round while that neighbour is blocked in signal_wait_until.
     * Sleeping first makes the waiter reach the wait before the first add. */
    *sig = 0;
    shmem_barrier_all();
    for (int k = 1; k <= ROUNDS; k++) {
        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        for (int a = 0; a < ADDERS_PER_ROUND; a++)
            shmem_signal_add(sig, 1, right);
        uint64_t got = shmem_signal_wait_until(sig, SHMEM_CMP_GE,
                                               (uint64_t)k * ADDERS_PER_ROUND);
        CHECK(got >= (uint64_t)k * ADDERS_PER_ROUND);
    }
    CHECK(shmem_signal_fetch(sig) == (uint64_t)ROUNDS * ADDERS_PER_ROUND);

    /* signal_set racing a waiter. A set is not idempotent across rounds, so
     * each round sets a distinct value and the waiter waits for exactly it. */
    shmem_barrier_all();
    for (int k = 1; k <= ROUNDS; k++) {
        shmem_signal_set(sig, (uint64_t)k << 32 | 0xabcd, right);
        CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, (uint64_t)k << 32 | 0xabcd)
              == ((uint64_t)k << 32 | 0xabcd));
        shmem_barrier_all();
    }

    /* A signal to the PE's own signal object, which the waiter is about to
     * check: already satisfied, so the wait returns without blocking. */
    shmem_signal_set(sig, 99, me);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 99) == 99);
    CHECK(shmem_signal_fetch(sig) == 99);

    shmem_free(mine);
    shmem_free(sig);
    shmem_free(data);
    shmem_finalize();
    return CHECK_DONE();
}
