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
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "allocator.h"
#include "barrier.h"
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
#include "transport.h"

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

/* Local PE m's copy of a symmetric object, at the place it has in its region
 * on every PE: every local PE's heap and statics are mapped in this process
 * (§4.1, §4.3), this PE's own statics where its program has them. */
static unsigned char *member_copy(uint32_t m, struct osoe_symmetric_location at)
{
    if (at.region == OSOE_REGION_STATICS)
        return osoe_node_statics_at(&osoe.node, m, m == osoe.bootstrap.local_id, at.offset);
    return (unsigned char *)osoe.node.heap_of[m] + at.offset;
}

/*
 * One data collective on `t`: a reduction if `reduction` is given, else a
 * broadcast from team rank `root`, which leaves the root's own dest alone if
 * `root_keeps_dest`. §5.5, as the file's comment walks through.
 */
static int data_collective(const char *routine, shmem_team_t team, void *dest,
                           const void *source, size_t nelems, size_t width,
                           const struct reduction *reduction, int root, bool root_keeps_dest)
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
    struct osoe_symmetric_location dest_at = osoe_symmetric_locate(routine, dest, bytes);
    struct osoe_symmetric_location source_at = osoe_symmetric_locate(routine, source, bytes);
    int root_rank = root >= 0 ? osoe_team_to_world(t, root) : -1;
    /* A deprecated broadcast's root keeps its dest as it was (broadcast.c). */
    bool keep_mine = root_keeps_dest && (uint32_t)root_rank == osoe.job.me;

    uint32_t k = *t->epoch, d = *t->data_epoch, members = t->local_members;

    /* A team of one (team.h): the caller is every member, its source the
     * result, and it touches none of the node-wide counters - they belong to
     * the node's team, not to this one, as for its sync (barrier.c). */
    if (t->singleton) {
        if (source != dest && !keep_mine)
            memmove(dest, source, bytes);
        *t->epoch = k + 1;
        *t->data_epoch = d + 1;
        atomic_store_explicit(t->published, k + 1, memory_order_release);
        return 0;
    }

    bool lead = osoe.bootstrap.local_id == 0;
    /*
     * Where this node's copy of the result is: the lead's dest - or, when a
     * deprecated broadcast's root is the lead itself, whose dest must stay as
     * it was, the lead's source, which already holds the data and which the
     * internode phase and the members' copy-outs only read.
     */
    bool lead_keeps_dest = root_keeps_dest && root_rank >= 0 &&
                           osoe_node_local_id(&osoe.node, (uint32_t)root_rank) == 0;
    struct osoe_symmetric_location result_at = lead_keeps_dest ? source_at : dest_at;
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
        unsigned char *accumulator = member_copy(0, result_at);
        if (reduction) {
            /* In place, the lead's source already is its dest. */
            if (source != dest)
                memcpy(accumulator, source, bytes);
            for (uint32_t m = 1; m < members; m++)
                reduction->combine(accumulator, member_copy(m, source_at), nelems);
        } else if (!lead_keeps_dest) {
            int32_t root_local = osoe_node_local_id(&osoe.node, (uint32_t)root_rank);
            if (root_local >= 0 &&
                !((uint32_t)root_local == osoe.bootstrap.local_id && source == dest))
                memcpy(accumulator, member_copy((uint32_t)root_local, source_at), bytes);
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
        if (!keep_mine)
            memcpy(dest, member_copy(0, result_at), bytes);
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
    return data_collective(routine, team, dest, source, nelems, element.width, &r, -1, false);
}

int osoe_broadcast(const char *routine, shmem_team_t team, void *dest, const void *source,
                   size_t nelems, size_t width, int root, bool root_keeps_dest)
{
    if (root < 0)
        osoe_fatal("%s: PE_root %d is not a PE number", routine, root);
    return data_collective(routine, team, dest, source, nelems, width, NULL, root,
                           root_keeps_dest);
}

/* Local PE m's number in `t`, a team every local PE belongs to: the world's
 * numbers PEs by rank, the shared team's by local id (team.c). */
static uint32_t local_member_rank(const struct osoe_team *t, uint32_t m)
{
    return t->number == OSOE_TEAM_SHARED ? m : osoe.bootstrap.gtids[m];
}

/*
 * A gather (spec §9.10.8): member r's block into every member's dest at
 * places[r] - or, with places NULL, at r * block, every block `block` bytes -
 * `total` bytes in all. This PE's own block is `block` bytes of source.
 *
 * The lead zeroes its dest and copies each local member's block to its place:
 * O(L n) of its own, under the O(P n) each member spends copying the result
 * out, so not the node's bottleneck. Then the internode phase - on entry this
 * node's blocks and zero elsewhere, on return every node's (internode.h) - and
 * the release and the copy-outs, as for a broadcast. Only the lead writes its
 * dest, and only once every member has arrived, which each does after its
 * copy-out of the collective before: no member writes memory another may still
 * be reading, and nothing is written in another node but its internal region
 * (§5.5, invariant 16).
 */
static int gather(const char *routine, struct osoe_team *t, void *dest, const void *source,
                  size_t block, const uint64_t *places, uint64_t total)
{
    if (total == 0)
        return 0;
    struct osoe_symmetric_location dest_at = osoe_symmetric_locate(routine, dest, total);
    struct osoe_symmetric_location source_at = osoe_symmetric_locate(routine, source, block);
    uint32_t k = *t->epoch, d = *t->data_epoch, members = t->local_members;

    if (t->singleton) {
        memcpy(dest, source, block);
        *t->epoch = k + 1;
        *t->data_epoch = d + 1;
        atomic_store_explicit(t->published, k + 1, memory_order_release);
        return 0;
    }

    bool lead = osoe.bootstrap.local_id == 0;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);
    if (osoe_epoch_release_bump_eq(t->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(t->arrived);

    if (lead) {
        osoe_epoch_wait_eq(t->arrived, (k + 1) * members, routine);
        unsigned char *result = dest;
        /* Every place is some member's: on one node the blocks cover the
         * result, and across nodes the others' places start as zeros. */
        if (t->internode)
            memset(result, 0, total);
        for (uint32_t m = 0; m < members; m++) {
            uint32_t r = local_member_rank(t, m);
            uint64_t place = places ? places[r] : (uint64_t)r * block;
            uint64_t length = places ? places[r + 1] - places[r] : block;
            if (length)
                memcpy(result + place, m == 0 ? source : member_copy(m, source_at), length);
        }
        if (t->internode)
            osoe_internode_collect(t->number, k + 1, result, total, routine);
        if (!osoe_epoch_release_bump_eq(t->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(t->released);
        if (members > 1)
            osoe_epoch_wait_eq(t->copied, (d + 1) * (members - 1), routine);
    } else {
        osoe_epoch_wait_eq(t->released, k + 1, routine);
        memcpy(dest, member_copy(0, dest_at), total);
        if (osoe_epoch_release_bump_eq(t->copied, (d + 1) * (members - 1)))
            osoe_epoch_wake_all(t->copied);
    }

    *t->epoch = k + 1;
    *t->data_epoch = d + 1;
    atomic_store_explicit(t->published, k + 1, memory_order_release);
    osoe_blocked_leave();
    return 0;
}

/*
 * collect's places are the prefix sums of every member's length, which every
 * PE learns by an fcollect of them first - eight bytes a member - into room the
 * symmetric heap lends for the call. Every member of the team takes the same
 * room and gives it back before returning; the allocator is a function of its
 * calls alone (§4.4), so a take and its give leave it as it was, on the PEs of
 * a team smaller than the job as on the others.
 */
static int collect_varying(const char *routine, struct osoe_team *t, void *dest,
                           const void *source, size_t block)
{
    uint64_t members = t->members;
    uint64_t room = (members + 1) * sizeof(uint64_t), offset;
    if (osoe_allocator_take(&osoe.allocator, room, sizeof(uint64_t), 0, &offset) != 0)
        osoe_fatal("%s: the symmetric heap has no %llu bytes free for the %llu members' "
                   "lengths collect gathers first; raise SHMEM_SYMMETRIC_SIZE", routine,
                   (unsigned long long)room, (unsigned long long)members);
    uint64_t *lengths = (uint64_t *)((unsigned char *)osoe.node.heap_of[osoe.bootstrap.local_id] +
                                     offset);
    uint64_t *mine = lengths + members;
    *mine = block;
    int status = gather(routine, t, lengths, mine, sizeof *mine, NULL, members * sizeof *mine);

    uint64_t *places = malloc((members + 1) * sizeof *places);
    if (!places)
        osoe_fatal("%s: out of memory for %llu members' places", routine,
                   (unsigned long long)members);
    places[0] = 0;
    for (uint64_t r = 0; r < members; r++) {
        if (lengths[r] > UINT64_MAX - places[r])
            osoe_fatal("%s: the members' blocks add up to more bytes than there are", routine);
        places[r + 1] = places[r] + lengths[r];
    }
    osoe_allocator_give(&osoe.allocator, offset);
    if (status == 0)
        status = gather(routine, t, dest, source, block, places, places[members]);
    free(places);
    return status;
}

int osoe_collect(const char *routine, shmem_team_t team, void *dest, const void *source,
                 size_t nelems, size_t width, bool fixed)
{
    struct osoe_team *t = osoe_team_of(routine, team);
    if (!t)
        return 1;                       /* SHMEM_TEAM_INVALID: undefined, answered nonzero */
    size_t block = osoe_rma_bytes(routine, nelems, width);
    if (!fixed)
        return collect_varying(routine, t, dest, source, block);
    if (block == 0)
        return 0;                       /* every member passes the same count */
    if (t->members > UINT64_MAX / block)
        osoe_fatal("%s: %u members' blocks of %zu bytes are more bytes than there are", routine,
                   t->members, block);
    return gather(routine, t, dest, source, block, NULL, (uint64_t)t->members * block);
}

/*
 * alltoall and alltoalls: each block goes straight from its source PE to its
 * dest PE - no tree shortens that - and the team's barrier after the puts
 * returns once every block addressed to this PE has landed (spec §9.10.5).
 * Nothing of the library's reads dest, so the precondition that it is ready on
 * every PE is the application's to keep, as the spec says. A block with both
 * strides 1 is one put; a strided one, one put an element (§9.10.6: member i's
 * block for member j is elements (j nelems + e) sst of source, landing at
 * (i nelems + e) dst of j's dest). The PEs are visited from this one's right
 * neighbour round, so that no PE is every PE's first target.
 */
int osoe_alltoall(const char *routine, shmem_team_t team, void *dest, const void *source,
                  ptrdiff_t dest_stride, ptrdiff_t source_stride, size_t nelems, size_t width)
{
    struct osoe_team *t = osoe_team_of(routine, team);
    if (!t)
        return 1;
    if (dest_stride < 1 || source_stride < 1)
        osoe_fatal("%s: strides %td and %td; both must be at least 1 (spec §9.10.6)", routine,
                   dest_stride, source_stride);
    size_t block = osoe_rma_bytes(routine, nelems, width);
    if (block == 0)
        return 0;
    uint64_t members = t->members;
    uint64_t stride = (uint64_t)(dest_stride > source_stride ? dest_stride : source_stride);
    /* The last element of the last block bounds what each array spans. */
    uint64_t last = members * nelems - 1;
    if (members > UINT64_MAX / nelems || last > (UINT64_MAX / width - 1) / stride)
        osoe_fatal("%s: the blocks reach past what an offset can count", routine);
    struct osoe_symmetric_location dest_at =
        osoe_symmetric_locate(routine, dest, (last * (uint64_t)dest_stride + 1) * width);
    osoe_symmetric_locate(routine, source, (last * (uint64_t)source_stride + 1) * width);

    struct osoe_transport_context *context = osoe_transport_context_default();
    uint64_t me = t->me;
    for (uint64_t step = 0; step < members; step++) {
        uint64_t j = (me + 1 + step) % members;
        int pe = osoe_team_to_world(t, (int)j);
        const unsigned char *from = (const unsigned char *)source +
                                    j * nelems * (uint64_t)source_stride * width;
        uint64_t to = dest_at.offset + me * nelems * (uint64_t)dest_stride * width;
        if (dest_stride == 1 && source_stride == 1) {
            osoe_transport_put(context, pe, dest_at.region, to, from, block, 0);
            continue;
        }
        for (size_t e = 0; e < nelems; e++)
            osoe_transport_put(context, pe, dest_at.region,
                               to + e * (uint64_t)dest_stride * width,
                               from + e * (uint64_t)source_stride * width, width, 0);
    }
    osoe_team_barrier(t, routine);
    return 0;
}

void osoe_active_set_whole_job(const char *routine, int PE_start, int logPE_stride,
                               int PE_size)
{
    /* Before or after the library's life the job's size is 0, and "not the
     * whole job of 0 PEs" would be a wrong reason for the right refusal. */
    osoe_require_initialized(routine);
    int npes = (int)osoe.bootstrap.world_size;
    if (PE_start == 0 && PE_size == npes && logPE_stride >= 0 &&
        (logPE_stride == 0 || PE_size == 1))
        return;
    osoe_fatal("%s: the active set (PE_start %d, logPE_stride %d, PE_size %d) is not the "
               "whole job of %d PEs, and osoe runs the deprecated active-set routines over "
               "the whole job only (design §7.4)", routine, PE_start, logPE_stride, PE_size,
               npes);
}
