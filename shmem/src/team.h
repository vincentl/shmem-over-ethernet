/*
 * Teams and contexts (§7.3, spec §9.4, §9.5).
 *
 * The parameter is on every entry point from the start. The predefined teams
 * exist from init, and since phase 1 step 7's second slice the splits create
 * more (team.c); SHMEM_CTX_DEFAULT is the only context, and everything that
 * would create another returns nonzero, which the spec permits.
 */
#ifndef OSOE_TEAM_H
#define OSOE_TEAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <shmem.h>

#include "base/epoch.h"

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

/* This process's descriptor for team number `number` (enum osoe_team_number),
 * or NULL if it has none: a number this PE is not a member of, one it has
 * destroyed, or none that exists. Nothing on the progress thread asks this -
 * it finds a team by number in the node's block (node.h), since in its
 * process there may be no descriptor at all. */
struct osoe_team *osoe_team_numbered(uint32_t number);

/* "SHMEM_TEAM_WORLD", "SHMEM_TEAM_SHARED", "the finalize team", or "team <n>",
 * for a message about a team known only by its number. */
const char *osoe_team_label(uint32_t number, char *buffer, size_t size);

struct osoe_team_block;

struct osoe_team {
    const char *name;
    uint32_t number;                /* enum osoe_team_number: indexes per-team state */
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
     * rather than the last local arriver releasing the epoch itself. `nodes` is
     * how many it spans: the size of its node set, the nodes holding at least
     * one member, sorted (§5.2).
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
     * stays, to say so. */
    bool destroyed;
    struct osoe_team_block *block;  /* this node's block for `number` (node.h) */
    uint32_t epoch;                 /* this PE's k for the team (§5.4) */
    uint32_t data_epoch;            /* and its d, data collectives completed */
    _Atomic uint32_t *published;    /* where k is published for osoe-stat */
};

/*
 * Resolve a team handle. Returns NULL for SHMEM_TEAM_INVALID, which several
 * routines answer with -1 rather than a failure. A created team's handle is
 * its descriptor's address, accepted while this PE's descriptor for its
 * number is that one; anything else - a handle the library never issued to
 * this PE, or one it has destroyed - is fatal, naming which: the spec makes it
 * undefined, and it is a program bug worth naming.
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
 * Resolve a context handle to the transport context. SHMEM_CTX_DEFAULT is the
 * only one that exists; SHMEM_CTX_INVALID is fatal here, and the routines the
 * spec says must ignore it (fence and quiet, §9.12.1-§9.12.2) test for it
 * before calling.
 */
struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx);

#endif /* OSOE_TEAM_H */
