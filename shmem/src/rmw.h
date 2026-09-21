/*
 * Applying a read-modify-write to a word (§3.16, §4.1, §4.10).
 *
 * **One definition, used by every path that applies an rmw.** §4.1's atomicity
 * argument is that osoe gets exclusivity across atomicity domains for free
 * "because every path is the same CPU atomic on the same mapping", and §4.10
 * says the progress thread applies a remote rmw "with the same instruction on
 * the same cache line, so the two unify". That is only true while the intranode
 * backend and the AM handler really do execute the same instruction - so they
 * share this, rather than each carrying a copy that could drift into a
 * different order or a different primitive without anything failing.
 *
 * §3.16's orders, which are not adjustable: CAS and fetch_* name seq_cst and
 * compile to locked instructions on every target; a plain write is a seq_cst
 * store because it is one half of a two-sided protocol; a read names acquire.
 */
#ifndef OSOE_RMW_H
#define OSOE_RMW_H

#include <stdatomic.h>
#include <stdint.h>

#include "transport.h"

#define OSOE_RMW_BODY(T)                                                              \
    _Atomic T *word = at;                                                             \
    T v = (T)operand, old = 0;                                                        \
    switch (op) {                                                                     \
    case OSOE_RMW_WRITE:                                                              \
        if (flags & OSOE_TRANSPORT_FETCH)                                             \
            old = atomic_exchange_explicit(word, v, memory_order_seq_cst);            \
        else                                                                          \
            atomic_store_explicit(word, v, memory_order_seq_cst);                     \
        break;                                                                        \
    case OSOE_RMW_READ:                                                               \
        old = atomic_load_explicit(word, memory_order_acquire);                       \
        break;                                                                        \
    case OSOE_RMW_CSWAP: {                                                            \
        T want = (T)compare;                                                          \
        atomic_compare_exchange_strong_explicit(word, &want, v, memory_order_seq_cst, \
                                                memory_order_seq_cst);                \
        old = want;                     /* the value found, swapped or not */         \
        break;                                                                        \
    }                                                                                 \
    case OSOE_RMW_ADD:                                                                \
        old = atomic_fetch_add_explicit(word, v, memory_order_seq_cst);               \
        break;                                                                        \
    case OSOE_RMW_AND:                                                                \
        old = atomic_fetch_and_explicit(word, v, memory_order_seq_cst);               \
        break;                                                                        \
    case OSOE_RMW_OR:                                                                 \
        old = atomic_fetch_or_explicit(word, v, memory_order_seq_cst);                \
        break;                                                                        \
    case OSOE_RMW_XOR:                                                                \
        old = atomic_fetch_xor_explicit(word, v, memory_order_seq_cst);               \
        break;                                                                        \
    }                                                                                 \
    result = (uint64_t)old;

/*
 * Apply `op` to the `width`-byte word at `at`, returning its previous value.
 * `width` is 4 or 8, and the caller has already checked alignment: an unaligned
 * atomic does not fault on aarch64 Linux, it is silently misaligned (§3.4).
 */
static inline uint64_t osoe_rmw_apply(void *at, enum osoe_rmw_op op, unsigned width,
                                      uint64_t operand, uint64_t compare, unsigned flags)
{
    uint64_t result = 0;
    if (width == 8) {
        OSOE_RMW_BODY(uint64_t)
    } else {
        OSOE_RMW_BODY(uint32_t)
    }
    return result;
}

#endif /* OSOE_RMW_H */
