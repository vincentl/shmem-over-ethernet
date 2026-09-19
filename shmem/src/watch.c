/*
 * Watch slots: how a PE blocks on its own symmetric variable (§4.9).
 *
 * One slot per local PE, in the node segment. It holds the range the PE is
 * waiting on, packed into one word by watch_range.h, and a 32-bit futex
 * sequence word. A writer - another local PE, or the progress thread applying a
 * remote operation - loads the range after it has updated the bytes, and if the
 * range covers them, bumps the sequence and wakes.
 *
 * The rules, each model-checked in docs/design/evidence/watch/:
 * - the waiter publishes its range BEFORE its first predicate check;
 * - it loads the sequence word before the variables;
 * - the writer updates the variables before it loads the range;
 * - both publications are seq_cst, because this is a two-sided protocol
 *   (§3.16); a put's memcpy is not, so notify for puts is best effort (§4.10).
 *
 * Under SHMEM_THREAD_SERIALIZED and below a PE is in at most one wait at a
 * time, so one slot needs no list and no key. §4.11 item 3's MULTIPLE design is
 * this record with a per-PE bitmap over one per thread, and publishes the same
 * word - which is why watch_range.h is that word and not a second encoding.
 */
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "fatal.h"
#include "metrics.h"
#include "state.h"
#include "transport.h"
#include "watch-range.h"

struct osoe_tp_watch {
    struct osoe_pe_slot *slot;
};

static struct osoe_tp_watch the_watch;      /* one wait at a time (§4.6) */

struct osoe_tp_watch *osoe_tp_watch(enum osoe_region region, uint64_t offset, size_t len)
{
    /* Only the heap has waiters today. The encoding names the region, so
     * OSOE_REGION_INTERNAL (§5.8) and statics (§4.3 phase 1.5) need this guard
     * removed and nothing else - not a second layout version. */
    if (region != OSOE_REGION_HEAP)
        osoe_fatal("watching region %d is not supported", (int)region);
    struct osoe_pe_slot *slot = &osoe.node.slots[osoe.boot.local_id];
    osoe_count(OSOE_C_TP_WATCH_CALLS, 1);
    /* One slot per PE is enough while a PE is in one wait at a time (§4.6). A
     * published watch here means two threads are inside the library at once,
     * which SHMEM_THREAD_SERIALIZED forbids; §4.11 is the design that lifts it. */
    if (atomic_load_explicit(&slot->watch_rng, memory_order_acquire) != 0)
        osoe_count(OSOE_C_INVARIANT_WATCH_REENTERED, 1);

    uint64_t rng = osoe_watch_pack((unsigned)region, offset, len);
    if ((rng & OSOE_WATCH_LEN_MAX) == OSOE_WATCH_LEN_MAX)
        osoe_count(OSOE_C_TP_WATCH_SATURATED, 1);   /* filtering to the region's end */
    the_watch.slot = slot;
    atomic_store_explicit(&slot->watch_rng, rng, memory_order_seq_cst);
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
    atomic_store_explicit(&w->slot->watch_rng, 0, memory_order_seq_cst);
    w->slot = NULL;
}

void osoe_tp_notify(int pe, enum osoe_region region, uint64_t offset, size_t len)
{
    int32_t local = osoe.node.local_of_rank[pe];
    if (local < 0)
        return;                             /* not on this node: the AM backend notifies */
    struct osoe_pe_slot *slot = &osoe.node.slots[local];
    osoe_count(OSOE_C_TP_NOTIFY_CALLS, 1);
    uint64_t rng = atomic_load_explicit(&slot->watch_rng, memory_order_acquire);
    if (!osoe_watch_covers(rng, (unsigned)region, offset, len))
        return;
    /* Against notify.calls, this is the filter's precision: every wake here
     * that the waiter's predicate then rejects cost a locked instruction and a
     * futex_wake syscall on a path whose floor is a memcpy (§8.4). */
    osoe_count(OSOE_C_TP_NOTIFY_WAKES, 1);
    atomic_fetch_add_explicit(&slot->watch_seq, 1, memory_order_seq_cst);
    osoe_futex_wake(&slot->watch_seq, INT_MAX, OSOE_FUTEX_SHARED);
}
