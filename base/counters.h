/*
 * Single-writer counter blocks (§3.14 cost rules).
 *
 * One block per thread - application thread, agent, progress thread - updated
 * only by its owning thread with an acquire load and a release store: §3.16's
 * single-writer publication, with no locked instruction. Readers in other
 * threads or processes load with acquire and accept a value a moment stale.
 *
 * A block is several cache lines rather than one, because §3.14's catalogue is
 * some forty counters and one line holds eight or sixteen. What matters is that
 * blocks do not share a line with each other, which the alignment gives; within
 * a block there is one writer, so there is nothing to share.
 */
#ifndef OSOE_BASE_COUNTERS_H
#define OSOE_BASE_COUNTERS_H

#include <stdatomic.h>
#include <stdint.h>

#include "base/cacheline.h"

/* 512 bytes: four lines on aarch64, eight elsewhere, and a whole number of
 * lines on both, so one block never shares a line with the next. */
#define OSOE_COUNTER_BLOCK_BYTES 512
enum { OSOE_COUNTERS_PER_BLOCK = OSOE_COUNTER_BLOCK_BYTES / 8 };

struct osoe_counter_block {
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t c[OSOE_COUNTERS_PER_BLOCK];
};

_Static_assert(sizeof(struct osoe_counter_block) % OSOE_CACHELINE == 0,
               "a counter block must be a whole number of cache lines, so that "
               "two blocks never share one");

/* Owning thread only. */
static inline void osoe_counter_add(struct osoe_counter_block *b, unsigned i, uint64_t n)
{
    uint64_t v = atomic_load_explicit(&b->c[i], memory_order_acquire);
    atomic_store_explicit(&b->c[i], v + n, memory_order_release);
}

static inline void osoe_counter_set(struct osoe_counter_block *b, unsigned i, uint64_t v)
{
    atomic_store_explicit(&b->c[i], v, memory_order_release);
}

/* A high-water mark: the owning thread's running maximum. */
static inline void osoe_counter_max(struct osoe_counter_block *b, unsigned i, uint64_t v)
{
    if (v > atomic_load_explicit(&b->c[i], memory_order_acquire))
        atomic_store_explicit(&b->c[i], v, memory_order_release);
}

/*
 * The same two operations on a lone counter word rather than a block slot, for
 * a record with one writer thread that is not a thread's own block - the node
 * header's AM counters, whose writer is the lead's progress thread (§3.12).
 */
static inline void osoe_counter_word_add(_Atomic uint64_t *w, uint64_t n)
{
    uint64_t v = atomic_load_explicit(w, memory_order_acquire);
    atomic_store_explicit(w, v + n, memory_order_release);
}

static inline void osoe_counter_word_max(_Atomic uint64_t *w, uint64_t v)
{
    if (v > atomic_load_explicit(w, memory_order_acquire))
        atomic_store_explicit(w, v, memory_order_release);
}

static inline uint64_t osoe_counter_word_read(const _Atomic uint64_t *w)
{
    return atomic_load_explicit(w, memory_order_acquire);
}

/* Any thread or process. */
static inline uint64_t osoe_counter_read(const struct osoe_counter_block *b, unsigned i)
{
    return atomic_load_explicit(&b->c[i], memory_order_acquire);
}

#endif /* OSOE_BASE_COUNTERS_H */
