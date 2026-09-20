/*
 * Measure the layout of every structure that lives in shared memory and is
 * declared in a header: offsets, sizes, alignments, and which cache line each
 * hot word falls on (base/cacheline.h: 128 on aarch64, 64 elsewhere).
 *
 * Build, from the repository root:
 *   cc -std=c11 -I. -Ishmem/src <this file> -o shared-layout
 */
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>

#include "base/cacheline.h"
#include "base/counters.h"
#include "base/epoch.h"
#include "connection.h"
#include "node.h"

#define F(T, f) printf("  %-28s off %5zu  size %4zu  line %3zu  %s\n", #f, offsetof(T, f), \
                       sizeof(((T *)0)->f), offsetof(T, f) / OSOE_CACHELINE,               \
                       offsetof(T, f) % 8 == 0 ? "" : "(not 8-aligned)")
#define S(T) printf("%s: size %zu align %zu (%zu lines of %d)\n", #T, sizeof(T), alignof(T), \
                    sizeof(T) / OSOE_CACHELINE, OSOE_CACHELINE)

int main(void)
{
    S(struct osoe_pe_slot);
    F(struct osoe_pe_slot, phase); F(struct osoe_pe_slot, agent_wake);
    F(struct osoe_pe_slot, watch_rng); F(struct osoe_pe_slot, watch_seq);
    F(struct osoe_pe_slot, rank); F(struct osoe_pe_slot, pid); F(struct osoe_pe_slot, tid);
    F(struct osoe_pe_slot, symmetric_size); F(struct osoe_pe_slot, heap_bytes);
    F(struct osoe_pe_slot, heap_name);
    F(struct osoe_pe_slot, blocked_gen); F(struct osoe_pe_slot, blocked_detail);
    F(struct osoe_pe_slot, blocked_since_ns); F(struct osoe_pe_slot, blocked_routine);
    F(struct osoe_pe_slot, sweeps); F(struct osoe_pe_slot, world_epoch);
    F(struct osoe_pe_slot, shared_epoch); F(struct osoe_pe_slot, blocks_exhausted);
    F(struct osoe_pe_slot, heap_in_use); F(struct osoe_pe_slot, heap_high_water);
    F(struct osoe_pe_slot, heap_largest_free);

    S(struct osoe_segment_directory);
    S(struct osoe_node_header);
    F(struct osoe_node_header, dir); F(struct osoe_node_header, heap_base);
    F(struct osoe_node_header, ready); F(struct osoe_node_header, joined);
    F(struct osoe_node_header, mapped); F(struct osoe_node_header, finalized);
    F(struct osoe_node_header, failed); F(struct osoe_node_header, aborting);
    F(struct osoe_node_header, abort_pe); F(struct osoe_node_header, abort_status);
    F(struct osoe_node_header, world_arrived); F(struct osoe_node_header, world_released);
    F(struct osoe_node_header, shared_arrived); F(struct osoe_node_header, arrival);

    S(struct osoe_landing_slot);
    F(struct osoe_landing_slot, bytes_issued); F(struct osoe_landing_slot, bytes_landed);
    F(struct osoe_landing_slot, counter_wake); F(struct osoe_landing_slot, wake);
    F(struct osoe_landing_slot, ready); F(struct osoe_landing_slot, length);
    F(struct osoe_landing_slot, value); F(struct osoe_landing_slot, token);
    printf("  %-28s off %5zu\n", "data", offsetof(struct osoe_landing_slot, data));

    S(struct osoe_connection_buffer);
    S(struct osoe_connection_shared);
    F(struct osoe_connection_shared, buffer[0].head); F(struct osoe_connection_shared, buffer[0].filled);
    F(struct osoe_connection_shared, buffer[0].seq); F(struct osoe_connection_shared, buffer[1].head);
    F(struct osoe_connection_shared, ack_requested); F(struct osoe_connection_shared, acked);
    F(struct osoe_connection_shared, credit);

    S(struct osoe_counter_block);
    S(struct osoe_epoch);
    printf("alignof(_Atomic uint64_t) %zu, sizeof %zu; alignof(_Atomic uint32_t) %zu\n",
           alignof(_Atomic uint64_t), sizeof(_Atomic uint64_t), alignof(_Atomic uint32_t));
    return 0;
}
