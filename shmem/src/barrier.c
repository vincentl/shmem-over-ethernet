#include <pshmem.h>
#include <shmem.h>

#include <stdatomic.h>

#include "barrier.h"
#include "blocked.h"
#include "collectives.h"
#include "metrics.h"
#include "base/epoch.h"
#include "export.h"
#include "fatal.h"
#include "internode.h"
#include "state.h"
#include "team.h"
#include "transport.h"

/*
 * A team sync on one node (§5.3-§5.5).
 *
 * Each member arrives with one locked increment. No member waits for a
 * designated one: whichever increment reaches the epoch's arrival target ends
 * the node's arrival. On a team within one node that member bumps `released`
 * itself - the single-node release path of §5.5. On a team spanning several it
 * posts the node's internode arrival instead and bumps nothing: the release
 * comes back from the root and is applied by this node's progress thread
 * (§5.3). Every member then waits for `released` to equal k + 1 (§5.4:
 * equality, bounded skew), and that wait is the same on both paths.
 *
 * **The target counts the team's members on this node, not its members**
 * (§5.2). The two coincide on one node and only there, so a target written
 * against `members` is a defect no single-node test can show: it is exactly
 * right until the first multi-node job, where every node would wait for the
 * whole team to arrive on it.
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

    /*
     * A team of one (team.h): the caller is the only member, so the
     * synchronization is over the moment it is called. It touches none of the
     * node-wide counters, which belong to the node's team and not to this one.
     */
    if (t->singleton) {
        *t->epoch = k + 1;
        atomic_store_explicit(t->published, k + 1, memory_order_release);
        return;
    }

    if (osoe_epoch_bump_eq(t->arrived, (k + 1) * t->local_members)) {
        if (t->internode) {
            /*
             * §5.3's other half. This node has now arrived for every member of
             * the team on it, and the release is not ours to give: the nodes
             * meet through internode.h, and the release comes back as its
             * upcall below, on the node's progress thread. So hand the arrival
             * over and then wait exactly like every other member - there is no
             * designated PE here either, and in particular this is not
             * necessarily the lead. The call does not block (internode.h).
             */
            osoe_internode_arrive(t->number, k + 1);
        } else {
            if (!osoe_epoch_release_bump_eq(t->released, k + 1))
                osoe_fatal("%s epochs diverged at epoch %u: PEs called different numbers "
                           "of collectives", t->name, k + 1);
            /* The single-node release: this thread is the last arriving
             * member, not the node's network, and on the cores this has been
             * measured on the kernel's one-pass wake-all beats the chain
             * (§5.3). The internode release below chains. */
            osoe_epoch_wake_all(t->released);
        }
    }
    /* Chained exactly when the release comes from the progress thread, which
     * wakes two and leaves the rest to the members (base/epoch.h). */
    if (t->internode)
        osoe_epoch_wait_eq_chained(t->released, k + 1, routine);
    else
        osoe_epoch_wait_eq(t->released, k + 1, routine);
    *t->epoch = k + 1;
    /* Publish it: comparing each PE's k against the team's counters is how
     * osoe-stat says which PE has not reached a barrier, the most common real
     * hang (§3.14 question 1). */
    atomic_store_explicit(t->published, k + 1, memory_order_release);
}

/*
 * internode.h's upcall: the nodes have met for (team, epoch), so release this
 * node's members. Called on the node's progress thread, once per (team,
 * epoch). A divisible event, so wake-many (§3.11) - as a chain, so that this
 * thread, which is the node's network, wakes two and not L (base/epoch.h).
 *
 * The bump is checked for the same reason the single-node path checks it: a
 * release that does not land exactly on k+1 means this node's members have run
 * a different number of collectives from the rest of the job, and §5.4's
 * equality waits turn that into a hang rather than an error unless it is named
 * here.
 *
 * The team is found by number (team.c), so a team is anything the table holds:
 * nothing here, and nothing in the backend, knows how many teams exist.
 */
void osoe_internode_released(uint32_t team, uint32_t epoch)
{
    struct osoe_team *t = osoe_team_numbered(team);
    if (!t || !t->internode)
        osoe_fatal("an internode release names team %u, which has no internode phase "
                   "on this node", team);
    if (!osoe_epoch_release_bump_eq(t->released, epoch))
        osoe_fatal("the internode release for %s arrived for epoch %u out of order: "
                   "the PEs of this job called different numbers of collectives", t->name,
                   epoch);
    osoe_epoch_release_wake(t->released, epoch);
}

/* A sync, timed and recorded, without the quiet. */
static void team_sync(struct osoe_team *t, const char *routine)
{
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, *t->epoch + 1);
    sync_team(t, routine);
    osoe_blocked_leave();
    osoe_count(OSOE_C_SHMEM_BARRIER_NS, osoe_now_ns() - t0);
}

/* §5.6: barrier_all is a quiet on the default context followed by a sync on the
 * world team (spec §9.10.1 Notes). The wait begins with the serializing fence,
 * which makes the calling thread's memory stores visible (§4.6). */
void osoe_barrier_all(const char *routine)
{
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    struct osoe_team *world = osoe_team_of(routine, SHMEM_TEAM_WORLD);
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, osoe.world_epoch + 1);
    osoe_transport_wait(osoe_transport_context_default(), NULL, 0, "shmem_quiet");
    sync_team(world, routine);
    osoe_blocked_leave();
    osoe_count(OSOE_C_SHMEM_BARRIER_NS, osoe_now_ns() - t0);
}

/*
 * The barrier inside shmem_finalize (spec §9.1.4): a quiet, then the finalize
 * team's own synchronization - the world's members on counters no application
 * collective touches. So a PE finalizing while another is in
 * shmem_barrier_all waits for it in finalize, and the other waits in its
 * barrier: a hang in a program that does not match its collectives, which is
 * that program's, rather than a barrier returning that nobody matched.
 */
void osoe_barrier_finalize(void)
{
    uint64_t t0 = osoe_now_ns();
    osoe_count(OSOE_C_SHMEM_BARRIER_CALLS, 1);
    osoe_blocked_enter("shmem_finalize", OSOE_BLOCKED_DETAIL_EPOCH, osoe.finalize_epoch + 1);
    osoe_transport_wait(osoe_transport_context_default(), NULL, 0, "shmem_quiet");
    sync_team(osoe_team_finalize(), "shmem_finalize");
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

/*
 * The deprecated active-set forms (Annex F), over the whole job only
 * (collectives.h): shmem_barrier is shmem_barrier_all and shmem_sync is
 * shmem_sync_all, pSync neither read nor written - which leaves it as the spec
 * requires it to be found afterwards, and makes its reuse rules hold trivially.
 */
OSOE_API void pshmem_barrier(int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    (void)pSync;
    osoe_active_set_whole_job("shmem_barrier", PE_start, logPE_stride, PE_size);
    osoe_barrier_all("shmem_barrier");
}

OSOE_API void pshmem_sync(int PE_start, int logPE_stride, int PE_size, long *pSync)
{
    (void)pSync;
    osoe_active_set_whole_job("shmem_sync", PE_start, logPE_stride, PE_size);
    team_sync(osoe_team_of("shmem_sync", SHMEM_TEAM_WORLD), "shmem_sync");
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(barrier_all)
OSOE_ALIAS(sync_all)
OSOE_ALIAS(team_sync)
OSOE_ALIAS(barrier)
OSOE_ALIAS(sync)
