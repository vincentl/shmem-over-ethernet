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
 * Everything that would create a new team or context returns nonzero and
 * assigns the invalid handle. spec §9.5.1 says an unsuccessful context creation
 * "is not treated as an error", and §9.4 says a PE that is not a member of a
 * new team receives SHMEM_TEAM_INVALID, so a program written against the spec
 * already has the branch this takes.
 */
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "collective-tree.h"
#include "export.h"
#include "fatal.h"
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
 * is never found half built. The node's progress thread does not look here: it
 * finds a team by number in the node's block (node.h), since the process it
 * runs in may hold no descriptor for the team at all.
 */
static _Atomic(struct osoe_team *) *numbered;
static uint32_t numbered_count;

/* The predefined teams' local list, every local id in order - beyond what the
 * node and the bootstrap already hold, the one array their shapes need. */
static uint32_t *identity;

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
 * member with the lowest team rank, is the rule for teams created by a split
 * (slice 2), whose leads the internode phase will find through their records.
 */
static void predefined_local_shape(void)
{
    uint32_t count = osoe.bootstrap.local_count;
    identity = malloc(sizeof *identity * count);
    if (!identity)
        osoe_fatal("out of memory describing this node's %u PEs", count);
    for (uint32_t i = 0; i < count; i++)
        identity[i] = i;
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
        t.world_stride = count == 1 ? 1 : stride;
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

/*
 * Write `t`'s record into this node's block for its number and publish it
 * (node.h): what the node's progress thread and internode backend learn the
 * team from, the first time they meet it, rather than from a call made here.
 *
 * For a team spanning several nodes, this node's place in §5.3's k-ary tree
 * over its node set - the nodes holding at least one member, sorted - naming
 * each neighbour node by its lead's rank: for the predefined teams, the rank
 * at its local id 0. For the world and finalize teams that set
 * is every node, and so needs no computation; the list is built anyway,
 * because a split team's is a real computation and the tree should not be able
 * to tell the difference. O(N) once per team, freed at once: the record keeps
 * the O(k) of it the tree needs (§5.2).
 */
static void write_record(struct osoe_team *t, const char *name)
{
    struct osoe_team_record *r = &t->block->record;
    r->local_members = t->local_members;
    r->nodes = t->nodes;
    r->index = 0;
    r->parent = UINT32_MAX;
    r->position = 0;
    r->children = 0;
    snprintf(r->name, sizeof r->name, "%s", name);
    if (t->internode) {
        uint32_t count = t->nodes;
        uint32_t *nodes = malloc(sizeof *nodes * count);
        if (!nodes)
            osoe_fatal("out of memory describing %s's %u nodes", t->name, count);
        for (uint32_t n = 0; n < count; n++)
            nodes[n] = n;
        struct osoe_collective_tree tree;
        if (!osoe_collective_tree(nodes, count, osoe.job.node, osoe.bootstrap.collective_fanout,
                                  &tree))
            osoe_fatal("%s's node set (%u nodes) is not sorted, repeats a node, or leaves out "
                       "node %u, which holds members", t->name, count, osoe.job.node);
        free(nodes);
        r->index = tree.index;
        if (tree.parent != OSOE_COLLECTIVE_TREE_NONE) {
            r->parent = lead_of(tree.parent);
            r->position = (tree.index - 1) % tree.fanout;
        }
        r->children = tree.children;
        for (uint32_t j = 0; j < tree.children; j++)
            r->child[j] = lead_of(tree.child[j]);
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

void osoe_team_start(void)
{
    numbered_count = osoe.bootstrap.max_teams;
    /* calloc: a null pointer in every entry, before any reader exists. */
    numbered = calloc(numbered_count, sizeof *numbered);
    if (!numbered)
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
     * nothing of the node's (team.h).
     */
    if (osoe.bootstrap.local_id == 0) {
        write_record(&world_team, "world");
        if (!shared_team.singleton)
            write_record(&shared_team, "shared");
        write_record(&finalize_team, "finalize");
    }
}

void osoe_team_stop(void)
{
    free(numbered);
    free(identity);
    numbered = NULL;
    numbered_count = 0;
    identity = NULL;
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
    osoe_fatal("%s: %p is not a team this library issued; only SHMEM_TEAM_WORLD and "
               "SHMEM_TEAM_SHARED exist (shmem_team_split_* is not implemented)",
               routine, (void *)team);
}

int osoe_team_to_world(const struct osoe_team *t, int pe)
{
    if (pe < 0 || (uint32_t)pe >= t->members)
        return -1;
    if (t->world_of_rank)
        return (int)t->world_of_rank[pe];
    return (int)(t->world_start + (uint32_t)pe * t->world_stride);
}

/*
 * The world rank `world` as a number in `t`, or -1 if it is not a member: the
 * inverse of the mapping above, by arithmetic for a strided team and by a scan
 * of the table - at most the node's PEs - for one that is not.
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
    if (w < t->world_start || (w - t->world_start) % t->world_stride != 0)
        return -1;
    uint32_t r = (w - t->world_start) / t->world_stride;
    return r < t->members ? (int)r : -1;
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
