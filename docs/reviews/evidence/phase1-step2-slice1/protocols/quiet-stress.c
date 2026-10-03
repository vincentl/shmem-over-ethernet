/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Evidence for finding P2 (report.md): with the acknowledgement echo removed
 * the obvious way (naive-echo-fix.patch), quiet can wait for an acknowledgement
 * nobody was asked for, because the request is attached to whichever segment is
 * finalized next and that may be an *earlier* one than the segment holding the
 * write.
 *
 * Two nodes, one PE each. PE 0 loops: put, put, quiet. On the unmodified tree
 * this always completes (the echo keeps acknowledgements flowing for ever).
 */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    long *x = shmem_malloc(2 * sizeof *x);
    x[0] = x[1] = 0;
    shmem_barrier_all();
    const char *n = getenv("OSOE_STRESS_ROUNDS");
    long rounds = n ? atol(n) : 20000;
    if (me == 0) {
        for (long i = 1; i <= rounds; i++) {
            shmem_putmem(&x[0], &i, sizeof i, 1);
            shmem_putmem(&x[1], &i, sizeof i, 1);
            shmem_quiet();
        }
        fprintf(stderr, "stress: %ld rounds of put, put, quiet completed\n", rounds);
    }
    shmem_barrier_all();
    shmem_free(x);
    shmem_finalize();
    return 0;
}
