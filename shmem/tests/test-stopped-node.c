/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A quiet waits as long as its peer keeps it waiting (§3.15; second review, D3).
 *
 * No wait in the library gives up because time passed - only because what it
 * watches stopped moving for OSOE_NO_PROGRESS_TIMEOUT, which by default is
 * never. Before slice 2 every internode wait carried the 60 s bootstrap timeout,
 * so a healthy quiet on a slow or briefly stalled network killed the job.
 *
 * Node 1's only PE stops itself (SIGSTOP), taking node 1's lead with it. PE 0
 * writes to it and quiets. PE 1, on node 0, continues node 1 after
 * OSOE_TEST_STOP_MS. With the no-progress limit unset, the quiet must then
 * complete; with it set below the stop, the quiet must end the job naming the
 * connection - both registered, as shmem.stopped-node.*.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    const char *stop = getenv("OSOE_TEST_STOP_MS");
    long stop_ms = stop ? atol(stop) : 3000;
    int *stopped_pid = shmem_calloc(1, sizeof *stopped_pid);
    long *target = shmem_calloc(1, sizeof *target);
    CHECK_FATAL(stopped_pid && target);
    shmem_barrier_all();

    if (me == 2) {
        int pid = (int)getpid();
        shmem_putmem(stopped_pid, &pid, sizeof pid, 1);
        shmem_quiet();
        raise(SIGSTOP);                 /* continued by PE 1 */
    } else if (me == 0) {
        long v = 42;
        struct timespec settle = { 0, 200 * 1000000L };
        nanosleep(&settle, NULL);
        shmem_putmem(target, &v, sizeof v, 2);
        shmem_quiet();
        printf("PE 0: quiet completed after node 1 was stopped for %ld ms\n", stop_ms);
        fflush(stdout);
    } else {
        shmem_int_wait_until(stopped_pid, SHMEM_CMP_NE, 0);
        struct timespec t = { stop_ms / 1000, (stop_ms % 1000) * 1000000L };
        nanosleep(&t, NULL);
        kill((pid_t)*stopped_pid, SIGCONT);
    }

    shmem_barrier_all();
    if (me == 2)
        CHECK(*target == 42);
    CHECK_INVARIANTS();
    shmem_free(target);
    shmem_free(stopped_pid);
    shmem_finalize();
    return CHECK_DONE();
}
