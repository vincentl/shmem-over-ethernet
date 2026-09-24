/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): §5.5's
 * accumulator, for reductions and broadcast.
 *
 * **Within a node** it is §5.5 as written. Every member arrives with one locked
 * increment of the team's `arrived`. The local lead - the team's member with the
 * lowest local id, local id 0 for both teams that exist - waits for all of
 * them, then reads each member's source (only now: invariant 17, push never
 * pull) and combines it into its own `dest`, which it alone writes before
 * release (invariant 16). It bumps `released`; every other member copies the
 * lead's `dest` into its own and bumps `copied`; the lead waits for the last
 * copy and leaves last. `dest` is undefined until the call returns, so the
 * intermediate values the lead's `dest` holds are unobservable.
 *
 * **Between nodes** the lead hands its node's result to internode.h's data
 * half - osoe_internode_reduce or osoe_internode_broadcast - after its local
 * combine and before its release, and gets the result for the whole team back
 * in its `dest`. What happens in between is the backend's (phase 1 step 5
 * slice 2): AM's is the generic tree of node leads over put, fenced rmw and a
 * wait (internode-generic.c), where that phase's description now lives. The
 * reduction goes down as values - its operation and its element - not as the
 * function this file combines with, so that a backend can hand it to hardware
 * that knows the operation and cannot call a function.
 *
 * Costs, with L local members and n bytes: the lead's intranode combine is
 * O(L n), each member's copy-out O(n); the internode phase is the backend's
 * (the generic one's: ceil(n / slot) passes of depth ceil(log_k N)).
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <shmem.h>

#include "base/epoch.h"
#include "blocked.h"
#include "collectives.h"
#include "combine.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "node.h"
#include "rma.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"

/* A reduction's operation and element, and the function that combines them
 * within the node (combine.h); a broadcast has none. */
struct reduction {
    enum osoe_reduction_operation operation;
    struct osoe_element element;
    osoe_combine_function combine;
};

/* The root's node as its index in the node set `t` was registered with
 * (team.c, osoe_internode_team): the world team's set is every node, so the
 * index is the node's number. Split teams, whose sets are not, are §7.3's. */
static uint32_t root_index(const struct osoe_team *t, int root_rank, const char *routine)
{
    if (t->number != OSOE_TEAM_WORLD)
        osoe_fatal("%s: %s spans nodes, and only the world team's node set is every node",
                   routine, t->name);
    return osoe_node_of(&osoe.job, (uint32_t)root_rank);
}

/*
 * One data collective on `t`: a reduction if `reduction` is given, else a
 * broadcast from team rank `root`. §5.5, as the file's comment walks through.
 */
static int data_collective(const char *routine, shmem_team_t team, void *dest,
                           const void *source, size_t nelems, size_t width,
                           const struct reduction *reduction, int root)
{
    struct osoe_team *t = osoe_team_of(routine, team);
    if (!t)
        return 1;                       /* SHMEM_TEAM_INVALID: undefined, answered nonzero */
    if (root >= 0 && (uint32_t)root >= t->members)
        osoe_fatal("%s: PE_root %d is not a PE of %s, which has %u", routine, root, t->name,
                   t->members);
    size_t bytes = osoe_rma_bytes(routine, nelems, width);
    if (bytes == 0)
        return 0;                       /* every member passes the same count: nothing to do */
    uint64_t dest_offset = osoe_symmetric_offset(routine, dest, bytes);
    uint64_t source_offset = osoe_symmetric_offset(routine, source, bytes);
    int root_rank = root >= 0 ? osoe_team_to_world(t, root) : -1;

    uint32_t k = *t->epoch, d = *t->data_epoch, members = t->local_members;

    /* A team of one (team.h): the caller is every member, its source the
     * result, and it touches none of the node-wide counters - they belong to
     * the node's team, not to this one, as for its sync (barrier.c). */
    if (t->singleton) {
        if (source != dest)
            memmove(dest, source, bytes);
        *t->epoch = k + 1;
        *t->data_epoch = d + 1;
        atomic_store_explicit(t->published, k + 1, memory_order_release);
        return 0;
    }

    bool lead = osoe.bootstrap.local_id == 0;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);

    /*
     * Arrive; the last to arrive wakes the lead, which waits for exactly that.
     * The bump is the stamping one (base/epoch.h), because the lead waits on
     * this counter: without the stamp, a sweep expiring just as a late member
     * arrives would be counted as a lost wake.
     */
    if (osoe_epoch_release_bump_eq(t->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(t->arrived);

    if (lead) {
        osoe_epoch_wait_eq(t->arrived, (k + 1) * members, routine);
        unsigned char *accumulator = dest;
        if (reduction) {
            /* In place, the lead's source already is its dest. */
            if (source != dest)
                memcpy(accumulator, source, bytes);
            for (uint32_t m = 1; m < members; m++)
                reduction->combine(accumulator,
                                   (const unsigned char *)osoe.node.heap_of[m] + source_offset,
                                   nelems);
        } else {
            int32_t root_local = osoe_node_local_id(&osoe.node, (uint32_t)root_rank);
            if (root_local >= 0 &&
                !((uint32_t)root_local == osoe.bootstrap.local_id && source == dest))
                memcpy(accumulator, (const unsigned char *)osoe.node.heap_of[root_local] +
                                source_offset, bytes);
        }
        if (t->internode) {
            if (reduction)
                osoe_internode_reduce(t->number, k + 1, accumulator, nelems,
                                      reduction->operation, reduction->element, routine);
            else
                osoe_internode_broadcast(t->number, k + 1, root_index(t, root_rank, routine),
                                         accumulator, bytes, routine);
        }

        /* The final dest is written; now the release, and the copy-outs. */
        if (!osoe_epoch_release_bump_eq(t->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(t->released);
        if (members > 1)
            osoe_epoch_wait_eq(t->copied, (d + 1) * (members - 1), routine);
    } else {
        osoe_epoch_wait_eq(t->released, k + 1, routine);
        memcpy(dest, (const unsigned char *)osoe.node.heap_of[0] + dest_offset, bytes);
        if (osoe_epoch_release_bump_eq(t->copied, (d + 1) * (members - 1)))  /* stamped: as arrived */
            osoe_epoch_wake_all(t->copied);
    }

    *t->epoch = k + 1;
    *t->data_epoch = d + 1;
    atomic_store_explicit(t->published, k + 1, memory_order_release);
    osoe_blocked_leave();
    return 0;
}

int osoe_reduce(const char *routine, shmem_team_t team, void *dest, const void *source,
                size_t nelems, enum osoe_reduction_operation operation,
                struct osoe_element element)
{
    struct reduction r = { operation, element, osoe_combine_lookup(operation, element) };
    if (!r.combine)
        osoe_fatal("%s: no reduction %d over elements of kind %d and width %u", routine,
                   (int)operation, (int)element.kind, element.width);
    return data_collective(routine, team, dest, source, nelems, element.width, &r, -1);
}

int osoe_broadcast(const char *routine, shmem_team_t team, void *dest, const void *source,
                   size_t nelems, size_t width, int root)
{
    if (root < 0)
        osoe_fatal("%s: PE_root %d is not a PE number", routine, root);
    return data_collective(routine, team, dest, source, nelems, width, NULL, root);
}
