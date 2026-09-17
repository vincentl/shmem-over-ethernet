/*
 * Watch slots: how a PE blocks on its own symmetric variable (§4.9).
 *
 * One slot per local PE, in the node segment. It holds the byte range the PE is
 * waiting on, rounded out to the aligned power-of-two block that contains it,
 * and a 32-bit futex sequence word. A writer - another local PE, or the
 * progress thread applying a remote operation - loads the block after it has
 * updated the bytes, and if the block covers them, bumps the sequence and
 * wakes.
 *
 * The rules, each model-checked in docs/design/evidence/watch/:
 * - the waiter publishes its block BEFORE its first predicate check;
 * - it loads the sequence word before the variables;
 * - the writer updates the variables before it loads the block;
 * - both publications are seq_cst, because this is a two-sided protocol
 *   (§3.16); a put's memcpy is not, so notify for puts is best effort (§4.10).
 *
 * Under SHMEM_THREAD_SERIALIZED and below a PE is in at most one wait at a
 * time, so one slot needs no list and no key. §4.11 has the MULTIPLE design.
 */
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "fatal.h"
#include "state.h"
#include "transport.h"

struct osoe_tp_watch {
    struct osoe_pe_slot *slot;
};

static struct osoe_tp_watch the_watch;      /* one wait at a time (§4.6) */

/* The smallest aligned power-of-two block containing [offset, offset + len),
 * as ((offset >> k) << 6) | (k + 1). Zero means no watch. */
static uint64_t block_of(uint64_t offset, uint64_t len)
{
    unsigned k = 0;
    uint64_t last = offset + (len ? len - 1 : 0);
    while ((offset >> k) != (last >> k))
        k++;
    if (k >= 63)
        return (UINT64_C(0) << 6) | 64;     /* the whole region */
    return ((offset >> k) << 6) | (k + 1);
}

static bool block_covers(uint64_t block, uint64_t offset, uint64_t len)
{
    if (block == 0)
        return false;
    unsigned k = (unsigned)(block & 63) - 1;
    uint64_t want = block >> 6;
    uint64_t first = offset >> k;
    uint64_t last = (offset + (len ? len - 1 : 0)) >> k;
    return first <= want && want <= last;
}

struct osoe_tp_watch *osoe_tp_watch(enum osoe_region region, uint64_t offset, size_t len)
{
    if (region != OSOE_REGION_HEAP)
        osoe_fatal("watching region %d is not supported", (int)region);
    struct osoe_pe_slot *slot = &osoe.node.slots[osoe.boot.local_id];
    the_watch.slot = slot;
    atomic_store_explicit(&slot->watch_block, block_of(offset, len), memory_order_seq_cst);
    return &the_watch;
}

uint32_t osoe_tp_watch_seq(struct osoe_tp_watch *w)
{
    return atomic_load_explicit(&w->slot->watch_seq, memory_order_acquire);
}

bool osoe_tp_watch_wait(struct osoe_tp_watch *w, uint32_t seq,
                        const struct osoe_deadline *deadline)
{
    return osoe_futex_wait(&w->slot->watch_seq, seq, deadline, OSOE_FUTEX_SHARED) == 0;
}

void osoe_tp_unwatch(struct osoe_tp_watch *w)
{
    atomic_store_explicit(&w->slot->watch_block, 0, memory_order_seq_cst);
    w->slot = NULL;
}

void osoe_tp_notify(int pe, enum osoe_region region, uint64_t offset, size_t len)
{
    if (region != OSOE_REGION_HEAP)
        return;
    int32_t local = osoe.node.local_of_rank[pe];
    if (local < 0)
        return;                             /* not on this node: the AM backend notifies */
    struct osoe_pe_slot *slot = &osoe.node.slots[local];
    uint64_t block = atomic_load_explicit(&slot->watch_block, memory_order_acquire);
    if (!block_covers(block, offset, len))
        return;
    atomic_fetch_add_explicit(&slot->watch_seq, 1, memory_order_seq_cst);
    osoe_futex_wake(&slot->watch_seq, INT_MAX, OSOE_FUTEX_SHARED);
}
