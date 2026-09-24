/*
 * shmem_malloc and shmem_free (spec §9.3.1, §9.3.2): size 0 and NULL need no
 * barrier; otherwise malloc barriers on exit and free on entry. Symmetry across
 * PEs is checked by shmem.rma, which compares each object's offset from another
 * between PEs. And shmem_align refuses an alignment on every PE or on none.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>

#include <shmem.h>

#include "arrivals.h"
#include "check.h"
#include "invariants.h"

int main(void)
{
    arrivals_open();
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();

    /* No barrier for size 0 or NULL: PE 0 would otherwise wait for the
     * others' first real call below, which never matches. */
    CHECK(shmem_malloc(0) == NULL);
    shmem_free(NULL);

    /* malloc barriers on exit. */
    arrivals_arrive(me);
    char *a = shmem_malloc(1000);
    CHECK(arrivals_left_after_all(n));
    CHECK_FATAL(a != NULL && (uintptr_t)a % 16 == 0);  /* NULL would be dereferenced just below */
    memset(a, 0xab, 1000);

    /* free barriers on entry. */
    arrivals_arrive(me);
    shmem_free(a);
    CHECK(arrivals_left_after_all(n));

    /* Memory is reused after free; distinct live blocks do not overlap. */
    char *b = shmem_malloc(1000);
    CHECK(b == a);
    char *c = shmem_malloc(1);
    CHECK(c != NULL && (c >= b + 1000 || c + 1 <= b));

    /* More than the heap fails on every PE, collectively, and does not
     * disturb later allocations. */
    CHECK(shmem_malloc((size_t)1 << 40) == NULL);
    char *d = shmem_malloc(64);
    CHECK(d != NULL);

    /* shmem_align decides a refusal every PE agrees on (malloc.c): an
     * alignment a PE's heap base may not meet - 4 MiB, on the odd ranks of
     * §8.2's heap-base shift at 2 MiB - fails on every PE or on none, and one
     * that succeeds is aligned. A disagreement is not freed: a collective free
     * on some PEs and a NULL one on the rest would hang. */
    size_t alignment = (size_t)4 << 20;
    char *aligned = shmem_align(alignment, 64);
    CHECK(aligned == NULL || (uintptr_t)aligned % alignment == 0);
    int *verdicts = shmem_calloc((size_t)n, sizeof(int));
    CHECK_FATAL(verdicts != NULL);
    int mine = aligned != NULL;
    for (int t = 0; t < n; t++)
        shmem_putmem(verdicts + me, &mine, sizeof mine, t);
    shmem_barrier_all();
    int agree = 1;
    for (int w = 0; w < n; w++)
        agree &= verdicts[w] == mine;
    CHECK(agree);
    shmem_free(verdicts);
    if (agree)
        shmem_free(aligned);

    shmem_free(d);
    shmem_free(c);
    shmem_free(b);

    /* Repeated sessions start from an empty heap. */
    shmem_finalize();
    shmem_init();
    char *e = shmem_malloc(1000);
    CHECK(e == a);
    shmem_free(e);
    CHECK_INVARIANTS_SESSION(1);        /* the second session (§6.4) */
    shmem_finalize();
    arrivals_close();
    return CHECK_DONE();
}
