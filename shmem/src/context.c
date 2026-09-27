/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Communication contexts (spec §9.5; design §4.6, §4.11, §7.3; phase 1 step
 * 8 slice 1): shmem_ctx_create, shmem_team_create_ctx, shmem_ctx_destroy and
 * shmem_ctx_get_team, and the resolution every routine taking a handle goes
 * through (context.h).
 *
 * The thread level stays at most SHMEM_THREAD_SERIALIZED, so every context is
 * serial (§4.11, "Context modes"): one call on it at a time, which the
 * application guarantees. The options are recorded and mean what §4.11 says
 * they will: SERIALIZED and PRIVATE choose the serial mode a context has
 * anyway below MULTIPLE, PRIVATE also records its creating thread, which
 * alone may destroy it (spec §9.5.3), and NOSTORE lets fence and quiet on the
 * context skip the fence that orders the calling thread's stores (§4.6).
 *
 * A created context is destroyed by the program, by the destroy of the team
 * it was created from (spec §9.4.8, for a shareable one), or by the last
 * shmem_finalize; each is the implicit quiet of §9.5.3 and then the release
 * of its transport state and its block.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "context.h"
#include "export.h"
#include "fatal.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"
#include "transport.h"

struct osoe_context {
    struct osoe_transport_context *transport;
    /* The team it was created from, as a handle and as the descriptor the
     * handle resolved to at creation: SHMEM_TEAM_WORLD for the default
     * context and shmem_ctx_create's, whose PEs are world ranks already. */
    shmem_team_t team;
    const struct osoe_team *descriptor;
    long options;
    pthread_t creator;              /* SHMEM_CTX_PRIVATE: the creating thread */
    bool live;
};

/*
 * Every context this PE may have at once, OSOE_MAX_CONTEXTS elements, the
 * default context element 0; a handle is an element's address. A destroyed
 * element serves a later creation. The transport keeps a pool of the same
 * size for its part, so an element is free exactly when the transport can
 * make a context for it.
 */
static struct osoe_context *pool;
static uint32_t pool_count;
/* Held while an element is claimed or given back, so that two threads creating
 * contexts at once (spec §9.5: creation is not collective) take different
 * ones; never across an operation or a wait. */
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static const long known_options = SHMEM_CTX_SERIALIZED | SHMEM_CTX_PRIVATE | SHMEM_CTX_NOSTORE;

void osoe_context_start(void)
{
    pool_count = osoe.bootstrap.max_contexts;
    pool = calloc(pool_count, sizeof *pool);
    if (!pool)
        osoe_fatal("out of memory for %u contexts", pool_count);
    pool[0] = (struct osoe_context){ .transport = osoe_transport_context_default(),
                                     .team = SHMEM_TEAM_WORLD, .live = true };
}

void osoe_context_stop(void)
{
    free(pool);
    pool = NULL;
    pool_count = 0;
}

/* The element a handle names, or NULL: SHMEM_CTX_DEFAULT is element 0, and
 * any other handle must be an element's address. `live` is the caller's to
 * check, so that a destroyed handle can be named as such. */
static struct osoe_context *element(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_DEFAULT)
        return &pool[0];
    uintptr_t at = (uintptr_t)ctx, base = (uintptr_t)pool;
    if (at < base || (at - base) % sizeof *pool != 0 || (at - base) / sizeof *pool >= pool_count)
        return NULL;
    return &pool[(at - base) / sizeof *pool];
}

static struct osoe_context *resolve(const char *routine, shmem_ctx_t ctx)
{
    osoe_require_initialized(routine);
    if (ctx == SHMEM_CTX_INVALID)
        osoe_fatal("%s: SHMEM_CTX_INVALID is not a context this operation may use", routine);
    struct osoe_context *c = element(ctx);
    if (!c)
        osoe_fatal("%s: %p is not a context this library issued", routine, (void *)ctx);
    if (!c->live)
        osoe_fatal("%s: context %p has been destroyed", routine, (void *)ctx);
    return c;
}

struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx)
{
    return resolve(routine, ctx)->transport;
}

/* `pe` in the context's team as a world rank, or fatal by name. */
static int translate(const char *routine, const struct osoe_context *c, int pe)
{
    if (c->team == SHMEM_TEAM_WORLD) {
        osoe_check_pe(routine, pe);
        return pe;
    }
    int world = osoe_team_to_world(c->descriptor, pe);
    if (world < 0)
        osoe_fatal("%s: PE %d is not in 0..%u of %s, the team of the context", routine, pe,
                   c->descriptor->members - 1, c->descriptor->name);
    return world;
}

struct osoe_transport_context *osoe_ctx_resolve(const char *routine, shmem_ctx_t ctx, int *pe)
{
    struct osoe_context *c = resolve(routine, ctx);
    *pe = translate(routine, c, *pe);
    return c->transport;
}

struct osoe_transport_context *osoe_ctx_resolve_list(const char *routine, shmem_ctx_t ctx,
                                                     const int *pes, size_t npes, int *world)
{
    struct osoe_context *c = resolve(routine, ctx);
    for (size_t i = 0; i < npes; i++)
        world[i] = translate(routine, c, pes[i]);
    return c->transport;
}

/* The transport's spelling of the options (transport.h): and, under
 * SHMEM_THREAD_MULTIPLE, a context neither PRIVATE nor SERIALIZED is one
 * several threads may use at once (§4.11's modes). */
static unsigned transport_options(long options)
{
    unsigned t = 0;
    int level = SHMEM_THREAD_SINGLE;
    pshmem_query_thread(&level);
    if (level == SHMEM_THREAD_MULTIPLE && !(options & (SHMEM_CTX_PRIVATE | SHMEM_CTX_SERIALIZED)))
        t |= OSOE_TRANSPORT_CONTEXT_CONCURRENT;
    if (options & SHMEM_CTX_SERIALIZED)
        t |= OSOE_TRANSPORT_CONTEXT_SERIALIZED;
    if (options & SHMEM_CTX_PRIVATE)
        t |= OSOE_TRANSPORT_CONTEXT_PRIVATE;
    if (options & SHMEM_CTX_NOSTORE)
        t |= OSOE_TRANSPORT_CONTEXT_NOSTORE;
    return t;
}

/*
 * spec §9.5.1 and §9.5.2: not collective; zero and a handle, or nonzero and
 * SHMEM_CTX_INVALID when no context can be made now - the pool is full, or
 * memory is short - which "is not treated as an error". An option the spec
 * does not define is a program bug, named. A team's num_contexts is recorded
 * at its split and not reserved against: §9.5.2 lets a creation fail
 * regardless of it.
 */
static int create(const char *routine, shmem_team_t team, const struct osoe_team *descriptor,
                  long options, shmem_ctx_t *ctx)
{
    osoe_require_initialized(routine);
    if (options & ~known_options)
        osoe_fatal("%s: options %#lx include bits no option of spec §9.5 has", routine, options);
    if (!ctx)
        osoe_fatal("%s: ctx is a null pointer", routine);
    *ctx = SHMEM_CTX_INVALID;
    pthread_mutex_lock(&pool_mutex);
    int status = 1;
    for (uint32_t i = 1; i < pool_count; i++) {
        struct osoe_context *c = &pool[i];
        if (c->live)
            continue;
        struct osoe_transport_context *transport =
            osoe_transport_context_create(transport_options(options));
        if (transport) {
            *c = (struct osoe_context){ .transport = transport, .team = team,
                                        .descriptor = descriptor, .options = options,
                                        .creator = pthread_self(), .live = true };
            *ctx = (shmem_ctx_t)c;
            status = 0;
        }
        break;
    }
    pthread_mutex_unlock(&pool_mutex);
    return status;
}

OSOE_API int pshmem_ctx_create(long options, shmem_ctx_t *ctx)
{
    return create("shmem_ctx_create", SHMEM_TEAM_WORLD, NULL, options, ctx);
}

OSOE_API int pshmem_team_create_ctx(shmem_team_t team, long options, shmem_ctx_t *ctx)
{
    /* spec §9.5.2: an invalid team yields an invalid context and nonzero. */
    const struct osoe_team *t = osoe_team_of("shmem_team_create_ctx", team);
    if (!t) {
        if (ctx)
            *ctx = SHMEM_CTX_INVALID;
        return 1;
    }
    return create("shmem_team_create_ctx", team, t, options, ctx);
}

static void destroy(struct osoe_context *c, const char *routine)
{
    osoe_transport_context_destroy(c->transport, routine);
    pthread_mutex_lock(&pool_mutex);
    *c = (struct osoe_context){ 0 };
    pthread_mutex_unlock(&pool_mutex);
}

/*
 * spec §9.5.3: the implicit quiet, then the context goes. SHMEM_CTX_INVALID
 * does nothing. The default context is not one the program created, so
 * destroying it is a bug this names rather than a quiet teardown of every
 * operation the program has left; a private context is its creator's to
 * destroy.
 */
OSOE_API void pshmem_ctx_destroy(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_INVALID)
        return;
    struct osoe_context *c = resolve("shmem_ctx_destroy", ctx);
    if (c == &pool[0])
        osoe_fatal("shmem_ctx_destroy: SHMEM_CTX_DEFAULT was not created by "
                   "shmem_ctx_create and cannot be destroyed");
    if ((c->options & SHMEM_CTX_PRIVATE) && !pthread_equal(c->creator, pthread_self()))
        osoe_fatal("shmem_ctx_destroy: context %p was created with SHMEM_CTX_PRIVATE by "
                   "another thread, which alone may destroy it (spec §9.5.3)", (void *)ctx);
    destroy(c, "shmem_ctx_destroy");
}

/* spec §9.5.4: the team the context was created from; the world team for the
 * default context and shmem_ctx_create's. */
OSOE_API int pshmem_ctx_get_team(shmem_ctx_t ctx, shmem_team_t *team)
{
    if (ctx == SHMEM_CTX_INVALID) {
        if (team)
            *team = SHMEM_TEAM_INVALID;
        return 1;
    }
    struct osoe_context *c = resolve("shmem_ctx_get_team", ctx);
    if (!team)
        osoe_fatal("shmem_ctx_get_team: team is a null pointer");
    *team = c->team;
    return 0;
}

void osoe_context_team_destroyed(const struct osoe_team *team, const char *routine)
{
    for (uint32_t i = 1; i < pool_count; i++) {
        struct osoe_context *c = &pool[i];
        if (!c->live || c->descriptor != team)
            continue;
        if (c->options & SHMEM_CTX_PRIVATE)
            osoe_fatal("%s: context %p, created from %s with SHMEM_CTX_PRIVATE, is still live; "
                       "spec §9.4.8 requires the program to destroy it first", routine,
                       (void *)c, team->name);
        destroy(c, routine);
    }
}

void osoe_context_finalize(void)
{
    for (uint32_t i = 1; i < pool_count; i++)
        if (pool[i].live)
            destroy(&pool[i], "shmem_finalize");
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(ctx_create)
OSOE_ALIAS(team_create_ctx)
OSOE_ALIAS(ctx_destroy)
OSOE_ALIAS(ctx_get_team)
