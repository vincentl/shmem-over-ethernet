/*
 * Waiting for a word of this PE's own region to satisfy a comparison (§4.9):
 * the watch published before the first check, the sequence word loaded before
 * the word itself, a wait on the sequence word with a safety-sweep deadline,
 * and the blocked record while it sleeps - for any region the transport
 * resolves.
 *
 * The SHMEM layer's point-to-point waits are this on the heap (wait.c); the
 * generic internode phase's are this on OSOE_REGION_INTERNAL
 * (internode-generic.c). It sits below the API, with its own comparisons,
 * so that code below the API can wait without including an API header
 * (phase 1 step 5 slice 2; cmake/CheckBoundary.cmake checks it).
 *
 * Values cross as uint64_t, which is what the boundary's load and rmw take,
 * extended from their width according to their signedness: `LESS` on an int
 * holding -1 is not the same question as on a uint32_t holding 0xffffffff.
 */
#ifndef OSOE_REGION_WAIT_H
#define OSOE_REGION_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#include "metrics.h"
#include "node.h"
#include "transport.h"

enum osoe_comparison {
    OSOE_COMPARISON_EQUAL,
    OSOE_COMPARISON_NOT_EQUAL,
    OSOE_COMPARISON_GREATER,
    OSOE_COMPARISON_GREATER_OR_EQUAL,
    OSOE_COMPARISON_LESS,
    OSOE_COMPARISON_LESS_OR_EQUAL,
};

/* A value `width` bytes wide (2, 4 or 8), as the 64-bit number its type says it
 * is. */
uint64_t osoe_region_wait_extend(uint64_t value, unsigned width, bool is_signed);

/* Which counter a wait that sweeps into an already-true predicate belongs in,
 * by the width of what it waits on: the rule in region-wait.c, shared with
 * wait.c's multiple-element forms so that one place decides. */
enum osoe_counter osoe_region_wait_late_wake_counter(unsigned width);

/* Whether `have` stands in `comparison` to `want`, both already extended. */
bool osoe_region_wait_satisfied(enum osoe_comparison comparison, uint64_t have, uint64_t want,
                           bool is_signed);

/*
 * Block until the `width`-byte word at `offset` of this PE's `region` satisfies
 * the comparison, and return the value that did. `want` is extended as the
 * word is. A wait that sleeps records `routine`, `kind` and `detail` in the
 * blocked record (§3.14) and sweeps every OSOE_SWEEP_NS (§4.10).
 */
uint64_t osoe_region_wait(const char *routine, enum osoe_region region, uint64_t offset,
                          unsigned width, bool is_signed, enum osoe_comparison comparison,
                          uint64_t want, enum osoe_blocked_detail kind, uint64_t detail);

/*
 * Block until `holds(context)` is true, where `holds` reads only eight-byte
 * words of this PE's `region` within [offset, offset + len), which one watch
 * covers: a condition over several words, such as a node lead's wait for
 * whichever of its inputs is ready first (internode-generic.c). The words are
 * updated only by rmw, so a sweep that finds the condition already true is a
 * lost wake, counted as osoe_region_wait's are at that width. Recorded and swept
 * as osoe_region_wait.
 */
void osoe_region_wait_condition(const char *routine, enum osoe_region region, uint64_t offset,
                                size_t len, bool (*holds)(const void *context),
                                const void *context, enum osoe_blocked_detail kind,
                                uint64_t detail);

#endif /* OSOE_REGION_WAIT_H */
