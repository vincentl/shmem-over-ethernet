/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The public headers under every language standard they promise (C99, C11, C17
 * and C23 here; C++ in headers.cpp), with every warning an error. Declarations
 * and calls only: the test is the compile, since what breaks here breaks a
 * program at its first #include. What each mode sees differs - the C11
 * generics, and the two-form routines under one name (shmem_sync's, and the
 * signal updates': with a context, or without), which C11 and later tell apart
 * by argument count and C99 does not have - so each mode compiles the calls it
 * has.
 */
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
    pshmem_sync(0, 0, 4, pSync);
    (void)shmem_long_max_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    shmem_int_atomic_inc(word, 0);
    shmem_signal_add(&signal_word, 1, 0);
    shmem_signal_set(&signal_word, 1, 0);
    shmem_ctx_signal_add(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
    shmem_ctx_signal_set(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    (void)shmem_sync(SHMEM_TEAM_WORLD);
    (void)shmem_sum_reduce(SHMEM_TEAM_WORLD, dst, src, 1);
    shmem_atomic_inc(word, 0);
    shmem_signal_add(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
    shmem_signal_set(SHMEM_CTX_DEFAULT, &signal_word, 1, 0);
#endif
}
