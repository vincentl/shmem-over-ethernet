/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): the engines the
 * reductions (reductions.c), broadcast (broadcast.c), collect and fcollect
 * (collect.c) and alltoall and alltoalls (alltoall.c) are written over.
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
 * A gather (spec §9.10.8): every member's nelems elements of `width` bytes, in
 * team rank order, into every member's dest - fcollect, when every member
 * passes the same count, or collect, when each passes its own (`fixed` false).
 * 0 on success; nonzero for SHMEM_TEAM_INVALID.
 */
int osoe_collect(const char *routine, shmem_team_t team, void *dest, const void *source,
                 size_t nelems, size_t width, bool fixed);

/*
 * An exchange (spec §9.10.5, §9.10.6): member i's source block j - nelems
 * elements of `width` bytes, every `source_stride`-th from the block's start -
 * to member j's dest block i, every `dest_stride`-th. alltoall's strides are 1.
 * 0 on success; nonzero for SHMEM_TEAM_INVALID.
 */
int osoe_alltoall(const char *routine, shmem_team_t team, void *dest, const void *source,
                  ptrdiff_t dest_stride, ptrdiff_t source_stride, size_t nelems, size_t width);

/* A word from team rank 0 to every member of `t`, borrowing nothing from the
 * heap (collectives.c): what a split broadcasts its first team number in. */
struct osoe_team;
uint64_t osoe_team_broadcast_word(const char *routine, struct osoe_team *t, uint64_t value);

/*
 * A prefix sum of nelems elements of `element` over `team` (spec §9.10.10):
 * member i's dest ends holding the sum of the sources of team ranks 0 .. i, or
 * 0 .. i - 1 when not `inclusive` (0 at rank 0). 0 on success; nonzero for
 * SHMEM_TEAM_INVALID.
 */
int osoe_scan(const char *routine, shmem_team_t team, void *dest, const void *source,
              size_t nelems, struct osoe_element element, bool inclusive);

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
