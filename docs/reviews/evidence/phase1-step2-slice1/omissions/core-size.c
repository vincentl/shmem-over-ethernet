/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* How big is one PE's core file? Each PE maps every sibling's heap, and heap
 * names are unlinked after mapping. 2 PEs, each touches 64 MiB of its heap. */
#include <stdlib.h>
#include <string.h>
#include <shmem.h>
int main(void)
{
    shmem_init();
    char *p = shmem_malloc((size_t)64 << 20);
    memset(p, shmem_my_pe() + 1, (size_t)64 << 20);
    shmem_barrier_all();
    if (shmem_my_pe() == 0) abort();
    shmem_barrier_all();
    shmem_finalize();
    return 0;
}
