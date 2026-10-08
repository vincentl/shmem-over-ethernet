/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Which thread owns which wire (docs/design/progress-helpers-proposal.md §3.2).
 *
 * An *owner* is a thread with a ring and the wires attached to it. The lead's
 * agent is owner 0; with H helper threads, helper h is owner h. A peer's data
 * wire, named by its peer slot s - node order with this node left out
 * (osoe_progress_slot) - belongs to owner 0 when H is 0, and to owner
 * 1 + (s mod H) otherwise, as the (s div H)th of that owner's wires: its
 * *index*, which numbers everything the owner keeps a wire - the wire, the
 * engine's bookkeeping for it, its busy bit, the dirty bit a writer sets for
 * it, its ring's completion tag. Arithmetic, so that a PE needs no table to
 * find the owner to wake, nor an owner one to find the peer.
 *
 * An array the node keeps with an entry a wire is grouped by owner: owner o's
 * wires sit from osoe_owner_first(o) on, in index order. With no helpers that
 * is peer-slot order, the order everything had before there were owners.
 *
 * A node's H is agreed among its PEs, since it shapes the connection area, and
 * free to differ between nodes: the peer never knows it.
 */
#ifndef OSOE_OWNER_MAP_H
#define OSOE_OWNER_MAP_H

#include <stdint.h>

/* The owner of peer slot `slot`'s data wire. */
static inline uint32_t osoe_owner_of(uint32_t slot, uint32_t helpers)
{
    return helpers ? 1 + slot % helpers : 0;
}

/* That wire's index among its owner's. */
static inline uint32_t osoe_owner_index(uint32_t slot, uint32_t helpers)
{
    return helpers ? slot / helpers : slot;
}

/* The peer slot of owner `owner`'s wire `index`: the inverse of the two above. */
static inline uint32_t osoe_owner_slot(uint32_t owner, uint32_t index, uint32_t helpers)
{
    return helpers ? index * helpers + (owner - 1) : index;
}

/* How many of a node's `peers` data wires owner `owner` holds. */
static inline uint32_t osoe_owner_wires(uint32_t owner, uint32_t peers, uint32_t helpers)
{
    if (helpers == 0)
        return owner == 0 ? peers : 0;
    if (owner == 0 || owner > helpers)
        return 0;
    return peers / helpers + (owner - 1 < peers % helpers ? 1 : 0);
}

/* Where owner `owner`'s wires start in an array grouped by owner: after
 * helpers 1 to owner - 1, the first peers mod H of which hold one wire more. */
static inline uint32_t osoe_owner_first(uint32_t owner, uint32_t peers, uint32_t helpers)
{
    if (helpers == 0 || owner == 0)
        return 0;
    uint32_t before = owner - 1, longer = peers % helpers;
    return before * (peers / helpers) + (before < longer ? before : longer);
}

/* Peer slot `slot`'s place in an array grouped by owner. */
static inline uint32_t osoe_owner_position(uint32_t slot, uint32_t peers, uint32_t helpers)
{
    return osoe_owner_first(osoe_owner_of(slot, helpers), peers, helpers) +
           osoe_owner_index(slot, helpers);
}

/*
 * The rings (the progress helpers proposal's §3.1): owner 0's carries its data
 * wires, by index, and after them every control wire, by peer slot, since the
 * control plane is the lead's; a helper's carries its data wires. So a data
 * wire's place on its ring is its index, as everything else its owner keeps a
 * wire is numbered, and a control wire's is owner 0's data wires' count on.
 * A node's wires in one array are grouped by ring in owner order.
 */
static inline uint32_t osoe_ring_wires(uint32_t owner, uint32_t peers, uint32_t helpers)
{
    return osoe_owner_wires(owner, peers, helpers) + (owner == 0 ? peers : 0);
}

/* Where owner `owner`'s ring's wires start in an array grouped by ring. */
static inline uint32_t osoe_ring_first(uint32_t owner, uint32_t peers, uint32_t helpers)
{
    return owner == 0 ? 0
                      : osoe_ring_wires(0, peers, helpers) +
                            osoe_owner_first(owner, peers, helpers);
}

/* Peer slot `slot`'s control wire: its place on owner 0's ring, and so in an
 * array grouped by ring. */
static inline uint32_t osoe_control_ring_slot(uint32_t slot, uint32_t peers, uint32_t helpers)
{
    return osoe_owner_wires(0, peers, helpers) + slot;
}

#endif /* OSOE_OWNER_MAP_H */
