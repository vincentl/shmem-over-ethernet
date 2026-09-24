/*
 * The intranode path of the transport boundary (transport.h, §4.10).
 *
 * Every local PE's heap is mapped in this process (§4.1), so an operation on a
 * PE the node map puts on this node (osoe_routes_in_process, job.h) is a memory
 * operation in the calling thread, applied before it returns. Where the bytes
 * are is osoe_transport_local_address; whether the application may have that
 * address is osoe_transport_direct, and §8.1's force-remote flag separates the
 * two. Nothing taking this path is ever
 * outstanding, which is why fence and wait have nothing to do for it.
 *
 * In step 1 this was the whole backend. It is now the near half of one:
 * transport-am.c owns §4.10's entry points and sends each operation here or
 * over the AM layer, on the answer the one locality function gives (§8.1).
 * Splitting them keeps this file what it was - the reference implementation
 * simple enough to trust, which is what the differential test of §8.1 diffs
 * against.
 *
 * The watch and notify halves of the boundary are in watch.c, which both this
 * path and the progress thread call.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "base/order.h"
#include "fatal.h"
#include "job.h"
#include "metrics.h"
#include "state.h"
#include "rmw.h"
#include "transport.h"
#include "transport-local.h"

void *osoe_transport_local_address(int pe, enum osoe_region region, uint64_t offset)
{
    if (pe < 0 || (uint32_t)pe >= osoe.bootstrap.world_size)
        return NULL;
    int32_t local = osoe_node_local_id(&osoe.node, (uint32_t)pe);
    if (local < 0)
        return NULL;
    /* OSOE_REGION_INTERNAL is the tail of the same mapping (internal-region.h). */
    switch (region) {
    case OSOE_REGION_HEAP:
        return (char *)osoe.node.heap_of[local] + offset;
    case OSOE_REGION_INTERNAL:
        return (char *)osoe.node.heap_of[local] + osoe.node.internal_offset + offset;
    case OSOE_REGION_STATICS:
        /* Each local PE's statics object, mapped whole (node.h); this PE's own
         * is the same object its program's globals are mapped from. */
        return osoe.node.statics_of ? (char *)osoe.node.statics_of[local] + offset : NULL;
    default:
        return NULL;
    }
}

void osoe_transport_local_put(int pe, enum osoe_region region, uint64_t offset,
                              const void *src, size_t len, unsigned flags)
{
    (void)flags;                /* applied in issue order already: FENCED and MORE are free */
    osoe_count(OSOE_C_TRANSPORT_PUT_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_PUT_BYTES, len);
    char *dst = osoe_transport_local_address(pe, region, offset);
    /* A put to the caller's own PE may name overlapping bytes. */
    if ((uint32_t)pe == osoe.bootstrap.rank)
        memmove(dst, src, len);
    else
        memcpy(dst, src, len);
    osoe_transport_notify(pe, region, offset, len);
}

void osoe_transport_local_get(int pe, enum osoe_region region, uint64_t offset,
                              void *dst, size_t len)
{
    osoe_count(OSOE_C_TRANSPORT_GET_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_GET_BYTES, len);
    const char *src = osoe_transport_local_address(pe, region, offset);
    if ((uint32_t)pe == osoe.bootstrap.rank)
        memmove(dst, src, len);
    else
        memcpy(dst, src, len);
}

/*
 * rmw and load are one C11 atomic operation on the shared mapping (§4.1), the
 * same instruction the progress thread will use to apply a remote operation,
 * so the two unify. §3.16 fixes the orders: CAS and fetch_add name seq_cst, a
 * set-style write names seq_cst because the watch protocol is two-sided, and so
 * does osoe_transport_load (transport-am.c), which is the waiter's half of it.
 */

uint64_t osoe_transport_local_rmw(int pe, enum osoe_region region, uint64_t offset,
                                  enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                  uint64_t compare, unsigned flags)
{
    osoe_count(OSOE_C_TRANSPORT_RMW_CALLS, 1);
    if (flags & OSOE_TRANSPORT_FETCH)
        osoe_count(OSOE_C_TRANSPORT_RMW_FETCHING, 1);
    void *ptr = osoe_transport_local_address(pe, region, offset);
    /* The same instruction the AM handler uses on the same cache line, which is
     * what makes §4.1's cross-domain exclusivity true rather than coincidental
     * (rmw.h). */
    uint64_t result = osoe_rmw_apply(ptr, op, width, operand, compare, flags);
    /* Exact: every applied rmw notifies (§4.10). */
    osoe_transport_notify(pe, region, offset, width);
    return flags & OSOE_TRANSPORT_FETCH ? result : 0;
}
