/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A PE that leaves without shmem_finalize ends the job, naming itself (§6.4;
 * second review, D14).
 *
 * `OSOE_TEST_MODE` picks the program:
 *
 *   init       shmem_init. PE 1 calls exit(0) with the library initialized -
 *              outside the specification (§4: a program ends its use of the
 *              library with shmem_finalize or shmem_global_exit). The others
 *              wait in shmem_barrier_all, which PE 1 will never reach. Before
 *              slice 2 that hung until the launcher's timeout, and the
 *              launcher acts only on a nonzero status. Now PE 1's exit is made
 *              a job abort on its node: its siblings stop at their next sweep.
 *
 *   start_pes  start_pes, which the specification finalizes implicitly at exit
 *              (§9.1, Annex F.2.3). PE 1 returns from main, so its implicit
 *              finalize runs; the others call shmem_barrier_all. Before slice
 *              2, finalize's barrier was the world barrier, so it matched
 *              theirs and they returned from a barrier nobody matched. Now
 *              finalize has counters of its own: nobody returns, and the job
 *              hangs - the program's mismatch, which ctest sees as the
 *              launcher's short timeout, while "BARRIER RETURNED" fails it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

int main(void)
{
    const char *mode = getenv("OSOE_TEST_MODE");
    int legacy = mode && strcmp(mode, "start_pes") == 0;
    if (legacy)
        start_pes(0);
    else
        shmem_init();
    shmem_barrier_all();

    if (shmem_my_pe() == 1) {
        if (legacy)
            return 0;               /* implicit finalize, at exit */
        exit(0);                    /* no finalize at all */
    }
    shmem_barrier_all();
    printf("BARRIER RETURNED on PE %d with PE 1 gone\n", shmem_my_pe());
    fflush(stdout);
    if (!legacy)
        shmem_finalize();
    return 0;
}
