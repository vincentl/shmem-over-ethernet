/*
 * Atomic memory operations (spec §9.7) between local PEs: every operation is
 * applied exactly once and is atomic against every other path, whether issued
 * by the owning PE or by another (§4.1). Both widths run, since the 32-bit path
 * is the one the wait routines need (§8.2).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { ROUNDS = 500 };

/* Every PE fetch-adds ROUNDS times on PE 0's counter. The counter must end at
 * ROUNDS * npes, and the returned previous values, gathered from every PE, must
 * be exactly 0 .. ROUNDS * npes - 1: one per operation, none lost, none
 * duplicated. */
static void contended_fetch_add(int me, int n)
{
    uint64_t *counter = shmem_malloc(sizeof *counter);
    uint64_t *seen = shmem_malloc(sizeof *seen * ROUNDS * (size_t)n);
    CHECK(counter && seen);
    *counter = 0;
    memset(seen, 0xff, sizeof *seen * ROUNDS * (size_t)n);
    shmem_barrier_all();

    uint64_t mine[ROUNDS];
    for (int i = 0; i < ROUNDS; i++)
        mine[i] = shmem_uint64_atomic_fetch_add(counter, 1, 0);
    shmem_putmem(seen + (size_t)me * ROUNDS, mine, sizeof mine, 0);
    shmem_barrier_all();

    if (me == 0) {
        CHECK(*counter == (uint64_t)ROUNDS * (uint64_t)n);
        char *hit = calloc(ROUNDS * (size_t)n, 1);
        CHECK(hit != NULL);
        int bad = 0;
        for (size_t i = 0; i < ROUNDS * (size_t)n; i++) {
            if (seen[i] >= ROUNDS * (uint64_t)n || hit[seen[i]])
                bad++;
            else
                hit[seen[i]] = 1;
        }
        CHECK(bad == 0);
        free(hit);
    }
    shmem_barrier_all();
    shmem_free(seen);
    shmem_free(counter);
}

/* A spin lock out of compare-and-swap, as hclib's work stealing uses (§8.6 U3):
 * only the PE holding the lock may increment, so the total is exact. */
static void cswap_lock(int me, int n)
{
    uint32_t *lock = shmem_malloc(sizeof *lock);
    uint64_t *guarded = shmem_malloc(sizeof *guarded);
    CHECK(lock && guarded);
    *lock = 0;
    *guarded = 0;
    shmem_barrier_all();

    for (int i = 0; i < 50; i++) {
        while (shmem_uint32_atomic_compare_swap(lock, 0, (uint32_t)me + 1, 0) != 0)
            ;
        uint64_t v = shmem_uint64_atomic_fetch(guarded, 0);
        shmem_uint64_atomic_set(guarded, v + 1, 0);
        CHECK(shmem_uint32_atomic_swap(lock, 0, 0) == (uint32_t)me + 1);
    }
    shmem_barrier_all();
    if (me == 0)
        CHECK(*guarded == 50u * (uint64_t)n);
    shmem_barrier_all();
    shmem_free(guarded);
    shmem_free(lock);
}

/* One PE at a time, so each operation's result is exactly defined. */
static void semantics(int me, int n)
{
    uint64_t *a = shmem_malloc(sizeof *a);
    uint32_t *b = shmem_malloc(sizeof *b);
    CHECK(a && b);
    for (int t = 0; t < n; t++) {
        shmem_barrier_all();
        if (me != t % n)
            continue;
        int peer = (t + 1) % n;             /* self when n == 1 */
        shmem_uint64_atomic_set(a, 100, peer);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 100);
        CHECK(shmem_uint64_atomic_swap(a, 7, peer) == 100);
        CHECK(shmem_uint64_atomic_compare_swap(a, 7, 9, peer) == 7);      /* swaps */
        CHECK(shmem_uint64_atomic_compare_swap(a, 7, 11, peer) == 9);     /* does not */
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 9);
        shmem_uint64_atomic_add(a, 5, peer);
        shmem_uint64_atomic_inc(a, peer);
        CHECK(shmem_uint64_atomic_fetch_inc(a, peer) == 15);
        CHECK(shmem_uint64_atomic_fetch_add(a, 10, peer) == 16);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 26);

        shmem_uint64_atomic_set(a, 0xf0f0, peer);
        shmem_uint64_atomic_and(a, 0xff00, peer);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 0xf000);
        shmem_uint64_atomic_or(a, 0x000f, peer);
        CHECK(shmem_uint64_atomic_fetch_and(a, 0xffff, peer) == 0xf00f);
        CHECK(shmem_uint64_atomic_fetch_or(a, 0x0ff0, peer) == 0xf00f);
        CHECK(shmem_uint64_atomic_fetch_xor(a, 0xffff, peer) == 0xffff);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 0);
        shmem_uint64_atomic_xor(a, 0x1234, peer);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == 0x1234);

        /* 32 bits, including wraparound in the modulus the spec gives. */
        shmem_uint32_atomic_set(b, 0xfffffffeu, peer);
        shmem_uint32_atomic_inc(b, peer);
        CHECK(shmem_uint32_atomic_fetch_inc(b, peer) == 0xffffffffu);
        CHECK(shmem_uint32_atomic_fetch(b, peer) == 0);
        CHECK(shmem_uint32_atomic_fetch_add(b, 0x80000000u, peer) == 0);
        CHECK(shmem_uint32_atomic_fetch_xor(b, 0xffffffffu, peer) == 0x80000000u);
        CHECK(shmem_uint32_atomic_fetch(b, peer) == 0x7fffffffu);

        /* A 64-bit value beyond 32 bits is not truncated. */
        shmem_uint64_atomic_set(a, UINT64_C(0x123456789abcdef0), peer);
        CHECK(shmem_uint64_atomic_fetch(a, peer) == UINT64_C(0x123456789abcdef0));
    }
    shmem_barrier_all();
    shmem_free(b);
    shmem_free(a);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    semantics(me, n);
    contended_fetch_add(me, n);
    cswap_lock(me, n);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
