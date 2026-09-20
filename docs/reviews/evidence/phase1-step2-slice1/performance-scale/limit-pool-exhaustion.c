/*
 * Limits experiment 1: the reply-descriptor pool is sized `peers x C` for gets
 * (reply.h, progress-host.c:233) but collective forwards draw on the same pool
 * with no credit of their own (progress.c:86 enqueue_collective, fatal when empty).
 *
 * A legal program: the last PE (alone on the last logical node, which is NOT the
 * collective root) puts 1 MiB toward node 0 and goes straight into the barrier.
 * While its two outbound buffers are still draining through the wire, get
 * requests from node 0's other PEs queue as descriptors, because drain() cannot
 * reserve. With C descriptors held, the node's own barrier arrival finds the
 * pool empty.
 *
 *   OSOE_AM_CREDITS=2 osoe-launch -n 4 -m 0,0,0,1 -- limit-pool-exhaustion
 *
 * At the default C = 64 the same state needs 64 concurrent getters on one peer
 * node, which a 4-core VM cannot produce but a 256-PE node can.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shmem.h>

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 200;
    shmem_init();
    int me = shmem_my_pe(), last = shmem_n_pes() - 1;
    size_t big = 1 << 20;
    char *buf = shmem_malloc(big);
    char *priv = malloc(big);
    memset(priv, 3, big);
    memset(buf, 0, big);
    shmem_barrier_all();
    for (int it = 0; it < iterations; it++) {
        if (me == last) {
            shmem_putmem(buf, priv, big, 0);
        } else if (me != 0) {
            for (int i = 0; i < 300; i++)
                shmem_getmem(priv, buf, 8, last);
        }
        shmem_barrier_all();
    }
    if (me == 0)
        printf("completed %d iterations\n", iterations);
    shmem_free(buf);
    shmem_finalize();
    return 0;
}
