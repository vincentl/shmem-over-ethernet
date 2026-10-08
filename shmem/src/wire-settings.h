/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The wire's settings (§3.3, §3.14): OSOE_WIRE_BYTES, _BUFFERS, _SPLIT, _SEED
 * and _UNSENT_BYTES, read and checked as values.
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

/*
 * The bytes the kernel may hold unsent on a connection (TCP_NOTSENT_LOWAT;
 * v0.2 step 4, design §3.2) - about, since the kernel takes a send whole while
 * it holds less. Left to the system, it held some 2.6 MB of a put stream at a
 * gigabit, and a fetch-add behind the stream waited 14 to 22 ms for them on
 * the Pi; limited to 16 KiB, 3.3 to 3.9 ms, the stream at line rate
 * (docs/decisions/2026-10-07-one-thread-profile.md). The limit is the link's
 * bandwidth times OSOE_WIRE_UNSENT_NS - what the link sends while the lead's
 * agent comes round to give the kernel more - and at least
 * OSOE_WIRE_UNSENT_FLOOR, set once the link is known, at the end of
 * shmem_init. OSOE_WIRE_UNSENT_BYTES sets it by hand, taken as given however
 * small; 0 leaves the system's own (net.ipv4.tcp_notsent_lowat, none by
 * default).
 */
#define OSOE_WIRE_UNSENT_FLOOR     (UINT32_C(16) << 10)
#define OSOE_WIRE_UNSENT_NS        UINT64_C(128000)
#define OSOE_WIRE_UNSENT_MAX       (UINT32_C(1) << 30)
_Static_assert(OSOE_LINK_BYTES_PER_SECOND_HIGHEST <= UINT64_MAX / OSOE_WIRE_UNSENT_NS,
               "the link's highest bandwidth times OSOE_WIRE_UNSENT_NS must fit 64 bits");

/* What the environment asks of the wire, checked. */
struct osoe_wire_settings {
    uint32_t buffer_bytes, buffer_count, split;
    uint64_t seed;
    bool unsent_set;                /* OSOE_WIRE_UNSENT_BYTES gives the limit */
    uint32_t unsent_bytes;          /* that limit, 0 for the system's own */
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

/* Read and check OSOE_WIRE_BYTES, _BUFFERS, _SPLIT, _SEED and _UNSENT_BYTES,
 * fatal on a bad one: every PE at init (init.c), where only a lead read them,
 * and only on a job of several nodes, until the round-three review's settings
 * sweep. */
void osoe_wire_settings_check(const struct osoe_bootstrap *b);

/* The limit on a connection's unsent bytes for a link of `bytes_per_second`,
 * 0 when none is known: as set, or the link's OSOE_WIRE_UNSENT_NS, at least
 * OSOE_WIRE_UNSENT_FLOOR and at most OSOE_WIRE_UNSENT_MAX. 0: the system's own. */
uint32_t osoe_wire_unsent_bytes(const struct osoe_wire_settings *s, uint64_t bytes_per_second);

#endif /* OSOE_WIRE_SETTINGS_H */
