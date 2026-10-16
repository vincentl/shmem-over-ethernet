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
 *
 * OSOE_TEST_GET_BYTES sizes the get, 8 bytes unless it is set. A get of more
 * than a piece issues its pieces together and waits for their bytes, or, past
 * the pieces the landing zone holds, for room - two waits other than a piece's
 * reply, each of which must name the node and the PE too (the adversarial read
 * of v0.2's blocking gets, finding 2).
 *
 * With three PEs, one a node, the room is another node's: PE 2 stops, PE 0's
 * nonblocking get from it fills PE 0's landing zone, and PE 0's blocking get
 * from PE 1 - live, its node answering while its own thread sleeps outside the
 * library - waits for room its own pieces do not hold. That wait must name no
 * node: it named node 1, which owed it nothing (the ninth review, Q5-2). The
 * zone is set to 64 KiB (OSOE_GET_LANDING_BYTES), which the nonblocking get's
 * one piece of 63 KiB and a byte fills to its last extent; were it two pieces,
 * the nonblocking get would wait itself, under its own routine's name. The
 * room is node 2's to give back, and since D25 the wait names it, and PE 2.
 *
 * OSOE_TEST_GET_NBI=1 makes PE 0's get from PE 1 a nonblocking one: past the
 * pieces the zone holds, its issue waits for room inside the call, and that
 * wait must name node 1 and PE 1, which owe every piece the zone holds (D25;
 * the ninth review's Q5-8: it named no node).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    const char *text = getenv("OSOE_TEST_GET_BYTES");
    size_t bytes = text ? strtoul(text, NULL, 10) : sizeof(long);
    CHECK_FATAL(bytes >= sizeof(long));
    long *v = shmem_calloc(1, bytes);
    long *out = malloc(bytes);
    CHECK_FATAL(v && out);
    *v = 99;
    int stopped = shmem_n_pes() == 3 ? 2 : 1;
    size_t filling = 63 * 1024 + 1;
    char *held = stopped == 2 ? shmem_calloc(1, filling) : NULL;
    char *into = stopped == 2 ? malloc(filling) : NULL;
    CHECK_FATAL(stopped == 1 || (held && into));
    shmem_barrier_all();

    if (me == stopped) {
        raise(SIGSTOP);                 /* never continued: the launcher kills it (-k) */
        for (;;)
            pause();
    }
    if (me != 0)                        /* PE 1 of three: its node answers PE 0 */
        for (;;)
            pause();

    struct timespec settle = { 0, 200 * 1000000L };
    nanosleep(&settle, NULL);
    if (stopped == 2)                       /* never lands: node 2 is stopped */
        shmem_getmem_nbi(into, held, filling, 2);
    *out = 0;
    const char *nonblocking = getenv("OSOE_TEST_GET_NBI");
    if (nonblocking && *nonblocking == '1') {
        shmem_getmem_nbi(out, v, bytes, 1); /* past the zone's pieces: never returns */
        shmem_quiet();
    } else {
        shmem_getmem(out, v, bytes, 1);     /* node 1 stopped, or PE 0's zone held: never returns */
    }
    printf("GET RETURNED %ld toward a stopped node\n", *out);
    fflush(stdout);
    free(out);
    shmem_free(v);
    shmem_finalize();
    return CHECK_DONE();
}
