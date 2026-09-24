/*
 * The public headers under every language standard they promise (C99, C11 and
 * C17 here; C++ in headers.cpp), with every warning an error. Declarations and
 * calls only: the test is the compile, since what breaks here breaks a program
 * at its first #include. What each mode sees differs - the C11 generics, and
 * shmem_sync's two forms under one name, which C11 tells apart by argument
 * count and C99 does not have - so each mode compiles the calls it has.
 */
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
    pshmem_sync(0, 0, 4, pSync);
    (void)shmem_long_max_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    shmem_int_atomic_inc(word, 0);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    (void)shmem_sync(SHMEM_TEAM_WORLD);
    (void)shmem_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    shmem_atomic_inc(word, 0);
#endif
}
