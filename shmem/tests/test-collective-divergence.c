/*
 * PEs that call different collectives end the job by name (§5.5's "asserted on
 * receipt"; internode-generic.c's piece headers): the PE whose environment has
 * OSOE_TEST_DIVERGE=1 calls a broadcast where every other PE calls a
 * reduction. Its lead's arrival counts toward its parent's reduction, but it
 * put no partial result, so the parent finds the slot holding something other
 * than the piece it waits for. With OSOE_TEST_DIVERGE_KIND=gather the one PE
 * fcollects a long where the others or-reduce as many: the same bytes over the
 * same tree, told apart only by the gather's mark in the piece header. The
 * test passes when the job ends with the fatal's words (CMakeLists.txt); any
 * other ending fails it.
 */
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

enum { COUNT = 8 };

int main(void)
{
    shmem_init();
    long *src = shmem_malloc(COUNT * sizeof *src);
    long *dst = shmem_malloc(COUNT * sizeof *dst);
    if (!src || !dst)
        return 2;
    for (int i = 0; i < COUNT; i++)
        src[i] = shmem_my_pe();
    const char *diverge = getenv("OSOE_TEST_DIVERGE");
    const char *kind = getenv("OSOE_TEST_DIVERGE_KIND");
    int npes = shmem_n_pes();
    if (kind && kind[0] == 'g') {
        /* A gather against an or of the same bytes, which the generic
         * implementation runs a gather as: only the piece header tells them
         * apart (internode-generic.c, its gather bit). */
        if (diverge && diverge[0] == '1')
            shmem_long_fcollect(SHMEM_TEAM_WORLD, dst, src, 1);
        else
            shmem_ulong_or_reduce(SHMEM_TEAM_WORLD, (unsigned long *)dst,
                                  (const unsigned long *)src, (size_t)npes);
    } else if (diverge && diverge[0] == '1') {
        shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, COUNT, 0);
    } else {
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, COUNT);
    }
    printf("PE %d returned from its collective, which it must not\n", shmem_my_pe());
    shmem_finalize();
    return 1;
}
