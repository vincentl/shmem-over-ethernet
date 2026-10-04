/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Teams and contexts (§7.3; spec §9.4, §9.5).
 *
 * Phase 1 step 1 has two predefined teams and one context. SHMEM_TEAM_WORLD is
 * every PE of the job; SHMEM_TEAM_SHARED is every PE whose memory this one can
 * reach with loads and stores (spec §7), which is the node. On one node those
 * are the same *membership* - and they are still two teams, with their own
 * epoch counters, because the spec makes them distinct predefined teams forming
 * separate atomicity domains, and because the moment a second node exists they
 * stop agreeing. Implementing SHARED as an alias for WORLD would pass every
 * test that can be written today and be wrong on the first multi-node job.
 *
 * A team's state is found by its number on each PE (phase 1 step 7; the team
 * redesign's step D, docs/design/team-resources-proposal.md §3): its
 * published epoch in the PE's row and its block of the PE's internal region;
 * and on each node by its *name*, the local id of its local lead there and its
 * number, which the node's map turns into the team's block of the node's pool
 * - §5.4's counters, the arrival handoff, and the record of the team's place
 * on this node that the progress thread reads (node.h). What the descriptor
 * holds is the team's shape: this node's members and which of them leads, and
 * its ranks as world ranks (team.h).
 *
 * **A number is each PE's own** (D1): no PE holds two live teams under one
 * number, and two teams with no PE in common may hold the same one, on one
 * node or on two. A PE takes a number when a split creates a team it is in,
 * and gives it back when it destroys the team. OSOE_MAX_TEAMS is how many a
 * PE may hold at once, the three predefined included (D8).
 *
 * **Teams are created by splitting a parent** (spec §9.4.6, §9.4.7; phase 1
 * step 7 slice 2, docs/prompts/done/phase1-step7-slice2-prompt.md; step D,
 * D2), collectively over the parent, in this order:
 *
 *   1. every parent PE checks the arguments alike: the spec's failures return
 *      nonzero on every one of them, and what it leaves undefined ends the job
 *      by name;
 *   2. every member builds its descriptor but for the number: its node's
 *      members in O(L), and from them whether the team spans nodes, O(1); and
 *      each node's local lead of the team - the one PE per node that needs it
 *      - the team's node set, O(S) node lookups for a team of S members whose
 *      walk meets the nodes in order and O(S log S) otherwise, O(S) memory
 *      either way: the one step that is not logarithmic, taken at creation
 *      only. The local lead claims its node's block for the team from the
 *      pool (node.h) - none of which needs the number;
 *   3. one AND reduction over the parent, in place, in the parent number's
 *      split words of the internal region (internal-region.h): every member
 *      contributes its *free mask* - bit t set when t is not predefined, this
 *      PE holds no team under t, and no block is mapped under (this PE, t) on
 *      its node, which a lead clears as it leaves its team there (node.h) -
 *      and every other parent PE all ones; and a word of marks, written
 *      inverted so that the one AND serves, a bit set saying "no complaint
 *      here": a block claimed, and the pool not found empty while a block may
 *      be about to come back - in the split's first round, or with a block on
 *      its way back (split) - and this PE below SHMEM_THREAD_MULTIPLE;
 *   4. every parent PE reads the same result: split_strided takes the lowest
 *      bit set, split_2d the lowest for every row and the next for every
 *      column - rows share no PE, nor columns, and each PE is in one of each.
 *      A round some PE was busy in is run again by every parent PE; a round
 *      with a dry pool, or with no bit left, fails the split alike on every
 *      parent PE, said by the PEs whose node is dry - naming OSOE_TEAM_BLOCKS
 *      and the node - or once by the parent's rank 0 - naming OSOE_MAX_TEAMS,
 *      the teams one PE may be in at once; a block claimed for it is returned.
 *      When any parent PE has SHMEM_THREAD_MULTIPLE, which the marks say, each
 *      member then takes its numbers pending, and a second AND says whether
 *      all did (below);
 *   5. each member takes the number - its descriptor's slot, its counters in
 *      the internal region zeroed (internode.h) - and each node's local lead
 *      of the new team names its block, writes the team's record into it and
 *      maps the block under the team's name (node.h, where the argument that
 *      every reader sees them is written);
 *   6. a sync over the parent, so that no collective on the new team starts
 *      anywhere before every member node's record is published and every
 *      member's counters are zero;
 *   7. members get the handle and the others SHMEM_TEAM_INVALID, every parent
 *      PE the same status.
 *
 * **SHMEM_THREAD_MULTIPLE** (D7; tools/modelcheck/split_numbers_model.py): two
 * threads of one PE may be in two splits at once, of two parents, and a PE's
 * free mask is one resource - two reductions reading it at once can agree the
 * same number. Nothing is held across the first reduction, which waits for
 * every parent PE and so, through other collectives, for anything else in the
 * program: the first design, a mask token held across it, deadlocked a valid
 * program that way (the adversarial read of 2026-10-03). Each member
 * takes its agreed numbers *pending* on its PE (take_pending): a word per
 * number, from 0 to the parent number plus one by compare-and-swap, and then
 * the number found not held. A word another split holds is waited for when
 * that split's parent number is higher and lost when it is lower; a second AND
 * reduction over the parent says whether every member took its numbers, and
 * then each marks them held and clears its words, or clears them and every
 * parent PE runs the round again. A split holds words only between its two
 * reductions, with every member past the first; it waits only for a higher
 * parent number, whose own waits are for higher numbers still - a team's
 * number being the same on all its members - so no waits close a cycle. The
 * lowest parent number among the splits active loses a round only to a
 * number another has made held. The pending take and the second reduction run
 * when any parent PE has SHMEM_THREAD_MULTIPLE, which the first reduction's
 * marks say alike on every parent PE - the levels of a job's PEs may differ,
 * and a PE below it takes its numbers uncontended. When none has, one thread
 * of each is in the library at a time, and a split takes its numbers after
 * the one reduction.
 *
 * A created team's descriptor lives in this process's pool, one per number
 * (osoe_team_of), used again when the PE takes the number again. A handle is
 * the descriptor's address plus its generation - how many times the PE has
 * taken the number - times the pool's size: never dereferenced, recovered by
 * division, and accepted while the descriptor is live at that generation, so
 * that one never issued, or of a team destroyed, is named, even when its
 * number names another team since (D6). Destroying a team is a sync over its
 * members, then each member leaves the node's block - the last to leave gives
 * it back to the pool (node.h) - its descriptor's arrays are freed and its
 * number given back.
 *
 * Contexts are context.c's (phase 1 step 8): a team's destroy destroys the
 * shareable contexts created from it, before its sync (spec §9.4.8).
 */
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pshmem.h>
#include <shmem.h>

#include "allocator.h"
#include "barrier.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "base/sweep.h"
#include "blocked.h"
#include "collective-tree.h"
#include "collectives.h"
#include "context.h"
#include "export.h"
#include "fatal.h"
#include "internal-region.h"
#include "internode.h"
#include "job.h"
#include "metrics.h"
#include "node.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"
#include "transport.h"

static struct osoe_team world_team, shared_team, finalize_team;

/*
 * Every team this process has a descriptor for, by number: OSOE_MAX_TEAMS
 * entries. Each is stored with release once its descriptor is complete and
 * loaded with acquire (§3.16), so that a team created while other threads run
 * is never found half built; a destroyed team's entry is cleared. The node's
 * progress thread does not look here: it finds a team by number in the node's
 * block (node.h), since the process it runs in may hold no descriptor for the
 * team at all.
 */
static _Atomic(struct osoe_team *) *numbered;
static uint32_t numbered_count;

/*
 * The descriptors of the teams a split creates, one per number from
 * OSOE_PREDEFINED_TEAMS on, `created_count` of them, each used again when this
 * PE takes its number again. A created team's handle is its descriptor's
 * address here plus its generation times the array's size (handle_of), which
 * is what lets osoe_team_of tell a handle it issued from one it did not, and a
 * live one from a destroyed one, without reading through either: a handle is
 * a descriptor's only if it falls on one, the array's size apart, and live
 * only if `numbered` holds the descriptor and the descriptor's generation is
 * the handle's. Destroying the team frees the descriptor's arrays and leaves
 * its number and generation, to say which team the handle was. Untouched
 * entries cost address space, not memory.
 */
static struct osoe_team *created;
static uint32_t created_count;

/*
 * The numbers this PE holds (D1): bit t set from the moment a split agrees t
 * for a team this PE is in until its destroy has given the team up, after
 * every use of the descriptor. Set by the split - while it holds the number
 * pending, when any PE of the parent has SHMEM_THREAD_MULTIPLE - and cleared
 * by the destroy, two threads may be in a split and a destroy at once, each
 * with one atomic read-modify-write; `mask_words` words, OSOE_MAX_TEAMS bits.
 */
static _Atomic uint64_t *held;
static uint32_t mask_words;

/*
 * The numbers pending on this PE (D7; tools/modelcheck/split_numbers_model.py):
 * per number, 0, or one more than the parent number of the split that holds it
 * pending - between a SHMEM_THREAD_MULTIPLE split's two reductions, and until
 * it is held if the split keeps it. OSOE_MAX_TEAMS words, each a futex word a
 * split waits on (take_pending). Process-local: a PE is a process, and only its
 * own threads contend.
 */
static _Atomic uint32_t *pending;

/* Whether two threads of this PE may be in splits at once - the program has
 * SHMEM_THREAD_MULTIPLE (D7). A split takes its numbers pending when any PE of
 * its parent has, which the first reduction's marks say alike on every parent
 * PE: the levels of a job's PEs may differ (spec §9.2), and the parent PEs must
 * run the same reductions. Set at start, before any split. */
static bool concurrent;

/* The predefined teams' local list, every local id in order - beyond what the
 * node and the bootstrap already hold, the one array their shapes need. */
static uint32_t *identity;

/* This node's world ranks in ascending order: the world and finalize teams'
 * members here by team rank, which the internode backend asks for
 * (osoe_internode_team_local_ranks). */
static uint32_t *ranks_ascending;

/* A team of one's list of local ranks: its only member is rank 0. */
static const uint32_t rank_zero = 0;

/*
 * The predefined teams' local list: every local PE, in local-id order, so that
 * the local lead is local id 0 - the node's lead, which the generic internode
 * phase and the job's table of leads (job.h) address on every node, whatever
 * order a hand-given GTIDS puts the node's ranks in (the bootstrap accepts
 * one that is not ascending). The members' team ranks, in the same order, are
 * the ranks the bootstrap lists by local id for the world and finalize teams,
 * and the local ids themselves for SHMEM_TEAM_SHARED. Design §5.2's lead, the
 * member with the lowest team rank, is the rule for teams created by a split,
 * whose leads the internode phase finds through their records.
 */
static void predefined_local_shape(void)
{
    const struct osoe_node *node = &osoe.node;
    uint32_t count = osoe.bootstrap.local_count;
    identity = malloc(sizeof *identity * count);
    ranks_ascending = malloc(sizeof *ranks_ascending * count);
    if (!identity || !ranks_ascending)
        osoe_fatal("out of memory describing this node's %u PEs", count);
    for (uint32_t i = 0; i < count; i++) {
        identity[i] = i;
        ranks_ascending[i] = node->local_block ? node->first_rank + i : node->sorted_rank[i];
    }
}

/*
 * A team spanning every node, numbered like the world: the world's own, and
 * the finalize team, which has the world's members on counters of its own
 * (node.h).
 */
static struct osoe_team every_pe(const char *name, uint32_t number)
{
    struct osoe_team t = {
        .name = name,
        .number = number,
        .members = osoe.bootstrap.world_size,
        /* Every local PE is a member, so the local member count is the node's
         * PE count (§5.2). */
        .local_members = osoe.bootstrap.local_count,
        .local_ids = identity,
        .local_ranks = osoe.bootstrap.gtids,
        .lead_local_id = 0,
        .internode = osoe.job.node_count > 1,
        .nodes = osoe.job.node_count,
        .me = osoe.bootstrap.rank,
        .world_start = 0,
        .world_stride = 1,
    };
    return t;
}

/*
 * SHMEM_TEAM_SHARED is the PEs whose memory this one can reach with loads and
 * stores (spec §7), and spec §7 also requires shmem_ptr to answer non-NULL for
 * every member of it. Under §8.1's force-remote flag a node-mate's pointer is
 * denied, so the only member left is this PE: the team is the singleton
 * {self}, not the node (§8.1; second review, conformance F2). Without the flag
 * it is the node, numbered by local id, and one node by definition (§4.8), so
 * it has no internode phase at all.
 */
static struct osoe_team shared(void)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    if (osoe.job.force_remote)
        return (struct osoe_team){
            .name = "SHMEM_TEAM_SHARED",
            .number = OSOE_TEAM_SHARED,
            .members = 1,
            .local_members = 1,
            .local_ids = &b->local_id,
            .local_ranks = &rank_zero,
            .lead_local_id = b->local_id,
            .nodes = 1,
            .singleton = true,
            .me = 0,
            .world_start = b->rank,
            .world_stride = 1,
        };
    struct osoe_team t = {
        .name = "SHMEM_TEAM_SHARED",
        .number = OSOE_TEAM_SHARED,
        .members = b->local_count,
        .local_members = b->local_count,
        .local_ids = identity,
        .local_ranks = identity,
        .lead_local_id = 0,
        .nodes = 1,
        .me = b->local_id,
    };
    /* Its rank r is local id r, world rank gtids[r]: strided when the node's
     * ranks are one progression - a block distribution's, stride 1, or a
     * cyclic one's, stride N - and a table of the node's L ranks when not. */
    uint32_t count = b->local_count;
    uint32_t stride = count > 1 && b->gtids[1] > b->gtids[0] ? b->gtids[1] - b->gtids[0] : 0;
    bool strided = count == 1 || stride > 0;
    for (uint32_t i = 2; strided && i < count; i++)
        strided = b->gtids[i] > b->gtids[i - 1] && b->gtids[i] - b->gtids[i - 1] == stride;
    if (strided) {
        t.world_start = b->gtids[0];
        t.world_stride = count == 1 ? 1 : (int32_t)stride;
    } else {
        t.world_of_rank = b->gtids;
    }
    return t;
}

/*
 * Node `node`'s lead: the rank at its local id 0, which the job's table of
 * leads holds (job.h). It is the local lead of every predefined team there
 * (predefined_local_shape), and the rank the generic internode phase puts to
 * (§5.8).
 */
static uint32_t lead_of(uint32_t node)
{
    uint32_t rank = osoe_job_node_lead(&osoe.job, node);
    if (rank == UINT32_MAX)
        osoe_fatal("the rank map has no lead for node %u", node);
    return rank;
}

/* ---- a team's node set (§5.2) -------------------------------------------- */

/*
 * The logical nodes holding at least one of a team's members, ascending, and
 * each one's lead: the rank the other nodes' leads put to there, and whose
 * node the progress thread's messages for the team go to (node.h, the
 * record). O(N) for a team of N nodes; built once, when the team is, by each
 * node's local lead of it alone, and dropped once its record is written - but
 * for the leads, which a created team's local lead keeps (team.h,
 * `node_leads`).
 */
struct node_set {
    uint32_t count;
    uint32_t *nodes;
    uint32_t *leads;
};

static void node_set_allocate(struct node_set *set, uint32_t count, const char *name)
{
    set->count = count;
    set->nodes = malloc(sizeof *set->nodes * (count ? count : 1));
    set->leads = malloc(sizeof *set->leads * (count ? count : 1));
    if (!set->nodes || !set->leads)
        osoe_fatal("out of memory describing %s's %u nodes", name, count);
}

static void node_set_free(struct node_set *set)
{
    free(set->nodes);
    free(set->leads);
    *set = (struct node_set){ 0 };
}

/* A predefined team's: every node, each led by its local id 0. */
static void every_node(struct node_set *set, const char *name)
{
    node_set_allocate(set, osoe.job.node_count, name);
    for (uint32_t n = 0; n < set->count; n++) {
        set->nodes[n] = n;
        set->leads[n] = lead_of(n);
    }
}

/* Team rank `rank`'s node, which exists for every rank a team has. */
static uint32_t node_of_member(const struct osoe_team *t, uint32_t rank, uint32_t *world)
{
    *world = (uint32_t)osoe_team_to_world(t, (int)rank);
    uint32_t node = osoe_node_of(&osoe.job, *world);
    if (node >= osoe.job.node_count)
        osoe_fatal("%s's rank %u is PE %u, which the rank map places on no node", t->name, rank,
                   *world);
    return node;
}

/* A member's node and team rank, for the walk that meets nodes out of order. */
struct member_node {
    uint32_t node;
    uint32_t rank;
};

/* By node, and on one node by team rank: each node's run then starts with
 * its member of lowest team rank, its lead. */
static int by_node_then_rank(const void *a, const void *b)
{
    const struct member_node *x = a, *y = b;
    if (x->node != y->node)
        return x->node < y->node ? -1 : 1;
    return x->rank < y->rank ? -1 : x->rank > y->rank;
}

/*
 * A created team's node set (decision 4 of the slice's brief), on each node's
 * local lead of the team, which alone needs it: a walk of the team's S ranks,
 * mapping each to its node, and per node its member with the lowest team rank
 * for the lead. No step of it costs anything in the job's node count.
 *
 * The walk goes in ascending world rank - team-rank order, or its reverse when
 * a negative stride makes the team's world ranks descend - and a block
 * distribution's nodes then come in ascending order too, each a run of
 * members: the set is the runs, and each run's lead its member of lowest team
 * rank, the first met, or the last when the walk goes backwards. O(S) node
 * lookups (each O(log R), job.h) and O(min(S, N)) memory. A team whose walk
 * meets a node twice or out of order - a cyclic distribution's, an interleaved
 * map's - is gathered instead as (node, team rank) pairs, sorted by node and
 * then rank, and each node's run of pairs gives the node and, first, its lead:
 * O(S) lookups again, O(S log S) to sort and O(S) memory.
 */
static void created_node_set(const struct osoe_team *t, struct node_set *set)
{
    uint32_t members = t->members, nodes = osoe.job.node_count;
    bool backwards = !t->world_of_rank && t->world_stride < 0;
    node_set_allocate(set, members < nodes ? members : nodes, t->name);
    uint32_t count = 0, world = 0;
    bool in_order = true;
    for (uint32_t i = 0; i < members && in_order; i++) {
        uint32_t rank = backwards ? members - 1 - i : i;
        uint32_t node = node_of_member(t, rank, &world);
        if (count > 0 && node == set->nodes[count - 1]) {
            if (backwards)
                set->leads[count - 1] = world;      /* a lower team rank, the same node */
            continue;
        }
        if ((count > 0 && node < set->nodes[count - 1]) || count == set->count) {
            in_order = false;
            break;
        }
        set->nodes[count] = node;
        set->leads[count] = world;
        count++;
    }
    if (in_order) {
        set->count = count;
        return;
    }

    node_set_free(set);
    struct member_node *pairs = malloc(sizeof *pairs * members);
    if (!pairs)
        osoe_fatal("out of memory finding the nodes of %s's %u members", t->name, members);
    for (uint32_t rank = 0; rank < members; rank++)
        pairs[rank] = (struct member_node){ node_of_member(t, rank, &world), rank };
    qsort(pairs, members, sizeof *pairs, by_node_then_rank);
    count = 0;
    for (uint32_t i = 0; i < members; i++)
        count += i == 0 || pairs[i].node != pairs[i - 1].node;
    node_set_allocate(set, count, t->name);
    for (uint32_t i = 0, j = 0; i < members; i++)
        if (i == 0 || pairs[i].node != pairs[i - 1].node) {
            set->nodes[j] = pairs[i].node;
            set->leads[j] = (uint32_t)osoe_team_to_world(t, (int)pairs[i].rank);
            j++;
        }
    free(pairs);
}

/*
 * Write `t`'s record into this node's block for its number and publish it
 * (node.h): what the node's progress thread and internode backend learn the
 * team from, the first time they meet it, rather than from a call made here.
 * By the team's local lead on this node: for the predefined teams the node's
 * lead, before its agent starts; for a created team its member with the lowest
 * team rank here, before the split's sync over the parent (node.h says why
 * that is enough).
 *
 * For a team spanning several nodes, this node's place in §5.3's k-ary tree
 * over `set`, its node set, naming each neighbour node by its lead's rank. The
 * record keeps the O(k) of the set the tree needs (§5.2). The set's lead for
 * this node must be the team's local lead here, which its local list names
 * independently: the two are computed apart, and disagreeing would have the
 * other nodes put to a PE that never reads its internal region for the team.
 */
static void write_record(const struct osoe_team *t, const char *name,
                         const struct node_set *set)
{
    struct osoe_team_record *r = &t->block->record;
    r->local_members = t->local_members;
    r->nodes = t->nodes;
    r->index = 0;
    r->parent = UINT32_MAX;
    r->position = 0;
    r->children = 0;
    snprintf(r->name, sizeof r->name, "%s", name);
    /* The team's world ranks, decision E (node.h): a progression, or for a
     * table - SHMEM_TEAM_SHARED's family, on one node - nothing. */
    r->members = t->members;
    r->strided = t->world_of_rank == NULL;
    r->world_start = r->strided ? t->world_start : 0;
    r->world_stride = r->strided ? t->world_stride : 0;
    if (t->internode && !r->strided)
        osoe_fatal("%s spans nodes and its ranks are not a progression of world ranks; every "
                   "team with an internode phase must be one (decision E)", t->name);
    if (t->internode) {
        uint32_t k = osoe.bootstrap.collective_fanout;
        struct osoe_collective_tree tree;
        if (!set || set->count != t->nodes ||
            !osoe_collective_tree(set->nodes, set->count, osoe.job.node, k, &tree))
            osoe_fatal("%s's node set (%u nodes) is not sorted, repeats a node, or leaves out "
                       "node %u, which holds members", t->name, set ? set->count : 0,
                       osoe.job.node);
        uint32_t lead = osoe.bootstrap.gtids[t->lead_local_id];
        if (set->leads[tree.index] != lead)
            osoe_fatal("%s's node set names PE %u as node %u's lead, and its members here name "
                       "PE %u", t->name, set->leads[tree.index], osoe.job.node, lead);
        r->index = tree.index;
        if (tree.index > 0) {
            r->parent = set->leads[(tree.index - 1) / k];
            r->position = (tree.index - 1) % k;
        }
        r->children = tree.children;
        for (uint32_t j = 0; j < tree.children; j++)
            r->child[j] = set->leads[(uint64_t)k * tree.index + 1 + j];
    }
    /* release: the record, then the word its readers load first (§3.16). */
    atomic_store_explicit(&r->published, 1, memory_order_release);
}

/*
 * Bind a descriptor to its block on this node and this PE's published epoch
 * for it, and make it findable by the number. A predefined team's block is at
 * the index of its number; a created team's is the one its local lead here
 * mapped under the team's name before the split's sync, which this follows -
 * the map entry's release and this acquire carry the block's name and record
 * with them (node.h). A member finding none is a broken invariant, not a
 * program's mistake.
 */
static void take_number(struct osoe_team *t)
{
    if (t->number < OSOE_PREDEFINED_TEAMS) {
        t->block_index = t->number;
    } else {
        t->block_index = osoe_node_team_find(&osoe.node, t->lead_local_id, t->number);
        if (t->block_index == UINT32_MAX)
            osoe_fatal("%s: no block is mapped on node %u under its name (local lead %u, "
                       "number %u) after the split's sync", t->name, osoe.job.node,
                       t->lead_local_id, t->number);
    }
    t->block = osoe_node_team(&osoe.node, t->block_index);
    if (!t->block)
        osoe_fatal("%s: its block on node %u is %u, past the pool's %u", t->name, osoe.job.node,
                   t->block_index, osoe.node.team_count);
    t->published = osoe_node_published_epoch(&osoe.node, osoe.bootstrap.local_id, t->number);
    osoe_team_publish_epoch(t, 0);
    atomic_store_explicit(&numbered[t->number], t, memory_order_release);
}

/*
 * A created team's handle (D6): its descriptor's address plus the descriptor's
 * generation times the array's size - an address past the array's end once
 * the number has been taken twice, never dereferenced, and never 0, 1 or 2,
 * the predefined handles, being at least the array's own. The arithmetic is on
 * integers: osoe runs on 64-bit address spaces, where a generation's 32 bits
 * times an array of at most 1,021 descriptors stays below 2^63 past any heap
 * address.
 */
static shmem_team_t handle_of(const struct osoe_team *t)
{
    return (shmem_team_t)((uintptr_t)t +
                          (uintptr_t)t->generation * created_count * sizeof *created);
}
_Static_assert(sizeof(uintptr_t) == 8, "a team handle is an address plus a generation's span");

/* The number of the created team whose descriptor a handle names, and the
 * generation it names it at, or UINT32_MAX if it names no descriptor of the
 * pool: arithmetic on the value, never a read through it. */
static uint32_t created_number(shmem_team_t team, uint64_t *generation)
{
    uintptr_t at = (uintptr_t)team, base = (uintptr_t)created;
    uintptr_t size = sizeof *created, span = (uintptr_t)created_count * size;
    if (!created || at < base || (at - base) % span % size != 0)
        return UINT32_MAX;
    *generation = (at - base) / span;
    return OSOE_PREDEFINED_TEAMS + (uint32_t)((at - base) % span / size);
}

/* A created team gone from this process: its number is forgotten, its arrays
 * freed, and the descriptor left saying which number and generation it was
 * (osoe_team_of). The number stays held until the caller gives it back. */
static void forget(struct osoe_team *t)
{
    uint32_t number = t->number, generation = t->generation;
    atomic_store_explicit(&numbered[number], NULL, memory_order_release);
    free((void *)t->local_ids);
    free((void *)t->local_ranks);
    free((void *)t->world_of_rank);
    free((void *)t->node_leads);
    *t = (struct osoe_team){ .number = number, .destroyed = true, .generation = generation };
}

void osoe_team_start(void)
{
    numbered_count = osoe.bootstrap.max_teams;
    created_count = numbered_count - OSOE_PREDEFINED_TEAMS;
    mask_words = osoe_internal_mask_words(numbered_count);
    int level = SHMEM_THREAD_SINGLE;
    pshmem_query_thread(&level);
    concurrent = level == SHMEM_THREAD_MULTIPLE;
    /* calloc: a null pointer in every entry, and no number held or pending,
     * before any reader exists. */
    numbered = calloc(numbered_count, sizeof *numbered);
    created = calloc(created_count, sizeof *created);
    held = calloc(mask_words, sizeof *held);
    pending = calloc(numbered_count, sizeof *pending);
    if (!numbered || !created || !held || !pending)
        osoe_fatal("out of memory for %u teams", numbered_count);
    for (uint32_t number = 0; number < OSOE_PREDEFINED_TEAMS; number++)
        atomic_fetch_or_explicit(&held[number / 64], UINT64_C(1) << (number % 64),
                                 memory_order_seq_cst);
    predefined_local_shape();
    world_team = every_pe("SHMEM_TEAM_WORLD", OSOE_TEAM_WORLD);
    shared_team = shared();
    finalize_team = every_pe("the finalize team", OSOE_TEAM_FINALIZE);
    take_number(&world_team);
    take_number(&shared_team);
    take_number(&finalize_team);
    /*
     * The records, by this node's lead: the process whose agent - started
     * after this returns - reads them, so its start orders them before any
     * message it will take for these teams, which is the order a record needs
     * (node.h). The predefined teams include every local PE, the lead too, so
     * it knows each one's shape. A team of one has no record: it touches
     * nothing of the node's (team.h). The world's node set is built anyway,
     * although it is every node, so that the tree cannot tell it from a
     * created team's: O(N) once per team, freed at once.
     */
    if (osoe.bootstrap.local_id == 0) {
        struct node_set every = { 0 };
        if (world_team.internode)
            every_node(&every, "the job");
        write_record(&world_team, "world", &every);
        if (!shared_team.singleton)
            write_record(&shared_team, "shared", NULL);
        write_record(&finalize_team, "finalize", &every);
        node_set_free(&every);
    }
}

void osoe_team_stop(void)
{
    /* Every created team the program did not destroy, as if it had. */
    for (uint32_t number = OSOE_PREDEFINED_TEAMS; numbered && number < numbered_count; number++) {
        struct osoe_team *t = atomic_load_explicit(&numbered[number], memory_order_acquire);
        if (t)
            forget(t);
    }
    free(created);
    free(numbered);
    free((void *)held);
    free((void *)pending);
    free(identity);
    free(ranks_ascending);
    created = NULL;
    created_count = 0;
    numbered = NULL;
    numbered_count = 0;
    held = NULL;
    pending = NULL;
    mask_words = 0;
    identity = NULL;
    ranks_ascending = NULL;
}

struct osoe_team *osoe_team_finalize(void)
{
    return &finalize_team;
}

struct osoe_team *osoe_team_numbered(uint32_t number)
{
    return number < numbered_count ? atomic_load_explicit(&numbered[number], memory_order_acquire)
                                   : NULL;
}

const char *osoe_team_label(uint32_t number, char *buffer, size_t size)
{
    static const char *const predefined[] = {
        [OSOE_TEAM_WORLD] = "SHMEM_TEAM_WORLD",
        [OSOE_TEAM_SHARED] = "SHMEM_TEAM_SHARED",
        [OSOE_TEAM_FINALIZE] = "the finalize team",
    };
    if (number < OSOE_PREDEFINED_TEAMS)
        return predefined[number];
    snprintf(buffer, size, "team %u", number);
    return buffer;
}

struct osoe_team *osoe_team_of(const char *routine, shmem_team_t team)
{
    osoe_require_initialized(routine);
    if (team == SHMEM_TEAM_INVALID)
        return NULL;
    if (team == SHMEM_TEAM_WORLD)
        return &world_team;
    if (team == SHMEM_TEAM_SHARED)
        return &shared_team;
    uint64_t generation = 0;
    uint32_t number = created_number(team, &generation);
    if (number == UINT32_MAX)
        osoe_fatal("%s: %p is not a team this library issued", routine, (void *)team);
    struct osoe_team *t = &created[number - OSOE_PREDEFINED_TEAMS];
    bool live = atomic_load_explicit(&numbered[number], memory_order_acquire) == t;
    if (live && generation == t->generation)
        return t;
    if (generation > t->generation || (generation == t->generation && !t->destroyed))
        osoe_fatal("%s: %p is not a team this library issued to this PE", routine,
                   (void *)team);
    osoe_fatal("%s: %p is team %u, which this PE has destroyed%s", routine, (void *)team, number,
               live || generation < t->generation ? "; the number names another team since"
                                                   : "");
}

int osoe_team_to_world(const struct osoe_team *t, int pe)
{
    if (pe < 0 || (uint32_t)pe >= t->members)
        return -1;
    if (t->world_of_rank)
        return (int)t->world_of_rank[pe];
    return (int)((int64_t)t->world_start + (int64_t)pe * t->world_stride);
}

/*
 * The world rank `world` as a number in `t`, or -1 if it is not a member: the
 * inverse of the mapping above, by arithmetic for a strided team - whose
 * stride may be negative, never 0 - and by a scan of the table - at most the
 * node's PEs - for one that is not.
 */
static int world_to_team(const struct osoe_team *t, int world)
{
    if (world < 0 || (uint32_t)world >= osoe.bootstrap.world_size)
        return -1;
    uint32_t w = (uint32_t)world;
    if (t->world_of_rank) {
        for (uint32_t r = 0; r < t->members; r++)
            if (t->world_of_rank[r] == w)
                return (int)r;
        return -1;
    }
    int64_t offset = (int64_t)w - t->world_start;
    if (offset % t->world_stride != 0)
        return -1;
    int64_t r = offset / t->world_stride;
    return r >= 0 && r < t->members ? (int)r : -1;
}

uint32_t osoe_team_node_index(const struct osoe_team *t, uint32_t node)
{
    if (!t->node_leads)
        return t->number < OSOE_PREDEFINED_TEAMS && node < t->nodes ? node : UINT32_MAX;
    uint32_t low = 0, high = t->nodes;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (osoe_node_of(&osoe.job, t->node_leads[middle]) < node)
            low = middle + 1;
        else
            high = middle;
    }
    return low < t->nodes && osoe_node_of(&osoe.job, t->node_leads[low]) == node ? low
                                                                                 : UINT32_MAX;
}

/* ---- what the internode backend asks of a team (internode.h) ------------ */

/*
 * The descriptor of the team whose block on this node is `block_index` - the
 * seam's `team` since step D (internode.h) - by the number its name holds:
 * asked on the thread of a member holding the block, whose descriptor under
 * that number is the block's team, a number naming one team of a PE's at a
 * time (D1); anything else is none.
 */
static const struct osoe_team *of_block(uint32_t block_index)
{
    const struct osoe_team_block *block = osoe_node_team(&osoe.node, block_index);
    if (!block)
        return NULL;
    const struct osoe_team *t =
        osoe_team_numbered(atomic_load_explicit(&block->name.number, memory_order_acquire));
    return t && t->block_index == block_index ? t : NULL;
}

uint32_t osoe_internode_team_lead(uint32_t team, uint32_t index)
{
    const struct osoe_team *t = of_block(team);
    if (!t || index >= t->nodes)
        return UINT32_MAX;
    if (t->node_leads)
        return t->node_leads[index];
    return t->number < OSOE_PREDEFINED_TEAMS ? osoe_job_node_lead(&osoe.job, index) : UINT32_MAX;
}

const uint32_t *osoe_internode_team_local_ranks(uint32_t team, uint32_t *count)
{
    const struct osoe_team *t = of_block(team);
    *count = t ? t->local_members : 0;
    if (!t)
        return NULL;
    /* The world's and finalize team's are listed in local-id order, which a
     * hand-given order need not make ascending; every other team's list is
     * ascending already - a created team's in team-rank order, SHARED's its
     * local ids. */
    if (t->number == OSOE_TEAM_WORLD || t->number == OSOE_TEAM_FINALIZE)
        return ranks_ascending;
    return t->local_ranks;
}

/* ---- team queries (§9.4) ----------------------------------------------- */

OSOE_API int pshmem_team_my_pe(shmem_team_t team)
{
    struct osoe_team *t = osoe_team_of("shmem_team_my_pe", team);
    return t ? (int)t->me : -1;
}

OSOE_API int pshmem_team_n_pes(shmem_team_t team)
{
    struct osoe_team *t = osoe_team_of("shmem_team_n_pes", team);
    return t ? (int)t->members : -1;
}

/*
 * spec §9.4.4: a valid team returns 0, SHMEM_TEAM_INVALID returns nonzero and
 * does nothing, and a mask of 0 performs no action. A created team answers
 * with the configuration it was created with (§9.4.3); the predefined teams
 * were created with none, so every field reads as its default.
 */
OSOE_API int pshmem_team_get_config(shmem_team_t team, long config_mask,
                                   shmem_team_config_t *config)
{
    struct osoe_team *t = osoe_team_of("shmem_team_get_config", team);
    if (!t)
        return 1;
    if (config_mask == 0 || !config)
        return 0;
    if (config_mask & SHMEM_TEAM_NUM_CONTEXTS)
        config->num_contexts = t->num_contexts;
    return 0;
}

/*
 * spec §9.4.5. -1 whenever either handle is invalid or src_pe is not a member
 * of both teams. Going through the world rank is not an implementation detail:
 * a world rank is the only name both teams share.
 */
OSOE_API int pshmem_team_translate_pe(shmem_team_t src_team, int src_pe,
                                     shmem_team_t dest_team)
{
    struct osoe_team *src = osoe_team_of("shmem_team_translate_pe", src_team);
    struct osoe_team *dst = osoe_team_of("shmem_team_translate_pe", dest_team);
    if (!src || !dst)
        return -1;
    int world = osoe_team_to_world(src, src_pe);
    if (world < 0)
        return -1;
    return world_to_team(dst, world);
}

/* ---- creating teams (§9.4.6, §9.4.7) ------------------------------------ */

/* The parent ranks a created team is made of: start + stride x i, for every i
 * below size. A strided split's triplet, and split_2d's rows (stride 1) and
 * columns (stride xrange). */
struct progression {
    int64_t start;
    int64_t stride;
    uint32_t size;
};

/*
 * One team of those a split creates, as this PE sees it: which of the numbers
 * the call agrees it takes - split_strided's one, split_2d's lowest for the
 * rows and the next for the columns - and, for a team this PE is in, its
 * descriptor as it is built before the number is known (step 2 of the file's
 * comment) and, on the team's local lead here, its node set and the block
 * claimed for it.
 */
struct membership {
    uint32_t which;                     /* 0: the lowest number agreed; 1: the next */
    bool member;                        /* whether this PE is in it */
    struct progression ranks;
    int num_contexts;                   /* spec §9.4.3, as this PE was given it */
    shmem_team_t *handle;               /* where its handle goes */
    struct osoe_team shape;             /* its descriptor but for the number */
    struct node_set set;                /* on its local lead here: its node set */
    uint32_t block;                     /* on its local lead here: its block, or UINT32_MAX */
};

static bool is_member(const struct progression *p, uint32_t parent_rank)
{
    int64_t offset = (int64_t)parent_rank - p->start;
    if (p->stride == 0)
        return offset == 0;
    if (offset % p->stride != 0)
        return false;
    int64_t i = offset / p->stride;
    return i >= 0 && i < p->size;
}

/*
 * spec §9.4.3: the num_contexts a split's configuration asks for - the default,
 * 0, unless the mask names the field. A mask that names it with no
 * configuration to read it from is undefined, and ends the job by name.
 */
static int contexts_requested(const char *routine, const char *argument,
                              const shmem_team_config_t *config, long mask)
{
    if (!(mask & SHMEM_TEAM_NUM_CONTEXTS))
        return 0;
    if (!config)
        osoe_fatal("%s: %s is a null pointer, and its mask asks for its num_contexts",
                   routine, argument);
    return config->num_contexts;
}

/*
 * This PE's descriptor for a team its call creates, whose members are the
 * parent ranks of `m` (step 2 of the file's comment), all but the number: its
 * world mapping, composed from the parent's; its members on this node in
 * team-rank order, by testing each local PE, O(L); from them whether it spans
 * nodes, O(1); and, on its local lead here, its node set, O(S) or O(S log S).
 */
static void shape(const struct osoe_team *parent, struct membership *m)
{
    const struct progression *p = &m->ranks;
    struct osoe_team *t = &m->shape;
    *t = (struct osoe_team){
        .name = "the new team",
        .members = p->size,
        .singleton = p->size == 1,
        .me = p->stride == 0 ? 0 : (uint32_t)(((int64_t)parent->me - p->start) / p->stride),
        .num_contexts = m->num_contexts,
    };
    m->block = UINT32_MAX;

    /* Team rank r is parent rank start + stride x r: a start and a stride in
     * world ranks when the parent's are, and a table - of at most the node's
     * PEs, a split of SHMEM_TEAM_SHARED's - when the parent has one. */
    if (parent->world_of_rank) {
        uint32_t *table = malloc(sizeof *table * p->size);
        if (!table)
            osoe_fatal("out of memory for %s's %u ranks", t->name, p->size);
        for (uint32_t r = 0; r < p->size; r++)
            table[r] = parent->world_of_rank[p->start + p->stride * r];
        t->world_of_rank = table;
    } else {
        t->world_start = (uint32_t)osoe_team_to_world(parent, (int)p->start);
        t->world_stride = p->size == 1 ? 1 : (int32_t)(p->stride * parent->world_stride);
    }

    /*
     * This node's members in team-rank order (team.h): each local PE's world
     * rank tested against the mapping, O(L). A strided team's ranks follow
     * its world ranks, so the node's ranks walked in ascending order - which
     * the node keeps (node.h) - list its members in team-rank order, or in
     * reverse for a negative stride. A team with a table lies on this node, as
     * its parent does, and is walked by team rank instead, each rank asked for
     * its local id. The lead is the first: the lowest team rank here (§5.2).
     */
    const struct osoe_node *node = &osoe.node;
    uint32_t capacity = t->world_of_rank ? t->members : node->local_count;
    uint32_t *ids = malloc(sizeof *ids * capacity), *ranks = malloc(sizeof *ranks * capacity);
    if (!ids || !ranks)
        osoe_fatal("out of memory for %s's members on this node", t->name);
    uint32_t count = 0;
    if (t->world_of_rank) {
        for (uint32_t r = 0; r < t->members; r++) {
            int32_t local = osoe_node_local_id(node, t->world_of_rank[r]);
            if (local >= 0) {
                ids[count] = (uint32_t)local;
                ranks[count++] = r;
            }
        }
    } else {
        for (uint32_t i = 0; i < node->local_count; i++) {
            uint32_t world = node->local_block ? node->first_rank + i : node->sorted_rank[i];
            int r = world_to_team(t, (int)world);
            if (r >= 0) {
                ids[count] = node->local_block ? i : node->sorted_local[i];
                ranks[count++] = (uint32_t)r;
            }
        }
        for (uint32_t i = 0; t->world_stride < 0 && i < count / 2; i++) {
            uint32_t id = ids[i], rank = ranks[i];
            ids[i] = ids[count - 1 - i];
            ranks[i] = ranks[count - 1 - i];
            ids[count - 1 - i] = id;
            ranks[count - 1 - i] = rank;
        }
    }
    if (count == 0 || ranks[0] > t->me)
        osoe_fatal("%s: this PE, its rank %u, is not among its members on this node", t->name,
                   t->me);
    t->local_members = count;
    t->local_ids = ids;
    t->local_ranks = ranks;
    t->lead_local_id = ids[0];

    /*
     * The team spans nodes exactly when some member is not here, which every
     * member knows from its own node's list; only the local lead here goes on
     * to the node set (team.h, `nodes`), which is this node alone for a team
     * whose members are all here - every split of SHMEM_TEAM_SHARED - and
     * otherwise found by walking the team.
     */
    t->internode = t->local_members < t->members;
    t->nodes = t->internode ? 0 : 1;
    if (osoe.bootstrap.local_id != t->lead_local_id)
        return;
    if (!t->internode) {
        node_set_allocate(&m->set, 1, t->name);
        m->set.nodes[0] = osoe.job.node;
        m->set.leads[0] = osoe.bootstrap.gtids[t->lead_local_id];
    } else {
        created_node_set(t, &m->set);
    }
    t->nodes = m->set.count;
}

/* Whether this PE leads `m`'s team on its node, and so claims its block. */
static bool leads(const struct membership *m)
{
    return m->member && osoe.bootstrap.local_id == m->shape.lead_local_id;
}

/* What a split that failed built of `m`: its arrays, its node set, and the
 * block it claimed, returned to the pool unnamed (node.h). */
static void unshape(struct membership *m)
{
    if (!m->member)
        return;
    free((void *)m->shape.local_ids);
    free((void *)m->shape.local_ranks);
    free((void *)m->shape.world_of_rank);
    node_set_free(&m->set);
    if (m->block != UINT32_MAX)
        osoe_node_team_return(&osoe.node, m->block);
    m->block = UINT32_MAX;
}

/*
 * Step 5 of the file's comment, for team `m` under `number`: the descriptor
 * moved into this PE's pool, to the slot of the number, whose generation it
 * raises (osoe_team_of); the number's counters in this PE's internal region
 * zeroed (internode.h) - nothing of the team this PE last held under the
 * number is still to arrive, every write of it having been waited for before
 * the PE left it (the proposal's "Why giving a number back is safe"); and on
 * the team's local lead here, its block named, the record written and
 * published, and the block mapped under the team's name last, with release -
 * what every member's acquire after the split's sync finds it by
 * (take_number). The name is free: the number was in this lead's free mask, so
 * no block was mapped under it.
 */
static struct osoe_team *finish(struct membership *m, uint32_t number)
{
    struct osoe_team *t = &created[number - OSOE_PREDEFINED_TEAMS];
    uint32_t generation = t->generation;
    if (generation == UINT32_MAX)
        osoe_fatal("this PE has taken team number %u %u times, every generation a handle can "
                   "name", number, generation);
    *t = m->shape;
    t->number = number;
    t->generation = generation + 1;
    snprintf(t->label, sizeof t->label, "team %u", number);
    t->name = t->label;
    m->shape = (struct osoe_team){ 0 };     /* its arrays are the descriptor's now */
    /* Only a number this PE has held before in the session has counters to
     * zero: a heap object is new each session, and only the teams this PE
     * holds a number for write its counters for it - so a first take touches
     * none of the pages a barrier-only team never uses. */
    if (generation > 0)
        osoe_internode_number_taken(number);
    if (m->block == UINT32_MAX)
        return t;
    t->block_index = m->block;
    t->block = osoe_node_team(&osoe.node, m->block);
    osoe_node_team_name(&osoe.node, m->block, t->lead_local_id, number, t->local_members);
    char name[16];
    snprintf(name, sizeof name, "%u", number);
    write_record(t, name, &m->set);
    atomic_store_explicit(osoe_node_team_map_entry(&osoe.node, t->lead_local_id, number),
                          m->block + 1, memory_order_release);
    m->block = UINT32_MAX;
    if (t->internode) {
        t->node_leads = m->set.leads;
        m->set.leads = NULL;
    }
    node_set_free(&m->set);
    return t;
}

/*
 * This PE's free mask (step 3 of the file's comment), `mask_words` words: bit
 * t set when t is not predefined - the three are held from the start - this PE
 * holds no team under t, and no block is mapped under (this PE, t) on its node.
 * The last is one map read a number not held, and holds whenever the first
 * two do: this PE, leading a team, clears its name's entry as it leaves the
 * team and before it gives the number back (node.h) - a check that the name
 * is free that costs nothing beside the reduction. Read without a lock: under
 * SHMEM_THREAD_MULTIPLE another thread's split may agree one of these numbers
 * meanwhile, which the pending take finds (D7, take_pending).
 */
static void free_mask(uint64_t *mask)
{
    uint32_t me = osoe.bootstrap.local_id;
    for (uint32_t w = 0; w < mask_words; w++) {
        uint64_t bits = ~atomic_load_explicit(&held[w], memory_order_acquire);
        if (w == mask_words - 1 && numbered_count % 64 != 0)
            bits &= (UINT64_C(1) << (numbered_count % 64)) - 1;
        for (uint64_t rest = bits; rest != 0; rest &= rest - 1) {
            uint32_t number = w * 64 + (uint32_t)__builtin_ctzll(rest);
            if (atomic_load_explicit(osoe_node_team_map_entry(&osoe.node, me, number),
                                     memory_order_acquire) != 0)
                bits &= ~(UINT64_C(1) << (number % 64));
        }
        mask[w] = bits;
    }
}

/* Give up the pending words of `count` numbers (take_pending), waking any
 * split of this PE that waits for one. */
static void give_pending(const uint32_t *numbers, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        atomic_store_explicit(&pending[numbers[i]], 0, memory_order_seq_cst);
        osoe_futex_wake(&pending[numbers[i]], INT_MAX, OSOE_FUTEX_PRIVATE);
    }
}

/*
 * Wait while number `number` is pending for the split whose parent number is
 * `holder` - 1. Bounded: that split's parent number is higher, it holds the
 * word only between its two reductions, with every member past the first, and
 * its own waits are for higher numbers still (the file's comment). Fair it is
 * not: the word cleared, the higher split can take it again in its next round
 * before this one's compare-and-swap, which only a schedule that keeps doing so
 * prolongs (split_numbers_model.py checks that a way out always exists). A wait
 * like every other: it sweeps (§4.10), counts for the deadlock detector while
 * it sleeps (base/sweep.h) - between its loads and its sleeps it does nothing
 * anyone could see - and says where it is blocked, the number as its detail,
 * for osoe-stat, once it is about to sleep (blocked.h).
 */
static void wait_pending(const char *routine, uint32_t number, uint32_t holder)
{
    _Atomic uint32_t *word = &pending[number];
    if (atomic_load_explicit(word, memory_order_acquire) != holder)
        return;
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_NONE, number);
    do {
        osoe_wait_sleeps();
        osoe_futex_wait(word, holder, &sweep, OSOE_FUTEX_PRIVATE);
        if (osoe_deadline_expired(&sweep)) {
            osoe_sweep_expired(routine);
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        }
    } while (atomic_load_explicit(word, memory_order_acquire) == holder);
    osoe_wait_ends();
    osoe_blocked_leave();
}

/*
 * Step 4's pending take, when any PE of the parent has SHMEM_THREAD_MULTIPLE
 * (D7; tools/modelcheck/split_numbers_model.py): this PE's `count` numbers of the
 * split over parent number `parent`, in turn, each taken pending - its word
 * from 0 to the parent number plus one, by compare-and-swap - and then found
 * not held. A word another split of this PE holds is waited for when that
 * split's parent number is higher, and lost when it is lower, so that a split
 * waits only for higher numbers and the lowest loses a round only to a number
 * made held. seq_cst, the swap and the held load as the held bit's or and the
 * word's clear of the split that keeps the number: one of the two sees the
 * other (the model's release_first). False, with the words this call took
 * given up, if any is lost.
 */
static bool take_pending(const char *routine, uint32_t parent, const uint32_t *numbers,
                         uint32_t count)
{
    uint32_t mine = parent + 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t number = numbers[i], holder = 0;
        while (!atomic_compare_exchange_strong_explicit(&pending[number], &holder, mine,
                                                        memory_order_seq_cst,
                                                        memory_order_seq_cst)) {
            if (holder == mine)
                osoe_fatal("%s: two threads of this PE split team %u at once; spec §9.4 "
                           "requires a team's collectives to be called in one order", routine,
                           parent);
            if (holder < mine) {
                give_pending(numbers, i);
                return false;
            }
            wait_pending(routine, number, holder);
            holder = 0;
        }
        if (atomic_load_explicit(&held[number / 64], memory_order_seq_cst) &
            UINT64_C(1) << (number % 64)) {
            give_pending(numbers, i + 1);
            return false;
        }
    }
    return true;
}

/* The marks word of a split's reduction (step 3 of the file's comment), a bit
 * set for no complaint, so that one AND carries every PE's. */
enum split_mark {
    MARK_NOT_BUSY = 1,      /* this PE's pool was not found empty while a block of
                             * it may be about to come back (split) */
    MARK_NOT_DRY = 2,       /* this PE claimed every block it leads, or leads none */
    MARK_NOT_CONCURRENT = 4,    /* no other thread of this PE can be in a split:
                                 * its level is below SHMEM_THREAD_MULTIPLE */
};

/*
 * The pool of this PE's node had no block for a team this PE would lead on it
 * (step 4 of the file's comment): said by this PE, which alone knows which
 * node it is, once a round that was not busy found it so.
 */
static void pool_dry(const char *routine)
{
    /* One write a line (osoe_notice), as a fatal is: several would-be leads of
     * a split_2d can find the pool dry at once, and unbuffered stderr would cut
     * their lines into each other (the review of step D's merge point). */
    osoe_notice("%s: node %u (%s) has no team block left for the new team this PE would "
                "lead there: OSOE_TEAM_BLOCKS is %u, the teams a node may hold at once (design "
                "§5.7); the split takes none and returns nonzero on every PE of its parent team",
                routine, osoe.job.node, osoe.node.hdr->hostname, osoe.node.team_count);
}

/* No number is free on every PE of the new teams (step 4 of the file's
 * comment): said once, by the parent's rank 0. */
static void numbers_used_up(const char *routine, uint32_t numbers)
{
    /* What runs out is what the members hold between them, which each may
     * hold less of (the review of step D's merge point, readiness Q5-1). */
    osoe_notice("%s: no %s free on every PE of the new team%s - between them %s, though each "
                "may hold fewer: OSOE_MAX_TEAMS is %u, the teams one PE may be in at once, %u "
                "of them predefined, and a larger one leaves more to choose from (design "
                "§5.7); the split takes none and returns nonzero on every PE of its parent team",
                routine, numbers == 1 ? "team number is" : "two team numbers are",
                numbers == 1 ? "" : "s",
                numbers == 1 ? "they hold every one a program may use"
                             : "they hold all but at most one of those a program may use",
                osoe.bootstrap.max_teams, OSOE_PREDEFINED_TEAMS);
}

/*
 * The protocol of the file's comment, from step 2, over parent `parent` for a
 * call agreeing `numbers` numbers - split_strided's one, split_2d's two - of
 * whose teams this PE sees `mine`: split_strided's one, split_2d's row and
 * column. A round is one reduction over the parent, and a second when any
 * parent PE has SHMEM_THREAD_MULTIPLE, which the first's marks say; every
 * parent PE reads their results alike, and so runs as many.
 */
static int split(const char *routine, struct osoe_team *parent, uint32_t numbers,
                 struct membership *mine, uint32_t mine_count)
{
    osoe_count(OSOE_C_SHMEM_TEAM_SPLIT_CALLS, 1);
    bool member = false;
    for (uint32_t i = 0; i < mine_count; i++) {
        mine[i].block = UINT32_MAX;
        mine[i].set = (struct node_set){ 0 };
        if (mine[i].member) {
            shape(parent, &mine[i]);
            member = true;
        }
    }

    uint32_t agreed[2] = { UINT32_MAX, UINT32_MAX }, found = 0;
    uint32_t taking[2] = { 0, 0 }, count = 0;
    uint64_t marks;
    bool dry;
    for (uint32_t round = 0;; round++) {
        /* A block for every team this PE leads here, before its mask; a claim
         * a round found dry is tried again in the next. A pool found empty
         * makes the round busy, not dry, while a block of it may be about to
         * come back: in the first round, whose claim may have come before a
         * parent PE's destroy just ahead of this call - which gives its blocks
         * back before that PE arrives in the round's reduction, and so before
         * the next round's claim; and while a block is on its way back - a
         * team destroyed, whose members here are past the destroy's sync and
         * still leaving it (node.h, osoe_node_team_returning). */
        dry = false;
        for (uint32_t i = 0; i < mine_count; i++)
            if (leads(&mine[i]) && mine[i].block == UINT32_MAX) {
                mine[i].block = osoe_node_team_claim(&osoe.node);
                dry |= mine[i].block == UINT32_MAX;
            }
        uint64_t *words = osoe_team_split_words(parent);
        if (member) {
            free_mask(words);
        } else {
            for (uint32_t w = 0; w < mask_words; w++)
                words[w] = UINT64_MAX;
        }
        marks = MARK_NOT_BUSY | MARK_NOT_DRY;
        if (dry)
            marks = round == 0 || osoe_node_team_returning(&osoe.node) ? MARK_NOT_DRY
                                                                         : MARK_NOT_BUSY;
        words[mask_words] = marks | (concurrent ? 0 : MARK_NOT_CONCURRENT);
        osoe_team_split_and(routine, parent, mask_words + 1);
        marks = words[mask_words];
        if (!(marks & MARK_NOT_BUSY)) {
            osoe_count(OSOE_C_SHMEM_TEAM_SPLIT_BUSY_ROUNDS, 1);
            continue;
        }
        found = 0;
        for (uint32_t w = 0; w < mask_words && found < numbers; w++)
            for (uint64_t rest = words[w]; rest != 0 && found < numbers; rest &= rest - 1)
                agreed[found++] = w * 64 + (uint32_t)__builtin_ctzll(rest);
        if (!(marks & MARK_NOT_DRY) || found < numbers || (marks & MARK_NOT_CONCURRENT))
            break;
        /* Some parent PE has SHMEM_THREAD_MULTIPLE, and another thread of a
         * member may be in a split of its own that agreed one of these numbers:
         * each member takes its numbers pending - which a member below that
         * level does uncontended - and a second AND says whether every member
         * did (D7). A round lost is run again by every parent PE, with nothing
         * pending. */
        count = 0;
        for (uint32_t i = 0; i < mine_count; i++)
            if (mine[i].member)
                taking[count++] = agreed[mine[i].which];
        bool took = take_pending(routine, parent->number, taking, count);
        words[0] = took;
        osoe_team_split_and(routine, parent, 1);
        if (words[0])
            break;
        if (took)
            give_pending(taking, count);
        osoe_count(OSOE_C_SHMEM_TEAM_SPLIT_LOST_ROUNDS, 1);
    }

    if (!(marks & MARK_NOT_DRY) || found < numbers) {
        if (!(marks & MARK_NOT_DRY)) {
            if (dry)
                pool_dry(routine);
        } else if (parent->me == 0) {
            numbers_used_up(routine, numbers);
        }
        for (uint32_t i = 0; i < mine_count; i++)
            unshape(&mine[i]);
        return 1;
    }

    /* The numbers are this PE's from here (D1): held, and only then no longer
     * pending, so that another split's take finds the one or the other. */
    for (uint32_t i = 0; i < mine_count; i++)
        if (mine[i].member) {
            uint32_t number = agreed[mine[i].which];
            atomic_fetch_or_explicit(&held[number / 64], UINT64_C(1) << (number % 64),
                                     memory_order_seq_cst);
        }
    if (!(marks & MARK_NOT_CONCURRENT))
        give_pending(taking, count);

    struct osoe_team *made[2] = { NULL, NULL };
    for (uint32_t i = 0; i < mine_count; i++)
        if (mine[i].member)
            made[i] = finish(&mine[i], agreed[mine[i].which]);
    /* Every member node's record is published, and every member's counters
     * zeroed, before any member leaves this, and so before any collective on a
     * new team starts (node.h). */
    osoe_team_sync(parent, routine);
    for (uint32_t i = 0; i < mine_count; i++)
        if (made[i]) {
            take_number(made[i]);
            *mine[i].handle = handle_of(made[i]);
        }
    return 0;
}

/*
 * spec §9.4.6. The triplet names parent ranks start + stride x i for i below
 * size; stride may be negative, the members then in reverse order of their
 * parent ranks. A triplet that is not valid - size not positive, or a rank
 * outside the parent, which is what a sequence that would wrap around has -
 * returns nonzero on every parent PE; a stride of 0 with a size other than 1
 * is undefined, and ends the job by name.
 */
OSOE_API int pshmem_team_split_strided(shmem_team_t parent_team, int start, int stride,
                                      int size, const shmem_team_config_t *config,
                                      long config_mask, shmem_team_t *new_team)
{
    static const char routine[] = "shmem_team_split_strided";
    struct osoe_team *parent = osoe_team_of(routine, parent_team);
    if (!new_team)
        osoe_fatal("%s: new_team is a null pointer", routine);
    *new_team = SHMEM_TEAM_INVALID;
    if (!parent)
        return 1;
    int contexts = contexts_requested(routine, "config", config, config_mask);
    if (size < 1)
        return 1;
    if (stride == 0 && size != 1)
        osoe_fatal("%s: stride 0 with size %d; spec §9.4.6 allows a stride of 0 only with a "
                   "size of 1", routine, size);
    int64_t last = (int64_t)start + (int64_t)stride * (size - 1);
    if (start < 0 || (uint32_t)start >= parent->members || last < 0 || last >= parent->members)
        return 1;
    struct membership m = {
        .ranks = { start, stride, (uint32_t)size },
        .num_contexts = contexts,
        .handle = new_team,
    };
    m.member = is_member(&m.ranks, parent->me);
    return split(routine, parent, 1, &m, 1);
}

/*
 * spec §9.4.7. The parent's ranks laid out in rows of xrange - no more than the
 * parent has - and every row a team and every column a team, all of them
 * created by this one call, under two numbers: every row the lowest the call
 * agrees and every column the next, since rows share no PE, nor columns (D2).
 * A PE is in one of each, its x-axis team its row and its y-axis team its
 * column. xrange not positive is undefined, and ends the job by name.
 */
OSOE_API int pshmem_team_split_2d(shmem_team_t parent_team, int xrange,
                                 const shmem_team_config_t *xaxis_config, long xaxis_mask,
                                 shmem_team_t *xaxis_team,
                                 const shmem_team_config_t *yaxis_config, long yaxis_mask,
                                 shmem_team_t *yaxis_team)
{
    static const char routine[] = "shmem_team_split_2d";
    struct osoe_team *parent = osoe_team_of(routine, parent_team);
    if (!xaxis_team || !yaxis_team)
        osoe_fatal("%s: %s is a null pointer", routine,
                   xaxis_team ? "yaxis_team" : "xaxis_team");
    *xaxis_team = SHMEM_TEAM_INVALID;
    *yaxis_team = SHMEM_TEAM_INVALID;
    if (!parent)
        return 1;
    int x_contexts = contexts_requested(routine, "xaxis_config", xaxis_config, xaxis_mask);
    int y_contexts = contexts_requested(routine, "yaxis_config", yaxis_config, yaxis_mask);
    if (xrange < 1)
        osoe_fatal("%s: xrange %d; spec §9.4.7 requires a positive one", routine, xrange);
    uint32_t members = parent->members;
    uint32_t columns = (uint32_t)xrange < members ? (uint32_t)xrange : members;
    uint32_t x = parent->me % columns, y = parent->me / columns;
    uint32_t row_size = members - y * columns < columns ? members - y * columns : columns;
    struct membership m[2] = {
        { .which = 0, .member = true,
          .ranks = { (int64_t)y * columns, 1, row_size },
          .num_contexts = x_contexts, .handle = xaxis_team },
        { .which = 1, .member = true,
          .ranks = { x, columns, (members - x + columns - 1) / columns },
          .num_contexts = y_contexts, .handle = yaxis_team },
    };
    return split(routine, parent, 2, m, 2);
}

/*
 * spec §9.4.8: collective over the team's members (§5.7) - a sync over the
 * team, and then this PE leaves the node's block, its descriptor goes and its
 * number is given back. The team's local lead on a node clears its name's
 * entry in the map as it leaves, and the last of the team's members there to
 * leave the block gives it back to the pool (node.h); the number is given back
 * last of all, once nothing of this PE uses the descriptor any more, since a
 * thread of this PE taking the number in a split of its own takes the
 * descriptor's slot (D1). Nothing of the team is still to arrive here or anywhere (the
 * proposal's "Why giving a number back is safe"). SHMEM_TEAM_INVALID does
 * nothing; a predefined team is undefined, which here means loud, and so is a
 * handle never issued or already destroyed (osoe_team_of).
 */
OSOE_API void pshmem_team_destroy(shmem_team_t team)
{
    if (team == SHMEM_TEAM_INVALID)
        return;
    struct osoe_team *t = osoe_team_of("shmem_team_destroy", team);
    if (t->number < OSOE_PREDEFINED_TEAMS)
        osoe_fatal("shmem_team_destroy: %s is a predefined team and cannot be destroyed",
                   t->name);
    /* The shareable contexts created from the team go first, each with its
     * implicit quiet, so that the sync below orders their last operations as
     * it orders everything else on the team (spec §9.4.8; context.c). */
    osoe_context_team_destroyed(t, "shmem_team_destroy");
    osoe_team_sync(t, "shmem_team_destroy");
    /* This PE's published epoch for the number says nothing any more, rather
     * than the team's last, which osoe-stat would pair with the block when it
     * holds another team (node.h). */
    uint32_t number = t->number;
    atomic_store_explicit(t->published, 0, memory_order_release);
    osoe_node_team_leave(&osoe.node, t->block_index,
                         osoe.bootstrap.local_id == t->lead_local_id);
    forget(t);
    atomic_fetch_and_explicit(&held[number / 64], ~(UINT64_C(1) << (number % 64)),
                              memory_order_seq_cst);
}

/*
 * spec §9.2.3 (shmem_team_ptr): identical to shmem_ptr when the team is
 * SHMEM_TEAM_WORLD, NULL for SHMEM_TEAM_INVALID, and NULL for a PE the calling
 * PE cannot reach with loads and stores.
 */
OSOE_API void *pshmem_team_ptr(shmem_team_t team, const void *dest, int pe)
{
    struct osoe_team *t = osoe_team_of("shmem_team_ptr", team);
    if (!t)
        return NULL;
    int world = osoe_team_to_world(t, pe);
    if (world < 0)
        return NULL;
    return pshmem_ptr(dest, world);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(team_my_pe)
OSOE_ALIAS(team_n_pes)
OSOE_ALIAS(team_get_config)
OSOE_ALIAS(team_translate_pe)
OSOE_ALIAS(team_split_strided)
OSOE_ALIAS(team_split_2d)
OSOE_ALIAS(team_destroy)
OSOE_ALIAS(team_ptr)
