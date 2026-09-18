/*
 * Signalling operations (spec §9.8, §9.11.15).
 *
 * A signal data object is a remotely accessible symmetric uint64_t (§9.8.1).
 * Every update to one is a boundary rmw on that word, so it is atomic against
 * every other signalling operation and against a waiter or test on it, which is
 * exactly the exclusivity §9.8.1 asks for (§4.10: rmw, and the intranode
 * backend's C11 atomic on the shared mapping).
 *
 * Put-with-signal is a put followed by an rmw carrying OSOE_TP_FENCED, never a
 * compound transport operation (§4.10). The flag places the signal after every
 * write issued earlier on the same (context, PE), so the signal never announces
 * bytes that have not landed - invariant 5, stated at the boundary rather than
 * left to a backend's stream order.
 *
 * shmem_signal_set is a set-style update: a seq_cst store against a waiting PE,
 * because the §4.9 watch protocol is two-sided (§3.16). The boundary's rmw
 * already names that order, so this file adds nothing.
 *
 * shmem_signal_fetch reads the caller's own signal through the boundary's
 * coherent load, not an ordinary load: on a backend whose NIC applies remote
 * atomics out of the CPU's sight, only that call is guaranteed to see them
 * (§4.10).
 */
#include <stddef.h>
#include <stdint.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "fatal.h"
#include "metrics.h"
#include "sym.h"
#include "team.h"
#include "transport.h"
#include "wait.h"

enum { OSOE_SIGNAL_WIDTH = 8 };

/* The signal operator selects which rmw applies the update (spec §9.8.2). */
static enum osoe_rmw_op signal_op(const char *routine, int sig_op)
{
    switch (sig_op) {
    case SHMEM_SIGNAL_SET: return OSOE_RMW_WRITE;
    case SHMEM_SIGNAL_ADD: return OSOE_RMW_ADD;
    default:
        osoe_fatal("%s: signal operator %d is neither SHMEM_SIGNAL_SET nor "
                   "SHMEM_SIGNAL_ADD", routine, sig_op);
    }
}

static void signal_update(const char *routine, shmem_ctx_t ctx, uint64_t *sig_addr,
                          uint64_t signal, enum osoe_rmw_op op, int pe, unsigned flags)
{
    osoe_check_pe(routine, pe);
    osoe_count(OSOE_C_SHMEM_SIGNAL_UPDATE_CALLS, 1);
    uint64_t offset = osoe_sym_word_offset(routine, sig_addr, OSOE_SIGNAL_WIDTH);
    osoe_tp_rmw(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, op,
                OSOE_SIGNAL_WIDTH, signal, 0, flags);
}

/*
 * The put returns at local completion, and the signal is placed after it, so
 * the routine "returns after the data has been copied out of the source array"
 * (§9.8.3) and the signal's arrival announces the data.
 *
 * nelems 0 still signals: the spec makes the signal update unconditional, and a
 * zero-length put is the empty transfer it announces.
 */
OSOE_API void pshmem_ctx_putmem_signal(shmem_ctx_t ctx, void *dest, const void *source,
                                      size_t nelems, uint64_t *sig_addr, uint64_t signal,
                                      int sig_op, int pe)
{
    static const char routine[] = "shmem_ctx_putmem_signal";
    enum osoe_rmw_op op = signal_op(routine, sig_op);
    osoe_check_pe(routine, pe);
    if (nelems > 0) {
        uint64_t offset = osoe_sym_offset(routine, dest, nelems);
        osoe_tp_put(osoe_ctx_of(routine, ctx), pe, OSOE_REGION_HEAP, offset, source,
                    nelems, 0);
    }
    /* The fence is on the same context as the put, which is what makes it
     * order the two (spec §9.12.1, 1.6 change #18). */
    signal_update(routine, ctx, sig_addr, signal, op, pe, OSOE_TP_FENCED);
}

OSOE_API void pshmem_putmem_signal(void *dest, const void *source, size_t nelems,
                                  uint64_t *sig_addr, uint64_t signal, int sig_op, int pe)
{
    pshmem_ctx_putmem_signal(SHMEM_CTX_DEFAULT, dest, source, nelems, sig_addr, signal,
                            sig_op, pe);
}

OSOE_API void pshmem_ctx_signal_set(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal,
                                   int pe)
{
    signal_update("shmem_ctx_signal_set", ctx, sig_addr, signal, OSOE_RMW_WRITE, pe, 0);
}

OSOE_API void pshmem_ctx_signal_add(shmem_ctx_t ctx, uint64_t *sig_addr, uint64_t signal,
                                   int pe)
{
    signal_update("shmem_ctx_signal_add", ctx, sig_addr, signal, OSOE_RMW_ADD, pe, 0);
}

OSOE_API void pshmem_signal_set(uint64_t *sig_addr, uint64_t signal, int pe)
{
    pshmem_ctx_signal_set(SHMEM_CTX_DEFAULT, sig_addr, signal, pe);
}

OSOE_API void pshmem_signal_add(uint64_t *sig_addr, uint64_t signal, int pe)
{
    pshmem_ctx_signal_add(SHMEM_CTX_DEFAULT, sig_addr, signal, pe);
}

/* Local: the signal object of the calling PE (§9.8.6 takes no pe argument). */
OSOE_API uint64_t pshmem_signal_fetch(const uint64_t *sig_addr)
{
    osoe_count(OSOE_C_SHMEM_SIGNAL_FETCH_CALLS, 1);
    uint64_t offset = osoe_sym_word_offset("shmem_signal_fetch", sig_addr, OSOE_SIGNAL_WIDTH);
    return osoe_tp_load(OSOE_REGION_HEAP, offset, OSOE_SIGNAL_WIDTH);
}

/*
 * Returns the value that satisfied the condition (§9.11.15). The spec also
 * requires that it "not return before the update of the memory indicated by
 * sig_addr is fully complete": the value comes from the boundary's coherent
 * load, which is exclusive against the rmw that wrote it, so a half-applied
 * update is not a value this can return.
 */
OSOE_API uint64_t pshmem_signal_wait_until(uint64_t *sig_addr, int cmp, uint64_t cmp_value)
{
    osoe_count(OSOE_C_SHMEM_SIGNAL_WAIT_CALLS, 1);
    return osoe_wait_until("shmem_signal_wait_until", sig_addr, OSOE_SIGNAL_WIDTH,
                           false, cmp, cmp_value);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(ctx_putmem_signal)
OSOE_ALIAS(putmem_signal)
OSOE_ALIAS(ctx_signal_set)
OSOE_ALIAS(ctx_signal_add)
OSOE_ALIAS(signal_set)
OSOE_ALIAS(signal_add)
OSOE_ALIAS(signal_fetch)
OSOE_ALIAS(signal_wait_until)
