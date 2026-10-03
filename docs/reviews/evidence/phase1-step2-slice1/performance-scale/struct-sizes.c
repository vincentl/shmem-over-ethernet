/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Prints the real sizes the scale table is computed from. Built against a
 * private copy of the tree: see run-struct-sizes.sh. */
#include <stdio.h>
#include <stddef.h>
#include "connection-area.h"
#include "connection.h"
#include "reply.h"
#include "node.h"
#include "metrics.h"
#include "progress.h"
#include "wire.h"
#include "team.h"
#include "am.h"
#include "allocator.h"
#include "state.h"
#include "job.h"
#define S(t) printf("%-44s %6zu\n", "sizeof(" #t ")", sizeof(t))
int main(void)
{
    S(struct osoe_connection_shared);
    S(struct osoe_connection_buffer);
    S(struct osoe_connection);
    S(struct osoe_am_segment_header);
    S(struct osoe_message_header);
    S(struct osoe_am_put);
    S(struct osoe_am_get_request);
    S(struct osoe_am_get_reply);
    S(struct osoe_am_rmw_request);
    S(struct osoe_am_rmw_reply);
    S(struct osoe_am_barrier);
    S(struct osoe_reply);
    S(struct osoe_progress_connection);
    S(struct osoe_am_parser);
    S(struct osoe_progress);
    S(struct osoe_wire);
    S(struct osoe_node_header);
    S(struct osoe_pe_slot);
    S(struct osoe_landing_slot);
    S(struct osoe_counter_block);
    S(struct osoe_metric_descriptor);
    S(struct osoe_team);
    S(struct osoe_allocator_block);
    S(struct osoe_state);
    uint32_t n; osoe_metrics_table(&n);
    printf("metric descriptors: %u\n", n);
    printf("blocks per PE (OSOE_MAX_THREADS default): %u\n", osoe_metrics_blocks_per_pe());
    printf("OSOE_TEAMS %d\n", (int)OSOE_TEAMS);
    for (uint32_t nodes = 2; nodes <= 8192; nodes *= 2) {
        uint64_t b; int st = osoe_am_choose_buffer(OSOE_AM_DEFAULT_CHUNK, 2*OSOE_AM_DEFAULT_CHUNK, OSOE_AM_DEFAULT_BUDGET, nodes, &b);
        printf("nodes %5u: status %d buffer %llu area %llu\n", nodes, st, (unsigned long long)b, (unsigned long long)osoe_am_area_bytes(nodes, b));
    }
    uint32_t pts[] = {4, 500, 1024, 1025, 4096};
    for (int i = 0; i < 5; i++) {
        uint64_t b; int st = osoe_am_choose_buffer(OSOE_AM_DEFAULT_CHUNK, 2*OSOE_AM_DEFAULT_CHUNK, OSOE_AM_DEFAULT_BUDGET, pts[i], &b);
        printf("nodes %5u: status %d buffer %llu area %llu\n", pts[i], st, (unsigned long long)b, (unsigned long long)osoe_am_area_bytes(pts[i], b));
    }
    return 0;
}
