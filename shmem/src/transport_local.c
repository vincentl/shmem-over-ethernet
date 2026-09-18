/*
 * The intranode backend of the transport boundary (transport.h, §4.10).
 *
 * Every local PE's heap is mapped in this process (§4.1), so every operation
 * is a memory operation in the calling thread, applied before it returns.
 * Nothing is ever outstanding: fence and wait are the serializing store alone.
 *
 * Phase 1 step 1 has one node, so this is the only backend and dispatch is
 * trivial. The watch and notify halves of the boundary are in watch.c, which
 * both this backend and a future progress thread call.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "base/order.h"
#include "fatal.h"
#include "metrics.h"
#include "state.h"
#include "transport.h"

struct osoe_tp_ctx {
    char unused;                /* ordering and dirty-set state belong to AM */
};

static struct osoe_tp_ctx default_ctx;

void osoe_tp_init(void)
{
}

void osoe_tp_fini(void)
{
}

struct osoe_tp_ctx *osoe_tp_ctx_default(void)
{
    return &default_ctx;
}

void *osoe_tp_direct(int pe, enum osoe_region region, uint64_t offset)
{
    if (region != OSOE_REGION_HEAP)
        return NULL;
    int32_t local = osoe.node.local_of_rank[pe];
    if (local < 0)
        return NULL;
    return (char *)osoe.node.heap_of[local] + offset;
}

void osoe_tp_fence(struct osoe_tp_ctx *ctx)
{
    (void)ctx;
    osoe_count(OSOE_C_TP_FENCE_CALLS, 1);
    osoe_serializing_store();
}

void osoe_tp_wait(struct osoe_tp_ctx *ctx, const int *pes, size_t npes)
{
    (void)ctx;
    if (pes && npes == 0)
        return;
    osoe_count(OSOE_C_TP_WAIT_CALLS, 1);
    /* Nothing is ever outstanding on this backend, so every wait finds nothing
     * to do. The counter says so rather than being omitted, because against the
     * AM backend the same name means something quite different. */
    osoe_count(OSOE_C_TP_WAIT_NOTHING_OUTSTANDING, 1);
    osoe_serializing_store();
}

void osoe_tp_put(struct osoe_tp_ctx *ctx, int pe, enum osoe_region region,
                 uint64_t offset, const void *src, size_t len, unsigned flags)
{
    (void)ctx;
    (void)flags;                /* applied in issue order already: FENCED and MORE are free */
    osoe_count(OSOE_C_TP_PUT_CALLS, 1);
    osoe_count(OSOE_C_TP_PUT_BYTES, len);
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
    osoe_count(OSOE_C_TP_GET_CALLS, 1);
    osoe_count(OSOE_C_TP_GET_BYTES, len);
    const char *src = osoe_tp_direct(pe, region, offset);
    if ((uint32_t)pe == osoe.boot.rank)
        memmove(dst, src, len);
    else
        memcpy(dst, src, len);
}

/*
 * rmw and load are one C11 atomic operation on the shared mapping (§4.1), the
 * same instruction the progress thread will use to apply a remote operation,
 * so the two unify. §3.16 fixes the orders: CAS and fetch_add name seq_cst, a
 * set-style write names seq_cst because the watch protocol is two-sided, and a
 * load names acquire.
 */
#define OSOE_RMW_BODY(T)                                                             \
    _Atomic T *p = ptr;                                                              \
    T v = (T)operand, old = 0;                                                       \
    switch (op) {                                                                    \
    case OSOE_RMW_WRITE:                                                             \
        if (flags & OSOE_TP_FETCH)                                                   \
            old = atomic_exchange_explicit(p, v, memory_order_seq_cst);               \
        else                                                                         \
            atomic_store_explicit(p, v, memory_order_seq_cst);                       \
        break;                                                                       \
    case OSOE_RMW_READ:                                                              \
        old = atomic_load_explicit(p, memory_order_acquire);                          \
        break;                                                                       \
    case OSOE_RMW_CSWAP: {                                                           \
        T want = (T)compare;                                                         \
        atomic_compare_exchange_strong_explicit(p, &want, v, memory_order_seq_cst,    \
                                                memory_order_seq_cst);                \
        old = want;                     /* the value found, swapped or not */        \
        break;                                                                        \
    }                                                                                 \
    case OSOE_RMW_ADD:                                                               \
        old = atomic_fetch_add_explicit(p, v, memory_order_seq_cst);                   \
        break;                                                                        \
    case OSOE_RMW_AND:                                                               \
        old = atomic_fetch_and_explicit(p, v, memory_order_seq_cst);                   \
        break;                                                                        \
    case OSOE_RMW_OR:                                                                \
        old = atomic_fetch_or_explicit(p, v, memory_order_seq_cst);                    \
        break;                                                                        \
    case OSOE_RMW_XOR:                                                               \
        old = atomic_fetch_xor_explicit(p, v, memory_order_seq_cst);                   \
        break;                                                                        \
    }                                                                                 \
    result = old;

uint64_t osoe_tp_rmw(struct osoe_tp_ctx *ctx, int pe, enum osoe_region region,
                     uint64_t offset, enum osoe_rmw_op op, unsigned width,
                     uint64_t operand, uint64_t compare, unsigned flags)
{
    (void)ctx;
    osoe_count(OSOE_C_TP_RMW_CALLS, 1);
    if (flags & OSOE_TP_FETCH)
        osoe_count(OSOE_C_TP_RMW_FETCHING, 1);
    void *ptr = osoe_tp_direct(pe, region, offset);
    uint64_t result = 0;
    if (width == 8) {
        OSOE_RMW_BODY(uint64_t)
    } else {
        OSOE_RMW_BODY(uint32_t)
    }
    /* Exact: every applied rmw notifies (§4.10). */
    osoe_tp_notify(pe, region, offset, width);
    return flags & OSOE_TP_FETCH ? result : 0;
}

uint64_t osoe_tp_load(enum osoe_region region, uint64_t offset, unsigned width)
{
    void *ptr = osoe_tp_direct((int)osoe.boot.rank, region, offset);
    switch (width) {
    case 8:
        return atomic_load_explicit((_Atomic uint64_t *)ptr, memory_order_acquire);
    case 4:
        return atomic_load_explicit((_Atomic uint32_t *)ptr, memory_order_acquire);
    default:
        return atomic_load_explicit((_Atomic uint16_t *)ptr, memory_order_acquire);
    }
}
