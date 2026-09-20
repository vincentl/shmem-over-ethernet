/*
 * shmem_calloc, shmem_realloc, shmem_align and shmem_malloc_with_hints
 * (spec §9.3.3-§9.3.6; design §4.4's edge semantics).
 *
 * The part of §9.3 that is easy to get wrong is not what these return but
 * **which barriers they take**, and a timing test cannot tell an entry barrier
 * from an exit one: both make every PE leave after the last PE arrives. So the
 * barriers are *counted*, out of the control segment (§3.14). The rules, in the
 * form a test can check:
 *
 *   malloc, calloc, align, malloc_with_hints   1, on exit
 *   free                                       1, on entry
 *   realloc, resizing                          2, entry and exit
 *   realloc(NULL, n)                           1 - it is shmem_malloc
 *   realloc(p, 0)                              1 - it is shmem_free
 *   anything that does nothing                 0
 *
 * That last line is the one with teeth: a zero size, a zero count, or a null
 * pointer must return *without* a barrier, because every PE makes the same call
 * and a barrier none of them can complete would hang the job. A single spurious
 * barrier there is a deadlock at L > 1, and a missing one anywhere else is a
 * PE reading memory another PE has already freed.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

/* This PE's own value for a PE-scope metric. */
static uint64_t pe_value(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    uint64_t v = UINT64_MAX;
    if (!osoe_segment_read_name(&s, name, (uint32_t)shmem_my_pe(), &v))
        v = UINT64_MAX;
    osoe_segment_close(&s);
    return v;
}

/* This PE's barrier count, read from its own counter blocks. */
static uint64_t barriers(void)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, "shmem.barrier.calls");
    uint64_t total = 0;
    uint32_t me = (uint32_t)shmem_my_pe();
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, me * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

#define BARRIERS(expect, expr) do {                                           \
        uint64_t before_ = barriers();                                        \
        expr;                                                                 \
        uint64_t after_ = barriers();                                         \
        if (after_ - before_ != (uint64_t)(expect))                           \
            fprintf(stderr, "%s:%d: %s took %llu barriers, expected %d\n",    \
                    __FILE__, __LINE__, #expr,                                \
                    (unsigned long long)(after_ - before_), (expect));        \
        CHECK(after_ - before_ == (uint64_t)(expect));                        \
    } while (0)

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    void *p = NULL;

    /* ---- nothing happens, and no barrier happens either ---------------- */
    BARRIERS(0, CHECK(shmem_calloc(0, 8) == NULL));
    BARRIERS(0, CHECK(shmem_calloc(8, 0) == NULL));
    BARRIERS(0, CHECK(shmem_calloc(0, 0) == NULL));
    BARRIERS(0, CHECK(shmem_align(64, 0) == NULL));
    BARRIERS(0, CHECK(shmem_malloc_with_hints(0, SHMEM_MALLOC_ATOMICS_REMOTE) == NULL));
    BARRIERS(0, CHECK(shmem_realloc(NULL, 0) == NULL));
    BARRIERS(0, shmem_free(NULL));

    /* ---- calloc (spec §9.3.6) ------------------------------------------ */
    unsigned char *z = NULL;
    BARRIERS(1, z = shmem_calloc(300, 7));
    CHECK(z != NULL);
    int nonzero = 0;
    for (size_t i = 0; i < 300 * 7; i++)
        if (z[i])
            nonzero++;
    CHECK(nonzero == 0);

    /* count * size overflowing is a failed allocation, not a small one - and a
     * failure still barriers, since every PE fails identically. */
    BARRIERS(1, CHECK(shmem_calloc(SIZE_MAX / 2, 4) == NULL));
    BARRIERS(1, shmem_free(z));

    /* ---- align (spec §9.3.4) ------------------------------------------- */
    for (size_t a = sizeof(void *); a <= 65536; a *= 2) {
        void *q = shmem_align(a, 1000);
        CHECK(q != NULL);
        CHECK((uintptr_t)q % a == 0);
        shmem_free(q);
    }
    BARRIERS(1, p = shmem_align(4096, 64));
    CHECK(p && (uintptr_t)p % 4096 == 0);
    BARRIERS(1, shmem_free(p));

    /* ---- hints (spec §9.3.5) ------------------------------------------- */
    /*
     * §4.4 places hinted allocations at the far end of the heap and unhinted
     * ones at the near end. Nothing in the spec requires that; what the test
     * pins down is that the hint reached the allocator at all, because a
     * discarded hint is indistinguishable from an honoured one in every other
     * observable way - which is exactly how this check was written wrong the
     * first time. "The hinted block is above the unhinted one" is true whether
     * or not the hint was kept, since the second of two ordinary allocations is
     * above the first. The distance is what tells them apart: half a heap says
     * the two came from opposite ends.
     */
    uint64_t heap_bytes = pe_value("pe.symmetric_size");
    CHECK(heap_bytes != UINT64_MAX && heap_bytes > 0);

    char *plain = shmem_malloc(4096);
    char *hinted = shmem_malloc_with_hints(4096, SHMEM_MALLOC_ATOMICS_REMOTE);
    CHECK(plain && hinted);
    CHECK(hinted > plain);
    CHECK((uint64_t)(hinted - plain) > heap_bytes / 2);

    /* Symmetric, like any other allocation: every PE has the same offset. */
    char *base = shmem_malloc(8);
    CHECK(base != NULL);
    {
        uint64_t mine = (uint64_t)(hinted - base), theirs = 0;
        uint64_t *slot = shmem_malloc(sizeof *slot);
        CHECK(slot != NULL);
        *slot = mine;
        shmem_barrier_all();
        int left = (me + n - 1) % n;
        shmem_getmem(&theirs, slot, sizeof theirs, left);
        CHECK(theirs == mine);
        shmem_free(slot);
    }
    shmem_free(base);

    /* ---- realloc (spec §9.3.3) ----------------------------------------- */

    /* A null pointer makes it shmem_malloc: one barrier, on exit. */
    char *grow = NULL;
    BARRIERS(1, grow = shmem_realloc(NULL, 64));
    CHECK(grow != NULL);
    memset(grow, 0x5a, 64);

    /* Resizing takes both barriers, and keeps the contents up to the lesser of
     * the two sizes. */
    char *grown = NULL;
    BARRIERS(2, grown = shmem_realloc(grow, 4096));
    CHECK(grown != NULL);
    int kept = 1;
    for (int i = 0; i < 64; i++)
        if ((unsigned char)grown[i] != 0x5a)
            kept = 0;
    CHECK(kept);

    /* Shrinking keeps the leading bytes too, and stays put. */
    char *shrunk = NULL;
    BARRIERS(2, shrunk = shmem_realloc(grown, 32));
    CHECK(shrunk == grown);
    kept = 1;
    for (int i = 0; i < 32; i++)
        if ((unsigned char)shrunk[i] != 0x5a)
            kept = 0;
    CHECK(kept);

    /* A zero size makes it shmem_free: one barrier, on entry. */
    BARRIERS(1, CHECK(shmem_realloc(shrunk, 0) == NULL));

    /* A realloc that cannot be satisfied returns NULL and leaves the block
     * alone - the caller's pointer is still good and still holds its data. */
    char *keep = shmem_malloc(128);
    CHECK(keep != NULL);
    memset(keep, 0x3c, 128);
    BARRIERS(2, CHECK(shmem_realloc(keep, (size_t)1 << 40) == NULL));
    kept = 1;
    for (int i = 0; i < 128; i++)
        if ((unsigned char)keep[i] != 0x3c)
            kept = 0;
    CHECK(kept);
    CHECK(keep[0] == 0x3c);

    /*
     * Hints survive a realloc that moves the block (spec §9.3.3). `hinted` is
     * at the far end; growing it must keep it there, which is observable
     * because an unhinted block of the same size would be placed at the near
     * end instead.
     */
    char *hinted2 = NULL;
    BARRIERS(2, hinted2 = shmem_realloc(hinted, 16384));
    CHECK(hinted2 != NULL);
    CHECK((uint64_t)(hinted2 - plain) > heap_bytes / 2);
    shmem_free(hinted2);
    shmem_free(plain);
    shmem_free(keep);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
