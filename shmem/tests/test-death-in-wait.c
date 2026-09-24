/*
 * A PE that dies while its siblings are blocked in a library wait ends the job
 * with a loud failure naming where each survivor was, not a hang (§3.15), in
 * every build.
 *
 * The last rank kills itself with SIGKILL - the one signal no handler can
 * intercept, so nothing in the library runs on its way out and its slot is left
 * exactly as a crash would leave it. Every other PE then blocks forever in the
 * routine named by OSOE_TEST_WAIT: a barrier the dead PE will never arrive at,
 * or a wait_until on a flag it will never write. The lead's agent finds the
 * death by its pidfd and declares the node's abort, and each survivor ends -
 * from its agent, which names the routine its thread waits in, or from that
 * wait's own sweep, which names it too. This was the development watchdog's
 * test until it was retired (2026-09-23).
 *
 * Run under `osoe-launch -k`, so the launcher does not terminate the survivors
 * when it reaps the victim: what ends this job has to be the library.
 */
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    const char *which = getenv("OSOE_TEST_WAIT");
    bool in_wait = which && strcmp(which, "wait_until") == 0;

    uint64_t *flag = shmem_malloc(sizeof *flag);
    CHECK_FATAL(flag);  /* NULL would be dereferenced just below */
    *flag = 0;
    shmem_barrier_all();

    if (n == 1) {
        /* Nothing can die on a one-PE job, and nothing else here would block.
         * Say so rather than pretending to have tested it. */
        fprintf(stderr, "death-in-wait: one PE, nothing to detect\n");
        shmem_free(flag);
        shmem_finalize();
        return CHECK_DONE();
    }

    if (me == n - 1) {
        raise(SIGKILL);
        fprintf(stderr, "death-in-wait: SIGKILL did not kill PE %d\n", me);
        return 2;
    }

    /* Unreachable in a passing run: the abort ends this PE inside one of these. */
    if (in_wait)
        shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 1);
    else
        shmem_barrier_all();

    fprintf(stderr, "death-in-wait: PE %d returned from a wait the dead PE never "
                    "completed\n", me);
    return 2;
}
