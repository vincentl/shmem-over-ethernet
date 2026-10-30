/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <shmem.h>
#include <stdio.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();

    long *box = shmem_malloc(sizeof *box);  /* collective: every PE gets one */
    *box = -1;
    shmem_barrier_all();                    /* every PE's box is ready */

    shmem_long_p(box, me, (me + 1) % npes); /* write into the next PE's box */
    shmem_barrier_all();                    /* completes the writes, then syncs */

    printf("PE %d of %d: my box holds %ld\n", me, npes, *box);
    shmem_free(box);
    shmem_finalize();
    return 0;
}
