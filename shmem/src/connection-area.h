/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The per-node AM connection area (§3.1, §3.4, §7.1 step 2).
 *
 * One shared-memory object per node, holding every outbound connection's two
 * buffers (§3.5). Application threads of every local PE reserve and copy into
 * them; the lead's agent seals, sends and recycles (§3.12). The lead creates it
 * once the node count is known - which is after the global rendezvous, not at
 * node-local rendezvous - and every local PE opens it by name.
 *
 * **It is a separate object from the control segment, and that is a decision.**
 * The connection buffers are the only AM-specific allocation in the design.
 * §10's open question - whether §3's data path stays hot in an RDMA build -
 * lists among its uncounted costs that "§3.4's connection buffers cost memory
 * whether or not they carry traffic". Folding them into the control segment
 * would pre-decide that question in the direction of "always allocated", in the
 * one segment osoe-stat maps read-only, whose size would then differ by build.
 * As a separate object a build that does not keep the AM data path hot simply
 * never creates it, and an RDMA backend registers one contiguous region rather
 * than N, with puts going from the caller's src straight to the NIC instead of
 * through a staging copy nothing needs.
 *
 * Landing zones and the get counters are deliberately NOT here. They belong to
 * every backend - they exist because the origin's dest may be private memory
 * the lead's process cannot see (§3.12) - and §3.12 and §4.6 put them in the
 * node control segment.
 *
 * **Sizing scales with node count, never PE count** (§1). Per node the area is
 * `2 x (nodes - 1) x buffer`, with no PE dimension at all: every local PE
 * contends on the same open buffer through §3.5's CAS. 256 PEs on a node cost
 * exactly what one PE costs.
 */
#ifndef OSOE_CONNECTION_AREA_H
#define OSOE_CONNECTION_AREA_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "am-configuration.h"
#include "bootstrap.h"
#include "connection.h"
#include "owner-map.h"
#include "shm-object.h"


enum osoe_am_size_status {
    OSOE_AM_SIZE_OK,
    OSOE_AM_SIZE_SHRUNK,    /* below what was asked for, still at or above the floor */
    OSOE_AM_SIZE_NO_ROOM,   /* even the floor exceeds the budget */
};

/*
 * Choose the per-buffer size for a job of `nodes` logical nodes.
 *
 * `chunk` and `requested` are powers of two and `requested >= 2 * chunk`, which
 * the environment parser has already enforced. The area needs
 * `2 * (nodes - 1) * buffer` bytes; while that exceeds `budget` the buffer is
 * halved, which keeps it a power of two and lands exactly on the floor because
 * the floor is one too. If the floor itself does not fit, this says so rather
 * than going below it: under the floor a full-size message leaves no room for a
 * second and aggregation stops at the top of the range (§3.4), which would show
 * up later as a performance mystery instead of now as an error.
 *
 * A job of one node has no peers and no connections, so any size "fits".
 */
enum osoe_am_size_status osoe_am_choose_buffer(uint64_t chunk, uint64_t requested,
                                               uint64_t budget, uint32_t nodes,
                                               uint64_t *chosen);

/* Bytes the area occupies for `nodes` nodes at this buffer size, on a node of
 * `helpers` progress helpers (owner-map.h). */
uint64_t osoe_am_area_bytes(uint32_t nodes, uint32_t helpers, uint64_t buffer);


/*
 * The send side's work discovery (§3.10, §3.12; phase 1 step 2 slice 3), one
 * set a thread that owns wires (owner-map.h; the progress helpers proposal's
 * §3.3): `dirty` is a bitmap over the owner's wires by index, 64 a word - a
 * writer that commits into a connection sets its bit, and the owner takes each
 * word with an exchange and visits only the connections whose bits it took,
 * plus those it already knows are busy. `sleeping` is 1 while the owner is
 * about to sleep or asleep, so that a writer calls futex_wake only then, and
 * `wake` the word a helper sleeps on (owner 0, the lead's agent, sleeps on
 * its PE slot's agent_wake, node.h, which stop and abort bump too). All in
 * the area, so every local PE sees them; this is a process's handle on them.
 */
struct osoe_connection_area_owner {
    _Atomic uint64_t *dirty;
    uint32_t dirty_words;
    _Atomic uint32_t *sleeping;
    _Atomic uint32_t *wake;
};

/* Process-private handle on the area. */
struct osoe_connection_area {
    char name[96];
    void *map;
    uint64_t bytes;
    uint32_t node;          /* this logical node */
    uint32_t nodes;         /* the job's logical node count */
    uint64_t buffer_bytes;
    uint64_t chunk_bytes;
    uint32_t credits;       /* §3.8's C, so the pool can be sized by it */
    struct osoe_connection *connection; /* nodes - 1 handles, by peer slot (osoe_peer_slot) */
    bool creator;           /* this PE is the node lead, so it made the object */
    uint32_t helpers;       /* the node's progress helpers (owner-map.h) */
    struct osoe_connection_area_owner *owner;   /* helpers + 1, by owner */
};

/*
 * Create (local lead) or open (everyone else) this node's area - the area
 * `lead_pid` made, the pid of the node's lead that created the control segment
 * this PE has joined, and no leftover a failed start by the same ids left ready
 * (shm-object.h, OSOE_SHM_ANY_CREATOR) - for a node of `helpers` progress
 * helpers, which every PE of the node must give alike, since it shapes the
 * area. Fatal on failure (§3.15). A job of one node maps nothing and leaves
 * the handle empty, so the caller need not special-case it.
 */
void osoe_connection_area_open(struct osoe_connection_area *a, const struct osoe_bootstrap *b,
                               uint32_t nodes, uint32_t helpers, uint32_t session,
                               int32_t lead_pid);

/* Unmap; the lead also removes the name. */
void osoe_connection_area_close(struct osoe_connection_area *a);

/*
 * The owner of this node's connection toward `peer` (owner-map.h), and in
 * `*index` the connection's index among that owner's wires: the one place a
 * writer names the owner, for its mark below and for the wake after it
 * (osoe_progress_host_wake). Named twice, the two could disagree, and a mark
 * in one owner's bitmap with a wake of another loses the wake
 * (lead_wake_model.py, `mark_wake_apart`).
 */
static inline uint32_t osoe_connection_area_owner(const struct osoe_connection_area *a,
                                                  uint32_t peer, uint32_t *index)
{
    uint32_t slot = osoe_peer_slot(a->node, peer);
    *index = osoe_owner_index(slot, a->helpers);
    return osoe_owner_of(slot, a->helpers);
}

/*
 * A writer's half of work discovery: say that connection toward `peer` has
 * something for its owner - committed bytes, or an acknowledgement request -
 * by the bit of the connection's index in its owner's bitmap (owner-map.h).
 * A seq_cst locked or, because it is the store side of a two-sided protocol
 * with the owner's flag (§3.16; lead_wake_model.py): the writer sets its bit
 * and then loads the owner's `sleeping`, the owner stores `sleeping` and then
 * loads the bits. Call it after the commit it announces, then
 * osoe_progress_host_wake.
 */
static inline void osoe_connection_area_mark(struct osoe_connection_area *a, uint32_t peer)
{
    uint32_t index;
    uint32_t owner = osoe_connection_area_owner(a, peer, &index);
    atomic_fetch_or_explicit(&a->owner[owner].dirty[index / 64], UINT64_C(1) << (index % 64),
                             memory_order_seq_cst);
}

/* This node's connection toward `peer`, or NULL if peer is this node. */
struct osoe_connection *osoe_connection_to(struct osoe_connection_area *a, uint32_t peer);

#endif /* OSOE_CONNECTION_AREA_H */
