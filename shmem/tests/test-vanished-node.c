/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A node that vanishes - its link dropping everything: no FIN, no reset - is
 * found by TCP keepalive (§3.15; phase 1 step 4 slice 1, part D). Tier 3 only,
 * where tools/launch/tier3.sh can cut a node's link: with TIER3_CUT=NODE it cuts
 * it two seconds after rank 0 has created the file OSOE_TEST_READY names, after
 * init and a barrier - long enough for what the job last sent to be
 * acknowledged, which keepalive, probing only an idle connection, needs.
 *
 * Then the ranks of the last node sleep outside the library, and every other
 * rank waits in a barrier the last node never reaches, so every connection to
 * that node is idle when it vanishes - keepalive's case; with data in flight
 * the kernel's retransmission limit ends it instead. Sleeping, not spinning:
 * ranks that spin can hold their node's agent off the CPU long enough that an
 * acknowledgement it owes goes out after the cut, and waits on that limit. vanished-node.sh requires
 * the job to end with a fatal saying which node stopped answering, and every
 * rank to leave with the failure status.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

enum { COMPUTE_SECONDS = 30 };

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    shmem_barrier_all();
    const char *ready = getenv("OSOE_TEST_READY");
    if (me == 0 && ready) {
        int fd = open(ready, O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            close(fd);
    }
    if (me < npes / 2) {
        shmem_barrier_all();                    /* never completes */
        fprintf(stderr, "PE %d: BARRIER RETURNED\n", me);
        return 3;
    }
    for (int s = 0; s < COMPUTE_SECONDS; s++)
        sleep(1);
    fprintf(stderr, "PE %d: NOT TERMINATED after %d s of computing\n", me, COMPUTE_SECONDS);
    return 3;
}
