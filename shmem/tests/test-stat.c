/*
 * osoe-stat against a live job, and against one deliberately stalled (§3.14).
 *
 * The tool's whole claim is that it reads a job that is not cooperating - a job
 * that is, in the case that matters, wedged. A test that only ran it against a
 * job about to exit would demonstrate none of that, so this test makes a PE sit
 * in a wait that nothing will satisfy until the observer says so, and checks
 * the two things that make such a job diagnosable:
 *
 *   - the **"blocked in" record** names the routine that PE is stuck in, read
 *     from another process under the generation protocol while that PE is still
 *     there to be read;
 *   - the **heartbeat** rises. `pe.sweeps` is the difference between "this PE is
 *     asleep" and "this PE is dead": a blocked PE's safety sweep still expires
 *     every OSOE_SWEEP_NS, so its sweep count climbs while it waits.
 *
 * The observer reads the segment two ways, deliberately. The precise assertions
 * go through the same reader osoe-stat uses (segment.h), because grepping a
 * table for a number is a test of the table's formatting. The tool itself is
 * then run as a subprocess, because "an installed binary reads this job" is the
 * claim, and only running the binary tests it.
 *
 * On one node a world rank and a local PE id are the same number (§6.1), which
 * is what lets the observer index the segment's slots by `shmem_my_pe`.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

enum { STALL_POLL_MS = 5, STALL_GIVE_UP_MS = 10000, HEARTBEAT_MS = 350 };

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/* Run osoe-stat with `args` on this job's segment and collect its output. */
static int run_stat(const char *args, const char *segment, char *out, size_t len)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        return -1;
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' %s '%s' 2>&1", tool, args, segment);
    FILE *f = popen(cmd, "r");
    if (!f) {
        perror("popen");
        return -1;
    }
    size_t n = fread(out, 1, len - 1, f);
    out[n] = '\0';
    return pclose(f);
}

/* What the tool must say about any job it can read at all. */
static void check_live_output(const char *segment, int n)
{
    char out[64 * 1024];

    CHECK(run_stat("-c", segment, out, sizeof out) == 0);
    CHECK(strstr(out, segment) != NULL);
    CHECK(strstr(out, "invariant.missed_wake") != NULL);
    /* One row per local PE, and the counters really were incremented by the
     * work this test has already done. */
    for (int i = 0; i < n; i++) {
        char pe[16];
        snprintf(pe, sizeof pe, "PE%d", i);
        CHECK(strstr(out, pe) != NULL);
    }
    CHECK(strstr(out, "shmem.barrier.calls") != NULL);

    CHECK(run_stat("-d", segment, out, sizeof out) == 0);
    CHECK(strstr(out, "pe.blocked (read)") != NULL);
    CHECK(strstr(out, "pe.watch (decoded)") != NULL);
    CHECK(strstr(out, "counter blocks") != NULL);

    CHECK(run_stat("-j -c", segment, out, sizeof out) == 0);
    CHECK(out[0] == '{');
    CHECK(strstr(out, "\"transport.rmw.calls\"") != NULL);

    CHECK(run_stat("-T", segment, out, sizeof out) == 0);
    CHECK(strstr(out, "application") != NULL);
    CHECK(strstr(out, "agent") != NULL);
}

/*
 * PE 0 watches PE 1 sit in a wait. Returns once it has seen what it came for,
 * or given up; either way the caller releases the waiter, so a failure here is
 * a failed assertion and not a hung test.
 */
static void observe_stalled(const char *segment, uint32_t who)
{
    struct osoe_segment s;
    char err[256];
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK(0);
        return;
    }

    /* Wait for the record to appear: it is published when the wait sleeps, not
     * when it is entered (§3.14 cost rules), so there is nothing to see until
     * PE 1 has really blocked. */
    struct osoe_segment_blocked b = { 0 };
    unsigned waited = 0;
    for (; waited < STALL_GIVE_UP_MS; waited += STALL_POLL_MS) {
        if (osoe_segment_blocked(&s, who, &b) && b.valid && b.blocked &&
            strstr(b.routine, "wait_until"))
            break;
        sleep_ms(STALL_POLL_MS);
    }
    CHECK(waited < STALL_GIVE_UP_MS);
    CHECK(b.valid && b.blocked);
    CHECK(strcmp(b.routine, "shmem_uint64_wait_until") == 0);
    CHECK(b.since_ns != 0 && b.since_ns < osoe_segment_now_ns());

    /* The watch word says it is waiting on its own heap, not on nothing. */
    uint64_t rng = 0;
    CHECK(osoe_segment_read_name(&s, "pe.watch_rng", who, &rng));
    CHECK(rng != 0);

    /* The heartbeat: a blocked PE still sweeps. */
    uint64_t before = 0, after = 0;
    CHECK(osoe_segment_read_name(&s, "pe.sweeps", who, &before));
    sleep_ms(HEARTBEAT_MS);
    CHECK(osoe_segment_read_name(&s, "pe.sweeps", who, &after));
    CHECK(after > before);

    /* And the tool says so too, in as many words. */
    char out[64 * 1024];
    CHECK(run_stat("-c", segment, out, sizeof out) == 0);
    CHECK(strstr(out, "shmem_uint64_wait_until") != NULL);

    osoe_segment_close(&s);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();

    char segment[OSOE_SEGMENT_NAME_MAX];
    osoe_test_segment_name(segment, sizeof segment, 0);

    uint64_t *flag = shmem_malloc(sizeof *flag);
    CHECK_FATAL(flag != NULL);  /* NULL would be dereferenced just below */
    *flag = 0;
    shmem_barrier_all();

    if (me == 0)
        check_live_output(segment, n);
    shmem_barrier_all();

    /*
     * The stalled job. PE 1 waits on a variable only PE 0 will ever set; PE 0
     * reads it while it waits and then releases it. With one PE there is nobody
     * to watch, so the live case above is the whole test.
     */
    if (n >= 2) {
        if (me == 1) {
            shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 1);
            CHECK(*flag == 1);
        } else if (me == 0) {
            observe_stalled(segment, 1);
            shmem_uint64_atomic_set(flag, 1, 1);
        }
    }
    shmem_barrier_all();

    shmem_free(flag);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
