/*
 * The wrapper's statics program (design §4.3): ISx's pattern, built with
 * nothing but oshcc - a global counter every PE adds to, and a static total
 * reduced with a static pSync and pWrk.
 */
#include <stdio.h>

#include <shmem.h>

long long counter;
static long long total, mine;
static long pSync[SHMEM_REDUCE_SYNC_SIZE];
static long long pWrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];

int main(void)
{
    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    shmem_longlong_atomic_add(&counter, me + 1, 0);
    mine = me + 1;
    shmem_barrier_all();
    shmem_longlong_sum_to_all(&total, &mine, 1, 0, 0, npes, pWrk, pSync);
    long long want = (long long)npes * (npes + 1) / 2;
    int ok = total == want && shmem_longlong_g(&counter, 0) == want;
    printf("PE %d of %d: %s\n", me, npes, ok ? "ok" : "WRONG");
    shmem_finalize();
    return ok ? 0 : 1;
}
