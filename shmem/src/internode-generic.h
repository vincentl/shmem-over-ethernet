/*
 * The generic implementation of internode.h's data half (§5.5, §5.8; phase 1
 * step 5 slice 2): §5.3's tree of node leads over the transport's put, fenced
 * rmw and wait, on OSOE_REGION_INTERNAL - correct over any backend's transport,
 * and so the one every backend may call.
 *
 * For backends only. Each backend defines osoe_internode_reduce and
 * osoe_internode_broadcast (internode.h), and may hand any call to the
 * functions here with the same arguments; one that does forwards its
 * osoe_internode_init and osoe_internode_team to them as well. The SHMEM layer
 * calls internode.h and never this.
 */
#ifndef OSOE_INTERNODE_GENERIC_H
#define OSOE_INTERNODE_GENERIC_H

#include <stddef.h>
#include <stdint.h>

#include "internode.h"

/* From the backend's osoe_internode_init: every team's state starts afresh,
 * as the internal region does with the session's heap. */
void osoe_internode_generic_init(void);

/* From the backend's osoe_internode_team, with its arguments, after it has
 * checked them. */
void osoe_internode_generic_team(uint32_t team, const uint32_t *nodes, uint32_t count);

/* internode.h's osoe_internode_reduce and osoe_internode_broadcast, whose
 * contracts these meet. */
void osoe_internode_generic_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                                   enum osoe_reduction_operation operation,
                                   struct osoe_element element, const char *routine);
void osoe_internode_generic_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index,
                                      void *buffer, size_t bytes, const char *routine);

#endif /* OSOE_INTERNODE_GENERIC_H */
