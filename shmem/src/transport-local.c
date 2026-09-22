/*
 * The intranode path of the transport boundary (transport.h, §4.10).
 *
 * Every local PE's heap is mapped in this process (§4.1), so an operation on a
 * PE that osoe_transport_direct accepts is a memory operation in the calling
 * thread, applied before it returns. Nothing taking this path is ever
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

void osoe_transport_local_put(int pe, enum osoe_region region, uint64_t offset,
                              const void *src, size_t len, unsigned flags)
{
    (void)flags;                /* applied in issue order already: FENCED and MORE are free */
    osoe_count(OSOE_C_TRANSPORT_PUT_CALLS, 1);
    osoe_count(OSOE_C_TRANSPORT_PUT_BYTES, len);
    char *dst = osoe_transport_direct(pe, region, offset);
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
    const char *src = osoe_transport_direct(pe, region, offset);
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
 * does the load below, which is the waiter's half of it.
 */

uint64_t osoe_transport_local_rmw(int pe, enum osoe_region region, uint64_t offset,
                                  enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                  uint64_t compare, unsigned flags)
{
    osoe_count(OSOE_C_TRANSPORT_RMW_CALLS, 1);
    if (flags & OSOE_TRANSPORT_FETCH)
        osoe_count(OSOE_C_TRANSPORT_RMW_FETCHING, 1);
    void *ptr = osoe_transport_direct(pe, region, offset);
    /* The same instruction the AM handler uses on the same cache line, which is
     * what makes §4.1's cross-domain exclusivity true rather than coincidental
     * (rmw.h). */
    uint64_t result = osoe_rmw_apply(ptr, op, width, operand, compare, flags);
    /* Exact: every applied rmw notifies (§4.10). */
    osoe_transport_notify(pe, region, offset, width);
    return flags & OSOE_TRANSPORT_FETCH ? result : 0;
}

/*
 * Every wait's predicate load (wait.c, signal.c). It follows the waiter's
 * seq_cst publication of its watch, and the writer's half is a seq_cst store or
 * rmw followed by notify's load of that watch: a two-sided protocol, so this
 * load is seq_cst (§3.16). Acquire is not enough - C11 does not order a seq_cst
 * store before a later acquire load, and GCC for Armv8.3 and later (a Pi 5's
 * Cortex-A76) emits it as ldapr, which passes the stlr: measured, 289 to 4,449
 * lost-wake outcomes in 2 million rounds (fence-litmus/store-buffer-litmus).
 * A seq_cst load is an ldar on every aarch64 target and a plain mov on x86-64.
 */
uint64_t osoe_transport_load(enum osoe_region region, uint64_t offset, unsigned width)
{
    void *ptr = osoe_transport_direct((int)osoe.bootstrap.rank, region, offset);
    switch (width) {
    case 8:
        return atomic_load_explicit((_Atomic uint64_t *)ptr, memory_order_seq_cst);
    case 4:
        return atomic_load_explicit((_Atomic uint32_t *)ptr, memory_order_seq_cst);
    default:
        return atomic_load_explicit((_Atomic uint16_t *)ptr, memory_order_seq_cst);
    }
}

/*
 * Nothing to do: every operation on this backend is a memory operation on a
 * mapping the process already holds, so no range needs preparing (§4.10). The
 * call is still made by the allocator, because the contract is the backend's to
 * ignore, not the caller's to skip.
 */
void osoe_transport_set_range(enum osoe_region region, uint64_t offset, uint64_t len,
                       unsigned attributes)
{
    (void)region;
    (void)offset;
    (void)len;
    (void)attributes;
    osoe_count(OSOE_C_TRANSPORT_SET_RANGE_CALLS, 1);
}
