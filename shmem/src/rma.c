/* shmem_putmem, shmem_getmem (spec §9.6.1) and shmem_ptr (§9.1.9), over the
 * transport boundary (§4.10). */
#include <stddef.h>

#include <shmem.h>

#include "export.h"
#include "fatal.h"
#include "state.h"
#include "sym.h"
#include "transport.h"

static void check_pe(const char *routine, int pe)
{
    if (pe < 0 || (uint32_t)pe >= osoe.boot.world_size)
        osoe_fatal("%s: PE %d is not in 0..%u", routine, pe, osoe.boot.world_size - 1);
}

/* Symmetric statics are not supported yet (§4.3, §7.4): an address outside
 * the heap is an error naming the routine, not a silent miss. */
static uint64_t heap_offset(const char *routine, const void *addr, size_t len)
{
    uint64_t offset;
    if (!osoe_sym_heap_offset(addr, len, &offset))
        osoe_fatal("%s: [%p, +%zu) is not within the symmetric heap (symmetric statics "
                   "are not yet supported)", routine, addr, len);
    return offset;
}

/* The context parameter is plumbed now (§7.3); only the default exists. */
static void ctx_putmem(struct osoe_tp_ctx *ctx, void *dest, const void *source,
                       size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    check_pe("shmem_putmem", pe);
    uint64_t offset = heap_offset("shmem_putmem", dest, nelems);
    osoe_tp_put(ctx, pe, OSOE_REGION_HEAP, offset, source, nelems, 0);
}

static void ctx_getmem(struct osoe_tp_ctx *ctx, void *dest, const void *source,
                       size_t nelems, int pe)
{
    if (nelems == 0)
        return;
    check_pe("shmem_getmem", pe);
    uint64_t offset = heap_offset("shmem_getmem", source, nelems);
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
