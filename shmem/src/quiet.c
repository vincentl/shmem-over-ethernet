/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Memory ordering: shmem_fence, shmem_quiet and shmem_pe_quiet
 * (spec §9.12.1-§9.12.3; design §4.6).
 *
 * Each is one call to the boundary, which is the point of §4.10 having a fence
 * and a wait rather than these three routines: fence orders delivery, wait
 * completes. On the intranode backend both are a fence (base/order.h), because
 * every operation was applied in the calling thread before its call returned
 * and nothing is ever outstanding. Against the AM backend the same two calls
 * seal buffers and wait for acknowledgements. Nothing here changes.
 *
 * The context forms (shmem_ctx_fence and the rest) arrive with §7.3's
 * plumbing; these are the default-context spellings, and the split is already
 * how rma.c is written.
 */
#include <stddef.h>
#include <stdlib.h>

#include <pshmem.h>
#include <shmem.h>

#include "context.h"
#include "export.h"
#include "fatal.h"
#include "metrics.h"
#include "transport.h"

/*
 * spec §9.12.1 and §9.12.2 both say that with SHMEM_CTX_INVALID "no operation
 * is performed". That is the one place an invalid context is not an error, and
 * it is tested here rather than inside osoe_ctx_of, which is fatal on it:
 * ignoring an invalid handle is right for the two routines the spec says so
 * about, and wrong for an RMA that would then silently do nothing.
 */
OSOE_API void pshmem_ctx_fence(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_INVALID)
        return;
    osoe_count(OSOE_C_SHMEM_FENCE_CALLS, 1);
    osoe_transport_fence(osoe_ctx_of("shmem_ctx_fence", ctx));
}

OSOE_API void pshmem_ctx_quiet(shmem_ctx_t ctx)
{
    if (ctx == SHMEM_CTX_INVALID)
        return;
    osoe_count(OSOE_C_SHMEM_QUIET_CALLS, 1);
    osoe_transport_wait(osoe_ctx_of("shmem_ctx_quiet", ctx), NULL, 0, "shmem_quiet");
}

OSOE_API void pshmem_fence(void)
{
    pshmem_ctx_fence(SHMEM_CTX_DEFAULT);
}

OSOE_API void pshmem_quiet(void)
{
    pshmem_ctx_quiet(SHMEM_CTX_DEFAULT);
}

/*
 * spec §9.12.3: with npes == 0, target_pes is ignored and the routine returns
 * immediately - so a null pointer with npes == 0 is a conforming call and must
 * not be dereferenced, which is why the length is tested first.
 *
 * The boundary distinguishes "every PE" from "these PEs" by the pointer, not
 * the count (osoe_transport_wait with pes == NULL means every PE), so passing an empty
 * list through would be wrong in the other direction: it would wait on nothing
 * rather than on everything only if the backend reads npes first. Returning
 * here says it once, where the spec says it.
 */
OSOE_API void pshmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes)
{
    if (npes == 0 || ctx == SHMEM_CTX_INVALID)
        return;
    osoe_count(OSOE_C_SHMEM_PE_QUIET_CALLS, 1);
    /* The targets are numbered in the context's team, as every PE a routine
     * on a context takes is (spec §9.5; context.h), and the transport takes
     * world ranks: translated into a list of its own, on the stack for the
     * lists real programs pass and from malloc past that. */
    int stack[64];
    int *world = npes <= sizeof stack / sizeof stack[0] ? stack : malloc(npes * sizeof *world);
    if (!world)
        osoe_fatal("shmem_ctx_pe_quiet: out of memory for %zu target PEs", npes);
    struct osoe_transport_context *context =
        osoe_ctx_resolve_list("shmem_ctx_pe_quiet", ctx, target_pes, npes, world);
    osoe_transport_wait(context, world, npes, "shmem_pe_quiet");
    if (world != stack)
        free(world);
}

OSOE_API void pshmem_pe_quiet(const int *target_pes, size_t npes)
{
    pshmem_ctx_pe_quiet(SHMEM_CTX_DEFAULT, target_pes, npes);
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(ctx_fence)
OSOE_ALIAS(ctx_quiet)
OSOE_ALIAS(fence)
OSOE_ALIAS(quiet)
OSOE_ALIAS(ctx_pe_quiet)
OSOE_ALIAS(pe_quiet)
