/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): §5.5's
 * accumulator, for reductions, broadcast and the gathers, and alltoall's
 * exchange.
 *
 * **Within a node** it is §5.5 as written. Every member arrives with one locked
 * increment of the team's `arrived`. The local lead - the member the
 * descriptor names (team.h): local id 0 for the predefined teams - waits for
 * all of them, then reads each member's source (only now: invariant 17, push
 * never pull) and combines it, in the descriptor's order, into its own `dest`,
 * which it alone writes before release (invariant 16). It bumps `released`;
 * every other member copies the lead's `dest` into its own and bumps `copied`;
 * the lead waits for the last copy and leaves last. `dest` is undefined until
 * the call returns, so the intermediate values the lead's `dest` holds are
 * unobservable.
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
 * (the generic one's: for a reduction or a broadcast ceil(n / slot) pieces
 * through a tree of depth ceil(log_k N), D of them in flight; for a gather
 * ceil(log2 N) steps of an allgather).
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
#include "internal-region.h"
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

/* The root's node as its position in the node set of `t` (team.h): for a
 * predefined team, spanning every node, the node's number; for a created one,
 * found in the list of its nodes' leads that its local lead keeps, which is
 * the PE asking. */
static uint32_t root_index(const struct osoe_team *t, int root_rank, const char *routine)
{
    uint32_t node = osoe_node_of(&osoe.job, (uint32_t)root_rank);
    uint32_t index = osoe_team_node_index(t, node);
    if (index == UINT32_MAX)
        osoe_fatal("%s: the root, PE %d, is on node %u, which is not among %s's %u nodes",
                   routine, root_rank, node, t->name, t->nodes);
    return index;
}

/* The local PE with local id `local`'s copy of a symmetric object, at the
 * place it has in its region on every PE: every local PE's heap and statics
 * are mapped in this process (§4.1, §4.3), this PE's own statics where its
 * program has them. */
static unsigned char *member_copy(uint32_t local, struct osoe_symmetric_location at)
{
    if (at.region == OSOE_REGION_INTERNAL)
        return (unsigned char *)osoe.node.heap_of[local] + osoe.node.internal_offset + at.offset;
    if (at.region == OSOE_REGION_STATICS)
        return osoe_node_statics_at(&osoe.node, local, local == osoe.bootstrap.local_id,
                                    at.offset);
    return (unsigned char *)osoe.node.heap_of[local] + at.offset;
}

/*
 * One data collective on `t`: a reduction if `reduction` is given, else a
 * broadcast from team rank `root`, which leaves the root's own dest alone if
 * `root_keeps_dest`. §5.5, as the file's comment walks through.
 */
static void data_collective_at(const char *routine, struct osoe_team *t, void *dest,
                               struct osoe_symmetric_location dest_at, const void *source,
                               struct osoe_symmetric_location source_at, size_t nelems,
                               size_t width, const struct reduction *reduction, int root,
                               bool root_keeps_dest);

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
    data_collective_at(routine, t, dest, dest_at, source, source_at, nelems, width, reduction,
                       root, root_keeps_dest);
    return 0;
}

/* The body of a data collective, given where dest and source are: the heap,
 * the statics, or - for the library's own words (osoe_team_broadcast_word) -
 * the internal region. */
static void data_collective_at(const char *routine, struct osoe_team *t, void *dest,
                               struct osoe_symmetric_location dest_at, const void *source,
                               struct osoe_symmetric_location source_at, size_t nelems,
                               size_t width, const struct reduction *reduction, int root,
                               bool root_keeps_dest)
{
    size_t bytes = nelems * width;
    int root_rank = root >= 0 ? osoe_team_to_world(t, root) : -1;
    /* A deprecated broadcast's root keeps its dest as it was (broadcast.c). */
    bool keep_mine = root_keeps_dest && (uint32_t)root_rank == osoe.job.me;

    uint32_t k = t->epoch, d = t->data_epoch, members = t->local_members;
    struct osoe_team_block *b = t->block;

    /* A team of one (team.h): the caller is every member, its source the
     * result, and it touches none of the node-wide counters - they belong to
     * the node's team, not to this one, as for its sync (barrier.c). */
    if (t->singleton) {
        if (source != dest && !keep_mine)
            memmove(dest, source, bytes);
        t->epoch = k + 1;
        t->data_epoch = d + 1;
        osoe_team_publish_epoch(t, k + 1);
        return;
    }

    uint32_t lead_local_id = t->lead_local_id;
    bool lead = osoe.bootstrap.local_id == lead_local_id;
    /*
     * Where this node's copy of the result is: the lead's dest - or, when a
     * deprecated broadcast's root is the lead itself, whose dest must stay as
     * it was, the lead's source, which already holds the data and which the
     * internode phase and the members' copy-outs only read.
     */
    bool lead_keeps_dest = root_keeps_dest && root_rank >= 0 &&
                           osoe_node_local_id(&osoe.node, (uint32_t)root_rank) ==
                               (int32_t)lead_local_id;
    struct osoe_symmetric_location result_at = lead_keeps_dest ? source_at : dest_at;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);

    /*
     * Arrive; the last to arrive wakes the lead, which waits for exactly that.
     * The bump is the stamping one (base/epoch.h), because the lead waits on
     * this counter: without the stamp, a sweep expiring just as a late member
     * arrives would be counted as a lost wake.
     */
    if (osoe_epoch_release_bump_eq(&b->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(&b->arrived);

    if (lead) {
        /* A reduction's internode phase is told of it before the lead waits
         * for its members, so that what the nodes below need first - the
         * generic implementation's grants, for a large one - leaves now
         * (internode.h, osoe_internode_reduce_enter). */
        if (reduction && t->internode)
            osoe_internode_reduce_enter(t->block_index, k + 1, nelems, reduction->operation,
                                        reduction->element, routine);
        osoe_epoch_wait_eq(&b->arrived, (k + 1) * members, routine);
        unsigned char *accumulator = member_copy(lead_local_id, result_at);
        if (reduction) {
            /* In place, the lead's source already is its dest. Then the other
             * members in the descriptor's order - local-id order for the
             * predefined teams, as before step 7, so a floating sum rounds as
             * it did: local_ids[0] is the lead. */
            if (source != dest)
                memcpy(accumulator, source, bytes);
            for (uint32_t m = 1; m < members; m++)
                reduction->combine(accumulator, member_copy(t->local_ids[m], source_at),
                                   nelems);
        } else if (!lead_keeps_dest) {
            int32_t root_local = osoe_node_local_id(&osoe.node, (uint32_t)root_rank);
            if (root_local >= 0 &&
                !((uint32_t)root_local == osoe.bootstrap.local_id && source == dest))
                memcpy(accumulator, member_copy((uint32_t)root_local, source_at), bytes);
        }
        if (t->internode) {
            if (reduction)
                osoe_internode_reduce(t->block_index, k + 1, accumulator, nelems,
                                      reduction->operation, reduction->element, routine);
            else
                osoe_internode_broadcast(t->block_index, k + 1, root_index(t, root_rank, routine),
                                         accumulator, bytes, routine);
        }

        /* The final dest is written; now the release, and the copy-outs. */
        if (!osoe_epoch_release_bump_eq(&b->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(&b->released);
        if (members > 1)
            osoe_epoch_wait_eq(&b->copied, (d + 1) * (members - 1), routine);
    } else {
        osoe_epoch_wait_eq(&b->released, k + 1, routine);
        if (!keep_mine)
            memcpy(dest, member_copy(lead_local_id, result_at), bytes);
        if (osoe_epoch_release_bump_eq(&b->copied, (d + 1) * (members - 1)))  /* stamped: as arrived */
            osoe_epoch_wake_all(&b->copied);
    }

    t->epoch = k + 1;
    t->data_epoch = d + 1;
    osoe_team_publish_epoch(t, k + 1);
    osoe_blocked_leave();
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

/* Spare word `which` of team `t`'s number in local PE `local`'s internal
 * region (internal-region.h), and where it is. */
static struct osoe_symmetric_location team_word_at(const struct osoe_team *t,
                                                   enum osoe_internal_team_spare which)
{
    struct osoe_symmetric_location at = {
        OSOE_REGION_INTERNAL,
        osoe_internal_team_spare_word(osoe.bootstrap.collective_fanout,
                                      osoe.bootstrap.max_teams,
                                      osoe.bootstrap.small_collective_bytes, t->number,
                                      which) };
    return at;
}

/* A split over `t`'s words (internal-region.h): its number's split lines in
 * local PE `local`'s internal region. */
static struct osoe_symmetric_location split_words_at(const struct osoe_team *t)
{
    struct osoe_symmetric_location at = {
        OSOE_REGION_INTERNAL,
        osoe_internal_team_split_offset(osoe.bootstrap.collective_fanout,
                                        osoe.bootstrap.max_teams,
                                        osoe.bootstrap.small_collective_bytes, t->number) };
    return at;
}

_Static_assert((OSOE_MAX_TEAMS_HIGHEST + 63) / 64 + 1 <=
                   OSOE_INTERNAL_SPLIT_LINES * OSOE_INTERNAL_LINE_BYTES / sizeof(uint64_t),
               "a split's lines hold the largest free mask and its marks word");

uint64_t *osoe_team_split_words(const struct osoe_team *t)
{
    return (uint64_t *)member_copy(osoe.bootstrap.local_id, split_words_at(t));
}

/*
 * The AND of every member's split words, in place, through the team number's
 * own lines of the internal region: a reduction that borrows nothing from the
 * heap, so that it stays symmetric when another thread allocates meanwhile
 * (phase 1 step 8 slice 2), and that a split over another parent on another
 * thread never shares, its parent's number being another (team.c, D7). Until
 * the team redesign's step D a broadcast of the split's first number took its
 * place, from a spare word of the release counter's line.
 */
void osoe_team_split_and(const char *routine, struct osoe_team *t, uint32_t count)
{
    struct reduction r = { OSOE_REDUCTION_AND,
                           { OSOE_ELEMENT_UNSIGNED_INTEGER, sizeof(uint64_t) }, NULL };
    r.combine = osoe_combine_lookup(r.operation, r.element);
    if (!r.combine)
        osoe_fatal("%s: no AND over eight-byte unsigned words", routine);
    struct osoe_symmetric_location at = split_words_at(t);
    uint64_t *words = (uint64_t *)member_copy(osoe.bootstrap.local_id, at);
    data_collective_at(routine, t, words, at, words, at, count, sizeof *words, &r, -1, false);
}

/*
 * collect's lengths to places, in place: word r becomes the sum of the words
 * before it, and word `count` - past the lengths - their sum.
 */
static void prefix_sums(const char *routine, uint64_t *words, uint64_t count)
{
    uint64_t sum = 0;
    for (uint64_t r = 0; r < count; r++) {
        uint64_t length = words[r];
        words[r] = sum;
        if (length > UINT64_MAX - sum)
            osoe_fatal("%s: the members' blocks add up to more bytes than there are", routine);
        sum += length;
    }
    words[count] = sum;
}

/*
 * A gather (spec §9.10.8): member r's block into every member's dest at
 * places[r] - or, with places NULL, at r * block, every block `block` bytes -
 * `total` bytes in all. This PE's own block is `block` bytes of source. Only
 * the lead reads `places`, and not all of them: at each of its own members'
 * ranks and the rank after it, and in the internode phase at each of its
 * intervals' ends (internode-generic.c) - every one of which collect fills
 * where it takes each node's total (collect_varying).
 *
 * The lead copies each local member's block to its place once every member has
 * arrived - O(L n) of its own, under the O(P n) each member spends copying the
 * result out, so not the node's bottleneck - and hands dest to the internode
 * phase, which returns every node's blocks (internode.h); then the release and
 * the copy-outs, as for a broadcast. Only the lead writes its dest, and only
 * once every member has arrived, which each does after its copy-out of the
 * collective before: no member writes memory another may still be reading.
 * The internode phase writes the other nodes' blocks into it - the generic
 * one by puts from the other nodes' leads - but only once the lead has handed
 * it over, since the lead leaves last, after every copy-out of the collective
 * before (§5.5, invariant 16; internode.h).
 *
 * With `lengths` it is collect's first gather (collect_varying): each block is
 * a member's length, eight bytes, and the lead turns the result into their
 * prefix sums in place - each member's place, and the total in the word after
 * the result - of which the other members copy the total alone. They need
 * nothing else, so the P words are the lead's to touch, not every member's.
 */
static int gather(const char *routine, struct osoe_team *t, void *dest, const void *source,
                  size_t block, const uint64_t *places, uint64_t total, bool lengths)
{
    if (total == 0)
        return 0;
    struct osoe_symmetric_location dest_at = osoe_symmetric_locate(routine, dest, total);
    struct osoe_symmetric_location source_at = osoe_symmetric_locate(routine, source, block);
    /*
     * A dest in the heap shorter than the result is named before anything is
     * written past it: the allocator knows where its block ends (phase 1 step
     * 8 slice 2). SOS's shmem_team_b2b_collectives collects 40 longs into a
     * dest of 10. Until the places left the heap, the lead's copy ran on into
     * the room that held them and a check of their order caught it by luck;
     * this asks directly, on every member, for its own dest. The statics'
     * sizes are the program's and not known here.
     */
    uint64_t room;
    if (dest_at.region == OSOE_REGION_HEAP &&
        osoe_allocator_room(&osoe.allocator, dest_at.offset, &room) == 0 && room < total)
        osoe_fatal("%s: dest has room for %llu bytes to the end of its allocation, and the "
                   "result is %llu bytes (spec §9.10.8: dest must hold every member's block)",
                   routine, (unsigned long long)room, (unsigned long long)total);
    uint32_t k = t->epoch, d = t->data_epoch, members = t->local_members;
    struct osoe_team_block *b = t->block;

    if (t->singleton) {
        memcpy(dest, source, block);
        if (lengths)
            prefix_sums(routine, dest, 1);
        t->epoch = k + 1;
        t->data_epoch = d + 1;
        osoe_team_publish_epoch(t, k + 1);
        return 0;
    }

    bool lead = osoe.bootstrap.local_id == t->lead_local_id;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);
    if (osoe_epoch_release_bump_eq(&b->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(&b->arrived);

    if (lead) {
        osoe_epoch_wait_eq(&b->arrived, (k + 1) * members, routine);
        unsigned char *result = dest;
        /* Each member's block at its team rank's place: member m is local id
         * local_ids[m] with rank local_ranks[m], and m = 0 is this lead. */
        for (uint32_t m = 0; m < members; m++) {
            uint32_t r = t->local_ranks[m];
            uint64_t place = places ? places[r] : (uint64_t)r * block;
            uint64_t length = places ? places[r + 1] - places[r] : block;
            if (length)
                memcpy(result + place,
                       m == 0 ? source : member_copy(t->local_ids[m], source_at), length);
        }
        if (t->internode)
            osoe_internode_collect(t->block_index, k + 1, result, total, block, places, routine);
        if (lengths)
            prefix_sums(routine, dest, t->members);
        if (!osoe_epoch_release_bump_eq(&b->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(&b->released);
        if (members > 1)
            osoe_epoch_wait_eq(&b->copied, (d + 1) * (members - 1), routine);
    } else {
        osoe_epoch_wait_eq(&b->released, k + 1, routine);
        unsigned char *lead_dest = member_copy(t->lead_local_id, dest_at);
        if (lengths)
            memcpy((unsigned char *)dest + total, lead_dest + total, sizeof(uint64_t));
        else
            memcpy(dest, lead_dest, total);
        if (osoe_epoch_release_bump_eq(&b->copied, (d + 1) * (members - 1)))
            osoe_epoch_wake_all(&b->copied);
    }

    t->epoch = k + 1;
    t->data_epoch = d + 1;
    osoe_team_publish_epoch(t, k + 1);
    osoe_blocked_leave();
    return 0;
}

/*
 * collect's places from each node's total (collect_varying's comment), on the
 * lead: `places` holds this node's members' lengths at their ranks. Their sum,
 * this node's total, goes at its position in a vector of the team's nodes,
 * zero elsewhere, which the internode reduction sums between the nodes; then,
 * the nodes taken in team-rank order (`runs`), each node's base - the totals
 * before it - at the first rank of its run, and for this node its members'
 * places from its base, the total at `members`. A sum past what eight bytes
 * count is refused, as prefix_sums refuses it. A sum at this node's position
 * other than its own total would be leads disagreeing about the team's nodes:
 * a broken invariant, named.
 */
static void places_from_node_totals(const char *routine, struct osoe_team *t, uint32_t epoch,
                                    const struct osoe_team_node_run *runs, uint64_t *places,
                                    struct osoe_element length)
{
    uint32_t nodes = t->nodes, own = t->own_node_run;
    uint32_t first = runs[own].first_rank, end = first + t->local_members;
    uint64_t *totals = calloc(nodes, sizeof *totals);
    if (!totals)
        osoe_fatal("%s: out of memory for the totals of %s's %u nodes", routine, t->name, nodes);
    uint64_t own_total = 0;
    for (uint32_t r = first; r < end; r++) {
        if (places[r] > UINT64_MAX - own_total)
            osoe_fatal("%s: the members' blocks add up to more bytes than there are", routine);
        own_total += places[r];
    }
    totals[runs[own].position] = own_total;
    osoe_internode_reduce(t->block_index, epoch, totals, nodes, OSOE_REDUCTION_SUM, length,
                          routine);
    if (totals[runs[own].position] != own_total)
        osoe_fatal("%s: %s's nodes' totals hold %llu bytes at this node's position, %u, where "
                   "its members here give %llu: the leads disagree about the team's nodes",
                   routine, t->name, (unsigned long long)totals[runs[own].position],
                   runs[own].position, (unsigned long long)own_total);
    uint64_t base = 0;
    for (uint32_t j = 0; j < nodes; j++) {
        uint64_t node_total = totals[runs[j].position];
        if (node_total > UINT64_MAX - base)
            osoe_fatal("%s: the members' blocks add up to more bytes than there are", routine);
        if (j == own) {
            uint64_t at = base;
            for (uint32_t r = first; r < end; r++) {
                uint64_t member_length = places[r];
                places[r] = at;
                at += member_length;
            }
        } else {
            places[runs[j].first_rank] = base;
        }
        base += node_total;
    }
    places[t->members] = base;
    free(totals);
}

/*
 * collect's places (phase 1 step 8 slice 2, the author's C4,
 * docs/prompts/done/phase1-step8-slice2-prompt.md; and his A11 of 2026-10-11,
 * P29 of docs/prompts/v0.2-open-items.md). They need not be symmetric - only
 * the lead reads them - so nothing is borrowed from the heap: each member
 * leaves its length in its own word of the team number's line
 * (internal-region.h), and the lead finds the places it reads in a vector of
 * the team's size plus one in its own memory, the last word the total, which
 * it leaves in its own word for its members to read after the release. Then
 * the gather itself. The places come one of two ways:
 *
 *   - **each node's total**, for a team spanning nodes each of which holds one
 *     run of consecutive team ranks - every team strided in world ranks over a
 *     block distribution: the internode reduction sums a vector of the team's
 *     nodes, each lead's own total at its node's position, and each node's base
 *     is the totals of the nodes before it in team-rank order - the node set's
 *     order under a positive stride and its reverse under a negative one, the
 *     set being sorted by node (places_from_node_totals). The lead fills the
 *     places of every run's first rank, of its own members, and the total at
 *     `members`: all the gather reads (gather), since an interval of the
 *     allgather is consecutive ranks on one node merged with the next where
 *     they touch, and so starts and ends where a node's run does. Eight bytes
 *     a node, at each level of the reduction's tree;
 *   - **the members' vector**, for a team on one node and one where some
 *     node's members are not one run - a cyclic or plane distribution's, whose
 *     intervals end among a node's members: the lead sets its members' lengths
 *     at their ranks in a vector of the team's size, the reduction sums the
 *     nodes' vectors into every lead's, and the lead turns the result into
 *     every member's place (prefix_sums). Eight bytes a member, at each level
 *     of the tree, in pieces.
 *
 * Which way a team takes is found once, at its first such collect, by its lead
 * on each node (team.h, osoe_team_node_runs), from what every PE holds alike -
 * the team's ranks and the job's rank map - so every lead takes the same one.
 * Were two ever to differ, their reductions would differ in size, which the
 * reduction's checks name where its pieces meet (internode-generic.c): a
 * piece's header carries its bytes, and a large one's grant its epoch,
 * identity and bytes - but for a lead that takes as small what its child takes
 * as large, whose child waits for a grant until the deadlock detector names the
 * hang (shmem.lanes.class-split-hang). The lead publishes how many lengths it
 * summed, a node's or a member's each (team.tree.collect_lengths, node.h). The
 * data keeps the allgather either way.
 */
static int collect_varying(const char *routine, struct osoe_team *t, void *dest,
                           const void *source, size_t block)
{
    uint64_t members = t->members;
    if (t->singleton)
        return gather(routine, t, dest, source, block, NULL, block, false);

    struct osoe_symmetric_location length_at = team_word_at(t, OSOE_INTERNAL_COLLECT_LENGTH);
    struct osoe_symmetric_location total_at = team_word_at(t, OSOE_INTERNAL_COLLECT_TOTAL);
    uint32_t lead_local_id = t->lead_local_id;
    bool lead = osoe.bootstrap.local_id == lead_local_id;
    uint32_t k = t->epoch, local = t->local_members;
    struct osoe_team_block *b = t->block;
    *(uint64_t *)member_copy(osoe.bootstrap.local_id, length_at) = block;

    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);
    if (osoe_epoch_release_bump_eq(&b->arrived, (k + 1) * local) && !lead)
        osoe_epoch_wake_all(&b->arrived);
    uint64_t *places = NULL, total;
    struct osoe_element length = { OSOE_ELEMENT_UNSIGNED_INTEGER, sizeof(uint64_t) };
    if (lead) {
        /* Each node's total where every node's members are one run, else the
         * members' vector (the comment above), the reduction entered before
         * the members' wait as every reduction is (data_collective_at): eight
         * bytes a length, so more than OSOE_SMALL_COLLECTIVE_BYTES / 8 of them
         * are summed as a large reduction, whose children wait for its grant. */
        const struct osoe_team_node_run *runs =
            t->internode ? osoe_team_node_runs(t, routine) : NULL;
        uint64_t summed = runs ? t->nodes : members;
        if (t->internode)
            osoe_internode_reduce_enter(t->block_index, k + 1, summed, OSOE_REDUCTION_SUM,
                                        length, routine);
        osoe_epoch_wait_eq(&b->arrived, (k + 1) * local, routine);
        places = calloc(members + 1, sizeof *places);
        if (!places)
            osoe_fatal("%s: out of memory for the places of %llu members", routine,
                       (unsigned long long)members);
        for (uint32_t m = 0; m < local; m++)
            places[t->local_ranks[m]] =
                *(const uint64_t *)member_copy(t->local_ids[m], length_at);
        if (runs) {
            places_from_node_totals(routine, t, k + 1, runs, places, length);
        } else {
            if (t->internode)
                osoe_internode_reduce(t->block_index, k + 1, places, members,
                                      OSOE_REDUCTION_SUM, length, routine);
            prefix_sums(routine, places, members);
        }
        /* Single-writer publication for osoe-stat and the tests (§3.16), before
         * the release, as the internode phase publishes its counts. */
        if (t->internode)
            atomic_store_explicit(&b->tree.collect_lengths, (uint32_t)summed,
                                  memory_order_release);
        total = places[members];
        *(uint64_t *)member_copy(lead_local_id, total_at) = total;
        if (!osoe_epoch_release_bump_eq(&b->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(&b->released);
    } else {
        osoe_epoch_wait_eq(&b->released, k + 1, routine);
        /* The lead writes it again only in the team's next collect, once every
         * member has arrived at the gather below, after this read. */
        total = *(const uint64_t *)member_copy(lead_local_id, total_at);
    }
    t->epoch = k + 1;
    osoe_team_publish_epoch(t, k + 1);
    osoe_blocked_leave();
    int status = gather(routine, t, dest, source, block, places, total, false);
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
    return gather(routine, t, dest, source, block, NULL, (uint64_t)t->members * block, false);
}

/*
 * alltoall and alltoalls: each block goes straight from its source PE to its
 * dest PE - no tree shortens that - and the team's barrier after the puts
 * returns once every block addressed to this PE has landed (spec §9.10.5).
 * alltoall itself reads no dest, so the precondition that dest is ready on
 * every PE is the application's to keep, as the spec says - including after a
 * data collective into the same array, whose lead's copy may still be being
 * copied out of when a fast member's put lands in it: a gather needs no sync
 * between, alltoall does. A block with both strides 1 is one put; a strided
 * one, one put an element: element-strided within each block, as the spec's
 * Example 33 and SOS read §9.10.6 - member i's block for member j is elements
 * (j nelems + e) sst of source, landing at (i nelems + e) dst of j's dest.
 * Every put but the last says more follows (OSOE_TRANSPORT_MORE), so a
 * backend may submit them together. The PEs are visited from this one's right
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
        bool last_block = step + 1 == members;
        if (dest_stride == 1 && source_stride == 1) {
            osoe_transport_put(context, pe, dest_at.region, to, from, block,
                               last_block ? 0 : OSOE_TRANSPORT_MORE);
            continue;
        }
        for (size_t e = 0; e < nelems; e++)
            osoe_transport_put(context, pe, dest_at.region,
                               to + e * (uint64_t)dest_stride * width,
                               from + e * (uint64_t)source_stride * width, width,
                               last_block && e + 1 == nelems ? 0 : OSOE_TRANSPORT_MORE);
    }
    osoe_team_barrier(t, routine);
    return 0;
}

/*
 * The prefix scans (spec §9.10.10; phase 1 step 10 slice 1). A team spanning
 * nodes hands the whole scan to internode.h, where every member runs its place
 * in the scan's tree (osoe_internode_scan), and takes no epoch: the node's
 * counters would only add an intranode barrier to a collective whose members
 * each wait for their own inputs, so osoe-stat's published epoch does not
 * count such a scan. A team on one node is its local lead's:
 * every member arrives, the lead sums the members' sources in team rank order
 * into each one's dest - reading a member's source before writing its dest,
 * so the two may be one array, and writing a dest only once its owner has
 * arrived, which the spec allows (every dest is ready before any PE calls) -
 * and releases them, a chunk of elements at a time so that its own memory
 * stays small. O(L n) on the lead; no copy-out, so no `copied`.
 * Floating results depend on the order of the additions, and so on the layout
 * (design §5.8): conforming, since the spec defines the sum mathematically.
 */
int osoe_scan(const char *routine, shmem_team_t team, void *dest, const void *source,
              size_t nelems, struct osoe_element element, bool inclusive)
{
    struct osoe_team *t = osoe_team_of(routine, team);
    if (!t)
        return 1;                       /* SHMEM_TEAM_INVALID: undefined, answered nonzero */
    osoe_combine_function add = osoe_combine_lookup(OSOE_REDUCTION_SUM, element);
    if (!add)
        osoe_fatal("%s: no sum over elements of kind %d and width %u", routine,
                   (int)element.kind, element.width);
    size_t bytes = osoe_rma_bytes(routine, nelems, element.width);
    if (bytes == 0)
        return 0;
    struct osoe_symmetric_location dest_at = osoe_symmetric_locate(routine, dest, bytes);
    struct osoe_symmetric_location source_at = osoe_symmetric_locate(routine, source, bytes);

    if (t->internode) {
        osoe_internode_scan(t->block_index, t->me, dest, source, nelems, element, inclusive, routine);
        return 0;
    }

    uint32_t k = t->epoch, members = t->local_members;
    if (t->singleton) {
        if (!inclusive)
            memset(dest, 0, bytes);
        else if (source != dest)
            memmove(dest, source, bytes);
        t->epoch = k + 1;
        osoe_team_publish_epoch(t, k + 1);
        return 0;
    }

    struct osoe_team_block *b = t->block;
    bool lead = osoe.bootstrap.local_id == t->lead_local_id;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);
    if (osoe_epoch_release_bump_eq(&b->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(&b->arrived);
    if (lead) {
        osoe_epoch_wait_eq(&b->arrived, (k + 1) * members, routine);
        /* The members in team rank order: every one-node team lists its
         * members ascending today (team.h), and the sort keeps this from
         * depending on it - O(L^2) at worst, once a call. */
        uint32_t *order = malloc((size_t)members * sizeof *order);
        /* A running sum and a member's source, a chunk at a time, so that the
         * lead's memory does not grow with the scan (the review of the slice). */
        enum { CHUNK = 16 << 10 };
        size_t per_chunk = CHUNK / element.width;
        size_t chunk_bytes = (nelems < per_chunk ? nelems : per_chunk) * element.width;
        unsigned char *running = malloc(chunk_bytes), *mine = malloc(chunk_bytes);
        if (!order || !running || !mine)
            osoe_fatal("%s: out of memory for a scan", routine);
        for (uint32_t m = 0; m < members; m++) {
            uint32_t at = m;
            while (at > 0 && t->local_ranks[order[at - 1]] > t->local_ranks[m]) {
                order[at] = order[at - 1];
                at--;
            }
            order[at] = m;
        }
        for (size_t done = 0; done < nelems;) {
            size_t n = nelems - done < per_chunk ? nelems - done : per_chunk;
            size_t at = done * element.width, length = n * element.width;
            memset(running, 0, length);
            for (uint32_t i = 0; i < members; i++) {
                uint32_t id = t->local_ids[order[i]];
                memcpy(mine, member_copy(id, source_at) + at, length);
                unsigned char *out = member_copy(id, dest_at) + at;
                if (inclusive) {
                    add(running, mine, n);
                    memcpy(out, running, length);
                } else {
                    memcpy(out, running, length);
                    add(running, mine, n);
                }
            }
            done += n;
        }
        free(order);
        free(running);
        free(mine);
        if (!osoe_epoch_release_bump_eq(&b->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(&b->released);
    } else {
        osoe_epoch_wait_eq(&b->released, k + 1, routine);
    }
    t->epoch = k + 1;
    osoe_team_publish_epoch(t, k + 1);
    osoe_blocked_leave();
    return 0;
}

