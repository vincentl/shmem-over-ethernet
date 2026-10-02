/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A start that hangs inside shmem_init, for the objects it leaves in /dev/shm
 * (start-leftovers.sh; the round-three review's R5a). Four PEs on two nodes,
 * 0,0,1,1:
 *
 *   rank 1, the second PE of node 0, stops inside shmem_init. SIGALRM, 3 s in,
 *       runs a handler that never returns; by then the PE has joined its node
 *       and waits in the job's rendezvous for the tables its lead makes
 *       (job.c), which the handler now keeps it from ever opening.
 *   node 1, both PEs, starts 8 s late, so that node 0's lead is still waiting
 *       for it when rank 1 stops - however loaded the host: rank 1 is stopped
 *       after it has joined, which is what a late start would make of it (the
 *       same margins as dead-pe-init.sh).
 *
 * Node 0's lead then meets node 1's, makes its node's job tables and connection
 * area, and opens each once - and each name stays, since the PEs that must open
 * them are two. The lead goes on, to a barrier that rank 1 never reaches; node 1
 * starts and finishes and waits in it too. The job is cancelled by whoever runs
 * it, as Slurm's scancel would, leaving those two names behind.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <shmem.h>

#include "launcher.h"

enum { STALLED_RANK = 1, STALL_AFTER_SECONDS = 3, LATE_SECONDS = 8 };

static void stall(int signal_number)
{
    (void)signal_number;
    for (;;)
        pause();
}

int main(void)
{
    const char *rank = osoe_test_launcher("PROCID"), *node = osoe_test_launcher("NODEID");
    const char *nodes = osoe_test_launcher("STEP_NUM_NODES");
    if (rank && atoi(rank) == STALLED_RANK) {
        signal(SIGALRM, stall);
        alarm(STALL_AFTER_SECONDS);
    }
    if (node && nodes && atoi(nodes) > 1 && atoi(node) == atoi(nodes) - 1)
        sleep(LATE_SECONDS);
    shmem_init();
    shmem_barrier_all();
    shmem_finalize();
    return 0;
}
