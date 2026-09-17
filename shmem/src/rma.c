/* shmem_putmem, shmem_getmem (spec §9.6.1) and shmem_ptr (§9.1.9), over the
 * transport boundary (§4.10). */
#include <stddef.h>

#include <shmem.h>

#include "export.h"
#include "state.h"
#include "sym.h"
#include "transport.h"

/* The context parameter is plumbed now (§7.3); only the default exists. */
static void ctx_putmem(struct osoe_tp_ctx *ctx, void *dest, const void *source,
                       size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    osoe_check_pe("shmem_putmem", pe);
    uint64_t offset = osoe_sym_offset("shmem_putmem", dest, nelems);
    osoe_tp_put(ctx, pe, OSOE_REGION_HEAP, offset, source, nelems, 0);
}

static void ctx_getmem(struct osoe_tp_ctx *ctx, void *dest, const void *source,
                       size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    osoe_check_pe("shmem_getmem", pe);
    uint64_t offset = osoe_sym_offset("shmem_getmem", source, nelems);
    osoe_tp_get(ctx, pe, OSOE_REGION_HEAP, offset, dest, nelems);
}

OSOE_API void shmem_putmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_putmem(osoe_tp_ctx_default(), dest, source, nelems, pe);
}

OSOE_API void shmem_getmem(void *dest, const void *source, size_t nelems, int pe)
{
    ctx_getmem(osoe_tp_ctx_default(), dest, source, nelems, pe);
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
