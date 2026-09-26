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
 * A team's state is found by its number (phase 1 step 7): its block of the
 * node header - §5.4's counters, the arrival handoff, and the record of the
 * team's place on this node that the progress thread reads - its published
 * epoch in each PE's row, and its block of the internal region. What the
 * descriptor holds is the team's shape: this node's members and which of them
 * leads, and its ranks as world ranks (team.h).
 *
 * **Teams are created by splitting a parent** (spec §9.4.6, §9.4.7; phase 1
 * step 7 slice 2, docs/prompts/phase1-step7-slice2-prompt.md), collectively
 * over the parent, in this order:
 *
 *   1. every parent PE checks the arguments alike: the spec's failures return
 *      nonzero on every one of them, and what it leaves undefined ends the job
 *      by name;
 *   2. the parent's rank 0 claims a number for each team the call creates,
 *      with one fetching add on a counter in world PE 0's internal region
 *      (internal-region.h) - numbers are job-wide and never reused within a
 *      session (§5.7), so no late message or straggling reader of a team's
 *      state ever meets another team under its number;
 *   3. a broadcast over the parent carries the first number, or 0 for a claim
 *      past OSOE_MAX_TEAMS, in eight bytes every parent PE borrows from the
 *      symmetric heap for the call, as collect borrows its lengths' room;
 *   4. every member builds its descriptor: its node's members in O(L), and the
 *      team's node set in O(team size), the one step that is not logarithmic,
 *      taken at creation only;
 *   5. each node's local lead of the new team writes the team's record into
 *      its node's block for the number and publishes it (node.h, where the
 *      argument that every reader sees it is written);
 *   6. a sync over the parent, so that no collective on the new team starts
 *      anywhere before every member node's record is published;
 *   7. members get the handle - the descriptor's address - and the others
 *      SHMEM_TEAM_INVALID, every parent PE the same status.
 *
 * A created team's descriptor lives in this process's pool, one per number
 * (osoe_team_of): a handle is accepted while it is the live descriptor for its
 * number, and one never issued or already destroyed is named, never
 * dereferenced. Destroying a team is a sync over its members, then its
 * descriptor's arrays are freed and its number forgotten here; the number, and
 * its blocks and records on every node, stay, since nothing reuses them.
 *
 * Contexts other than the default are not created: shmem_ctx_create and
 * shmem_team_create_ctx return nonzero and assign SHMEM_CTX_INVALID, which
 * spec §9.5.1 permits - an unsuccessful context creation "is not treated as an
 * error" - so a program written against the spec already has the branch this
 * takes.
 */
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
#include "collective-tree.h"
#include "collectives.h"
#include "export.h"
#include "fatal.h"
#include "internal-region.h"
#include "internode.h"
#include "job.h"
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
 * OSOE_PREDEFINED_TEAMS on, `created_count` of them. A created team's handle is
 * its descriptor's address here, which is what lets osoe_team_of tell a handle
 * it issued from one it did not, and a live one from a destroyed one, without
 * reading through either: a handle is a descriptor only if it points into this
 * array, and live only if `numbered` still holds it. A number is never reused,
 * so neither is its descriptor: destroying the team frees the descriptor's
 * arrays and leaves its number, to say which team the handle was. Untouched
 * entries cost address space, not memory.
 */
static struct osoe_team *created;
static uint32_t created_count;

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
    const struct osoe_node *n = &osoe.node;
    uint32_t count = osoe.bootstrap.local_count;
    identity = malloc(sizeof *identity * count);
    ranks_ascending = malloc(sizeof *ranks_ascending * count);
    if (!identity || !ranks_ascending)
        osoe_fatal("out of memory describing this node's %u PEs", count);
    for (uint32_t i = 0; i < count; i++) {
        identity[i] = i;
        ranks_ascending[i] = n->local_block ? n->first_rank + i : n->sorted_rank[i];
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
 * record). O(N) for a team of N nodes; built once, when the team is, and
 * dropped once its record is written - but for the leads, which a created
 * team's local lead keeps (team.h, `node_leads`).
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

/*
 * A created team's node set (decision 4 of the slice's brief): a walk of the
 * team's ranks, mapping each to its node, and per node its member with the
 * lowest team rank for the lead.
 *
 * The walk goes in ascending world rank - team-rank order, or its reverse when
 * a negative stride makes the team's world ranks descend - and a block
 * distribution's nodes then come in ascending order too, each a run of
 * members: the set is the runs, and each run's lead its member of lowest team
 * rank, the first met, or the last when the walk goes backwards. O(size) node
 * lookups (each O(log R), job.h) and O(nodes) memory. A team whose ranks visit
 * a node twice or out of order - a cyclic distribution's, an interleaved
 * map's - is walked once more, in team-rank order, into a table of the job's
 * N nodes, where the first member met on each node is its lead: O(size)
 * lookups again, O(N) words for the moment it takes, and O(N) to read the set
 * out of it.
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
    uint32_t *lead_on = malloc(sizeof *lead_on * nodes);
    if (!lead_on)
        osoe_fatal("out of memory finding %s's nodes among the job's %u", t->name, nodes);
    memset(lead_on, 0xff, sizeof *lead_on * nodes);
    count = 0;
    for (uint32_t rank = 0; rank < members; rank++) {
        uint32_t node = node_of_member(t, rank, &world);
        if (lead_on[node] == UINT32_MAX) {
            lead_on[node] = world;
            count++;
        }
    }
    node_set_allocate(set, count, t->name);
    for (uint32_t node = 0, i = 0; node < nodes; node++)
        if (lead_on[node] != UINT32_MAX) {
            set->nodes[i] = node;
            set->leads[i] = lead_on[node];
            i++;
        }
    free(lead_on);
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
    struct osoe_team_record *r = &osoe_node_team(&osoe.node, t->number)->record;
    r->local_members = t->local_members;
    r->nodes = t->nodes;
    r->index = 0;
    r->parent = UINT32_MAX;
    r->position = 0;
    r->children = 0;
    snprintf(r->name, sizeof r->name, "%s", name);
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
    /* Then the node's high-water mark of published numbers (node.h), raised
     * past this one and never lowered: a record another process writes may
     * have raised it further first. A compare-and-swap, seq_cst as every one
     * is (§3.16), which releases the record with it. */
    _Atomic uint32_t *high = &osoe.node.hdr->team_numbers_published;
    uint32_t seen = atomic_load_explicit(high, memory_order_acquire);
    while (seen < t->number + 1 &&
           !atomic_compare_exchange_strong_explicit(high, &seen, t->number + 1,
                                                    memory_order_seq_cst, memory_order_acquire))
        ;
}

/* Bind a descriptor to its number's block on this node and this PE's
 * published epoch for it, and make it findable by the number. */
static void take_number(struct osoe_team *t)
{
    t->block = osoe_node_team(&osoe.node, t->number);
    t->published = osoe_node_published_epoch(&osoe.node, osoe.bootstrap.local_id, t->number);
    atomic_store_explicit(&numbered[t->number], t, memory_order_release);
}

/* The number of the created team whose descriptor is at `team`, or UINT32_MAX
 * if it is not one of the pool's: a comparison of addresses, never a read. */
static uint32_t created_number(shmem_team_t team)
{
    uintptr_t at = (uintptr_t)team, base = (uintptr_t)created;
    size_t size = sizeof *created;
    if (!created || at < base || at - base >= (uintptr_t)created_count * size ||
        (at - base) % size != 0)
        return UINT32_MAX;
    return OSOE_PREDEFINED_TEAMS + (uint32_t)((at - base) / size);
}

/* A created team gone from this process: its number is forgotten, its arrays
 * freed, and the descriptor left saying which number it was (osoe_team_of). */
static void forget(struct osoe_team *t)
{
    uint32_t number = t->number;
    atomic_store_explicit(&numbered[number], NULL, memory_order_release);
    free((void *)t->local_ids);
    free((void *)t->local_ranks);
    free((void *)t->world_of_rank);
    free((void *)t->node_leads);
    *t = (struct osoe_team){ .number = number, .destroyed = true };
}

void osoe_team_start(void)
{
    numbered_count = osoe.bootstrap.max_teams;
    created_count = numbered_count - OSOE_PREDEFINED_TEAMS;
    /* calloc: a null pointer in every entry, before any reader exists. */
    numbered = calloc(numbered_count, sizeof *numbered);
    created = calloc(created_count, sizeof *created);
    if (!numbered || !created)
        osoe_fatal("out of memory for %u teams", numbered_count);
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
    free(identity);
    free(ranks_ascending);
    created = NULL;
    created_count = 0;
    numbered = NULL;
    numbered_count = 0;
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
    uint32_t number = created_number(team);
    if (number == UINT32_MAX)
        osoe_fatal("%s: %p is not a team this library issued", routine, (void *)team);
    struct osoe_team *t = &created[number - OSOE_PREDEFINED_TEAMS];
    if (atomic_load_explicit(&numbered[number], memory_order_acquire) != t)
        osoe_fatal("%s: %p is team %u, which %s", routine, (void *)team, number,
                   t->destroyed ? "this PE has destroyed" : "this PE was never given");
    return t;
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

uint32_t osoe_internode_team_lead(uint32_t team, uint32_t index)
{
    const struct osoe_team *t = osoe_team_numbered(team);
    if (!t || index >= t->nodes)
        return UINT32_MAX;
    if (t->node_leads)
        return t->node_leads[index];
    return team < OSOE_PREDEFINED_TEAMS ? osoe_job_node_lead(&osoe.job, index) : UINT32_MAX;
}

const uint32_t *osoe_internode_team_local_ranks(uint32_t team, uint32_t *count)
{
    const struct osoe_team *t = osoe_team_numbered(team);
    *count = t ? t->local_members : 0;
    if (!t)
        return NULL;
    /* The world's and finalize team's are listed in local-id order, which a
     * hand-given order need not make ascending; every other team's list is
     * ascending already - a created team's in team-rank order, SHARED's its
     * local ids. */
    if (team == OSOE_TEAM_WORLD || team == OSOE_TEAM_FINALIZE)
        return ranks_ascending;
    return t->local_ranks;
}

struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx)
{
    osoe_require_initialized(routine);
    if (ctx == SHMEM_CTX_DEFAULT)
        return osoe_transport_context_default();
    if (ctx == SHMEM_CTX_INVALID)
        osoe_fatal("%s: SHMEM_CTX_INVALID is not a context this operation may use",
                   routine);
    osoe_fatal("%s: %p is not a context this library issued; only SHMEM_CTX_DEFAULT "
               "exists (shmem_ctx_create is not implemented)", routine, (void *)ctx);
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

/* One team of those a split creates, as this PE sees it. */
struct membership {
    uint32_t offset;                    /* its number, less the call's first */
    bool member;                        /* whether this PE is in it */
    struct progression ranks;
    int num_contexts;                   /* spec §9.4.3, as this PE was given it */
    shmem_team_t *handle;               /* where its handle goes */
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
 * No team number is left for a call creating `count` teams (§5.7), said once,
 * by the parent's rank 0, which made the claim: the split fails on every PE of
 * its parent, as spec §9.4.6 says a team that cannot be created does.
 */
static void out_of_numbers(const char *routine, uint32_t count, uint64_t claimed)
{
    fprintf(stderr, "osoe: PE %u: %s: no team numbers are left for the %u team%s this call "
            "creates: OSOE_MAX_TEAMS is %u, %u of them predefined, %llu claimed before this "
            "call, and a number is never reused within a session (design §5.7); the split "
            "returns nonzero on every PE of its parent team\n", osoe.bootstrap.rank, routine,
            count, count == 1 ? "" : "s", osoe.bootstrap.max_teams, OSOE_PREDEFINED_TEAMS,
            (unsigned long long)claimed);
}

/*
 * The parent's rank 0 claims the numbers of the `count` teams its call creates
 * (step 2 of the file's comment): one blocking fetching add on world PE 0's
 * counter (internal-region.h). The first is OSOE_PREDEFINED_TEAMS past the
 * value fetched - or 0, which no created team has, when the claim would reach
 * OSOE_MAX_TEAMS: the numbers stay counted, as every number claimed does.
 */
static uint64_t claim(const char *routine, uint32_t count)
{
    uint32_t max = osoe.bootstrap.max_teams;
    uint64_t counter = osoe_internal_team_counter_offset(osoe.bootstrap.collective_fanout,
                                                         osoe.bootstrap.collective_pipeline, max);
    uint64_t claimed = osoe_transport_rmw(osoe_transport_context_default(), 0,
                                          OSOE_REGION_INTERNAL, counter, OSOE_RMW_ADD,
                                          sizeof(uint64_t), count, 0, OSOE_TRANSPORT_FETCH);
    if (claimed <= max && OSOE_PREDEFINED_TEAMS + claimed + count <= max)
        return OSOE_PREDEFINED_TEAMS + claimed;
    out_of_numbers(routine, count, claimed);
    return 0;
}

/*
 * This PE's descriptor for team `number`, which its call has just claimed and
 * whose members are the parent ranks `p` (steps 4 and 5 of the file's
 * comment): its world mapping, composed from the parent's; its members on this
 * node in team-rank order, by testing each local PE, O(L); its node set, once,
 * O(size); and, on its local lead here, the record, written and published,
 * and the node set's leads kept.
 */
static struct osoe_team *build(const struct osoe_team *parent, uint32_t number,
                               const struct progression *p, int num_contexts)
{
    struct osoe_team *t = &created[number - OSOE_PREDEFINED_TEAMS];
    *t = (struct osoe_team){
        .number = number,
        .members = p->size,
        .singleton = p->size == 1,
        .me = p->stride == 0 ? 0 : (uint32_t)(((int64_t)parent->me - p->start) / p->stride),
        .num_contexts = num_contexts,
    };
    snprintf(t->label, sizeof t->label, "team %u", number);
    t->name = t->label;

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
    const struct osoe_node *n = &osoe.node;
    uint32_t capacity = t->world_of_rank ? t->members : n->local_count;
    uint32_t *ids = malloc(sizeof *ids * capacity), *ranks = malloc(sizeof *ranks * capacity);
    if (!ids || !ranks)
        osoe_fatal("out of memory for %s's members on this node", t->name);
    uint32_t count = 0;
    if (t->world_of_rank) {
        for (uint32_t r = 0; r < t->members; r++) {
            int32_t local = osoe_node_local_id(n, t->world_of_rank[r]);
            if (local >= 0) {
                ids[count] = (uint32_t)local;
                ranks[count++] = r;
            }
        }
    } else {
        for (uint32_t i = 0; i < n->local_count; i++) {
            uint32_t world = n->local_block ? n->first_rank + i : n->sorted_rank[i];
            int r = world_to_team(t, (int)world);
            if (r >= 0) {
                ids[count] = n->local_block ? i : n->sorted_local[i];
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

    /* The node set: this node alone for a split of a parent on one node, which
     * is every split of SHMEM_TEAM_SHARED; otherwise found by walking. */
    struct node_set set = { 0 };
    if (parent->nodes == 1) {
        node_set_allocate(&set, 1, t->name);
        set.nodes[0] = osoe.job.node;
        set.leads[0] = osoe.bootstrap.gtids[t->lead_local_id];
    } else {
        created_node_set(t, &set);
    }
    t->nodes = set.count;
    t->internode = set.count > 1;
    if (osoe.bootstrap.local_id == t->lead_local_id) {
        char name[16];
        snprintf(name, sizeof name, "%u", number);
        write_record(t, name, &set);
        if (t->internode) {
            t->node_leads = set.leads;
            set.leads = NULL;
        }
    }
    node_set_free(&set);
    return t;
}

/*
 * The protocol of the file's comment, from step 2, over parent `parent` - its
 * handle `parent_handle` - for a call creating `count` teams, of which this PE
 * sees `mine`: split_strided's one, split_2d's row and column.
 */
static int split(const char *routine, shmem_team_t parent_handle, struct osoe_team *parent,
                 uint32_t count, const struct membership *mine, uint32_t mine_count)
{
    /* More teams than there are numbers fails before anything is claimed:
     * every parent PE computes the count alike, as it checked the arguments. */
    if (count > osoe.bootstrap.max_teams - OSOE_PREDEFINED_TEAMS) {
        if (parent->me == 0)
            out_of_numbers(routine, count, 0);
        return 1;
    }

    /* The room the first number travels in, at the same place on every parent
     * PE: the allocator is a function of its calls alone (§4.4), and a take and
     * its give leave it as it was. A heap without eight bytes to spare answers
     * nonzero on every parent PE alike, before anything is claimed. */
    uint64_t offset;
    int taken = osoe_allocator_take(&osoe.allocator, sizeof(uint64_t), sizeof(uint64_t), 0,
                                    &offset);
    if (taken < 0)
        osoe_fatal("%s: out of memory for the symmetric heap's own records", routine);
    if (taken > 0)
        return 1;
    uint64_t *word = (uint64_t *)((unsigned char *)osoe.node.heap_of[osoe.bootstrap.local_id] +
                                  offset);
    if (parent->me == 0)
        *word = claim(routine, count);
    osoe_broadcast(routine, parent_handle, word, word, 1, sizeof *word, 0, false);
    uint64_t first = *word;
    osoe_allocator_give(&osoe.allocator, offset);
    if (first == 0)
        return 1;

    struct osoe_team *made[2] = { NULL, NULL };
    for (uint32_t i = 0; i < mine_count; i++)
        if (mine[i].member)
            made[i] = build(parent, (uint32_t)first + mine[i].offset, &mine[i].ranks,
                            mine[i].num_contexts);
    /* Every member node's record is published before any member leaves this,
     * and so before any collective on a new team starts (node.h). */
    osoe_team_sync(parent, routine);
    for (uint32_t i = 0; i < mine_count; i++)
        if (made[i]) {
            take_number(made[i]);
            *mine[i].handle = (shmem_team_t)made[i];
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
    return split(routine, parent_team, parent, 1, &m, 1);
}

/*
 * spec §9.4.7. The parent's ranks laid out in rows of xrange - no more than the
 * parent has - and every row a team and every column a team, all of them
 * created by this one call: rows first, then columns, in the numbers it
 * claims. A PE is in one of each, its x-axis team its row and its y-axis team
 * its column. xrange not positive is undefined, and ends the job by name.
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
    uint32_t rows = (members + columns - 1) / columns;
    uint32_t x = parent->me % columns, y = parent->me / columns;
    uint32_t row_size = members - y * columns < columns ? members - y * columns : columns;
    struct membership m[2] = {
        { .offset = y, .member = true,
          .ranks = { (int64_t)y * columns, 1, row_size },
          .num_contexts = x_contexts, .handle = xaxis_team },
        { .offset = rows + x, .member = true,
          .ranks = { x, columns, (members - x + columns - 1) / columns },
          .num_contexts = y_contexts, .handle = yaxis_team },
    };
    return split(routine, parent_team, parent, rows + columns, m, 2);
}

/*
 * spec §9.4.8: collective over the team's members (§5.7) - a sync over the
 * team, and then this PE's descriptor goes. The number stays taken, and the
 * team's blocks and records stay on every node: a late reader or a late
 * message meets the team it was for, never another. SHMEM_TEAM_INVALID does
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
    osoe_team_sync(t, "shmem_team_destroy");
    forget(t);
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

/* ---- contexts (§9.5) --------------------------------------------------- */

OSOE_API int pshmem_ctx_create(long options, shmem_ctx_t *ctx)
{
    (void)options;
    if (ctx)
        *ctx = SHMEM_CTX_INVALID;
    return 1;
}

OSOE_API int pshmem_team_create_ctx(shmem_team_t team, long options, shmem_ctx_t *ctx)
{
    (void)options;
    /* spec §9.5.2: an invalid team yields an invalid context and nonzero; so,
     * here, does every other, contexts beyond the default not being created. */
    osoe_team_of("shmem_team_create_ctx", team);
    if (ctx)
        *ctx = SHMEM_CTX_INVALID;
    return 1;
}

/*
 * spec §9.5.3: SHMEM_CTX_INVALID does nothing. The default context is not one
 * the program created, so destroying it is a bug this names rather than a quiet
 * teardown of every operation the program has left.
 */
OSOE_API void pshmem_ctx_destroy(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_INVALID)
        return;
    if (ctx == SHMEM_CTX_DEFAULT)
        osoe_fatal("shmem_ctx_destroy: SHMEM_CTX_DEFAULT was not created by "
                   "shmem_ctx_create and cannot be destroyed");
    osoe_fatal("shmem_ctx_destroy: %p is not a context this library issued",
               (void *)ctx);
}

/* spec §9.5.4: the default context belongs to the world team. */
OSOE_API int pshmem_ctx_get_team(shmem_ctx_t ctx, shmem_team_t *team)
{
    if (ctx == SHMEM_CTX_INVALID) {
        if (team)
            *team = SHMEM_TEAM_INVALID;
        return 1;
    }
    osoe_ctx_of("shmem_ctx_get_team", ctx);
    if (team)
        *team = SHMEM_TEAM_WORLD;
    return 0;
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
OSOE_ALIAS(ctx_create)
OSOE_ALIAS(team_create_ctx)
OSOE_ALIAS(ctx_destroy)
OSOE_ALIAS(ctx_get_team)
