/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* shmem_putmem, shmem_getmem (spec §9.6.1) and shmem_ptr (§9.1.9), over the
 * transport boundary (§4.10). */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "context.h"
#include "export.h"
#include "fatal.h"
#include "rma.h"
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
size_t osoe_rma_bytes(const char *routine, size_t nelems, size_t width)
{
    if (width > 1 && nelems > SIZE_MAX / width)
        osoe_fatal("%s: %zu elements of %zu bytes are more bytes than a size_t can count",
                   routine, nelems, width);
    return nelems * width;
}

void osoe_rma_put(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                  size_t nelems, size_t width, int pe, unsigned flags)
{
    if (nelems == 0)
        return;
    size_t len = osoe_rma_bytes(routine, nelems, width);
    /* The context's team numbers the PE (context.h); the transport takes a
     * world rank (§4.10). */
    struct osoe_transport_context *context = osoe_ctx_resolve(routine, ctx, &pe);
    struct osoe_symmetric_location at = osoe_symmetric_locate(routine, dest, len);
    osoe_transport_put(context, pe, at.region, at.offset, source, len, flags);
}

void osoe_rma_get(const char *routine, shmem_ctx_t ctx, void *dest, const void *source,
                  size_t nelems, size_t width, int pe, unsigned flags)
{
    if (nelems == 0)
        return;
    size_t len = osoe_rma_bytes(routine, nelems, width);
    struct osoe_transport_context *context = osoe_ctx_resolve(routine, ctx, &pe);
    struct osoe_symmetric_location at = osoe_symmetric_locate(routine, source, len);
    osoe_transport_get(context, pe, at.region, at.offset, dest, len, flags);
}

void osoe_rma_complete_gets(const char *routine, shmem_ctx_t ctx)
{
    osoe_transport_wait_gets(osoe_ctx_of(routine, ctx), routine);
}

OSOE_API void pshmem_putmem(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_put("shmem_putmem", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe, 0);
}

OSOE_API void pshmem_getmem(void *dest, const void *source, size_t nelems, int pe)
{
    osoe_rma_get("shmem_getmem", SHMEM_CTX_DEFAULT, dest, source, nelems, 1, pe, 0);
}

OSOE_API void pshmem_ctx_putmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    osoe_rma_put("shmem_ctx_putmem", ctx, dest, source, nelems, 1, pe, 0);
}

OSOE_API void pshmem_ctx_getmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    osoe_rma_get("shmem_ctx_getmem", ctx, dest, source, nelems, 1, pe, 0);
}

/* NULL for anything not reachable by loads and stores in this process: a
 * non-symmetric address, a PE out of range, or a PE the network carries
 * (§4.8). Returning NULL is always conforming. */
OSOE_API void *pshmem_ptr(const void *dest, int pe)
{
    /* Bypasses osoe_check_pe/osoe_symmetric_locate - both fatal in ways this
     * routine's own contract forbids (an out-of-range pe or a non-symmetric
     * address is a defined NULL here, not an error) - so it needs its own
     * check (conformance review F5): otherwise a call after the last
     * shmem_finalize dereferences whatever osoe_transport_direct finds in a
     * heap table that used to be real. */
    osoe_require_initialized("shmem_ptr");
    struct osoe_symmetric_location at;
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size ||
        !osoe_symmetric_find(dest, 1, &at))
        return NULL;
    return osoe_transport_direct(pe, at.region, at.offset);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(putmem)
OSOE_ALIAS(getmem)
OSOE_ALIAS(ctx_putmem)
OSOE_ALIAS(ctx_getmem)
OSOE_ALIAS(ptr)
