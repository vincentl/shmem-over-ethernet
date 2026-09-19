#include <pshmem.h>
#include <shmem.h>

#include <stdatomic.h>

#include "barrier.h"
#include "blocked.h"
#include "metrics.h"
#include "base/epoch.h"
#include "export.h"
#include "fatal.h"
#include "state.h"
#include "team.h"
#include "transport.h"

/*
 * A team sync on one node (§5.3-§5.5).
 *
 * Each member arrives with one locked increment. No member waits for a
 * designated one: whichever increment reaches the epoch's arrival target
 * releases the epoch. Here every team spans one node, so that member bumps
 * `released` itself - the single-node release path of §5.5 - where a
 * multi-node team would send the internode arrival AM instead. Every member
 * then waits for `released` to equal k + 1 (§5.4: equality, bounded skew).
 *
 * The counters and the epoch count are the *team's*, which is why they arrive
 * through struct osoe_team rather than being read from the header here. Two
 * teams with the same membership - as SHMEM_TEAM_WORLD and SHMEM_TEAM_SHARED
 * have on one node - must not share them, or a sync on one would release a wait
 * on the other.
 */
static void sync_team(struct osoe_team *t, const char *routine)
{
    uint32_t k = *t->epoch;

    if (osoe_epoch_bump_eq(t->arrived, (k + 1) * t->members)) {
        if (!osoe_epoch_bump_eq(t->released, k + 1))
            osoe_fatal("%s epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", t->name, k + 1);
        osoe_epoch_wake_all(t->released);
    }
    osoe_epoch_wait_eq(t->released, k + 1, routine);
    *t->epoch = k + 1;
    /* Publish it: comparing each PE's k against the team's counters is how
     * osoe-stat says which PE has not reached a barrier, the most common real
     * hang (§3.14 question 1). */
    atomic_store_explicit(t->published, k + 1, memory_order_release);
}

/* A sync, timed and recorded, without the quiet. */
static void team_sync(struct osoe_team *t, const char *routine)
{
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    osoe_blocked_enter(routine, *t->epoch + 1);
    sync_team(t, routine);
    osoe_blocked_leave();
    osoe_count(OSOE_C_SHMEM_BARRIER_NS, osoe_now_ns() - t0);
}

/* §5.6: barrier_all is a quiet on the default context followed by a sync on the
 * world team (spec §9.10.1 Notes). The wait begins with the serializing store,
 * which makes the calling thread's memory stores visible (§4.6). */
void osoe_barrier_all(const char *routine)
{
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    struct osoe_team *world = osoe_team_of(routine, SHMEM_TEAM_WORLD);
    osoe_blocked_enter(routine, osoe.world_epoch + 1);
    osoe_transport_wait(osoe_transport_context_default(), NULL, 0);
    sync_team(world, routine);
    osoe_blocked_leave();
    osoe_count(OSOE_C_SHMEM_BARRIER_NS, osoe_now_ns() - t0);
}

OSOE_API void pshmem_barrier_all(void)
{
    osoe_barrier_all("shmem_barrier_all");
}

/*
 * §5.6, and the difference that matters: sync synchronizes and does not
 * complete. Implementing it as a barrier would be correct and needlessly slow;
 * implementing barrier_all without the quiet would be incorrect and would pass
 * most tests, which is why the two are written apart rather than one in terms
 * of the other.
 */
OSOE_API void pshmem_sync_all(void)
{
    team_sync(osoe_team_of("shmem_sync_all", SHMEM_TEAM_WORLD), "shmem_sync_all");
}

/* spec §9.10.3. SHMEM_TEAM_INVALID is not a team to synchronize, and returns
 * nonzero rather than blocking forever on a team that does not exist. */
OSOE_API int pshmem_team_sync(shmem_team_t team)
{
    struct osoe_team *t = osoe_team_of("shmem_team_sync", team);
    if (!t)
        return 1;
    team_sync(t, "shmem_team_sync");
    return 0;
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(barrier_all)
OSOE_ALIAS(sync_all)
OSOE_ALIAS(team_sync)
