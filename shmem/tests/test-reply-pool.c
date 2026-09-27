/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The reply descriptor pool (§3.8; review round two, D1).
 *
 * A get or fetching AMO arriving at its target takes a reply descriptor from
 * the node-wide pool; the collective forwards take one too. The origin's
 * per-connection credit C bounds the replies, and §3.8 sizes the pool to match:
 * per connection, C + degree x max_teams + slack, so that no legal program can
 * empty it. Before slice 2 it was sized peers x C - no term for the forwards -
 * and an empty pool dropped the request without a word: the origin waited out
 * a timeout and then blamed the wrong node.
 *
 * Two modes, one program:
 *
 *   default   The review's scenario (performance-scale/limit-pool-exhaustion.c):
 *             one PE streams a large put toward node 0 while node 0's other PEs
 *             issue gets toward it and everyone meets in a barrier, at C = 2,
 *             the smallest credit at which the forwards and the gets can both
 *             be outstanding. Legal, so it must complete.
 *
 *   overflow  OSOE_TEST_POOL=overflow, with the target node given a smaller
 *             OSOE_AM_CREDITS than the origin. That is a broken distributed
 *             invariant - the origin hands out credits the target never sized
 *             for - and the only way left to empty a correctly sized pool. It
 *             must end the job with a fatal on the target naming the peer node,
 *             the queue depths and the kind of request (reply.h), not a drop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), last = shmem_n_pes() - 1;
    const char *mode = getenv("OSOE_TEST_POOL");
    int overflow = mode && strcmp(mode, "overflow") == 0;
    int iterations = overflow ? 50 : 100;
    size_t big = (size_t)1 << 20;
    char *buf = shmem_malloc(big);
    char *mine = malloc(big);
    CHECK_FATAL(buf && mine);
    memset(mine, 3, big);
    memset(buf, 0, big);
    shmem_barrier_all();

    for (int it = 0; it < iterations; it++) {
        if (me == last) {
            if (!overflow)
                shmem_putmem(buf, mine, big, 0);
        } else if (me != 0 || overflow) {
            for (int i = 0; i < 300; i++)
                shmem_getmem(mine, buf, 8, last);
        }
        shmem_barrier_all();
    }

    if (me == 0)
        printf("completed %d iterations\n", iterations);
    CHECK_INVARIANTS();
    free(mine);
    shmem_free(buf);
    shmem_finalize();
    return CHECK_DONE();
}
