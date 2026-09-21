/*
 * shmem_fence, shmem_quiet and shmem_pe_quiet (spec §9.12.1-§9.12.3).
 *
 * On one node there is never anything outstanding, so no timing here can show
 * these routines doing work; what a test can show is that they mean what the
 * spec says and that each reaches the boundary. Three things, then:
 *
 *   - the contract they exist for: a put made visible by a quiet before the
 *     flag that announces it is set, seen by the target. This is invariant 5
 *     stated at the API rather than inside put-with-signal;
 *   - `shmem_pe_quiet(NULL, 0)`, which spec §9.12.3 makes a conforming call -
 *     target_pes is ignored and the routine returns at once - so passing a null
 *     pointer must not be dereferenced;
 *   - that the calls arrive at the transport boundary at all, counted. This is
 *     read back out of the control segment through osoe-stat's reader (§3.14),
 *     which is a thing the introspection is now good for: an entry point that
 *     quietly did nothing would pass the first two checks.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

enum { N = 64, ROUNDS = 8 };

/* This PE's total for a thread-scope counter, summed over its blocks. */
static uint64_t my_counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);

    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    uint64_t total = 0;
    if (m) {
        uint32_t me = (uint32_t)shmem_my_pe();     /* rank == local id on one node */
        for (uint32_t i = 0; i < s.dir->blocks_per_pe; i++) {
            uint64_t v;
            if (osoe_segment_read(&s, m, me * s.dir->blocks_per_pe + i, &v))
                total += v;
        }
    } else
        total = UINT64_MAX;
    osoe_segment_close(&s);
    return total;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    int right = (me + 1) % n, left = (me + n - 1) % n;

    unsigned char *buf = shmem_malloc(N);
    uint64_t *flag = shmem_malloc(sizeof *flag);
    CHECK_FATAL(buf && flag);  /* NULL would be dereferenced just below */
    memset(buf, 0, N);
    *flag = 0;
    shmem_barrier_all();

    /* npes == 0 ignores target_pes entirely, including a null one. */
    shmem_pe_quiet(NULL, 0);
    int nobody[1] = { 0 };
    shmem_pe_quiet(nobody, 0);

    /* Nothing else in the library calls fence or quiet, so these start at zero
     * and every later count is this test's. The two calls above had npes == 0
     * and must not appear: the spec makes them return before doing anything. */
    CHECK(my_counter("shmem.fence.calls") == 0);
    CHECK(my_counter("shmem.quiet.calls") == 0);
    CHECK(my_counter("shmem.pe_quiet.calls") == 0);

    /*
     * The contract. Write the payload to the right neighbour, quiet, then
     * announce it. The neighbour waits for the announcement and must find the
     * payload complete. Every PE both sends and receives, so no PE is idle and
     * the check runs n ways at once.
     */
    for (int round = 1; round <= ROUNDS; round++) {
        unsigned char want = (unsigned char)(round * 7 + me);
        unsigned char mine[N];
        memset(mine, (int)want, N);

        shmem_putmem(buf, mine, N, right);
        shmem_quiet();
        shmem_uint64_atomic_set(flag, (uint64_t)round, right);

        shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, (uint64_t)round);
        unsigned char expect = (unsigned char)(round * 7 + left);
        int wrong = 0;
        for (int i = 0; i < N; i++)
            if (buf[i] != expect)
                wrong++;
        CHECK(wrong == 0);
        shmem_barrier_all();
    }

    /* The same shape with a fence, which orders delivery rather than waiting
     * for completion: the flag is a write to the same PE issued after the put,
     * so the fence is what keeps it behind (spec §9.12.1, Table 14). */
    for (int round = 1; round <= ROUNDS; round++) {
        unsigned char mine[N];
        memset(mine, (int)(unsigned char)(round + me), N);
        shmem_putmem(buf, mine, N, right);
        shmem_fence();
        shmem_uint64_atomic_set(flag, (uint64_t)(ROUNDS + round), right);

        shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, (uint64_t)(ROUNDS + round));
        unsigned char expect = (unsigned char)(round + left);
        int wrong = 0;
        for (int i = 0; i < N; i++)
            if (buf[i] != expect)
                wrong++;
        CHECK(wrong == 0);
        shmem_barrier_all();
    }

    /* pe_quiet with a real list, including this PE and every other. */
    int all[64];
    for (int i = 0; i < n && i < 64; i++)
        all[i] = i;
    shmem_pe_quiet(all, (size_t)(n < 64 ? n : 64));
    int just_right[1] = { right };
    shmem_pe_quiet(just_right, 1);

    /*
     * Each entry point was called what it was called, and - the part that
     * matters - each reached the *boundary*. An entry point that counted itself
     * and then returned would pass every check above, so the boundary's own
     * counters are checked too. Nothing but shmem_fence calls osoe_transport_fence, so
     * that one is exact; osoe_transport_wait is shared with the barrier (§5.6), so the
     * check there is the identity that composition implies.
     */
    uint64_t api_fence = my_counter("shmem.fence.calls");
    uint64_t api_quiet = my_counter("shmem.quiet.calls");
    uint64_t api_pe_quiet = my_counter("shmem.pe_quiet.calls");
    uint64_t tp_fence = my_counter("transport.fence.calls");
    uint64_t tp_wait = my_counter("transport.wait.calls");
    uint64_t barriers = my_counter("shmem.barrier.calls");

    CHECK(api_fence == ROUNDS);
    CHECK(api_quiet == ROUNDS);
    CHECK(api_pe_quiet == 2);
    CHECK(tp_fence == ROUNDS);
    CHECK(tp_wait == api_quiet + api_pe_quiet + barriers);

    shmem_free(flag);
    shmem_free(buf);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
