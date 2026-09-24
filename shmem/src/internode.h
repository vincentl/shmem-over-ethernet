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
 * the node set - which node its trees are rooted at, who forwards to whom - is
 * its own business and does not cross this header; the one exception is a
 * broadcast's root, which the data half names by its node's position in the
 * registered set, since the data starts there.
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
 * during initialization, before the progress thread starts (below). The data
 * half, osoe_internode_reduce and osoe_internode_broadcast, is called by the
 * team's local lead on its own application thread, and blocks.
 *
 * Errors, and waiting. As transport.h: there is no error return, and a failure
 * ends the job (§3.15). The barrier's half never blocks, so it never sweeps;
 * its waiting is the SHMEM layer's own epoch wait (base/epoch.h), which does.
 * The data half blocks, so it sweeps on §4.10's schedule like every other
 * blocking call, and names the caller's routine in what it records.
 *
 * **The data half is a call, not an arrival** (phase 1 step 5 slice 2). This
 * header once forecast that a reduction would cross as the barrier's arrival
 * grown a payload, with the result coming back in the release upcall. That is
 * the barrier's shape, and it is wrong for data: §5.8 rule 2 keeps a
 * collective's O(n) combining off the progress thread and on the lead's
 * application thread, which is blocked in the collective regardless, so an
 * upcall could only hand the data back to the thread already waiting for it.
 * So the lead hands its node's partial result over once, after the intranode
 * phase, and gets the result back in the same buffer. The barrier's half stays
 * asynchronous, because nothing in it is O(n).
 *
 * **The operation and the element are values, not code**: a reduction
 * operation over elements described by kind and width, the vocabulary an
 * offload engine can be told (libfabric's and UCC's have the same shape) where
 * a combine function could only be called. What every implementation owes the
 * caller, whatever does the work:
 *   - every participating node ends with the same bits, each element's result
 *     computed once and copied, never recomputed along different paths (§5.8
 *     rule 3), because floating sum and product are not associative;
 *   - integer sum and product wrap modulo 2^(8 x width), as two's complement;
 *   - a floating element as wide as long double is this platform's long
 *     double, whatever its format - which every node of the job must share:
 *     nothing checks it, and x86-64's extended format against aarch64's
 *     quadruple one would reduce to garbage;
 *   - the buffer is the backend's only between the call and its return: nothing
 *     reads it by reference afterwards (§5.5's hazard), because the lead
 *     releases its members, and they may reuse their dest, as soon as it returns.
 *
 * **Every backend has the generic implementation to call**
 * (internode-generic.h): §5.3's tree over put, fenced rmw and a wait on
 * OSOE_REGION_INTERNAL, which is correct on any transport. A backend may offload
 * some calls and hand the rest to it, deciding per call and inside the backend,
 * so nothing above this header asks what a backend supports (§4.10). That is
 * the difference from the negotiated collective offload the RoCE review rejects
 * (docs/reviews/roce-transport.md §8.4): this is a mandatory operation with a
 * generic fallback. AM hands every call to it. **The choice must be the same
 * on every node**: a function of the call's arguments - team, count or bytes,
 * operation, element - and of configuration every node agreed at the
 * rendezvous, as the fan-out is (§6.2), and never of anything local, such as
 * an engine's free resources. A node that offloads while another falls back
 * waits for it in a different place, and both wait forever.
 *
 * **The buffer is the lead's dest**: symmetric, in the heap, at the same offset
 * on every node that receives into it, so a backend may address a peer's
 * buffer by its own offset - an RDMA backend writing the result straight into
 * each child's buffer, where the generic implementation stages it through its
 * slots. It stays the lead's own memory: nothing but this call writes it
 * before the lead's release (§5.5, invariant 16), and nothing reads it by
 * reference afterwards (above).
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
 *   - osoe_internode_reduce: the generic implementation, whose puts and fenced
 *     adds become RDMA writes and atomics with no change to it; or, for the
 *     (operation, element) pairs a NIC's collective engine or an in-network
 *     reduction supports, the reduction posted there and waited for, with the
 *     rest - long double, complex, a vector past the engine's limit - handed to
 *     the generic implementation.
 *   - osoe_internode_broadcast: likewise, where a multicast of the root's
 *     buffer is the offload the RoCE review names (§6).
 */
#ifndef OSOE_INTERNODE_H
#define OSOE_INTERNODE_H

#include <stddef.h>
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

/*
 * The internode tree's fan-out k when OSOE_COLLECTIVE_FANOUT does not set one.
 * The backend's, because the right k depends on the fabric (§5.3): the
 * critical path is about depth x (hop latency h + k x per-message cost o), so
 * k grows with h/o, and on RDMA the up-sweep's k arrivals are applied by the
 * parent's NIC rather than its CPU, which favours a wider tree still. The
 * minimum is broad - on RoCE at 1,000 nodes every k from 4 to 32 is within
 * about 10% - so this is a default, not a tuning knob that must be right.
 * Read during bootstrap, before any other call here; every node lead must end
 * up with the same k, which the job rendezvous checks (§6.2).
 */
uint32_t osoe_internode_fanout_default(void);

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
 * set is O(N), and what a tree with a fixed root needs from it is O(fan-out)
 * (§5.3). A broadcast re-rooted at every call needs node positions the set
 * gives: the world team's are the node numbers, and a split team's will need
 * the set, or a way to search it, kept (§7.3).
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
 * team: increasing, but not consecutive, since the data half below numbers its
 * collectives from the same sequence - a barrier, a reduction and a barrier
 * arrive here as 3 and then 5. Bounded skew holds across this call exactly as
 * within a node: no node arrives for a later epoch before it has been released
 * from this one.
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

/* ---- The data half: reductions and broadcast --------------------------- */

/* The spec's reduction operations (Table 10), below the boundary's spelling. */
enum osoe_reduction_operation {
    OSOE_REDUCTION_AND,
    OSOE_REDUCTION_OR,
    OSOE_REDUCTION_XOR,
    OSOE_REDUCTION_MAXIMUM,
    OSOE_REDUCTION_MINIMUM,
    OSOE_REDUCTION_SUM,
    OSOE_REDUCTION_PRODUCT,
};

enum osoe_element_kind {
    OSOE_ELEMENT_SIGNED_INTEGER,
    OSOE_ELEMENT_UNSIGNED_INTEGER,
    OSOE_ELEMENT_FLOATING,
    OSOE_ELEMENT_COMPLEX,
};

/*
 * What a reduction's elements are: a kind and a width in bytes - an integer of
 * 1, 2, 4 or 8; a float, double or long double by its size; a float or double
 * complex by its size. Every type of the spec's Table 10 is one of these, and
 * types that are the same element - int and int32_t, long and ptrdiff_t on
 * LP64 - are reduced alike, as they must be.
 */
struct osoe_element {
    enum osoe_element_kind kind;
    uint32_t width;
};

/*
 * The internode phase of a reduction: on entry `buffer` holds this node's
 * `count` elements, its members already combined; on return it holds the
 * operation applied across every participating node's, elementwise, the same
 * bits on every node (the contract above).
 *
 * Called by the team's local lead, once per node per collective, after its
 * intranode phase and before it releases its members; every participating
 * node's lead calls it with the same team, epoch, count, operation and element.
 * `epoch` is the team's §5.4 epoch of this collective, from the sequence the
 * barrier's arrivals use, so the two halves never disagree on which collective
 * is which. Blocks until the result is in `buffer`, sweeping as it waits;
 * `routine` names the caller in what the wait records and in a fatal.
 */
void osoe_internode_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                           enum osoe_reduction_operation operation,
                           struct osoe_element element, const char *routine);

/*
 * The internode phase of a broadcast: on return every participating node's
 * `buffer` holds the `bytes` the root's node held on entry. `root_index` is the
 * root's node's position in the node set the team was registered with
 * (osoe_internode_team) - addressing stays by team. On other nodes `buffer`'s
 * contents on entry are not read. Called as osoe_internode_reduce is.
 */
void osoe_internode_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index, void *buffer,
                              size_t bytes, const char *routine);

#endif /* OSOE_INTERNODE_H */
