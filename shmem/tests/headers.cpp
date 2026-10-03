// SPDX-License-Identifier: LGPL-3.0-or-later
// The public headers as C++ sees them (headers.c has the C modes): no C11
// generics, and the two-form routines as overloads - shmem_sync's, the
// deprecated active-set one with C linkage and the team one inline, and the
// signal updates', the one without a context with C linkage and the one with it
// inline.
#include <shmem.h>
#include <pshmem.h>
#include <shmemx.h>
#include <mpp/shmem.h>

long pSync[SHMEM_REDUCE_SYNC_SIZE];
long pWrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
uint64_t signal_word;

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
    shmem_signal_add(&signal_word, 1, 0);
    shmem_signal_set(&signal_word, 1, 0);
    /* The specification's C++ has no context-first spelling (shmem.h), and a
     * program may take the routine's address, which an overload would make
     * ambiguous. */
    void (*add)(uint64_t *, uint64_t, int) = &shmem_signal_add;
    void (*set)(uint64_t *, uint64_t, int) = &shmem_signal_set;
    add(&signal_word, 1, 0);
    set(&signal_word, 1, 0);
    shmem_ctx_signal_add(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
    shmem_ctx_signal_set(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
}

#if defined(__GNUC__) && !defined(__EDG__) && !defined(__NVCOMPILER) && !defined(__CUDACC__)
// The complex reductions, which g++ and clang++ let C++ see as an extension
// (shmem.h). The condition is restated rather than read from shmem.h's
// OSOE_CXX_COMPLEX, so that a header that stopped defining it fails here;
// __extension__ keeps this file's -Wpedantic quiet about the type.
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
