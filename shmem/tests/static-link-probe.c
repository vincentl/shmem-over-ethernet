/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A tiny consumer of the installed API, compiled and linked by
 * cmake/CheckStaticLink.cmake against libosoe.a alone (portability review,
 * F10: a static build's archive did not carry osoe_base, so a third party
 * could not link it).
 *
 * Taking the address of one routine from each of init, the allocator, RMA and
 * the two collectives pulls in most of libosoe's own translation units and,
 * through them, all of osoe_base - which is the point: F10's over a hundred
 * undefined references came from base/, not from shmem/src/. Nothing here is
 * ever run; the whole test is that this file compiles and links.
 */
#include <stdint.h>

#include "shmem.h"

int main(void)
{
    uintptr_t sum = 0;
    sum += (uintptr_t)&shmem_init;
    sum += (uintptr_t)&shmem_finalize;
    sum += (uintptr_t)&shmem_n_pes;
    sum += (uintptr_t)&shmem_malloc;
    sum += (uintptr_t)&shmem_putmem;
    sum += (uintptr_t)&shmem_getmem;
    sum += (uintptr_t)&shmem_quiet;
    sum += (uintptr_t)&shmem_barrier_all;
    return sum == 0; /* addresses are never all zero; keeps sum "live" */
}
