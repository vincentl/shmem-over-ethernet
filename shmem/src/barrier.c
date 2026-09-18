#include <shmem.h>

#include <stdatomic.h>

#include "barrier.h"
#include "blocked.h"
#include "metrics.h"
#include "base/epoch.h"
#include "export.h"
#include "fatal.h"
#include "state.h"
#include "transport.h"

/*
 * shmem_team_sync on the world team, on one node (§5.3-§5.5).
 *
 * Each member arrives with one locked increment. No member waits for a
 * designated one: whichever increment reaches the epoch's arrival target
 * releases the epoch. Here the world team spans one node, so that member bumps
 * `released` itself - the single-node release path of §5.5 - where a
 * multi-node team would send the internode arrival AM instead. Every member
 * then waits for `released` to equal k + 1 (§5.4: equality, bounded skew).
 */
static void sync_world(const char *routine)
{
    struct osoe_node_header *h = osoe.node.hdr;
    uint32_t members = osoe.boot.local_count;
    uint32_t k = osoe.world_epoch;

    if (osoe_epoch_bump_eq(&h->world_arrived, (k + 1) * members)) {
        if (!osoe_epoch_bump_eq(&h->world_released, k + 1))
            osoe_fatal("world team epochs diverged at epoch %u: PEs called different "
                       "numbers of collectives", k + 1);
        osoe_epoch_wake_all(&h->world_released);
    }
    osoe_epoch_wait_eq(&h->world_released, k + 1, routine);
    osoe.world_epoch = k + 1;
    /* Publish it: comparing each PE's k against the team's counters is how
     * osoe-stat says which PE has not reached a barrier, the most common real
     * hang (§3.14 question 1). */
    atomic_store_explicit(&osoe.node.slots[osoe.boot.local_id].world_epoch, k + 1,
                          memory_order_release);
}

/* §5.6: barrier_all is a quiet on the default context followed by a sync on
 * the world team (spec §9.10.1 Notes). The wait begins with the serializing
 * store, which makes the calling thread's memory stores visible (§4.6). */
void osoe_barrier_all(const char *routine)
{
    /* A barrier is a blocking operation, which §3.14 allows to be timed. */
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    osoe_blocked_enter(routine, osoe.world_epoch + 1);
    osoe_tp_wait(osoe_tp_ctx_default(), NULL, 0);
    sync_world(routine);
    osoe_blocked_leave();
    osoe_count(OSOE_C_SHMEM_BARRIER_NS, osoe_now_ns() - t0);
}

OSOE_API void shmem_barrier_all(void)
{
    osoe_barrier_all("shmem_barrier_all");
}
