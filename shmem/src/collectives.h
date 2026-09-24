/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): the engine the
 * reductions (reductions.c) and broadcast (broadcast.c) are written over.
 */
#ifndef OSOE_COLLECTIVES_H
#define OSOE_COLLECTIVES_H

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
 * to every member's dest, the root's included (spec §9.10.7). */
int osoe_broadcast(const char *routine, shmem_team_t team, void *dest, const void *source,
                   size_t nelems, size_t width, int root);

#endif /* OSOE_COLLECTIVES_H */
