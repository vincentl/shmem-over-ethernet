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
#include <stdint.h>

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

/*
 * A split over `t` (team.c; the team redesign's step D, D2): the words of t's
 * number's split lines in this PE's internal region, which the caller fills
 * with its contribution - its free mask and its marks - and then
 * osoe_team_split_and, collective over `t`, replaces with the AND of every
 * member's first `count` words, borrowing nothing from the heap.
 */
struct osoe_team;
uint64_t *osoe_team_split_words(const struct osoe_team *t);
void osoe_team_split_and(const char *routine, struct osoe_team *t, uint32_t count);

/*
 * A collective call as every PE of a node must make it (P24): under
 * OSOE_CHECK_COLLECTIVES each member says its call before it arrives -
 * osoe_call_say, into its own internal region's words for the team's number
 * (internal-region.h's spare words), with plain stores its arrival's release
 * publishes - and the node-mate that meets every member's arrival, the lead
 * of a data collective or the last to arrive at a sync, compares them all
 * with its own before anything is released: osoe_call_compare ends the job
 * naming two PEs and their calls where they differ, or a PE that said none.
 * `operation` is a reduction's or scan's operation, element kind and width
 * (osoe_call_operation); `bytes` what every member must pass, 0 where they
 * may differ (a collect's blocks); `detail` a broadcast's root, an alltoall's
 * strides, a scan's inclusiveness. Off, neither says nor reads anything.
 */
enum osoe_call_kind {
    OSOE_CALL_SYNC = 1,
    OSOE_CALL_BROADCAST,
    OSOE_CALL_REDUCTION,
    OSOE_CALL_FCOLLECT,
    OSOE_CALL_COLLECT,
    OSOE_CALL_ALLTOALL,
    OSOE_CALL_SCAN,
};

struct osoe_call {
    enum osoe_call_kind kind;
    uint32_t operation;
    uint64_t bytes;
    uint64_t detail;
};

static inline uint32_t osoe_call_operation(uint32_t operation, struct osoe_element element)
{
    return (operation & 0xff) << 16 | ((uint32_t)element.kind & 0xff) << 8 |
           (element.width & 0xff);
}

void osoe_call_say(const struct osoe_team *t, uint32_t epoch, const struct osoe_call *call);
void osoe_call_compare(const char *routine, const struct osoe_team *t, uint32_t epoch,
                       const struct osoe_call *mine);

/*
 * A word of a call's kind and up to three of its arguments, any one of which
 * changing changes it but once in 2^64: splitmix64's finalizer, a part at a
 * time. Its AND over the PEs beside its inverse's says whether they all passed
 * the same (P6's splits, team.c; P23's allocations, malloc.c): a bit some PE
 * has and another has not is 0 in both, where the word's AND alone would let a
 * PE whose word is a bit-subset of another's pass.
 */
static inline uint64_t osoe_arguments_word(uint64_t kind, uint64_t a, uint64_t b, uint64_t c)
{
    const uint64_t parts[] = { kind, a, b, c };
    uint64_t h = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        h += parts[i] + UINT64_C(0x9e3779b97f4a7c15);
        h = (h ^ (h >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        h = (h ^ (h >> 27)) * UINT64_C(0x94d049bb133111eb);
        h ^= h >> 31;
    }
    return h;
}

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
