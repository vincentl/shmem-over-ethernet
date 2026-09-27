/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
 * The deprecated active-set routines (Annex F). The whole job - PE_start 0,
 * logPE_stride 0 and PE_size the job's size, any stride in a job of one PE -
 * takes the world team's collectives; any other set, the member-only
 * algorithms of active-set.c (phase 1 step 5 slice 7). A set that is not one
 * of this job, or a caller outside it, ends the job by name.
 */
struct osoe_active_set {
    int start, stride_log, size;
    int rank;                   /* the calling PE's place in the set */
};

/* True for the whole job; otherwise checks the set, fills `s` and is false. */
bool osoe_active_set_is_whole_job(const char *routine, int PE_start, int logPE_stride,
                                  int PE_size, struct osoe_active_set *s);

void osoe_active_set_barrier(const char *routine, const struct osoe_active_set *s,
                             long *pSync, bool quiet);
void osoe_active_set_broadcast(const char *routine, const struct osoe_active_set *s,
                               void *dest, const void *source, size_t bytes, int root,
                               long *pSync);
void osoe_active_set_fcollect(const char *routine, const struct osoe_active_set *s,
                              void *dest, const void *source, size_t bytes, long *pSync);
void osoe_active_set_collect(const char *routine, const struct osoe_active_set *s,
                             void *dest, const void *source, size_t bytes, long *pSync);
void osoe_active_set_reduce(const char *routine, const struct osoe_active_set *s, void *dest,
                            const void *source, size_t nreduce,
                            enum osoe_reduction_operation operation,
                            struct osoe_element element, long *pSync);
void osoe_active_set_alltoall(const char *routine, const struct osoe_active_set *s,
                              void *dest, const void *source, ptrdiff_t dest_stride,
                              ptrdiff_t source_stride, size_t nelems, size_t width,
                              long *pSync);

#endif /* OSOE_COLLECTIVES_H */
