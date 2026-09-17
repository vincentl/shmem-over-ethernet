/*
 * shmem_malloc and shmem_free (spec §9.3.1, §9.3.2): size 0 and NULL need no
 * barrier; otherwise malloc barriers on exit and free on entry. Symmetry across
 * PEs is checked by shmem.rma, which can compare addresses between PEs.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"

enum { STAGGER_MS = 30 };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void stagger(int me)
{
    struct timespec ts = { 0, (long)me * STAGGER_MS * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    double need = (n - 1) * STAGGER_MS / 1000.0 * 0.95;

    /* No barrier for size 0 or NULL: PE 0 would otherwise wait for the
     * others' first real call below, which never matches. */
    CHECK(shmem_malloc(0) == NULL);
    shmem_free(NULL);

    /* malloc barriers on exit. */
    shmem_barrier_all();
    double t0 = now();
    stagger(me);
    char *a = shmem_malloc(1000);
    CHECK(now() - t0 >= need);
    CHECK(a != NULL && (uintptr_t)a % 16 == 0);
    memset(a, 0xab, 1000);

    /* free barriers on entry. */
    t0 = now();
    stagger(me);
    shmem_free(a);
    CHECK(now() - t0 >= need);

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

    shmem_free(d);
    shmem_free(c);
    shmem_free(b);

    /* Repeated sessions start from an empty heap. */
    shmem_finalize();
    shmem_init();
    char *e = shmem_malloc(1000);
    CHECK(e == a);
    shmem_free(e);
    shmem_finalize();
    return CHECK_DONE();
}
