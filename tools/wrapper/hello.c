/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The compiler wrapper's test program: it builds with nothing but oshcc, runs,
 * and checks that it is the job the launcher started.
 */
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    const char *want = getenv("OSOE_STEP_NUM_TASKS");
    int ok = want && atoi(want) == npes;
    long *word = shmem_malloc(sizeof *word);
    ok = ok && word;
    if (word) {
        *word = me;
        shmem_barrier_all();
        ok = ok && shmem_long_g(word, (me + 1) % npes) == (me + 1) % npes;
        shmem_barrier_all();
        shmem_free(word);
    }
    printf("PE %d of %d: %s\n", me, npes, ok ? "ok" : "WRONG");
    shmem_finalize();
    return ok ? 0 : 1;
}
