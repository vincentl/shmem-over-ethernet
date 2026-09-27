/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The deprecated routines of Annex F.
 *
 * They exist because the spec says they must — "support for start_pes is still
 * required in OpenSHMEM libraries" — and for no other reason. Each is the
 * modern routine under its old name, so there is one implementation and the two
 * spellings cannot drift.
 *
 * The deprecated atomic names (F.2.12: shmem_TYPENAME_fetch, _set, _swap,
 * _cswap, _finc, _inc, _fadd and _add) are the same rule at fourteen or forty
 * routines instead of one, so they are macro-generated from shmem.h's
 * OSOE_AMO_DEPRECATED_TYPES and OSOE_AMO_DEPRECATED_EXTENDED_TYPES lists,
 * the way amo.c generates the modern routines they call - a type appears once,
 * here and there both, and cannot end up with a definition in one and not the
 * other.
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
    pshmem_finalize();              /* stands aside on an abort's exit (abort.h) */
}

/*
 * The profiling names (spec §10 item 1; design §7.4). The spec asks for a
 * `pshmem_` entry point for every function and does not say how to shift a
 * name that never had a `shmem_` prefix, so both readings are provided: `p`
 * and the name - `pstart_pes`, `p_my_pe`, `pshmalloc` - which is what Sandia
 * OpenSHMEM defines and so what existing profiling tools call, and the literal
 * `pshmem_` and the name - `pshmem_start_pes`, `pshmem__my_pe`. The `p` form
 * is the definition; the `pshmem_` form is a second strong name for it, and
 * the old name a weak alias a tool may replace, as every `shmem_` routine is.
 * (Until 2026-09-26 neither existed, a known non-conformance; the author
 * chose to provide both.)
 */
#define OSOE_DEPRECATED_NAMES(name)                                           \
    extern __typeof__(p##name) name                                          \
        __attribute__((weak, alias("p" #name), visibility("default")));      \
    extern __typeof__(p##name) pshmem_##name                                 \
        __attribute__((alias("p" #name), visibility("default")));

OSOE_API void pstart_pes(int npes)
{
    (void)npes;                     /* meaningless since before this project */
    if (started)
        return;
    started = true;
    pshmem_init();
    /* Registered after init, so it runs before anything init registered. */
    atexit(finalize_at_exit);
}

OSOE_API int p_my_pe(void)
{
    return pshmem_my_pe();
}

OSOE_API int p_num_pes(void)
{
    return pshmem_n_pes();
}

OSOE_API void *pshmalloc(size_t size)
{
    return pshmem_malloc(size);
}

OSOE_API void pshfree(void *ptr)
{
    pshmem_free(ptr);
}

OSOE_API void *pshrealloc(void *ptr, size_t size)
{
    return pshmem_realloc(ptr, size);
}

OSOE_API void *pshmemalign(size_t alignment, size_t size)
{
    return pshmem_align(alignment, size);
}

OSOE_DEPRECATED_NAMES(start_pes)
OSOE_DEPRECATED_NAMES(_my_pe)
OSOE_DEPRECATED_NAMES(_num_pes)
OSOE_DEPRECATED_NAMES(shmalloc)
OSOE_DEPRECATED_NAMES(shfree)
OSOE_DEPRECATED_NAMES(shrealloc)
OSOE_DEPRECATED_NAMES(shmemalign)

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
 * The deprecated atomic memory operations (Annex F.2.12). fetch, set and swap
 * take the five deprecated-extended types (Table 6's plain integers plus
 * Table 7's float and double); cswap, finc, inc, fadd and add take the three
 * deprecated types (Table 6's plain integers only) - the same split shmem.h
 * declares from, so a type cannot be defined here without being declared
 * there or vice versa.
 */
#define OSOE_AMO_DEPRECATED_EXTENDED(T, NAME)                                \
    OSOE_API T pshmem_##NAME##_fetch(const T *source, int pe)                \
    {                                                                        \
        return pshmem_##NAME##_atomic_fetch(source, pe);                     \
    }                                                                        \
    OSOE_API void pshmem_##NAME##_set(T *dest, T value, int pe)              \
    {                                                                        \
        pshmem_##NAME##_atomic_set(dest, value, pe);                         \
    }                                                                        \
    OSOE_API T pshmem_##NAME##_swap(T *dest, T value, int pe)                \
    {                                                                        \
        return pshmem_##NAME##_atomic_swap(dest, value, pe);                 \
    }

#define OSOE_AMO_DEPRECATED_STANDARD(T, NAME)                                \
    OSOE_API T pshmem_##NAME##_cswap(T *dest, T cond, T value, int pe)       \
    {                                                                        \
        return pshmem_##NAME##_atomic_compare_swap(dest, cond, value, pe);   \
    }                                                                        \
    OSOE_API T pshmem_##NAME##_finc(T *dest, int pe)                         \
    {                                                                        \
        return pshmem_##NAME##_atomic_fetch_inc(dest, pe);                   \
    }                                                                        \
    OSOE_API void pshmem_##NAME##_inc(T *dest, int pe)                       \
    {                                                                        \
        pshmem_##NAME##_atomic_inc(dest, pe);                                \
    }                                                                        \
    OSOE_API T pshmem_##NAME##_fadd(T *dest, T value, int pe)                \
    {                                                                        \
        return pshmem_##NAME##_atomic_fetch_add(dest, value, pe);            \
    }                                                                        \
    OSOE_API void pshmem_##NAME##_add(T *dest, T value, int pe)              \
    {                                                                        \
        pshmem_##NAME##_atomic_add(dest, value, pe);                         \
    }

OSOE_AMO_DEPRECATED_EXTENDED_TYPES(OSOE_AMO_DEPRECATED_EXTENDED)
OSOE_AMO_DEPRECATED_TYPES(OSOE_AMO_DEPRECATED_STANDARD)

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

#define OSOE_AMO_ALIAS_DEPRECATED_EXTENDED(T, NAME)                          \
    OSOE_ALIAS(NAME##_fetch)                                                 \
    OSOE_ALIAS(NAME##_set)                                                   \
    OSOE_ALIAS(NAME##_swap)

#define OSOE_AMO_ALIAS_DEPRECATED_STANDARD(T, NAME)                          \
    OSOE_ALIAS(NAME##_cswap)                                                 \
    OSOE_ALIAS(NAME##_finc)                                                  \
    OSOE_ALIAS(NAME##_inc)                                                   \
    OSOE_ALIAS(NAME##_fadd)                                                  \
    OSOE_ALIAS(NAME##_add)

OSOE_AMO_DEPRECATED_EXTENDED_TYPES(OSOE_AMO_ALIAS_DEPRECATED_EXTENDED)
OSOE_AMO_DEPRECATED_TYPES(OSOE_AMO_ALIAS_DEPRECATED_STANDARD)
