/*
 * A PE that dies without finalizing ends the job (§3.15; phase 1 step 4
 * slice 1, part C), in a release build as in a development one.
 *
 *   test-dead-pe RANK       world rank RANK dies of SIGKILL after a barrier
 *   test-dead-pe 0 init     rank 0, a lead, dies of SIGALRM during shmem_init,
 *                           3 s in: after its node's rendezvous, however loaded
 *                           the host, and before the job's, which the last node
 *                           holds back by starting 8 s late
 *
 * The lead's agent holds a pidfd for every other local PE and every other
 * PE's agent one for its lead (liveness.h), so whichever of them dies, the
 * agent that sees it declares the node's abort and the abort ends every PE of
 * the job. Of the survivors, the odd ranks compute outside the library, which
 * only their agents can end, and the even ones wait in a barrier the dead PE
 * never reaches. dead-pe.sh requires every survivor to leave with the failure
 * status - none killed by the launcher at its timeout, or still computing 30 s
 * later.
 *
 * In init, before any agent runs, the sweeps of init's own waits watch instead
 * (liveness.h): dead-pe-init.sh requires rank 1, waiting on its lead in the
 * job's rendezvous, to find the death and leave with the failure status. The
 * last node's ranks, which cannot reach a dead lead, are the launcher's.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

enum { COMPUTE_SECONDS = 30 };

int main(int argc, char **argv)
{
    int dies = argc > 1 ? atoi(argv[1]) : 1;
    if (argc > 2 && strcmp(argv[2], "init") == 0) {
        const char *rank = getenv("OSOE_PROCID"), *node = getenv("OSOE_NODEID");
        const char *nodes = getenv("OSOE_STEP_NUM_NODES");
        if (rank && atoi(rank) == dies)
            alarm(3);                           /* SIGALRM ends it inside shmem_init */
        if (node && nodes && atoi(node) == atoi(nodes) - 1 && atoi(nodes) > 1)
            sleep(8);                           /* holds the job's rendezvous open */
    }
    shmem_init();
    int me = shmem_my_pe();
    shmem_barrier_all();
    if (me == dies) {
        struct timespec settle = { 0, 200 * 1000 * 1000 };
        nanosleep(&settle, NULL);               /* the others are in place by now */
        raise(SIGKILL);
    }
    if (me % 2 == 0) {
        shmem_barrier_all();                    /* never completes */
        fprintf(stderr, "PE %d: BARRIER RETURNED\n", me);
        return 3;
    }
    time_t until = time(NULL) + COMPUTE_SECONDS;
    volatile unsigned long spins = 0;
    while (time(NULL) < until)
        spins++;
    fprintf(stderr, "PE %d: NOT TERMINATED after %d s of computing\n", me, COMPUTE_SECONDS);
    return 3;
}
