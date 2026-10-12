/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A profiler written as the specification's §10 example is (pshmem_example.c,
 * Example 48 of 1.6): pshmem.h included, and each routine it intercepts
 * defined by its plain name, forwarding to its pshmem_ spelling - here the
 * three whose C11 synopsis gives one name two argument lists, shmem_sync,
 * shmem_signal_add and shmem_signal_set, which shmem.h makes macros that count
 * their arguments. Their form without a context passes its arguments through as
 * written, so each definition below is one of the name in parentheses; when
 * the macros put each argument in parentheses, these did not compile ("expected
 * declaration specifiers", the round-four review's conformance M1). The test
 * is the compile, under C11, where the macros are; and the calls after the
 * definitions show the count still choosing, both forms of each.
 */
#include <pshmem.h>

static long calls;

void shmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe)
{
    calls++;
    pshmem_signal_add(sig_addr, signal, pe);
}

void shmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe)
{
    calls++;
    pshmem_signal_set(sig_addr, signal, pe);
}

void shmem_sync(int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    calls++;
    pshmem_sync(PE_start, logPE_stride, PE_size, pSync);
}

long both_forms(uint64_t *word, long *pSync);
long both_forms(uint64_t *word, long *pSync)
{
    shmem_signal_add(word, 1, 0);
    shmem_signal_add(SHMEM_CTX_DEFAULT, word, 1, 0);
    shmem_signal_set(word, 1, 0);
    shmem_signal_set(SHMEM_CTX_DEFAULT, word, 1, 0);
    shmem_sync(0, 0, 2, pSync);
    return shmem_sync(SHMEM_TEAM_WORLD) + calls;
}
