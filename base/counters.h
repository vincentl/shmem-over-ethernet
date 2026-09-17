/*
 * Single-writer counter blocks (§3.14 cost rules).
 *
 * One block per thread, cache-line aligned, updated only by its owning thread
 * with an acquire load and a release store - §3.16's single-writer
 * publication, with no locked instruction. Readers in other threads or
 * processes load with acquire and accept a value a moment stale.
 */
#ifndef OSOE_BASE_COUNTERS_H
#define OSOE_BASE_COUNTERS_H

#include <stdatomic.h>
#include <stdint.h>

#include "base/cacheline.h"

enum { OSOE_COUNTERS_PER_BLOCK = OSOE_CACHELINE / 8 };

struct osoe_counter_block {
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t c[OSOE_COUNTERS_PER_BLOCK];
};

_Static_assert(sizeof(struct osoe_counter_block) == OSOE_CACHELINE,
               "a counter block occupies exactly one cache line");

/* Owning thread only. */
static inline void osoe_counter_add(struct osoe_counter_block *b, unsigned i, uint64_t n)
{
    uint64_t v = atomic_load_explicit(&b->c[i], memory_order_acquire);
    atomic_store_explicit(&b->c[i], v + n, memory_order_release);
}

/* Any thread or process. */
static inline uint64_t osoe_counter_read(const struct osoe_counter_block *b, unsigned i)
{
    return atomic_load_explicit(&b->c[i], memory_order_acquire);
}

#endif /* OSOE_BASE_COUNTERS_H */
