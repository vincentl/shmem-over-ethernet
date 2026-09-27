/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Communication sessions (spec §9.9).
 *
 * A session tells a context what it is about to be used for, so that an
 * implementation may batch or coalesce. The spec is explicit that "sessions do
 * not affect the completion or ordering semantics of any OpenSHMEM routines in
 * the program", so a library that does nothing with them is conforming, and
 * these are no-ops.
 *
 * They are not, however, pointless to have now. The thing a session would
 * inform is §3.9's aggregation, through the boundary's OSOE_TRANSPORT_MORE flag, and
 * §4.10 records the session routines as that flag's first driver. On one node
 * every put is a memcpy that is finished before its call returns, so there is
 * nothing to hold back and nothing to tell. The entry points exist so that the
 * backend which can act on them does not also have to introduce them.
 *
 * spec §9.9.2: SHMEM_CTX_INVALID performs no action - the same rule as fence
 * and quiet (§9.12), and for the same reason, so the handle is tested here
 * rather than resolved.
 */
#include <stddef.h>

#include <pshmem.h>
#include <shmem.h>

#include "context.h"
#include "export.h"

OSOE_API void pshmem_ctx_session_start(shmem_ctx_t ctx, long options,
                                       const shmem_ctx_session_config_t *config,
                                       long config_mask)
{
    (void)options;
    (void)config;
    (void)config_mask;
    if (ctx == SHMEM_CTX_INVALID)
        return;
    /* Resolved even though nothing is done with it: a context handle this
     * library never issued is a program bug whether or not the routine acts on
     * it, and a no-op that accepts anything teaches a program the wrong thing. */
    osoe_ctx_of("shmem_ctx_session_start", ctx);
}

OSOE_API void pshmem_ctx_session_stop(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_INVALID)
        return;
    osoe_ctx_of("shmem_ctx_session_stop", ctx);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(ctx_session_start)
OSOE_ALIAS(ctx_session_stop)
