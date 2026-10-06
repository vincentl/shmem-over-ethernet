/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The AM layer's sizes, as the environment sets them (§3.2, §3.4, §3.8, §4.6).
 *
 * In every build, and read in any job: the chunk and the landing zone's bytes
 * lay out every PE's landing zone in the node segment (node.c), whatever the
 * job's shape, and every PE checks them all as values before its node joins
 * (osoe_transport_settings_check), so that a bad one is refused the same way
 * on one node and on many. The connection area sizes its buffers and the reply
 * pool its credits from the rest (connection-area.c, progress-host.c), in a job
 * of more than one node.
 */
#ifndef OSOE_AM_CONFIGURATION_H
#define OSOE_AM_CONFIGURATION_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Defaults (§3.2, §3.4).
 *
 * The chunk is §3.2's application-level fragmentation size, and so the largest
 * message the wire ever carries. The buffer default is §3.4's floor of twice
 * that, and the floor is the right default because everything else pushes
 * *down*: §3.2 bounds head-of-line blocking at roughly two segments, which on
 * 1 GbE is about 2 ms at this size and doubles with the buffer, while §3.9's
 * self-clocking already grows the batch under load without needing headroom -
 * the buffer only has to hold what accumulates during one in-flight send.
 * Going above the floor is an opt-in for links fast enough that a millisecond
 * of wire time stops mattering.
 *
 * The budget is a guard rail, not a routine mechanism: at these defaults it is
 * not reached until about a thousand nodes, well past the "hundreds of nodes"
 * §1 targets, so inside the supported range the behaviour is simply "128 KiB
 * buffers, always" with no heuristic to reason about.
 */
#define OSOE_AM_DEFAULT_CHUNK   (UINT64_C(64) << 10)
#define OSOE_AM_MIN_CHUNK       (UINT64_C(1) << 10)
#define OSOE_AM_MAX_CHUNK       (UINT64_C(1) << 20)
#define OSOE_AM_DEFAULT_BUDGET  (UINT64_C(256) << 20)

/*
 * §3.8's per-connection reply credit C. It bounds the target's deferred reply
 * queues, so a node's descriptor pool is `connections x C` - which scales with
 * node count as §1 requires, and is why the bound is per connection rather
 * than per PE.
 */
#define OSOE_AM_DEFAULT_CREDITS 64u
#define OSOE_AM_MIN_CREDITS     1u
#define OSOE_AM_MAX_CREDITS     65536u

/*
 * The bytes of a PE's landing zone, which every reply to its gets shares
 * (§4.6, §4.11 item 7; node.h): 256 KiB by default, in the control segment,
 * 256 extents of 1 KiB. Past it a PE completes what it has in flight before it
 * asks for more.
 */
#define OSOE_DEFAULT_GET_LANDING_BYTES  (UINT64_C(256) << 10)
/* Since layout 23 the zone is 1 KiB extents (node.h), at least one and at most
 * as many as a token's 16 bits name, less the one that means none. */
#define OSOE_MIN_GET_LANDING_BYTES      UINT64_C(1024)
#define OSOE_MAX_GET_LANDING_BYTES      (UINT64_C(65535) * 1024)

/* What the environment asks for, before the budget is applied. Fatal on a
 * malformed or out-of-range value, never a silent default (base/environment.h). */
struct osoe_am_configuration {
    uint64_t chunk;         /* OSOE_AM_CHUNK */
    uint64_t buffer;        /* OSOE_AM_BUFFER */
    uint64_t budget;        /* OSOE_AM_BUFFER_BUDGET */
    uint32_t credits;       /* OSOE_AM_CREDITS: §3.8's C, per connection */
    bool hugepages;         /* OSOE_AM_HUGEPAGES */
    uint64_t get_landing_bytes;     /* OSOE_GET_LANDING_BYTES, per PE */
};

void osoe_am_configuration_read(struct osoe_am_configuration *c);

#endif /* OSOE_AM_CONFIGURATION_H */
