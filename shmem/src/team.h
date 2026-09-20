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

/* The two predefined teams, resolved to the state a collective needs. */
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
    uint32_t me;                    /* this PE's number within it */
    struct osoe_epoch *arrived;     /* the team's own epoch counters (§5.4) */
    struct osoe_epoch *released;
    uint32_t *epoch;                /* this PE's k for the team */
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
