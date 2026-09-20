/*
 * Evidence for finding P2 (report.md), second program: a put of many chunks,
 * then quiet, at DEFAULT sizes (64 KiB chunk, 128 KiB buffers, 16 KiB wire).
 * The buffers cycle under backpressure, so a sealed segment is usually waiting
 * behind the one in flight when quiet's acknowledgement request is taken, and
 * the request is attached to it rather than to the later segment that holds
 * the put's last chunk.
 */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    size_t bytes = 1u << 20;
    char *x = shmem_malloc(bytes);
    char *src = malloc(bytes);
    memset(src, 7, bytes);
    shmem_barrier_all();
    const char *n = getenv("OSOE_STRESS_ROUNDS");
    long rounds = n ? atol(n) : 200;
    if (me == 0) {
        for (long i = 1; i <= rounds; i++) {
            shmem_putmem(x, src, bytes, 1);
            shmem_quiet();
        }
        fprintf(stderr, "stress: %ld rounds of 1 MiB put, quiet completed\n", rounds);
    }
    shmem_barrier_all();
    shmem_free(x);
    shmem_finalize();
    return 0;
}
