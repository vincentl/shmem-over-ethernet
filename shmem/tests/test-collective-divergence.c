/*
 * PEs that call different collectives end the job by name (§5.5's "asserted on
 * receipt"; internode-generic.c's piece headers): the PE whose environment has
 * OSOE_TEST_DIVERGE=1 calls a broadcast where every other PE calls a
 * reduction. Its lead's arrival counts toward its parent's reduction, but it
 * put no partial result, so the parent finds the slot holding something other
 * than the piece it waits for. The test passes when the job ends with the
 * fatal's words (CMakeLists.txt); any other ending fails it.
 *
 * With OSOE_TEST_DIVERGE_KIND=gather the one PE fcollects two longs where the
 * others fcollect one: a gather's lead says the shape of the gather it has
 * entered - its epoch, bytes and block - in the READY line of the lead that
 * writes to it first, and that lead finds it differs from its own before it
 * puts a byte (internode-generic.c).
 *
 * Until phase 1 step 5 slice 6 a gather ran as an or over the reduction's tree,
 * and the gather case fcollected where the others or-reduced the same bytes,
 * told apart only by the gather's mark in the piece header. A gather is an
 * allgather now, on words of its own, and meets no reduction's pieces: the two
 * wait in different places, as collectives whose trees share no edge do.
 */
#include <stdbool.h>
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
    bool diverging = diverge && diverge[0] == '1';
    if (kind && kind[0] == 'g') {
        /* dst has room for two longs from every PE, up to COUNT / 2 PEs. */
        if (shmem_n_pes() > COUNT / 2)
            return 2;
        shmem_long_fcollect(SHMEM_TEAM_WORLD, dst, src, diverging ? 2 : 1);
    } else if (diverging) {
        shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, COUNT, 0);
    } else {
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, COUNT);
    }
    printf("PE %d returned from its collective, which it must not\n", shmem_my_pe());
    shmem_finalize();
    return 1;
}
