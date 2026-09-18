/*
 * The deprecated routines of Annex F.
 *
 * They exist because the spec says they must — "support for start_pes is still
 * required in OpenSHMEM libraries" — and for no other reason. Each is the
 * modern routine under its old name, so there is one implementation and the two
 * spellings cannot drift.
 *
 * `start_pes` is the exception with behaviour of its own. Its `npes` argument
 * has meant nothing for decades and is ignored. More importantly it has no
 * matching finalize: programs written against it never call one, because
 * finalization used to be implicit (Annex F.2.3). So it registers one at exit.
 * That is not tidiness — without it every such program leaves its control
 * segment and its heaps in /dev/shm, which is the leak `osoe-launch` fails on
 * and the state `osoe-stat --gc` exists to clean up after a crash. A program
 * that exits normally should not look like one that crashed.
 *
 * Calling `start_pes` more than once is a no-op after the first, which is what
 * the routine has always done: it predates the reference counting that makes
 * repeated `shmem_init` meaningful, and a second implicit finalize registered
 * at exit would tear down a library the first call still owns.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"

static bool started;

static void finalize_at_exit(void)
{
    pshmem_finalize();
}

OSOE_API void start_pes(int npes)
{
    (void)npes;                     /* meaningless since before this project */
    if (started)
        return;
    started = true;
    pshmem_init();
    /* Registered after init, so it runs before anything init registered. */
    atexit(finalize_at_exit);
}

OSOE_API int _my_pe(void)
{
    return pshmem_my_pe();
}

OSOE_API int _num_pes(void)
{
    return pshmem_n_pes();
}

OSOE_API void *shmalloc(size_t size)
{
    return pshmem_malloc(size);
}

OSOE_API void shfree(void *ptr)
{
    pshmem_free(ptr);
}

OSOE_API void *shrealloc(void *ptr, size_t size)
{
    return pshmem_realloc(ptr, size);
}

OSOE_API void *shmemalign(size_t alignment, size_t size)
{
    return pshmem_align(alignment, size);
}

/*
 * SHMEM_CACHE (Annex F.2.7). "Originally implemented for systems with
 * cache-management instructions. This API has largely gone unused on
 * cache-coherent system architectures." Every machine osoe targets is
 * cache-coherent, and §4.1 rests on it: a PE's heap is one mapping that every
 * local PE writes with ordinary instructions. So these are no-ops, and a
 * no-op is the correct implementation rather than an unimplemented one.
 */
OSOE_API void pshmem_clear_cache_inv(void) { }
OSOE_API void pshmem_set_cache_inv(void) { }
OSOE_API void pshmem_set_cache_line_inv(void *dest) { (void)dest; }
OSOE_API void pshmem_clear_cache_line_inv(void *dest) { (void)dest; }
OSOE_API void pshmem_udcflush(void) { }
OSOE_API void pshmem_udcflush_line(void *dest) { (void)dest; }

/*
 * The weak `shmem_` aliases (export.h), for the prefixed names only. The
 * unprefixed ones — start_pes, _my_pe, shmalloc and the rest — have no
 * `shmem_` to weaken and no `pshmem_` spelling to weaken it towards: the
 * profiling interface of spec §10 is defined over the `shmem_` namespace, and
 * inventing `pstart_pes` would be inventing an interface rather than
 * implementing one.
 */
OSOE_ALIAS(clear_cache_inv)
OSOE_ALIAS(set_cache_inv)
OSOE_ALIAS(set_cache_line_inv)
OSOE_ALIAS(clear_cache_line_inv)
OSOE_ALIAS(udcflush)
OSOE_ALIAS(udcflush_line)
