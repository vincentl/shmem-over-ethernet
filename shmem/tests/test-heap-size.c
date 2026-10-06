/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A heap at least as large as SHMEM_SYMMETRIC_SIZE says (§8): "The resulting
 * size is implementation-defined and must be at least as large as the integer
 * ceiling of the product of the numeric prefix and the scaling factor", whose
 * own example is "3.1M", 3,250,586 bytes. Every PE allocates that many bytes,
 * OSOE_TEST_HEAP_BYTES, in one object and writes its last byte; before the
 * v0.2 step 1 review (C1) the heap held a multiple of 16 rounded down, 10
 * bytes fewer, and the allocation failed.
 */
#include <shmem.h>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"

int main(void)
{
    shmem_init();
    const char *bytes = getenv("OSOE_TEST_HEAP_BYTES");
    CHECK_FATAL(bytes != NULL);
    size_t n = (size_t)strtoull(bytes, NULL, 10);
    char *p = shmem_malloc(n);
    CHECK(p != NULL);
    if (p) {
        p[n - 1] = 1;
        shmem_barrier_all();
        shmem_free(p);
    }
    if (shmem_my_pe() == 0)
        printf("a heap of SHMEM_SYMMETRIC_SIZE=%s holds %zu bytes: %s\n",
               getenv("SHMEM_SYMMETRIC_SIZE"), n, p ? "yes" : "no");
    shmem_finalize();
    return CHECK_DONE();
}
