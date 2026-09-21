/*
 * An internode quiet sweeps like every other wait (§4.10; review round two, D2).
 *
 * §4.10's contract is that every blocking call sweeps at least every
 * OSOE_SWEEP_NS, and the reason is what this test does: a wait that never
 * sweeps never reads the abort word and never runs the liveness check, so when
 * a sibling dies it sits there until the launcher's timeout - and while it sits
 * there it publishes no "blocked in" record, so osoe-stat cannot say where.
 *
 * Node 1's only PE stops itself (SIGSTOP), which stops node 1's lead and with it
 * every acknowledgement node 1 would send. PE 0 then writes to it and quiets:
 * the quiet can never finish. PE 1 runs the real osoe-stat on its node until PE
 * 0 shows as blocked in shmem_quiet, then kills the stopped node (cleanup - the
 * stop is what the test needed) and itself. PE 0's next sweep must find the
 * dead sibling and end the job, naming the routine it was in; ctest checks the
 * message, and the launcher's short timeout is the bound on "next sweep".
 *
 * Run with -k, so the launcher leaves the survivors to the library, and with
 * the development watchdog on, since that is what reads a sibling's liveness
 * at a sweep (§3.15).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/* osoe-stat's per-PE table for this node, or an empty string. */
static void run_stat(char *out, size_t len)
{
    out[0] = '\0';
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool)
        return;
    char segment[OSOE_SEGMENT_NAME_MAX], cmd[512];
    osoe_test_segment_name(segment, sizeof segment, 0);
    snprintf(cmd, sizeof cmd, "'%s' '%s' 2>&1", tool, segment);
    FILE *f = popen(cmd, "r");
    if (!f)
        return;
    size_t n = fread(out, 1, len - 1, f);
    out[n] = '\0';
    pclose(f);
}

/* Does local PE 0's row - the first after the table's header - say it is in
 * shmem_quiet waiting on node 1? On map 0,0,1 local PE 0 of node 0 is rank 0. */
static int pe0_blocked_in_quiet(const char *out)
{
    const char *header = strstr(out, "blocked in\n");
    if (!header)
        return 0;
    const char *row = header + strlen("blocked in\n");
    const char *end = strchr(row, '\n');
    size_t n = end ? (size_t)(end - row) : strlen(row);
    char line[512];
    snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), row);
    return strstr(line, "shmem_quiet (node 1)") != NULL;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    /* Symmetric statics are §4.3's phase 1.5, so these are allocated. */
    int *stopped_pid = shmem_calloc(1, sizeof *stopped_pid);
    long *target = shmem_calloc(1, sizeof *target);
    CHECK_FATAL(stopped_pid && target);
    shmem_barrier_all();

    if (me == 2) {
        /* Tell PE 1 who to clean up, then take node 1's lead off the air. */
        int pid = (int)getpid();
        shmem_putmem(stopped_pid, &pid, sizeof pid, 1);
        shmem_quiet();
        raise(SIGSTOP);
        for (;;)
            pause();
    }

    if (me == 0) {
        long v = 1;
        sleep_ms(200);
        shmem_putmem(target, &v, sizeof v, 2);
        shmem_quiet();                  /* node 1 is stopped: never returns */
        printf("QUIET RETURNED toward a stopped node\n");
        fflush(stdout);
        _Exit(1);
    }

    /* PE 1. */
    shmem_int_wait_until(stopped_pid, SHMEM_CMP_NE, 0);
    char out[64 * 1024];
    int seen = 0;
    for (int tries = 0; tries < 30 && !seen; tries++) {
        sleep_ms(100);
        run_stat(out, sizeof out);
        seen = pe0_blocked_in_quiet(out);
    }
    if (seen)
        printf("PE 1: osoe-stat shows PE 0 blocked in shmem_quiet\n");
    else
        printf("BLOCKED RECORD WRONG: osoe-stat does not show PE 0 in shmem_quiet:\n%s\n", out);
    fflush(stdout);
    kill((pid_t)*stopped_pid, SIGKILL);
    raise(SIGKILL);
    return 1;
}
