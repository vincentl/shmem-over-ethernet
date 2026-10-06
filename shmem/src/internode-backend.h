/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * internode-backend.h - what a job of more than one node needs of the
 * transport, as the part every job uses calls it (design §4.10).
 *
 * The transport below transport.h and internode.h is in two parts. The part
 * every job uses is transport-am.c: the dispatch on locality, the intranode
 * path it hands node-mates to (transport-local.c), the pool of contexts and
 * their modes, and the fence and wait of a job of one node, which are the
 * calling thread's fences alone, since every operation there is applied before
 * its call returns. The part only a job of more than one node uses is the
 * internode backend, and on AM it is these files: the AM layer's far half
 * (transport-internode.c); the engine, the wires and the ring the lead's agent
 * runs (progress-host.c, progress.c, wire.c, ring.c); the connection area, the
 * connections, the reply pool and the messages (connection-area.c,
 * connection.c, reply.c, am.c); the rendezvous of the node leads (rendezvous.c,
 * listener.c); and internode.h over AM (internode-am.c). The generic phase AM
 * hands the data collectives to (internode-generic.c) is on the boundary and
 * built in every build, since internode.h's team-number calls reach it in any
 * job.
 *
 * **The part every job uses reaches the internode backend through this header
 * and nothing else** - shmem.boundary holds the files every build compiles to
 * that (cmake/CheckBoundary.cmake) - and only in a job of more than one node,
 * but for the check every PE of every job makes before its node joins. So a
 * second internode backend of AM's shape - one that keeps the first part's path
 * to node-mates, as the refusing one does - replaces those files without
 * touching the first part. A backend on RDMA does not keep it: its NIC may
 * serve node-mates' atomics, which design §4.10 dispatches per PE and operation
 * class, where the first part dispatches per PE, and it has its own
 * osoe_transport_set_range, which lives in transport-am.c; it replaces
 * transport-am.c as well and implements transport.h and internode.h whole (the
 * v0.2 step 1 review, readiness Q5-8). The SHMEM layer reaches the internode
 * backend through internode.h, and a job of one node makes four of its calls:
 * the default fan-out, read while bootstrapping; init and fini; and a team
 * number taken again - the last three the generic phase's, in both builds.
 *
 * Every entry point below is one of §4.10's calls, or its share of one, for a
 * job of more than one node - which a backend on RDMA provides too, as the
 * comment beside it says, in its own transport-am.c - and the internode backend
 * also implements internode.h whole, the collectives' seam. Its metrics are the
 * control segment's - the `am.*` counters and the connection scope (node.h,
 * metrics.c) - which every build lays out and only this backend writes.
 *
 * Contexts are the AM backend's (transport-context.h): this header passes them
 * by pointer, and the internode backend keeps its own state for each behind
 * the context's `internode`.
 */
#ifndef OSOE_INTERNODE_BACKEND_H
#define OSOE_INTERNODE_BACKEND_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"

/*
 * The fan-out a job's collectives are laid out for when OSOE_COLLECTIVE_FANOUT
 * sets none (internode.h's osoe_internode_fanout_default): AM's, 4 over
 * Ethernet (internode-am.c says why). Every internode backend returns it, so
 * that a job of one node - whose collectives have no internode tree, but
 * whose internal region and team blocks the fan-out sizes - is laid out alike
 * whichever is built.
 */
enum { OSOE_INTERNODE_FANOUT_DEFAULT = 4 };

struct osoe_bootstrap;
struct osoe_deadlock_message;
struct osoe_rank_run;
struct osoe_transport_context;

/* What a lead learns from the job rendezvous (osoe_internode_backend_rendezvous,
 * below): malloc'd, the caller frees. */
struct osoe_rendezvous_result {
    struct osoe_rank_run *runs;
    uint32_t run_count;
    uint32_t *ipv4;                 /* node_count addresses, network order */
    unsigned char *blobs;           /* node_count x OSOE_JOB_BACKEND_BLOB_BYTES */
};

/* ---- Start-up and the end -------------------------------------------- */

/*
 * osoe_transport_settings_check's first step, made by every PE of every job
 * before its node joins (init.c): can this backend run a job of
 * b->node_count nodes here? NULL if it can; if not, `why`, a sentence naming
 * what is missing, written into `len` bytes - on AM, the io_uring facility its
 * ring needs, for a job of more than one node. The kernel and the job's node
 * count are every local PE's alike, so the part every job uses refuses with
 * it telling no one, as init's futex check does (transport-am.c): a lead's
 * last word would leave its node a tombstone in /dev/shm for nothing
 * (node.h). A backend's settings are checked as values beside it, in every
 * build (am-configuration.h, wire-settings.h).
 */
const char *osoe_internode_backend_check(const struct osoe_bootstrap *b, char *why,
                                         size_t len);

/* osoe_transport_advertise (transport.h): ready what other nodes' leads need
 * to reach this node and describe it in `blob` - on AM, the node-pair
 * listener's port. This node's lead, during the job rendezvous. */
void osoe_internode_backend_advertise(void *blob);

/*
 * The job rendezvous (§6.2): this lead meets every other node's, carrying its
 * address and `blob` - the advertisement osoe_transport_advertise filled -
 * and learns the rank map, every node's address and every node's blob, in
 * `result`. This node's lead, from job.c, before it writes the tables its
 * node's PEs read. Out of band, as §6.2 says an RDMA backend's queue-pair and
 * key material must travel: on AM the listener's sockets (rendezvous.c).
 */
void osoe_internode_backend_rendezvous(const struct osoe_bootstrap *b, uint32_t session,
                                       const void *blob, struct osoe_rendezvous_result *result);

/*
 * osoe_transport_init's share: whatever a job of more than one node needs -
 * on AM the connection area, which every local PE maps, and on the lead the
 * wires to every other node and the engine its agent will run - complete
 * before it returns, since the agent starts next and polls it. Every PE, after
 * the job rendezvous, before the default context opens. `check_quiet` is
 * OSOE_CHECK_QUIET, which the part every job uses reads (transport-am.c).
 */
void osoe_internode_backend_start(uint32_t session, bool check_quiet);

/* osoe_transport_fini's share: what start made, once the agent has been
 * joined; what this node owed the network was drained on that thread, in
 * osoe_internode_backend_agent_exit. */
void osoe_internode_backend_stop(void);

/* ---- The agent thread (§3.12) ---------------------------------------- */

/*
 * transport.h's agent calls, in a job of more than one node. A job of one node
 * has nothing of the internode backend's to run on its agents: its pass moves
 * nothing, its wait sleeps on the agent's word alone, its exit and abort tell
 * no one, and it is idle and quiet with nothing applied (transport-am.c). Here
 * every agent of the job may be lent: on AM only each node's lead runs the
 * engine and waits in its ring, and every other agent passes nothing and
 * sleeps on its word.
 */
bool osoe_internode_backend_agent_pass(void);
bool osoe_internode_backend_agent_wait(_Atomic uint32_t *wake, uint32_t seen);
void osoe_internode_backend_agent_exit(void);
void osoe_internode_backend_agent_abort(void);
bool osoe_internode_backend_node_idle(uint64_t *applied);
bool osoe_internode_backend_node_quiet(uint64_t *applied);
void osoe_internode_backend_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m);

/* ---- Contexts (§4.11) ------------------------------------------------- */

/*
 * osoe_transport_context_create's share, and the default context's at init:
 * the state this backend keeps for `context` - on AM, its dirty set, sized by
 * the peer nodes (§4.6) - in context->internode. False, with nothing kept, if
 * memory is short: a creation then fails without error (spec §9.5.1). Close
 * gives it back, after the context's last wait.
 */
bool osoe_internode_backend_context_open(struct osoe_transport_context *context);
void osoe_internode_backend_context_close(struct osoe_transport_context *context);

/* ---- Operations toward a PE on another node -------------------------- */

/*
 * osoe_transport_put, _get and _rmw (transport.h), for a PE that does not
 * route in this process (job.h): reached only in a job of more than one node,
 * with the contracts transport.h gives them - `flags` the same, a get's
 * OSOE_TRANSPORT_NONBLOCKING included. The part every job uses has counted
 * the call.
 */
void osoe_internode_backend_put(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, const void *src,
                                size_t len, unsigned flags);
void osoe_internode_backend_get(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, void *dst, size_t len,
                                unsigned flags);
uint64_t osoe_internode_backend_rmw(struct osoe_transport_context *context, int pe,
                                    enum osoe_region region, uint64_t offset,
                                    enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                    uint64_t compare, unsigned flags);

/* ---- Ordering and completion (§4.6) ---------------------------------- */

/*
 * osoe_transport_fence's share: what a fence owes the network, once the part
 * every job uses has made its release fence - on AM, every peer this context
 * wrote toward asked to acknowledge, its dirty set kept.
 */
void osoe_internode_backend_fence(struct osoe_transport_context *context);

/*
 * osoe_transport_wait and osoe_transport_wait_gets, whole, in a job of more
 * than one node: on AM the context's nonblocking gets copied out, the
 * serializing fence, and the peers of its dirty set - or of `pes` - waited on,
 * in either of §4.11's modes, with the shared mode's pair of get counters and
 * its mutex; and their counters (transport.wait.*).
 */
void osoe_internode_backend_wait(struct osoe_transport_context *context, const int *pes,
                                 size_t npes, const char *routine);
void osoe_internode_backend_wait_gets(struct osoe_transport_context *context,
                                      const char *routine);

#endif /* OSOE_INTERNODE_BACKEND_H */
