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

#if defined(__GNUC__)
// The complex reductions, which GCC and Clang let C++ see as an extension
// (shmem.h); __extension__ keeps this file's -Wpedantic quiet about the type.
__extension__ typedef float _Complex complex_float;
__extension__ typedef double _Complex complex_double;

void exercise_complex(complex_float *dst, const complex_float *src, complex_double *d,
                      const complex_double *s, complex_double *work);
void exercise_complex(complex_float *dst, const complex_float *src, complex_double *d,
                      const complex_double *s, complex_double *work)
{
    (void)shmem_complexf_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    (void)shmem_complexd_prod_reduce(SHMEM_TEAM_WORLD, d, s, 1);
    shmem_complexd_sum_to_all(d, s, 1, 0, 0, 4, work, pSync);
}
#endif
