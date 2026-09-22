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

#include "bootstrap.h"
#include "connection.h"

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

/* Bytes the area occupies for `nodes` nodes at this buffer size. */
uint64_t osoe_am_area_bytes(uint32_t nodes, uint64_t buffer);

/* What the environment asks for, before the budget is applied. Fatal on a
 * malformed or out-of-range value, never a silent default (base/environment.h). */
struct osoe_am_config {
    uint64_t chunk;         /* OSOE_AM_CHUNK */
    uint64_t buffer;        /* OSOE_AM_BUFFER */
    uint64_t budget;        /* OSOE_AM_BUFFER_BUDGET */
    uint32_t credits;       /* OSOE_AM_CREDITS: §3.8's C, per connection */
    bool hugepages;         /* OSOE_AM_HUGEPAGES */
};

void osoe_am_config_read(struct osoe_am_config *c);

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
    struct osoe_connection *conn; /* nodes - 1 handles, indexed by peer_slot() */
    bool creator;           /* this PE is the node lead, so it made the object */
    /*
     * The send side's work discovery (§3.10, §3.12; phase 1 step 2 slice 3).
     * `dirty` is a bitmap over peer slots, 64 per word: a writer that commits
     * into a connection sets its bit, and the lead takes each word with an
     * exchange and visits only the connections whose bits it took, plus those it
     * already knows are busy. `lead_sleeping` is 1 while the lead is about to
     * sleep or asleep, so a writer calls futex_wake only then. Both in the
     * area's header, so every local PE sees them.
     */
    _Atomic uint64_t *dirty;
    uint32_t dirty_words;
    _Atomic uint32_t *lead_sleeping;
};

/*
 * Create (local lead) or open (everyone else) this node's area. Fatal on
 * failure (§3.15). A job of one node maps nothing and leaves the handle empty,
 * so the caller need not special-case it.
 */
void osoe_connection_area_open(struct osoe_connection_area *a, const struct osoe_bootstrap *b,
                        uint32_t nodes, uint32_t session);

/* Unmap; the lead also removes the name. */
void osoe_connection_area_close(struct osoe_connection_area *a);

/*
 * A writer's half of work discovery: say that connection toward `peer` has
 * something for the lead - committed bytes, or an acknowledgement request.
 * A seq_cst locked or, because it is the store side of a two-sided protocol
 * with the lead's flag (§3.16; lead_wake_model.py): the writer sets its bit and
 * then loads lead_sleeping, the lead stores lead_sleeping and then loads the
 * bits. Call it after the commit it announces, then osoe_progress_host_wake_lead.
 */
static inline void osoe_connection_area_mark(struct osoe_connection_area *a, uint32_t peer)
{
    uint32_t slot = peer < a->node ? peer : peer - 1;
    atomic_fetch_or_explicit(&a->dirty[slot / 64], UINT64_C(1) << (slot % 64),
                             memory_order_seq_cst);
}

/* This node's connection toward `peer`, or NULL if peer is this node. */
struct osoe_connection *osoe_connection_to(struct osoe_connection_area *a, uint32_t peer);

#endif /* OSOE_CONNECTION_AREA_H */
