/*
 * A reply is not starved by puts (§3.8's DRAINING state; review round two, C7).
 *
 * Replies - get replies, AMO results, and the barrier's own arrivals and
 * releases - reach the wire only when the progress thread can reserve room for
 * them in a connection buffer. Application writers reserve in the same buffer.
 * If they always get there first, a reply waits as long as they keep writing:
 * not a deadlock, since it goes out once they stop, but a get whose latency is
 * set by somebody else's put loop, and a barrier that cannot complete while
 * any PE of the far node is still streaming data.
 *
 * So: node 1 holds the get's target and a crowd of writers, all putting toward
 * node 0 as fast as they can; node 0's one PE issues a few small gets toward
 * node 1 and then tells the writers to stop. Every get must complete while the
 * writers are still running - which the writers themselves report, by saying
 * whether they stopped because they were told to or because their own safety
 * deadline passed. No clock is compared between PEs (§8.2): a writer that hit
 * its deadline means the gets did not finish in time, whatever the timings.
 *
 * The connection is made as small as §3.4 allows and the puts sized so that two
 * fill a buffer, which is the configuration most hostile to a reply: every
 * freed buffer is one writer's reservation away from full.
 *
 *   OSOE_TEST_PUT_BYTES   bytes per put (default 900)
 *   OSOE_TEST_GETS        gets node 0 issues (default 20)
 *   OSOE_TEST_DEADLINE_MS writers' own stop, the test's time bound (default 10000)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { TOLD_TO_STOP = 1, DEADLINE_PASSED = 2 };

static long env_long(const char *name, long fallback)
{
    const char *v = getenv(name);
    return v && *v ? atol(v) : fallback;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    size_t put_bytes = (size_t)env_long("OSOE_TEST_PUT_BYTES", 900);
    long gets = env_long("OSOE_TEST_GETS", 20);
    uint64_t deadline_ns = (uint64_t)env_long("OSOE_TEST_DEADLINE_MS", 10000) * 1000000u;

    /* Symmetric statics are §4.3's phase 1.5, so the flags are allocated. */
    int *stop = shmem_calloc(2, sizeof *stop), *why = stop + 1;
    char *landing = shmem_malloc(put_bytes);
    long *source = shmem_malloc(sizeof *source);
    char *payload = malloc(put_bytes);
    CHECK_FATAL(stop && landing && source && payload);
    memset(payload, 7, put_bytes);
    *source = 1000 + me;
    shmem_barrier_all();

    if (me == 0) {
        /* Let the writers saturate the connection before asking for anything. */
        struct timespec settle = { 0, 100 * 1000000L };
        nanosleep(&settle, NULL);
        uint64_t worst = 0;
        for (long i = 0; i < gets; i++) {
            uint64_t t0 = now_ns();
            long v = 0;
            shmem_getmem(&v, source, sizeof v, 1);
            uint64_t took = now_ns() - t0;
            if (took > worst)
                worst = took;
            CHECK(v == 1001);
        }
        printf("PE 0: %ld gets toward node 1 under %d writers, slowest %.3f ms\n", gets,
               npes - 2, (double)worst / 1e6);
        int one = 1;
        for (int pe = 2; pe < npes; pe++)
            shmem_putmem(stop, &one, sizeof one, pe);
        shmem_quiet();
    } else if (me >= 2) {
        uint64_t t0 = now_ns();
        long puts = 0;
        for (;;) {
            if (shmem_int_atomic_fetch(stop, me)) {
                *why = TOLD_TO_STOP;
                break;
            }
            if (now_ns() - t0 > deadline_ns) {
                *why = DEADLINE_PASSED;
                break;
            }
            shmem_putmem(landing, payload, put_bytes, 0);
            puts++;
        }
        shmem_quiet();
        printf("PE %d: %ld puts of %zu bytes, %s\n", me, puts, put_bytes,
               *why == TOLD_TO_STOP ? "told to stop" : "stopped at its own deadline");
    }

    shmem_barrier_all();
    if (me == 0)
        for (int pe = 2; pe < npes; pe++) {
            int reason = 0;
            shmem_getmem(&reason, why, sizeof reason, pe);
            CHECK(reason == TOLD_TO_STOP);
        }
    CHECK_INVARIANTS();
    free(payload);
    shmem_free(stop);
    shmem_free(source);
    shmem_free(landing);
    shmem_finalize();
    return CHECK_DONE();
}
