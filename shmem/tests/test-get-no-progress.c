/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A blocking get names its target when it gives up (§4.10; diagnosability
 * review, D-6). shmem.stopped-node.no-progress covers the aggregate quiet
 * wait, which has no single PE to name (§4.10's dirty set is per node); this
 * covers the other half - a get or a fetching AMO waits for one PE's reply,
 * and before this fix the fatal said only "no %s reply after %llu s", naming
 * neither the node nor the PE it was waiting on.
 *
 * PE 1 stops itself once its symmetric variable is initialized; PE 0's
 * shmem_getmem then never lands a reply. With OSOE_NO_PROGRESS_TIMEOUT set
 * low, the wait must give up naming node 1 and PE 1 by number.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    long *v = shmem_calloc(1, sizeof *v);
    CHECK_FATAL(v);
    *v = 99;
    shmem_barrier_all();

    if (me == 1) {
        raise(SIGSTOP);                 /* never continued: the launcher kills it (-k) */
        for (;;)
            pause();
    }

    struct timespec settle = { 0, 200 * 1000000L };
    nanosleep(&settle, NULL);
    long out = 0;
    shmem_getmem(&out, v, sizeof out, 1);   /* node 1 is stopped: never returns */
    printf("GET RETURNED %ld toward a stopped node\n", out);
    fflush(stdout);
    shmem_free(v);
    shmem_finalize();
    return CHECK_DONE();
}
