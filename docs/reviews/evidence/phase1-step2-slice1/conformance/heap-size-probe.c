/* Reports whether a 2 MiB allocation fits, so the effective symmetric heap size
 * can be observed from outside: run with SHMEM_SYMMETRIC_SIZE=1m and with the
 * deprecated SMA_SYMMETRIC_SIZE=1m (spec section 8, Annex F.2.9). */
#include <shmem.h>
#include <stdio.h>

int main(void)
{
    shmem_init();
    void *p = shmem_malloc(2u << 20);
    if (shmem_my_pe() == 0)
        printf("2 MiB allocation %s\n", p ? "SUCCEEDED" : "FAILED");
    shmem_free(p);
    shmem_finalize();
    return 0;
}
