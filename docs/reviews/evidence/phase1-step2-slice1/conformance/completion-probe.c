/* Two completion guarantees, checked from a PE that is neither the origin nor
 * the target, which is where "visible to all PEs" (spec 9.12.2, 9.12.3) can be
 * seen to fail:
 *
 *  1. After PE 0's shmem_pe_quiet({1}) returns, PE 2 reads PE 1's buffer with a
 *     get and must see every byte of PE 0's put.
 *  2. A put-with-signal larger than an AM chunk: when PE 1's
 *     shmem_signal_wait_until returns, every byte of the data must be there
 *     (spec 9.8.3: the signal "indicates the delivery of its corresponding dest
 *     data words").
 * Needs at least 3 PEs. */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BYTES = 64 * 1024, ROUNDS = 100 };

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes(), bad = 0;
    if (n < 3) { shmem_finalize(); return 2; }
    unsigned char *buffer = shmem_calloc(BYTES, 1);
    uint64_t *signal = shmem_malloc_with_hints(sizeof *signal, SHMEM_MALLOC_SIGNAL_REMOTE);
    long *go = shmem_calloc(1, sizeof *go);
    *signal = 0;
    unsigned char *local = malloc(BYTES);
    shmem_barrier_all();

    for (int round = 1; round <= ROUNDS; round++) {
        if (me == 0) {
            memset(local, round, BYTES);
            int target = 1;
            shmem_putmem(buffer, local, BYTES, 1);
            shmem_putmem(buffer, local, BYTES, n - 1);   /* a second, unquieted target */
            shmem_pe_quiet(&target, 1);
            shmem_long_atomic_set(go, round, 2);
        } else if (me == 2) {
            shmem_long_wait_until(go, SHMEM_CMP_EQ, round);
            shmem_getmem(local, buffer, BYTES, 1);
            for (int i = 0; i < BYTES; i++)
                if (local[i] != (unsigned char)round) {
                    printf("round %d: after pe_quiet, byte %d on PE 1 is %d\n", round, i, local[i]);
                    bad = 1;
                    break;
                }
        }
        shmem_barrier_all();

        if (me == 0) {
            memset(local, 0x80 | round, BYTES);
            shmem_putmem_signal(buffer, local, BYTES, signal, 1, SHMEM_SIGNAL_ADD, 1);
        } else if (me == 1) {
            uint64_t seen = shmem_signal_wait_until(signal, SHMEM_CMP_EQ, (uint64_t)round);
            if (seen != (uint64_t)round)
                bad = 1;
            for (int i = 0; i < BYTES; i++)
                if (buffer[i] != (unsigned char)(0x80 | round)) {
                    printf("round %d: signalled, but byte %d is %d\n", round, i, buffer[i]);
                    bad = 1;
                    break;
                }
        }
        shmem_barrier_all();
    }
    if (me == 0)
        printf("completion probe: %d rounds of %d bytes\n", ROUNDS, BYTES);
    if (bad)
        printf("PE %d: FAILED\n", me);
    free(local);
    shmem_free(go);
    shmem_free(signal);
    shmem_free(buffer);
    shmem_finalize();
    return bad;
}
