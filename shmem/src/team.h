/*
 * Teams and contexts (§7.3, spec §9.4, §9.5).
 *
 * The parameter is on every entry point from the start; only
 * SHMEM_TEAM_WORLD, SHMEM_TEAM_SHARED and SHMEM_CTX_DEFAULT exist. Everything
 * that would create a new one returns nonzero, which the spec permits.
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
 * or NULL if it has none: a number this PE is not a member of, or none that
 * exists. Nothing on the progress thread asks this - it finds a team by
 * number in the node's block (node.h), since in its process there may be no
 * descriptor at all. */
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
     * This node's members, in team-rank order: their local ids and their team
     * ranks, `local_members` of each (§5.2). `local_ids[0]` is the local lead,
     * the member with the lowest team rank, which alone writes its `dest` before
     * a data collective's release (§5.5); `lead_local_id` is the same number.
     * What the collectives read to find a member's memory and its place in a
     * result - never a local id's position, which differs from its team rank
     * as soon as a node's ranks are not the team's first (0,1,2,0,1,0).
     */
    const uint32_t *local_ids;
    const uint32_t *local_ranks;
    uint32_t lead_local_id;
    /*
     * Members on more than one logical node, so the team has §5.3's internode
     * phase: the node's arrival goes to the root and the release comes back,
     * rather than the last local arriver releasing the epoch itself. `nodes` is
     * how many it spans; it spans every node when that is the job's count, and
     * then a node's place in its node set is the node's number.
     */
    bool internode;
    uint32_t nodes;
    /*
     * A team of one member, which today is only SHMEM_TEAM_SHARED under §8.1's
     * force-remote flag. Its sync has nobody to wait for and touches no shared
     * counter - it must not, since every PE of the node would otherwise be
     * counting on the same node-wide words for teams that are not the same team.
     */
    bool singleton;
    uint32_t me;                    /* this PE's number within it */
    /*
     * Team rank r as a world rank: `world_start` + r x `world_stride` for a team
     * strided in world ranks - the world, a node's block or cyclic ranks, a
     * strided split of either - and `world_of_rank[r]` for one that is not,
     * such as SHMEM_TEAM_SHARED of a node whose ranks interleave with the
     * others' (at most the node's PEs). osoe_team_to_world reads these, and
     * nothing asks which team it is to find its shape.
     */
    uint32_t world_start;
    uint32_t world_stride;
    const uint32_t *world_of_rank;
    struct osoe_team_block *block;  /* this node's block for `number` (node.h) */
    uint32_t epoch;                 /* this PE's k for the team (§5.4) */
    uint32_t data_epoch;            /* and its d, data collectives completed */
    _Atomic uint32_t *published;    /* where k is published for osoe-stat */
};

/*
 * Resolve a team handle. Returns NULL for SHMEM_TEAM_INVALID, which several
 * routines answer with -1 rather than a failure, and is fatal for a handle that
 * is neither predefined nor invalid - the spec makes that undefined, and a
 * handle the library never issued is a program bug worth naming.
 */
struct osoe_team *osoe_team_of(const char *routine, shmem_team_t team);

/* A team-relative PE number as a world rank, or -1 if it is not in the team. */
int osoe_team_to_world(const struct osoe_team *t, int pe);

/*
 * Resolve a context handle to the transport context. SHMEM_CTX_DEFAULT is the
 * only one that exists; SHMEM_CTX_INVALID is fatal here, and the routines the
 * spec says must ignore it (fence and quiet, §9.12.1-§9.12.2) test for it
 * before calling.
 */
struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx);

#endif /* OSOE_TEAM_H */
