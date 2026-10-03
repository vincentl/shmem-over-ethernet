/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Spec section 7: SHMEM_TEAM_SHARED is "the team of all PEs that would mutually
 * return a non-null address from a call to shmem_ptr or shmem_team_ptr for all
 * symmetric heap objects". Checks, for every world PE, that membership of
 * SHMEM_TEAM_SHARED and a non-null shmem_ptr agree, and that a store through
 * the pointer is what a getmem then reads. */
#include <shmem.h>
#include <stdio.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes(), bad = 0;
    long *object = shmem_calloc(1, sizeof(long));
    for (int pe = 0; pe < n; pe++) {
        int in_shared = shmem_team_translate_pe(SHMEM_TEAM_WORLD, pe, SHMEM_TEAM_SHARED);
        long *direct = shmem_ptr(object, pe);
        if ((in_shared >= 0) != (direct != NULL)) {
            printf("PE %d: PE %d is %s SHMEM_TEAM_SHARED but shmem_ptr returns %s\n", me, pe,
                   in_shared >= 0 ? "in" : "not in", direct ? "non-null" : "NULL");
            bad = 1;
        }
        if (in_shared >= 0 && shmem_team_ptr(SHMEM_TEAM_SHARED, object, in_shared) != direct) {
            printf("PE %d: shmem_team_ptr(SHARED, %d) disagrees with shmem_ptr(%d)\n", me,
                   in_shared, pe);
            bad = 1;
        }
        if (shmem_addr_accessible(object, pe) != 1 || shmem_pe_accessible(pe) != 1)
            bad = 1;
    }
    if (shmem_ptr(object, me) != (void *)object) {
        printf("PE %d: shmem_ptr to self is not the object\n", me);
        bad = 1;
    }
    /* a put to the next PE (a node-mate under some maps), then read it back */
    long value = 1000 + me, back = -1;
    int next = (me + 1) % n;
    shmem_putmem(object, &value, sizeof value, next);
    shmem_quiet();
    shmem_getmem(&back, object, sizeof back, next);
    if (back != value) {
        printf("PE %d: put/get to PE %d read %ld, expected %ld\n", me, next, back, value);
        bad = 1;
    }
    shmem_barrier_all();
    if (me == 0 && !bad)
        printf("consistent: SHARED has %d of %d PEs\n", shmem_team_n_pes(SHMEM_TEAM_SHARED), n);
    shmem_free(object);
    shmem_finalize();
    return bad;
}
