// The public headers as C++ sees them (headers.c has the C modes): no C11
// generics, and shmem_sync's two forms as overloads - the deprecated active-set
// one with C linkage, the team one inline.
#include <shmem.h>
#include <pshmem.h>
#include <shmemx.h>
#include <mpp/shmem.h>

long pSync[SHMEM_REDUCE_SYNC_SIZE];
long pWrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];

void exercise(long *dst, long *src, int *word);
void exercise(long *dst, long *src, int *word)
{
    shmem_long_sum_to_all(dst, src, 1, 0, 0, 4, pWrk, pSync);
    shmem_broadcast64(dst, src, 1, 0, 0, 0, 4, pSync);
    shmem_barrier(0, 0, 4, pSync);
    shmem_sync(0, 0, 4, pSync);
    (void)shmem_sync(SHMEM_TEAM_WORLD);
    (void)shmem_long_max_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    shmem_int_atomic_inc(word, 0);
}
