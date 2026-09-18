/* shmem_putmem, shmem_getmem (spec §9.6.1) and shmem_ptr (§9.1.9), over the
 * transport boundary (§4.10). */
#include <stddef.h>

#include <shmem.h>

#include "export.h"
#include "state.h"
#include "sym.h"
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
    uint64_t offset = osoe_sym_offset(routine, dest, nelems);
    osoe_tp_put(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, source, nelems, 0);
}

static void ctx_getmem(const char *routine, shmem_ctx_t ctx, void *dest,
                       const void *source, size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    osoe_check_pe(routine, pe);
    uint64_t offset = osoe_sym_offset(routine, source, nelems);
    osoe_tp_get(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, dest, nelems);
}

OSOE_API void shmem_putmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_putmem("shmem_putmem", SHMEM_CTX_DEFAULT, dest, source, nelems, pe);
}

OSOE_API void shmem_getmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_getmem("shmem_getmem", SHMEM_CTX_DEFAULT, dest, source, nelems, pe);
}

OSOE_API void shmem_ctx_putmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    ctx_putmem("shmem_ctx_putmem", ctx, dest, source, nelems, pe);
}

OSOE_API void shmem_ctx_getmem(shmem_ctx_t ctx, void *dest, const void *source,
                               size_t nelems, int pe)
{
    ctx_getmem("shmem_ctx_getmem", ctx, dest, source, nelems, pe);
}

/* NULL for anything not reachable by loads and stores in this process: a
 * non-symmetric address, a PE out of range, or a PE the network carries
 * (§4.8). Returning NULL is always conforming. */
OSOE_API void *shmem_ptr(const void *dest, int pe)
{
    uint64_t offset;
    if (pe < 0 || (uint32_t)pe >= osoe.boot.world_size ||
        !osoe_sym_heap_offset(dest, 1, &offset))
        return NULL;
    return osoe_tp_direct(pe, OSOE_REGION_HEAP, offset);
}
