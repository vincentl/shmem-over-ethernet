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
 * Everything that would create a new team or context returns nonzero and
 * assigns the invalid handle. spec §9.5.1 says an unsuccessful context creation
 * "is not treated as an error", and §9.4 says a PE that is not a member of a
 * new team receives SHMEM_TEAM_INVALID, so a program written against the spec
 * already has the branch this takes.
 */
#include <stddef.h>

#include <pshmem.h>
#include <shmem.h>

#include "export.h"
#include "fatal.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"
#include "transport.h"

static struct osoe_team world_team, shared_team, finalize_team;

struct osoe_team *osoe_team_finalize(void)
{
    finalize_team = (struct osoe_team){
        .name = "the finalize team",
        .number = OSOE_TEAM_FINALIZE,
        .members = osoe.bootstrap.world_size,
        .local_members = osoe.bootstrap.local_count,
        .internode = osoe.job.node_count > 1,
        .me = osoe.bootstrap.rank,
        .arrived = &osoe.node.hdr->finalize_arrived,
        .released = &osoe.node.hdr->finalize_released,
        .epoch = &osoe.finalize_epoch,
        .published = &osoe.node.slots[osoe.bootstrap.local_id].finalize_epoch,
    };
    return &finalize_team;
}

struct osoe_team *osoe_team_of(const char *routine, shmem_team_t team)
{
    if (team == SHMEM_TEAM_INVALID)
        return NULL;
    if (team == SHMEM_TEAM_WORLD) {
        world_team = (struct osoe_team){
            .name = "SHMEM_TEAM_WORLD",
            .number = OSOE_TEAM_WORLD,
            .members = osoe.bootstrap.world_size,
            /* Every local PE is a member of the world team, so the world's
             * local member count is the node's PE count (§5.2). */
            .local_members = osoe.bootstrap.local_count,
            .internode = osoe.job.node_count > 1,
            .me = osoe.bootstrap.rank,
            .arrived = &osoe.node.hdr->world_arrived,
            .released = &osoe.node.hdr->world_released,
            .epoch = &osoe.world_epoch,
            .published = &osoe.node.slots[osoe.bootstrap.local_id].world_epoch,
        };
        return &world_team;
    }
    if (team == SHMEM_TEAM_SHARED) {
        shared_team = (struct osoe_team){
            .name = "SHMEM_TEAM_SHARED",
            .number = OSOE_TEAM_SHARED,
            .members = osoe.bootstrap.local_count,
            /* SHMEM_TEAM_SHARED is one node by definition (§4.8), so every
             * member is local and it has no internode phase at all. */
            .local_members = osoe.bootstrap.local_count,
            .internode = false,
            .me = osoe.bootstrap.local_id,
            .arrived = &osoe.node.hdr->shared_arrived,
            .released = &osoe.node.hdr->shared_released,
            .epoch = &osoe.shared_epoch,
            .published = &osoe.node.slots[osoe.bootstrap.local_id].shared_epoch,
        };
        return &shared_team;
    }
    osoe_fatal("%s: %p is not a team this library issued; only SHMEM_TEAM_WORLD and "
               "SHMEM_TEAM_SHARED exist (shmem_team_split_* is not implemented)",
               routine, (void *)team);
}

int osoe_team_to_world(const struct osoe_team *t, int pe)
{
    if (pe < 0 || (uint32_t)pe >= t->members)
        return -1;
    if (t == &shared_team)
        return (int)osoe.bootstrap.gtids[pe];
    return pe;                          /* the world team numbers PEs by rank */
}

/* The world rank `world` as a number in `t`, or -1 if it is not a member. */
static int world_to_team(const struct osoe_team *t, int world)
{
    if (world < 0 || (uint32_t)world >= osoe.bootstrap.world_size)
        return -1;
    if (t == &shared_team) {
        int32_t local = osoe.node.local_of_rank[world];
        return local;                   /* -1 when the rank is on another node */
    }
    return world;
}

struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx)
{
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
 * does nothing, and a mask of 0 performs no action. Neither predefined team was
 * created with a configuration, so every field reads as its default.
 */
OSOE_API int pshmem_team_get_config(shmem_team_t team, long config_mask,
                                   shmem_team_config_t *config)
{
    if (!osoe_team_of("shmem_team_get_config", team))
        return 1;
    if (config_mask == 0 || !config)
        return 0;
    if (config_mask & SHMEM_TEAM_NUM_CONTEXTS)
        config->num_contexts = 0;       /* the §9.4.3 default */
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

/* §7.3: plumbed, not implemented. The caller's branch for a non-member PE is
 * the branch it takes here. */
OSOE_API int pshmem_team_split_strided(shmem_team_t parent_team, int start, int stride,
                                      int size, const shmem_team_config_t *config,
                                      long config_mask, shmem_team_t *new_team)
{
    (void)start;
    (void)stride;
    (void)size;
    (void)config;
    (void)config_mask;
    osoe_team_of("shmem_team_split_strided", parent_team);
    if (new_team)
        *new_team = SHMEM_TEAM_INVALID;
    return 1;
}

OSOE_API int pshmem_team_split_2d(shmem_team_t parent_team, int xrange,
                                 const shmem_team_config_t *xaxis_config, long xaxis_mask,
                                 shmem_team_t *xaxis_team,
                                 const shmem_team_config_t *yaxis_config, long yaxis_mask,
                                 shmem_team_t *yaxis_team)
{
    (void)xrange;
    (void)xaxis_config;
    (void)xaxis_mask;
    (void)yaxis_config;
    (void)yaxis_mask;
    osoe_team_of("shmem_team_split_2d", parent_team);
    if (xaxis_team)
        *xaxis_team = SHMEM_TEAM_INVALID;
    if (yaxis_team)
        *yaxis_team = SHMEM_TEAM_INVALID;
    return 1;
}

/* spec §9.4.8: SHMEM_TEAM_INVALID does nothing, and destroying a predefined
 * team is undefined - which here means loud, since no other team can exist and
 * so every call naming one is a program bug. */
OSOE_API void pshmem_team_destroy(shmem_team_t team)
{
    if (team == SHMEM_TEAM_INVALID)
        return;
    osoe_fatal("shmem_team_destroy: %s is a predefined team and cannot be destroyed",
               osoe_team_of("shmem_team_destroy", team)->name);
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
    /* spec §9.5.2: an invalid team yields an invalid context and nonzero. */
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
