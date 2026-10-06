/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The wire's settings (§3.3, §3.14): OSOE_WIRE_BYTES, _BUFFERS, _SPLIT and
 * _SEED, read and checked as values.
 *
 * In every build: every PE of every job checks them before its node joins
 * (osoe_transport_settings_check, through the internode backend's share), so
 * a bad one is refused the same way on one node and on many, as it was when
 * the wire alone read them (wire.c, which takes them from here).
 */
#ifndef OSOE_WIRE_SETTINGS_H
#define OSOE_WIRE_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "bootstrap.h"

/*
 * Provided buffers: OSOE_WIRE_BUFFERS of OSOE_WIRE_BYTES each, shared by every
 * wire of the ring, so the memory a node holds for receiving does not grow with
 * its peers. The count is what bounds the completion queue (§3.3); the size
 * only how finely a segment is split. A small size is how the tests make the
 * kernel split what a sender wrote whole.
 */
#define OSOE_WIRE_DEFAULT_BYTES    (UINT32_C(16) << 10)
#define OSOE_WIRE_MIN_BYTES        UINT32_C(64)
#define OSOE_WIRE_MAX_BYTES        (UINT32_C(1) << 20)
#define OSOE_WIRE_DEFAULT_BUFFERS  UINT32_C(64)
#define OSOE_WIRE_MAX_BUFFERS      UINT32_C(32768)

/* What the environment asks of the wire, checked. */
struct osoe_wire_settings {
    uint32_t buffer_bytes, buffer_count, split;
    uint64_t seed;
};

/*
 * §3.14's fault injection, stateless and hash-derived, so the same seed gives
 * the same decision for the same transfer however the threads interleave. A
 * shared generator would make the split each site sees depend on arrival order
 * and destroy reproducibility, which is the whole value.
 */
static inline uint64_t osoe_wire_hash(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

/* Read them, fatal on a bad one; with `announce`, say on stderr whenever the
 * fault injection is on, so a failure names what to reproduce it with. */
void osoe_wire_settings_read(struct osoe_wire_settings *s, const struct osoe_bootstrap *b,
                             bool announce);

/* Read and check OSOE_WIRE_BYTES, _BUFFERS, _SPLIT and _SEED, fatal on a bad
 * one: every PE at init (init.c), where only a lead read them, and only on a
 * job of several nodes, until the round-three review's settings sweep. */
void osoe_wire_settings_check(const struct osoe_bootstrap *b);

#endif /* OSOE_WIRE_SETTINGS_H */
