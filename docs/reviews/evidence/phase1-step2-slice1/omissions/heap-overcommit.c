/* A symmetric heap larger than /dev/shm can back. shmem_init and shmem_malloc
 * both succeed (ftruncate is sparse); who pays, and when? Map 0,1,1: PE 0 puts
 * into PE 2's heap, which node 1's lead (PE 1) applies on PE 2's behalf. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <shmem.h>
int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    size_t bytes = (size_t)40 << 20;
    char *big = shmem_malloc(bytes);
    fprintf(stderr, "PE %d: shmem_malloc(40 MiB) = %p\n", me, (void *)big);
    shmem_barrier_all();
    if (me == 0) {
        char *src = malloc(bytes); memset(src, 1, bytes);
        for (int pe = n - 1; pe >= 1; pe--) {
            fprintf(stderr, "PE 0: putting 40 MiB to PE %d\n", pe);
            shmem_putmem(big, src, bytes, pe);
        }
    }
    shmem_barrier_all();
    fprintf(stderr, "PE %d: done\n", me);
    shmem_finalize();
    return 0;
}
