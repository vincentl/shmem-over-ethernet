/* shmem_putmem, shmem_getmem (spec §9.6.1) and shmem_ptr (§9.1.9), over the
 * transport boundary (§4.10). */
#include <stddef.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"
#include "transport.h"

/*
 * The context form is the definition and the plain form calls it with
 * SHMEM_CTX_DEFAULT, rather than the other way round (§7.3). That way the
 * parameter is carried on the real path from the start; a plain form that did
 * the work and a context form that ignored its argument would be the shape that
 * breaks when a second context exists.
 */
static void ctx_putmem(const char *routine, shmem_ctx_t ctx, void *dest,
                       const void *source, size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    osoe_check_pe(routine, pe);
    uint64_t offset = osoe_symmetric_offset(routine, dest, nelems);
    osoe_transport_put(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, source, nelems, 0);
}

static void ctx_getmem(const char *routine, shmem_ctx_t ctx, void *dest,
                       const void *source, size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    osoe_check_pe(routine, pe);
    uint64_t offset = osoe_symmetric_offset(routine, source, nelems);
    osoe_transport_get(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, dest, nelems, 0);
}

OSOE_API void pshmem_putmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_putmem("shmem_putmem", SHMEM_CTX_DEFAULT, dest, source, nelems, pe);
}

OSOE_API void pshmem_getmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_getmem("shmem_getmem", SHMEM_CTX_DEFAULT, dest, source, nelems, pe);
}

OSOE_API void pshmem_ctx_putmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    ctx_putmem("shmem_ctx_putmem", ctx, dest, source, nelems, pe);
}

OSOE_API void pshmem_ctx_getmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    ctx_getmem("shmem_ctx_getmem", ctx, dest, source, nelems, pe);
}

/* NULL for anything not reachable by loads and stores in this process: a
 * non-symmetric address, a PE out of range, or a PE the network carries
 * (§4.8). Returning NULL is always conforming. */
OSOE_API void *pshmem_ptr(const void *dest, int pe)
{
    /* Bypasses osoe_check_pe/osoe_symmetric_offset - both fatal in ways this
     * routine's own contract forbids (an out-of-range pe or a non-symmetric
     * address is a defined NULL here, not an error) - so it needs its own
     * check (conformance review F5): otherwise a call after the last
     * shmem_finalize dereferences whatever osoe_transport_direct finds in a
     * heap table that used to be real. */
    osoe_require_initialized("shmem_ptr");
    uint64_t offset;
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size ||
        !osoe_symmetric_heap_offset(dest, 1, &offset))
        return NULL;
    return osoe_transport_direct(pe, OSOE_REGION_HEAP, offset);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(putmem)
OSOE_ALIAS(getmem)
OSOE_ALIAS(ctx_putmem)
OSOE_ALIAS(ctx_getmem)
OSOE_ALIAS(ptr)
