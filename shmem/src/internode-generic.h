/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The generic implementation of internode.h's data half (§5.5, §5.8; phase 1
 * step 5 slice 2): §5.3's tree of node leads over the transport's put, fenced
 * rmw and wait, on OSOE_REGION_INTERNAL - correct over any backend's transport,
 * and so the one every backend may call.
 *
 * For backends only. Each backend defines osoe_internode_reduce and
 * osoe_internode_broadcast (internode.h), and may hand any call to the
 * functions here with the same arguments; one that does forwards its
 * osoe_internode_init and osoe_internode_fini to them as well. A team is
 * learnt from its record in the node's block (node.h) the first time a call
 * here names it. The SHMEM layer calls internode.h and never this.
 */
#ifndef OSOE_INTERNODE_GENERIC_H
#define OSOE_INTERNODE_GENERIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internode.h"

/* From the backend's osoe_internode_init: every team's state starts afresh,
 * as the internal region does with the session's heap, one entry per block of
 * the node's pool of teams (OSOE_TEAM_BLOCKS). And from its
 * osoe_internode_fini: freed. */
void osoe_internode_generic_init(void);
void osoe_internode_generic_fini(void);

/* osoe_internode_number_taken's, for the counters of this PE's internal region
 * (internal-region.h). */
void osoe_internode_generic_number_taken(uint32_t number);

/* internode.h's osoe_internode_reduce_enter, osoe_internode_reduce,
 * osoe_internode_broadcast and osoe_internode_collect, and collect's
 * osoe_internode_scan_node_totals and osoe_internode_collect_ranges, whose
 * contracts these meet. */
void osoe_internode_generic_reduce_enter(uint32_t team, uint32_t epoch, size_t count,
                                         enum osoe_reduction_operation operation,
                                         struct osoe_element element, const char *routine);
void osoe_internode_generic_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                                   enum osoe_reduction_operation operation,
                                   struct osoe_element element, const char *routine);
void osoe_internode_generic_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index,
                                      void *buffer, size_t bytes, const char *routine);
void osoe_internode_generic_collect(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                                    size_t block, const uint64_t *places, const char *routine);
void osoe_internode_generic_scan_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                             uint64_t *before, uint64_t *total,
                                             const char *routine);
void osoe_internode_generic_collect_ranges(uint32_t team, uint32_t epoch, void *buffer,
                                           size_t bytes, uint64_t share_start,
                                           uint64_t share_end, bool reverse,
                                           const char *routine);
void osoe_internode_generic_scan(uint32_t team, uint32_t rank, void *dest, const void *source,
                                 size_t count, struct osoe_element element, bool inclusive,
                                 const char *routine);

#endif /* OSOE_INTERNODE_GENERIC_H */
