/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Communication contexts (spec §9.5; design §4.6, §4.11, §7.3; phase 1 step
 * 8 slice 1, docs/prompts/done/phase1-step8-slice1-prompt.md).
 *
 * A context is two parts. The transport's - its dirty set and the completion
 * of its nonblocking gets, with a block in the node segment (transport.h,
 * node.h) - and this layer's: the team it was created from, its options, and
 * the thread that created a private one. This layer's part is an element of a
 * per-process pool of OSOE_MAX_CONTEXTS, and a handle is the element's
 * address, so that a handle the library never issued, or one it has
 * destroyed, is named rather than read through (osoe_ctx_of).
 * SHMEM_CTX_DEFAULT is element 0, live from init to finalize.
 *
 * Every point-to-point routine on a context works in the team-relative PE
 * numbering of the context's team (spec §9.5): osoe_ctx_resolve translates,
 * once, before the transport, which knows only world ranks (§4.10).
 */
#ifndef OSOE_CONTEXT_H
#define OSOE_CONTEXT_H

#include <stddef.h>

#include <shmem.h>

struct osoe_team;
struct osoe_transport_context;

/*
 * The pool for a session: element 0 becomes the default context. Called by
 * shmem_init after osoe_transport_init; stop, by the last shmem_finalize
 * before osoe_transport_fini, frees it.
 */
void osoe_context_start(void);
void osoe_context_stop(void);

/*
 * Destroy every context the program created and did not destroy, each with
 * its implicit quiet: the last shmem_finalize, before its barrier, since
 * finalize releases every resource (spec §9.1.4) and the transport is still
 * up to complete what the contexts have outstanding.
 */
void osoe_context_finalize(void);

/*
 * shmem_team_destroy destroys the shareable contexts created from the team
 * (spec §9.4.8), each with its implicit quiet; a live one created with
 * SHMEM_CTX_PRIVATE is the program's to have destroyed first, undefined
 * otherwise, and ends the job by name. Before the team's own sync.
 */
void osoe_context_team_destroyed(const struct osoe_team *team, const char *routine);

/*
 * Resolve a context handle to its transport context, for a routine with no
 * PE: fence, quiet, the sessions, an iget's completion. SHMEM_CTX_INVALID is
 * fatal here; the routines the spec says must ignore it (fence, quiet,
 * pe_quiet, the sessions) test for it before calling. A handle never issued,
 * or destroyed, ends the job naming `routine`.
 */
struct osoe_transport_context *osoe_ctx_of(const char *routine, shmem_ctx_t ctx);

/*
 * As osoe_ctx_of, and translate `*pe` from the numbering of the context's
 * team to a world rank, checking it against the team's size: on the default
 * context and on one from shmem_ctx_create that is the world's, as
 * osoe_check_pe checks it. A PE outside the team is undefined (spec §9.5) and
 * ends the job naming the routine, the PE and the team.
 */
struct osoe_transport_context *osoe_ctx_resolve(const char *routine, shmem_ctx_t ctx, int *pe);

/* As osoe_ctx_resolve for `npes` PEs, the world ranks written to `world`,
 * which the caller sizes: shmem_ctx_pe_quiet's list. */
struct osoe_transport_context *osoe_ctx_resolve_list(const char *routine, shmem_ctx_t ctx,
                                                     const int *pes, size_t npes, int *world);

#endif /* OSOE_CONTEXT_H */
