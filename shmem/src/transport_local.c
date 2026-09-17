/*
 * The intranode backend of the transport boundary (transport.h, §4.10).
 *
 * Every local PE's heap is mapped in this process (§4.1), so every operation
 * is a memory operation in the calling thread, applied before it returns.
 * Nothing is ever outstanding: fence and wait are the serializing store alone.
 *
 * Phase 1 step 1 has one node, so this is the only backend and dispatch is
 * trivial. rmw, load and watch arrive with AMOs and wait_until.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/order.h"
#include "fatal.h"
#include "state.h"
#include "transport.h"

struct osoe_tp_ctx {
    char unused;                /* ordering and dirty-set state belong to AM */
};

static struct osoe_tp_ctx default_ctx;
static int32_t *local_of_rank;  /* world rank -> local id, or -1 */

void osoe_tp_init(void)
{
    local_of_rank = malloc(sizeof *local_of_rank * osoe.boot.world_size);
    if (!local_of_rank)
        osoe_fatal("out of memory");
    for (uint32_t r = 0; r < osoe.boot.world_size; r++)
        local_of_rank[r] = -1;
    for (uint32_t i = 0; i < osoe.boot.local_count; i++)
        local_of_rank[osoe.boot.gtids[i]] = (int32_t)i;
}

void osoe_tp_fini(void)
{
    free(local_of_rank);
    local_of_rank = NULL;
}

struct osoe_tp_ctx *osoe_tp_ctx_default(void)
{
    return &default_ctx;
}

void *osoe_tp_direct(int pe, enum osoe_region region, uint64_t offset)
{
    if (region != OSOE_REGION_HEAP)
        return NULL;
    int32_t local = local_of_rank[pe];
    if (local < 0)
        return NULL;
    return (char *)osoe.node.heap_of[local] + offset;
}

void osoe_tp_fence(struct osoe_tp_ctx *ctx)
{
    (void)ctx;
    osoe_serializing_store();
}

void osoe_tp_wait(struct osoe_tp_ctx *ctx, const int *pes, size_t npes)
{
    (void)ctx;
    if (pes && npes == 0)
        return;
    osoe_serializing_store();
}

void osoe_tp_put(struct osoe_tp_ctx *ctx, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags)
{
    (void)ctx;
    (void)flags;                /* applied in issue order already: FENCED is free */
    char *dst = osoe_tp_direct(pe, region, offset);
    /* A put to the caller's own PE may name overlapping bytes. */
    if ((uint32_t)pe == osoe.boot.rank)
        memmove(dst, src, len);
    else
        memcpy(dst, src, len);
    osoe_tp_notify(pe, region, offset, len);
}

void osoe_tp_get(struct osoe_tp_ctx *ctx, int pe, enum osoe_region region,
                 uint64_t offset, void *dst, size_t len)
{
    (void)ctx;
    const char *src = osoe_tp_direct(pe, region, offset);
    if ((uint32_t)pe == osoe.boot.rank)
        memmove(dst, src, len);
    else
        memcpy(dst, src, len);
}

/* No watch slots exist until wait_until (§4.9), so there is nobody to wake:
 * doing nothing is exactly the contract until then. */
void osoe_tp_notify(int pe, enum osoe_region region, uint64_t offset, size_t len)
{
    (void)pe;
    (void)region;
    (void)offset;
    (void)len;
}
