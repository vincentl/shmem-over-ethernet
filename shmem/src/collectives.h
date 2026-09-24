/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): the engine the
 * reductions (reductions.c) and broadcast (broadcast.c) are written over.
 */
#ifndef OSOE_COLLECTIVES_H
#define OSOE_COLLECTIVES_H

#include <stdbool.h>
#include <stddef.h>

#include <shmem.h>

#include "internode.h"

/*
 * A reduction of nelems elements of `element` over `team` by `operation`: every
 * member's dest ends holding the combination of every member's source, the same
 * bits on every PE (§5.8 rule 3). 0 on success; nonzero for SHMEM_TEAM_INVALID.
 * `routine` is the API routine, for diagnostics.
 */
int osoe_reduce(const char *routine, shmem_team_t team, void *dest, const void *source,
                size_t nelems, enum osoe_reduction_operation operation,
                struct osoe_element element);

/* A broadcast of nelems elements of `width` bytes from team rank `root`'s source
 * to every member's dest - the root's included (spec §9.10.7), unless
 * `root_keeps_dest`, which the deprecated active-set forms ask for: their
 * root's dest is left as it was (broadcast.c). */
int osoe_broadcast(const char *routine, shmem_team_t team, void *dest, const void *source,
                   size_t nelems, size_t width, int root, bool root_keeps_dest);

/*
 * The deprecated active-set routines (Annex F) run over the whole job and
 * nothing else (design §7.4): PE_start 0, logPE_stride 0 and PE_size the job's
 * size - with any stride in a job of one PE. Any other active set ends the job
 * by name: osoe forms no group for it, and answering over the wrong PEs would
 * be a wrong answer rather than a missing one.
 */
void osoe_active_set_whole_job(const char *routine, int PE_start, int logPE_stride,
                               int PE_size);

#endif /* OSOE_COLLECTIVES_H */
