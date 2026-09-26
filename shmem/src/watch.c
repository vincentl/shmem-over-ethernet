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
 * - both publications are seq_cst, and so are both loads that follow them -
 *   the writer's load of the range here, the waiter's load of the variable in
 *   osoe_transport_load - because this is a two-sided protocol (§3.16); a
 *   put's memcpy is not seq_cst, so notify for puts is best effort (§4.10).
 *
 * Since phase 1 step 8 slice 2 the slot is a record per thread and a per-PE
 * bitmap over them (§4.11 item 3, tools/modelcheck/watch_list_model.py): under
 * SHMEM_THREAD_MULTIPLE several threads of a PE may wait at once, and a
 * record found by thread index needs no key, no join and no probe chain. Below
 * MULTIPLE it is the phase-1 slot with at most one bit set. The word is
 * watch_range.h's, one encoding for both.
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

_Static_assert(OSOE_REGION_COUNT <= 1u << OSOE_WATCH_REGION_BITS,
               "a watch names its region in OSOE_WATCH_REGION_BITS (watch-range.h)");

struct osoe_transport_watch {
    struct osoe_thread_record *record;
    _Atomic uint64_t *active;           /* the word of the PE's bitmap with its bit */
    uint64_t bit;
};

/* This thread's watch: a thread holds at most one (transport.h). */
static _Thread_local struct osoe_transport_watch my_watch;

struct osoe_transport_watch *osoe_transport_watch(enum osoe_region region, uint64_t offset, size_t len)
{
    /* Every region has waiters: the encoding names the region, so each new one
     * needed this guard widened and nothing else. */
    if (region != OSOE_REGION_HEAP && region != OSOE_REGION_INTERNAL &&
        region != OSOE_REGION_STATICS)
        osoe_fatal("watching region %d is not supported", (int)region);
    uint32_t local = osoe.bootstrap.local_id, index = osoe_metrics_thread_index();
    struct osoe_thread_record *record = osoe_node_thread(&osoe.node, local, index);
    osoe_count(OSOE_C_TRANSPORT_WATCH_CALLS, 1);
    /* A thread is in one wait at a time: a published watch here is a wait
     * entered from inside another, which nothing does. */
    if (atomic_load_explicit(&record->watch_range, memory_order_acquire) != 0)
        osoe_count(OSOE_C_INVARIANT_WATCH_REENTERED, 1);
    uint64_t rng = osoe_watch_pack((unsigned)region, offset, len);
    if ((rng & OSOE_WATCH_LEN_MAX) == OSOE_WATCH_LEN_MAX)
        osoe_count(OSOE_C_TRANSPORT_WATCH_SATURATED, 1);   /* filtering to the region's end */
    my_watch = (struct osoe_transport_watch){
        record, &osoe.node.slots[local].watch_active[index / 64], UINT64_C(1) << (index % 64) };
    /* The range, then the bit: a writer that sees the bit sees the range (the
     * model's `bitfirst`), and both are seq_cst, the waiter's stores in a
     * two-sided protocol (§3.16) - a plain bit set loses a wake under TSO
     * (`plainbit`). */
    atomic_store_explicit(&record->watch_range, rng, memory_order_seq_cst);
    atomic_fetch_or_explicit(my_watch.active, my_watch.bit, memory_order_seq_cst);
    return &my_watch;
}

uint32_t osoe_transport_watch_seq(struct osoe_transport_watch *w)
{
    return atomic_load_explicit(&w->record->watch_sequence, memory_order_acquire);
}

bool osoe_transport_watch_wait(struct osoe_transport_watch *w, uint32_t seq,
                        const struct osoe_deadline *deadline)
{
    return osoe_futex_wait(&w->record->watch_sequence, seq, deadline, OSOE_FUTEX_SHARED) == 0;
}

void osoe_transport_unwatch(struct osoe_transport_watch *w)
{
    atomic_fetch_and_explicit(w->active, ~w->bit, memory_order_seq_cst);
    atomic_store_explicit(&w->record->watch_range, 0, memory_order_seq_cst);
    w->record = NULL;
}

/*
 * The writer's half (§4.9, §4.11 item 3): after the write, the PE's bitmap,
 * and every record whose bit is set and whose range the write overlaps is
 * bumped and woken - every one, not the first, since two threads waiting on
 * one variable is ordinary (the model's `firstmatch`). When nobody on the PE
 * waits it is a load of the bitmap's words, one at the default
 * OSOE_MAX_THREADS: the phase-1 slot's cost.
 */
void osoe_transport_notify(int pe, enum osoe_region region, uint64_t offset, size_t len)
{
    int32_t local = osoe_node_local_id(&osoe.node, (uint32_t)pe);
    if (local < 0)
        return;                             /* not on this node: the AM backend notifies */
    struct osoe_pe_slot *slot = &osoe.node.slots[local];
    osoe_count(OSOE_C_TRANSPORT_NOTIFY_CALLS, 1);
    uint32_t threads = osoe.node.threads_per_pe;
    for (uint32_t word = 0; word * 64 < threads; word++) {
        /* seq_cst: the writer's load in the two-sided protocol (§3.16). */
        uint64_t bits = atomic_load_explicit(&slot->watch_active[word], memory_order_seq_cst);
        while (bits) {
            uint32_t index = word * 64 + (uint32_t)__builtin_ctzll(bits);
            bits &= bits - 1;
            struct osoe_thread_record *record = osoe_node_thread(&osoe.node, (uint32_t)local,
                                                                 index);
            uint64_t rng = atomic_load_explicit(&record->watch_range, memory_order_seq_cst);
            if (!osoe_watch_covers(rng, (unsigned)region, offset, len))
                continue;
            /* Against notify.calls, this is the filter's precision: every wake
             * that the waiter's predicate then rejects cost a locked
             * instruction and a futex_wake syscall on a path whose floor is a
             * memcpy (§8.4). */
            osoe_count(OSOE_C_TRANSPORT_NOTIFY_WAKES, 1);
            atomic_fetch_add_explicit(&record->watch_sequence, 1, memory_order_seq_cst);
            osoe_futex_wake(&record->watch_sequence, INT_MAX, OSOE_FUTEX_SHARED);
        }
    }
}
