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
#include <stdint.h>

#include <shmem.h>

#include "base/epoch.h"
#include "collective-tree.h"

/*
 * Build the predefined teams for this session, and give each one that spans
 * several nodes to the internode backend (internode.h). Called once by
 * shmem_init after osoe_internode_init and before the agent thread starts, so
 * the thread that will release these teams only ever reads them.
 */
void osoe_team_start(void);

/* The library's internal finalize team (node.h, OSOE_TEAM_FINALIZE): the
 * world's membership on counters of its own. */
struct osoe_team *osoe_team_finalize(void);

/* The team with this number (enum osoe_team_number), or NULL if there is none.
 * What internode.h's release upcall resolves its team with. */
struct osoe_team *osoe_team_numbered(uint32_t number);

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
     * Members on more than one logical node, so the team has §5.3's internode
     * phase: the node's arrival goes to the root and the release comes back,
     * rather than the last local arriver releasing the epoch itself.
     */
    bool internode;
    /*
     * A team of one member, which today is only SHMEM_TEAM_SHARED under §8.1's
     * force-remote flag. Its sync has nobody to wait for and touches no shared
     * counter - it must not, since every PE of the node would otherwise be
     * counting on the same node-wide words for teams that are not the same team.
     */
    bool singleton;
    uint32_t me;                    /* this PE's number within it */
    struct osoe_epoch *arrived;     /* the team's own epoch counters (§5.4) */
    struct osoe_epoch *released;
    /* §5.4's third counter, the non-leads' copy-outs of a data collective;
     * NULL on a team that has none (the finalize team). */
    struct osoe_epoch *copied;
    uint32_t *epoch;                /* this PE's k for the team */
    uint32_t *data_epoch;           /* and its d, data collectives completed */
    _Atomic uint32_t *published;    /* where k is published for osoe-stat */

    /*
     * A data collective's internode phase, kept by the local lead only
     * (collectives.c): the tree rooted at the team's first node, as world
     * ranks of the neighbouring leads, found once; and how many pieces have
     * arrived at this lead from its children and come down to it from its
     * parent over the team's life - cumulative, compared for equality (§5.4),
     * since the counts in the internal region are never reset.
     */
    bool lead_tree_known;
    uint32_t lead_parent;           /* the parent lead's rank, or UINT32_MAX at the root */
    uint32_t lead_position;         /* this node among the parent's children */
    uint32_t lead_children;
    uint32_t lead_child[OSOE_COLLECTIVE_FANOUT_MAX];
    uint32_t arrivals_expected;
    uint32_t releases_expected;
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
