/*
 * internode.h - the boundary between a collective's intranode phase and its
 * internode phase (design §5.1, §5.3, §10).
 *
 * A collective in osoe is two-level (§5.1): the members on one node meet in
 * shared memory, and then the *nodes* meet. This header is where the second
 * half is handed to a backend. It sits beside transport.h, not inside it:
 * §4.10 stays point-to-point and PE-addressed, and nothing here is addressed to
 * a PE. It is bound at build time exactly as transport.h is - no table of
 * function pointers, one implementation per build - and the AM layer's tree
 * (internode-am.c, over progress.c) is one implementation of it.
 *
 * Why it exists (§10, decided 2026-09-20). The internode phase used to be
 * reached by name: barrier.c called the AM engine's host directly. The reasons
 * once given for keeping it off any boundary were that it is addressed to
 * nodes, and that its release is a futex wake §4.10's notify does not cover.
 * The code contradicted both: what crossed down was already (team, epoch), and
 * what came back was one release call. A backend that can do better than AM -
 * a tree over RDMA put and atomics, or a switch that runs the barrier itself -
 * needs a place to do it that is not the SHMEM layer's own files.
 *
 * **Addressing is by team and epoch, never by node or PE.** The SHMEM layer
 * owns teams (§5.2); it tells the backend once, at team creation, which nodes
 * take part, and from then on names the team only. What a backend does with
 * the node set - which node is the root, who forwards to whom - is its own
 * business and does not cross this header.
 *
 * **The release is an upcall.** The backend calls osoe_internode_released when
 * the group has been released, on its own thread; the SHMEM layer advances its
 * own epoch counter and wakes its own waiters (§5.4). The backend never learns
 * that a futex is involved, and the SHMEM layer never learns how the release
 * travelled.
 *
 * Threads. osoe_internode_arrive may be called by any application thread of
 * any local PE, and must not block: it hands the arrival to the backend and
 * returns, and the caller then waits on its own team's counter like every other
 * member. osoe_internode_released is called by the backend on the node's
 * progress thread (§3.12), in whichever local process runs it, exactly once per
 * (team, epoch) on every participating node. osoe_internode_team is called
 * during initialization, before the progress thread starts (below).
 *
 * Errors, and waiting. As transport.h: there is no error return, and a failure
 * ends the job (§3.15). Nothing here blocks, so nothing here sweeps; the
 * waiting is the SHMEM layer's own epoch wait (base/epoch.h), which sweeps.
 *
 * **Why this shape survives data-carrying collectives.** §3.8's known future
 * break is a tree reduce, which forwards a partial result of count x
 * sizeof(type) bytes at every level. That is the arrival growing a payload,
 * and the release growing one on the way back down (a broadcast of the
 * result) - so `arrive` will take (team, epoch, op, type, count, source) and
 * `released` will be told where the result is. Nothing in the present
 * signature forecloses it: the arrival is already per (team, epoch) and made
 * once per node after the intranode phase, which is exactly where §5.5 puts the
 * lead's local combine; and the release is already an upcall that happens once
 * per node, which is where a result is copied out to local members. What would
 * foreclose it is a node-addressed call, or a release expressed as an rmw on a
 * counter, and this header has neither. It is not built: there are no
 * data-carrying collectives yet (§7.1), and a payload with no caller would be a
 * guess.
 *
 * What an RDMA backend does with each call (§4.10's discipline: say it, so the
 * interface is checked against more than the one backend it has):
 *   - osoe_internode_team: computes the same k-ary tree over the node set, and
 *     registers (or reuses) a small per-team slab of counters on each node
 *     that its tree neighbours write with RDMA atomics.
 *   - osoe_internode_arrive: once the node has arrived and its children's
 *     counters show theirs, an RDMA fetch-add on the parent's counter - the
 *     tree built out of put and atomics, with the progress thread (or a NIC
 *     completion) noticing the counter move rather than a message landing.
 *   - osoe_internode_released: the root's release travels down the same tree
 *     as RDMA writes of the epoch; each node forwards to its children before
 *     calling this, so depth latency is not serialized behind local wakes.
 *   - Hardware offload (an in-network barrier, where a fabric has one - RoCE
 *     switches do not): arrive posts to the offload engine, and its completion
 *     calls released. Nothing above this header changes.
 */
#ifndef OSOE_INTERNODE_H
#define OSOE_INTERNODE_H

#include <stdint.h>

/* ---- Lifecycle ------------------------------------------------------- */

/*
 * Called once per session by the first shmem_init, after osoe_transport_init
 * and before any team is registered or the node's progress thread starts; and
 * by the last shmem_finalize after that thread has been joined and before
 * osoe_transport_fini. A backend that carries its messages over the transport's
 * own engine, as AM's does, relies on that bracket: the engine exists for the
 * whole of the internode backend's life.
 */
void osoe_internode_init(void);
void osoe_internode_fini(void);

/* ---- Teams ------------------------------------------------------------ */

/*
 * The node set a team's internode phase spans (§5.2): the logical nodes holding
 * at least one member, sorted ascending, with no repeats, and including this
 * node. `count` is at least 2 - a team on one node has no internode phase and
 * is never registered.
 *
 * Called on every local PE of every participating node with identical
 * arguments, as a consequence of team creation being collective - never by
 * coordination (the same rule §4.10 gives osoe_transport_set_range). A backend
 * keeps what it needs once per node and ignores the repeats.
 *
 * For the predefined teams this runs during shmem_init, after
 * osoe_transport_init and before the node's progress thread starts, so the
 * thread that will carry the team's arrivals never races it. A split team
 * (§7.3) will register from inside the split's own collective, while that
 * thread runs; when it arrives, the call must be ordered before the team's
 * first arrival reaches the backend, which on AM means carrying it through the
 * same handoff an arrival takes (§5.3). No split team exists yet.
 *
 * The array is only read during the call. A backend must not keep it: the node
 * set is O(N), and what a tree needs from it is O(fan-out) (§5.3).
 */
void osoe_internode_team(uint32_t team, const uint32_t *nodes, uint32_t count);

/* ---- The phase itself ------------------------------------------------- */

/*
 * Down: this node has arrived for (team, epoch) - every local member of the
 * team is in (§5.3). Called by whichever member's increment reached the team's
 * arrival target, once per node per epoch, and never for a team that was not
 * registered. Does not block.
 *
 * Epochs are the team's §5.4 epoch count, k + 1 for the k-th collective on the
 * team, and consecutive. Bounded skew holds across this call exactly as within
 * a node: no node arrives for epoch k + 1 before it has been released from k.
 */
void osoe_internode_arrive(uint32_t team, uint32_t epoch);

/*
 * Up, and implemented *above* this header: the internode phase of (team, epoch)
 * is complete, so this node's members may leave. Called by the backend, on the
 * node's progress thread, exactly once per (team, epoch) on every participating
 * node, and only after every participating node has arrived for it.
 *
 * Must not block and must not call back into this header: it advances the
 * team's release counter and wakes the node's waiters (barrier.c).
 */
void osoe_internode_released(uint32_t team, uint32_t epoch);

#endif /* OSOE_INTERNODE_H */
