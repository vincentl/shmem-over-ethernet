/*
 * shmem_putmem, shmem_getmem and shmem_ptr between every pair of local PEs,
 * including a PE and itself (spec §9.6.1, §9.1.9); and symmetry of
 * shmem_malloc's addresses across PEs (§4.4).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"

enum { ALLOCS = 64 };

static unsigned char pattern(int writer, int target, size_t i, size_t size)
{
    return (unsigned char)(writer * 31 + target * 17 + i * 7 + size);
}

/* Every PE writes a distinct pattern into its own band of `buf` on every PE,
 * then reads each band back from every PE. */
static void pairwise(size_t size)
{
    int me = shmem_my_pe(), n = shmem_n_pes();
    unsigned char *buf = shmem_malloc(size * (size_t)n);
    unsigned char *tmp = malloc(size);
    CHECK(buf != NULL && tmp != NULL);
    /* Clear the local copy, then synchronize: without the barrier, another PE
     * could already be putting into this copy while it is being cleared. */
    memset(buf, 0, size * (size_t)n);
    shmem_barrier_all();

    for (int t = 0; t < n; t++) {
        for (size_t i = 0; i < size; i++)
            tmp[i] = pattern(me, t, i, size);
        shmem_putmem(buf + (size_t)me * size, tmp, size, t);
    }
    shmem_barrier_all();

    /* Each band of the local copy holds what its writer put here. */
    int bad = 0;
    for (int w = 0; w < n; w++)
        for (size_t i = 0; i < size; i++)
            bad += buf[(size_t)w * size + i] != pattern(w, me, i, size);
    CHECK(bad == 0);

    /* This PE's band, fetched from every PE, holds what this PE put there. */
    for (int t = 0; t < n; t++) {
        memset(tmp, 0, size);
        shmem_getmem(tmp, buf + (size_t)me * size, size, t);
        bad = 0;
        for (size_t i = 0; i < size; i++)
            bad += tmp[i] != pattern(me, t, i, size);
        CHECK(bad == 0);
    }

    /* shmem_ptr reaches every local PE's copy directly. */
    for (int t = 0; t < n; t++) {
        unsigned char *p = shmem_ptr(buf, t);
        CHECK(p != NULL);
        if (p)
            CHECK(p[(size_t)me * size + size - 1] == pattern(me, t, size - 1, size));
    }
    CHECK(shmem_ptr(buf, me) == buf);

    free(tmp);
    shmem_free(buf);
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();

    /* Symmetry: the same allocation sequence, including out-of-order frees,
     * yields the same addresses on every PE. Every PE puts its list into row
     * `me` of a table on every PE; each then compares all rows with its own. */
    uintptr_t addr[ALLOCS];
    for (int i = 0; i < ALLOCS; i++)
        addr[i] = (uintptr_t)shmem_malloc((size_t)(i * 37 % 500 + 1));
    for (int i = 0; i < ALLOCS; i += 3)
        shmem_free((void *)addr[i]);
    for (int i = 0; i < ALLOCS; i += 3)
        addr[i] = (uintptr_t)shmem_malloc((size_t)(i * 11 % 300 + 1));
    uintptr_t *table = shmem_malloc(sizeof addr * (size_t)n);
    CHECK(table != NULL);
    for (int t = 0; t < n; t++)
        shmem_putmem(table + me * ALLOCS, addr, sizeof addr, t);
    shmem_barrier_all();
    for (int w = 0; w < n; w++)
        CHECK(memcmp(table + w * ALLOCS, addr, sizeof addr) == 0);
    shmem_free(table);
    for (int i = ALLOCS - 1; i >= 0; i--)
        shmem_free((void *)addr[i]);

    pairwise(1);
    pairwise(7);
    pairwise(4096);
    pairwise((size_t)1 << 20);

    /* Stores through shmem_ptr land in the target PE's heap: each PE sets its
     * own element on every PE, and the barrier's serializing store makes the
     * stores visible before anyone reads (§4.6). */
    int *flags = shmem_malloc(sizeof(int) * (size_t)n);
    CHECK(flags != NULL);
    for (int t = 0; t < n; t++) {
        int *p = shmem_ptr(flags, t);
        CHECK(p != NULL);
        if (p)
            p[me] = me + 1000;
    }
    shmem_barrier_all();
    for (int w = 0; w < n; w++)
        CHECK(flags[w] == w + 1000);

    /* NULL for what is not reachable. */
    int local = 0;
    CHECK(shmem_ptr(&local, me) == NULL);
    CHECK(shmem_ptr(flags, n) == NULL);
    CHECK(shmem_ptr(flags, -1) == NULL);

    /* A zero-length transfer touches nothing, even at a private address. */
    shmem_putmem(&local, &local, 0, me);
    shmem_getmem(&local, &local, 0, me);

    /* A put to the caller's own PE may overlap. */
    unsigned char *o = shmem_malloc(16);
    for (int i = 0; i < 16; i++)
        o[i] = (unsigned char)i;
    shmem_putmem(o + 1, o, 8, me);
    CHECK(o[1] == 0 && o[8] == 7 && o[9] == 9);
    shmem_free(o);

    shmem_free(flags);
    shmem_finalize();
    return CHECK_DONE();
}
