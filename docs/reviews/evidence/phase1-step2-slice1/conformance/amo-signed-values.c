/* Negative operands through the C11 generic AMOs at both widths, and every
 * exact-width and size type reaching a routine. The boundary carries a
 * uint64_t; a 32-bit compare_swap whose cond is -1 must still match. */
#include <shmem.h>
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), pe = (me + 1) % shmem_n_pes(), bad = 0;
    int *i32 = shmem_calloc(1, sizeof *i32);
    long long *i64 = shmem_calloc(1, sizeof *i64);
    float *f = shmem_calloc(1, sizeof *f);
    size_t *z = shmem_calloc(1, sizeof *z);
    ptrdiff_t *d = shmem_calloc(1, sizeof *d);
    int32_t *x32 = shmem_calloc(1, sizeof *x32);
    uint64_t *u64 = shmem_calloc(1, sizeof *u64);

    shmem_atomic_set(i32, -1, pe);
    bad |= shmem_atomic_compare_swap(i32, -1, -7, pe) != -1;
    bad |= shmem_atomic_fetch_add(i32, -3, pe) != -7;
    bad |= shmem_atomic_fetch(i32, pe) != -10;
    bad |= shmem_atomic_swap(i32, INT32_MIN, pe) != -10;
    bad |= shmem_atomic_fetch_inc(i32, pe) != INT32_MIN;

    shmem_atomic_set(i64, -1LL, pe);
    bad |= shmem_atomic_compare_swap(i64, -1LL, -5000000000LL, pe) != -1LL;
    bad |= shmem_atomic_fetch_add(i64, -1LL, pe) != -5000000000LL;

    shmem_atomic_set(f, -0.5f, pe);
    bad |= shmem_atomic_swap(f, 2.25f, pe) != -0.5f;
    bad |= shmem_atomic_fetch((const float *)f, pe) != 2.25f;

    shmem_atomic_add(z, (size_t)5, pe);
    shmem_atomic_add(d, (ptrdiff_t)-5, pe);
    bad |= shmem_atomic_fetch(z, pe) != 5 || shmem_atomic_fetch(d, pe) != -5;
    shmem_atomic_or(x32, INT32_MIN, pe);
    bad |= shmem_atomic_fetch_xor(x32, -1, pe) != INT32_MIN;
    shmem_atomic_or(SHMEM_CTX_DEFAULT, u64, UINT64_C(0xffffffff00000000), pe);
    bad |= shmem_atomic_fetch_and(SHMEM_CTX_DEFAULT, u64, UINT64_C(0xff), pe) != UINT64_C(0xffffffff00000000);

    shmem_barrier_all();
    /* and the waits compare as the type says: -10 < 0 for a signed type */
    int *w = shmem_calloc(1, sizeof *w);
    shmem_atomic_set(w, -10, me);
    bad |= shmem_test(w, SHMEM_CMP_LT, 0) != 1;
    unsigned *uw = shmem_calloc(1, sizeof *uw);
    shmem_atomic_set(uw, 4000000000u, me);
    bad |= shmem_test(uw, SHMEM_CMP_GT, 5u) != 1;
    if (bad)
        printf("PE %d: FAILED\n", me);
    else if (me == 0)
        printf("signed and exact-width AMOs agree\n");
    shmem_finalize();
    return bad;
}
