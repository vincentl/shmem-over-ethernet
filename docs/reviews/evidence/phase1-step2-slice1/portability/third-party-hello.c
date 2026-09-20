/* A third party's first program, compiled against the *installed* tree only. */
#include <stdio.h>
#include <shmem.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    long *x = shmem_malloc(sizeof *x);
    *x = me;
    shmem_barrier_all();
    long got = -1;
    shmem_getmem(&got, x, sizeof got, (me + 1) % n);
    printf("PE %d of %d read %ld from PE %d\n", me, n, got, (me + 1) % n);
    shmem_free(x);
    shmem_finalize();
    return got != (me + 1) % n;
}
