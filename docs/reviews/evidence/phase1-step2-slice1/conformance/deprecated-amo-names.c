/* Annex F: shmem_TYPENAME_{fetch,set,cswap,swap,finc,inc,fadd,add} and the C11
 * shmem_{fetch,...} are deprecated since 1.4 with "Last Version Supported:
 * Current" - F.1: "The deprecated API must be supported". */
#include <shmem.h>

int main(void)
{
    shmem_init();
    long *counter = shmem_calloc(1, sizeof(long));
    shmem_long_inc(counter, 0);
    long old = shmem_long_finc(counter, 0);
    old += shmem_long_cswap(counter, 2, 5, 0);
    old += shmem_long_swap(counter, 7, 0);
    shmem_long_add(counter, 1, 0);
    old += shmem_long_fadd(counter, 1, 0);
    shmem_long_set(counter, 0, 0);
    old += shmem_long_fetch(counter, 0);
    shmem_free(counter);
    shmem_finalize();
    return (int)old;
}
