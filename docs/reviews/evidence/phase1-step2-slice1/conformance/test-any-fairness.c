/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Spec 9.11.10: "If an entry i in ivars within the test set satisfies the test
 * condition, a series of calls to shmem_test_any must eventually return i."
 * (the same sentence is in 9.11.3 for shmem_wait_until_any). Elements 0 and 3
 * both satisfy the condition and nothing is masked. */
#include <shmem.h>
#include <stdio.h>

int main(void)
{
    shmem_init();
    int *flags = shmem_calloc(4, sizeof(int));
    shmem_atomic_set(&flags[0], 1, shmem_my_pe());
    shmem_atomic_set(&flags[3], 1, shmem_my_pe());
    unsigned long seen[4] = { 0 }, waited[4] = { 0 };
    for (int i = 0; i < 100000; i++) {
        seen[shmem_test_any(flags, 4, NULL, SHMEM_CMP_EQ, 1)]++;
        waited[shmem_wait_until_any(flags, 4, NULL, SHMEM_CMP_EQ, 1)]++;
    }
    if (shmem_my_pe() == 0) {
        printf("test_any returned index 0 %lu times, index 3 %lu times\n", seen[0], seen[3]);
        printf("wait_until_any returned index 0 %lu times, index 3 %lu times\n",
               waited[0], waited[3]);
    }
    shmem_free(flags);
    shmem_finalize();
    return 0;
}
