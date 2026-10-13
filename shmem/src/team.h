/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Teams (§5.7, §7.3; spec §9.4).
 *
 * The parameter is on every entry point from the start. The predefined teams
 * exist from init, and since phase 1 step 7's second slice the splits create
 * more (team.c). Contexts, which every point-to-point routine takes beside a
 * PE of a team, are context.h's (phase 1 step 8).
 */
#ifndef OSOE_TEAM_H
#define OSOE_TEAM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <shmem.h>

#include "base/epoch.h"
#include "node.h"

/*
 * Build the predefined teams for this session, and write each one's record
 * into this node's block for its number (node.h) - on the node's lead, whose
 * agent reads the records, so that they are written before that thread exists.
 * Called once by shmem_init after osoe_internode_init and before the agent
 * thread starts.
 */
void osoe_team_start(void);

/* Free what osoe_team_start allocated. Called by the last shmem_finalize, after
 * the agent has been joined and before the node's mappings go. */
void osoe_team_stop(void);

/* The library's internal finalize team (node.h, OSOE_TEAM_FINALIZE): the
 * world's membership on counters of its own. */
struct osoe_team *osoe_team_finalize(void);

/* This process's descriptor for the team it holds under number `number`
 * (enum osoe_team_number), or NULL if it holds none: a number this PE is in no
 * team under, or none that exists. Since the team redesign's step D a number
 * is each PE's own (D1): another PE may hold another team under it. Nothing on
 * the progress thread asks this - it finds a team by its block in the node's
 * pool (node.h), since in its process there may be no descriptor at all. */
struct osoe_team *osoe_team_numbered(uint32_t number);

/* "SHMEM_TEAM_WORLD", "SHMEM_TEAM_SHARED", "the finalize team", or "team <n>",
 * for a message about a team known only by its number. */
const char *osoe_team_label(uint32_t number, char *buffer, size_t size);

struct osoe_team_block;

/* One node's run of a team's ranks (osoe_team_node_runs): the run's first team
 * rank, and the node's position in the team's node set. */
struct osoe_team_node_run {
    uint32_t first_rank;
    uint32_t position;
};

/* How a team's runs, met in team-rank order, meet its node set's positions
 * (osoe_team_node_runs): in their order - the world, any positive stride over a
 * block distribution whose nodes are numbered as the ranks rise - in their
 * reverse - a negative stride over one - or neither, a block map numbered out
 * of the ranks' order. collect's scan of the nodes' totals (P30) needs one of
 * the first two (collectives.c). */
enum osoe_team_runs_order {
    OSOE_TEAM_RUNS_IN_ORDER,
    OSOE_TEAM_RUNS_REVERSED,
    OSOE_TEAM_RUNS_UNORDERED,
};

struct osoe_team {
    const char *name;
    /* enum osoe_team_number: indexes this PE's per-team state. This PE's own
     * since the team redesign's step D: no other team this PE is in has it
     * while this one lives, and another PE may hold another team under it. */
    uint32_t number;
    uint32_t members;               /* PEs in the team */
    /*
     * §5.2's local member count: team members *on this node*, which is not the
     * local PE count and not the team size. It is what the intranode phase
     * counts arrivals against (§5.3), and writing `members` there is the defect
     * a one-node job cannot show - on one node every member is local and the
     * two coincide exactly.
     */
    uint32_t local_members;
    /*
     * This node's members: their local ids, and their team ranks in the same
     * order, `local_members` of each (§5.2). `local_ids[0]` is the local lead,
     * which alone writes its `dest` before a data collective's release (§5.5),
     * and the others are combined into it in list order; `lead_local_id` is the
     * same number. For the predefined teams the list is in local-id order and
     * the lead is local id 0, the node's lead, which the generic internode
     * phase addresses on every node (team.c). A team a split creates lists its
     * members in team-rank order, its lead the one with the lowest team rank,
     * as §5.2 says, and the internode phase finds it through the team's
     * records. What the collectives read to find a member's memory and its
     * place in a result - never a position in the list, whose team rank
     * differs from it as soon as a node's ranks are not the team's first
     * (0,1,2,0,1,0).
     */
    const uint32_t *local_ids;
    const uint32_t *local_ranks;
    uint32_t lead_local_id;
    /*
     * Members on more than one logical node, so the team has §5.3's internode
     * phase: the node's arrival goes to the root and the release comes back,
     * rather than the last local arriver releasing the epoch itself - for a
     * created team, exactly when not all its members are on this node. `nodes`
     * is how many it spans: the size of its node set, the nodes holding at
     * least one member, sorted (§5.2). A created team's is counted by each
     * node's local lead of it, which alone builds the set; its other members
     * have 1 for a team on this node alone and 0, not counted, for one that
     * spans nodes.
     */
    bool internode;
    uint32_t nodes;
    /*
     * Each node's lead by the node's position in the team's node set, `nodes`
     * of them - the rank the generic internode phase puts to there, a created
     * team's member with the lowest team rank on it (§5.2, §5.5). Kept only by
     * each node's local lead of a team a split creates, the one PE per node
     * whose thread runs the data half, which re-roots a broadcast's tree at
     * any node (internode.h, osoe_internode_team_lead); O(the team's nodes)
     * there and NULL everywhere else. The predefined teams span every node,
     * and the job's table of leads is theirs (job.h).
     */
    const uint32_t *node_leads;
    /*
     * Whether every node of the team holds one run of consecutive team ranks,
     * which decides how collect finds its places (collectives.c,
     * collect_varying): found at the team's first collect of differing blocks
     * by its local lead on each node, the one PE there that reads it, and kept
     * there - like `node_leads`, O(the team's nodes) - until the team goes.
     * `node_runs_found` says whether the walk has been made (osoe_team_node_runs);
     * `node_runs` is NULL where some node's members are not one run - a cyclic
     * or plane distribution's - and otherwise each node's run in team-rank
     * order, `nodes` of them, this node's at `own_node_run`, and how they meet
     * the node set's positions in `node_runs_order` (P30).
     */
    bool node_runs_found;
    const struct osoe_team_node_run *node_runs;
    uint32_t own_node_run;
    enum osoe_team_runs_order node_runs_order;
    /*
     * A team of one member: SHMEM_TEAM_SHARED under §8.1's force-remote flag,
     * and any team a split makes of one PE. Its sync has nobody to wait for and
     * touches no shared counter - it must not, for SHARED, since every PE of
     * the node would otherwise be counting on the same node-wide words for
     * teams that are not the same team.
     */
    bool singleton;
    uint32_t me;                    /* this PE's number within it */
    /*
     * Team rank r as a world rank: `world_start` + r x `world_stride` for a team
     * strided in world ranks - the world, a node's block or cyclic ranks, a
     * strided split of either, whose stride is negative when the split's is -
     * and `world_of_rank[r]` for one that is not, such as SHMEM_TEAM_SHARED of
     * a node whose ranks interleave with the others', and its splits (at most
     * the node's PEs). osoe_team_to_world reads these, and nothing asks which
     * team it is to find its shape.
     */
    uint32_t world_start;
    int32_t world_stride;
    const uint32_t *world_of_rank;
    /* spec §9.4.3's num_contexts as the split was given it, 0 by default and
     * for the predefined teams; what shmem_team_get_config returns. */
    int num_contexts;
    /* A created team's `name`: "team 3". */
    char label[16];
    /* A created team's descriptor once destroyed (osoe_team_of): its number
     * stays, to say so, until this PE takes the number again. */
    bool destroyed;
    /* How many times this PE has taken the descriptor's number for a team, the
     * one it holds now counted: a created team's handle carries it (team.c,
     * osoe_team_of), so that a handle of a destroyed team never names the
     * team holding its number since. Not the block's generation (node.h),
     * which is the node's and counts every team the block has held. */
    uint32_t generation;
    /* This node's block for the team (node.h): since step D a block of the
     * node's pool, found through the map by the team's name - its local lead's
     * local id here and its number - after the split's sync; the predefined
     * teams' at the block index of their number. `block_index` is what the
     * internode seam and the progress engine know the team by (internode.h). */
    struct osoe_team_block *block;
    uint32_t block_index;
    uint32_t epoch;                 /* this PE's k for the team (§5.4) */
    uint32_t data_epoch;            /* and its d, data collectives completed */
    /* Where k is published for osoe-stat, beside the block (node.h,
     * osoe_node_published_epoch): osoe_team_publish_epoch writes it. */
    _Atomic uint64_t *published;
};

/* Publish this PE's epoch count `epoch` for `t` (§3.14), with the team's block
 * beside it, a single-writer store (§3.16). */
static inline void osoe_team_publish_epoch(const struct osoe_team *t, uint32_t epoch)
{
    atomic_store_explicit(t->published, OSOE_EPOCH_WORD(t->block_index, epoch),
                          memory_order_release);
}

/*
 * Resolve a team handle. Returns NULL for SHMEM_TEAM_INVALID, which several
 * routines answer with -1 rather than a failure. A created team's handle is
 * its descriptor's address and the descriptor's generation (team.c), accepted
 * while this PE's descriptor for its number is that one at that generation;
 * anything else - a handle the library never issued to this PE, or one of a
 * team it has destroyed, whose number may name another team since - is fatal,
 * naming which: the spec makes it undefined, and it is a program bug worth
 * naming.
 */
struct osoe_team *osoe_team_of(const char *routine, shmem_team_t team);

/* A team-relative PE number as a world rank, or -1 if it is not in the team. */
int osoe_team_to_world(const struct osoe_team *t, int pe);

/*
 * The position of node `node` in `t`'s sorted node set, or UINT32_MAX if the
 * team has no member there: for a team spanning every node the node's number,
 * and for a created one a binary search of its `node_leads`, so only where
 * those are kept - on the team's local lead, which is where a broadcast's
 * root is looked for (collectives.c). O(log N) node lookups.
 */
uint32_t osoe_team_node_index(const struct osoe_team *t, uint32_t node);

/*
 * `t`'s runs of team ranks, one a node in team-rank order (`node_runs`), or
 * NULL where some node's members are not one run of consecutive team ranks.
 * Asked by collect on the team's local lead here, for a team with an internode
 * phase, whose node set that PE keeps. Found at the first call by a walk of the
 * team's ranks in order - each one's world rank (the progression, or
 * `world_of_rank`) and its node (job.h), O(members) node lookups - noting each
 * node's first team rank and its position in the node set, until a node is met
 * again after its run ended; then kept, O(nodes), with the order in which the
 * runs meet the node set's positions (`node_runs_order`). Every lead finds the
 * same answer, from what every PE holds alike: the team's ranks and the job's
 * rank map.
 */
const struct osoe_team_node_run *osoe_team_node_runs(struct osoe_team *t, const char *routine);

#endif /* OSOE_TEAM_H */
