/*
 * The §7.4 mandatory surface: the parts of the specification that are required
 * for conformance and force no design decision (spec §9.1.7, §9.1.8, §9.9,
 * §9.10's deprecated constants, §10.1.1, Annex F).
 *
 * What a test can establish about a surface like this is that it is *there*:
 * that the headers exist and are includable, that the constants are usable the
 * way a program uses them, and that every entry point is callable and does the
 * documented nothing without disturbing anything. That is most of the value —
 * the failure these guard against is a program that will not compile or will
 * not link, not one that computes the wrong answer.
 *
 * Two things here are more than that, and are checked as such: the accessibility
 * queries answer differently for a symmetric and a non-symmetric address, which
 * is §7.4's recorded non-conformance about statics rather than a stub; and the
 * deprecated allocators really are the modern ones, checked by freeing with the
 * other spelling.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>

/* All five spellings of the interface must exist and agree (§7.4). */
#include <mpp/shmem.h>
#include <mpp/shmemx.h>
#include <pshmem.h>
#include <shmem.h>
#include <shmemx.h>

#include "check.h"
#include "invariants.h"

/* A conforming program declares this and initializes it, whether or not it
 * ever reaches a routine that takes it (Annex F.2.8). */
static long pSync[SHMEM_BARRIER_SYNC_SIZE];

int main(void)
{
    for (int i = 0; i < SHMEM_BARRIER_SYNC_SIZE; i++)
        pSync[i] = SHMEM_SYNC_VALUE;

    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();

    /* The constants are usable and self-consistent. */
    CHECK(SHMEM_BARRIER_SYNC_SIZE > 0);
    CHECK(SHMEM_SYNC_SIZE >= SHMEM_BARRIER_SYNC_SIZE);
    CHECK(SHMEM_BCAST_SYNC_SIZE > 0 && SHMEM_COLLECT_SYNC_SIZE > 0);
    CHECK(SHMEM_REDUCE_SYNC_SIZE > 0 && SHMEM_REDUCE_MIN_WRKDATA_SIZE > 0);
    CHECK(SHMEM_ALLTOALL_SYNC_SIZE > 0 && SHMEM_ALLTOALLS_SYNC_SIZE > 0);
    CHECK(_SHMEM_SYNC_VALUE == SHMEM_SYNC_VALUE);
    CHECK(_SHMEM_BARRIER_SYNC_SIZE == SHMEM_BARRIER_SYNC_SIZE);
    CHECK(pSync[0] == SHMEM_SYNC_VALUE && pSync[SHMEM_BARRIER_SYNC_SIZE - 1] == SHMEM_SYNC_VALUE);

    /* §9.1.7: every PE of this job runs the same executable. */
    for (int i = 0; i < n; i++)
        CHECK(shmem_pe_accessible(i) == 1);
    CHECK(shmem_pe_accessible(n) == 0);
    CHECK(shmem_pe_accessible(-1) == 0);

    /*
     * §9.1.8, and §7.4's recorded non-conformance: a heap object is accessible
     * and a static or automatic one is not, because symmetric statics wait for
     * §4.3's phase 1.5. Answering 1 here and faulting at the first put would be
     * the worse failure.
     */
    uint64_t *heap = shmem_malloc(sizeof *heap);
    CHECK_FATAL(heap != NULL);  /* NULL would be dereferenced (*heap = 0) below */
    int automatic = 0;
    for (int i = 0; i < n; i++)
        CHECK(shmem_addr_accessible(heap, i) == 1);
    CHECK(shmem_addr_accessible(heap, n) == 0);
    CHECK(shmem_addr_accessible(&automatic, 0) == 0);
    CHECK(shmem_addr_accessible(pSync, 0) == 0);
    CHECK(shmem_addr_accessible(NULL, 0) == 0);

    /* §9.9: sessions are hints and do nothing, including for the invalid
     * context, and communication across them is unaffected. */
    shmem_ctx_session_config_t session = { .total_ops = 1000 };
    shmem_ctx_session_start(SHMEM_CTX_DEFAULT, SHMEM_CTX_SESSION_BATCH, &session,
                            SHMEM_CTX_SESSION_TOTAL_OPS);
    *heap = 0;
    shmem_barrier_all();
    shmem_uint64_atomic_add(heap, 7, (me + 1) % n);
    shmem_ctx_session_stop(SHMEM_CTX_DEFAULT);
    shmem_barrier_all();
    CHECK(*heap == 7);
    shmem_ctx_session_start(SHMEM_CTX_DEFAULT, 0, NULL, 0);
    shmem_ctx_session_stop(SHMEM_CTX_DEFAULT);
    shmem_ctx_session_start(SHMEM_CTX_INVALID, 0, NULL, 0);
    shmem_ctx_session_stop(SHMEM_CTX_INVALID);
    /* §9.9.3: stopping a stopped session has no effect. */
    shmem_ctx_session_stop(SHMEM_CTX_DEFAULT);

    /* §10.1.1: the library makes no use of shmem_pcontrol. */
    shmem_pcontrol(0);
    shmem_pcontrol(1);

    /* Annex F.2.7: SHMEM_CACHE on a cache-coherent machine. */
    shmem_set_cache_inv();
    shmem_set_cache_line_inv(heap);
    shmem_udcflush();
    shmem_udcflush_line(heap);
    shmem_clear_cache_line_inv(heap);
    shmem_clear_cache_inv();
    CHECK(*heap == 7);

    /*
     * Annex F.2.4: the deprecated allocators are the modern ones, which is
     * checked by mixing the spellings - memory taken with shmalloc is freed
     * with shmem_free and the other way round. Two separate allocators would
     * fail here rather than somewhere later and stranger.
     */
    CHECK(_my_pe() == me);
    CHECK(_num_pes() == n);

    char *old_style = shmalloc(64);
    CHECK_FATAL(old_style != NULL);  /* NULL would be dereferenced just below */
    memset(old_style, 0x7e, 64);
    char *grown = shrealloc(old_style, 256);
    CHECK_FATAL(grown != NULL);      /* NULL would be indexed by the very next check */
    CHECK((unsigned char)grown[0] == 0x7e && (unsigned char)grown[63] == 0x7e);
    shmem_free(grown);                  /* freed with the modern spelling */

    char *modern = shmem_malloc(64);
    CHECK(modern != NULL);
    shfree(modern);                     /* freed with the old one */

    char *aligned = shmemalign(4096, 100);
    CHECK(aligned != NULL);
    CHECK((uintptr_t)aligned % 4096 == 0);
    shfree(aligned);

    shmem_free(heap);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
