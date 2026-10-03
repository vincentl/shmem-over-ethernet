/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* A small whole-job program for odd shapes: uneven PEs per node, one PE per
 * node, P not a power of two, one PE in total. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shmem.h>
int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    long *slot = shmem_malloc(sizeof(long) * 1024);
    long *count = shmem_malloc(sizeof(long));
    if (!slot || !count) { fprintf(stderr, "PE %d: malloc failed\n", me); return 1; }
    memset(slot, 0, sizeof(long) * 1024); *count = 0;
    shmem_barrier_all();
    long src[1024];
    for (int i = 0; i < 1024; i++) src[i] = me * 10000L + i;
    shmem_putmem(slot, src, sizeof src, (me + 1) % n);
    shmem_long_atomic_add(count, me + 1, 0);
    shmem_barrier_all();
    int from = (me + n - 1) % n, bad = 0;
    for (int i = 0; i < 1024; i++) bad += slot[i] != from * 10000L + i;
    long got[4];
    shmem_getmem(got, slot, sizeof got, (me + 1) % n);
    bad += got[3] != me * 10000L + 3;
    if (me == 0 && *count != (long)n * (n + 1) / 2) bad++;
    int shared = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    if (bad) fprintf(stderr, "PE %d of %d: %d errors\n", me, n, bad);
    else if (me == 0) printf("ring ok: %d PEs, TEAM_SHARED here has %d\n", n, shared);
    shmem_free(count); shmem_free(slot);
    shmem_finalize();
    return bad != 0;
}
