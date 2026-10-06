/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node's progress engine, as a PE runs it (§3.12, §5.3, §7.1 step 2).
 *
 * progress.c is the engine's mechanism and knows nothing about jobs, teams or
 * heaps: it asks its *host* to resolve an address, find a landing zone, wake a
 * PE's waiters and release a team. This file is that host. It owns the wires
 * and supplies those four callbacks out of the library's own state; the
 * release goes up through internode.h's upcall. §5.3's arrival handoff is
 * internode-am.c's, the AM implementation of internode.h.
 *
 * Everything here is below the transport boundary, and the internode
 * backend's (internode-backend.h): transport-internode.c and internode-am.c
 * call it, and nothing in the SHMEM layer or the part of the transport every
 * job uses does (cmake/CheckBoundary.cmake).
 *
 * **Only the local lead's agent runs the engine** (§3.12). Every other PE of
 * the node maps the connection area - its application threads reserve into the
 * buffers like any writer (§3.5) - and does nothing else here. A job of one
 * logical node has no engine at all, and every entry point below is a no-op.
 */
#ifndef OSOE_PROGRESS_HOST_H
#define OSOE_PROGRESS_HOST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Open the connection area, the wires and the engine, or do nothing on a job
 * of one logical node. Called by osoe_transport_init, after the job-wide
 * rendezvous and before the agent thread starts: the agent polls what this sets
 * up, so it must be complete before that thread exists.
 */
void osoe_progress_host_start(uint32_t session);

/*
 * osoe_transport_advertise's AM half: open this lead's node-pair listener on a
 * port the kernel chooses, and write the port into the blob (a uint16_t at its
 * start) for the rendezvous to carry to every node (§6.2).
 */
void osoe_progress_host_advertise(void *blob);

/*
 * The lead's last turn on its agent thread (osoe_transport_agent_exit): put
 * everything this node still owes the network on the wire, then close every
 * wire. On the agent thread because the engine's resources are that thread's
 * (transport.h). Does nothing on any other PE, or on a job of one logical node.
 */
void osoe_progress_host_exit(void);

/*
 * The job is ending (osoe_transport_agent_abort): send every peer an abortive
 * goodbye - within OSOE_ABORT_GRACE_SECONDS - before this process ends. On the
 * agent thread. Does nothing on any other PE, or on a job of one logical node.
 */
void osoe_progress_host_abort(void);

/* Free what start allocated. Called by osoe_transport_fini, after the agent
 * thread has been joined, so nothing is polling any more. */
void osoe_progress_host_stop(void);

/*
 * One pass of the engine, plus the arrivals waiting to be forwarded. Called by
 * every agent each iteration and answered with false by all but the lead's.
 * Returns true if anything moved, which is what tells the agent whether it may
 * sleep (§3.10).
 *
 * osoe_transport_agent_pass is this, and transport.h states its caller's
 * obligation: load the wake word before the pass (§4.9).
 */
bool osoe_progress_host_poll(void);

/*
 * The agent's idle wait (osoe_transport_agent_wait): until the wake word moves
 * past `seen`, or a sweep interval passes (§4.10) - and, for the lead of a
 * multi-node job, until anything completes on its ring. That is §3.10's loop:
 * one io_uring submission carries the sockets' completions and an armed
 * IORING_OP_FUTEX_WAIT on the word, so the lead waits on both in one call.
 *
 * Step 2 could not: its wire was shared memory and a futex wait takes one
 * address, so arriving bytes were found by looking every 250 us, which cost
 * the internode tree one interval per hop (§5.3). Phase 1 step 3 deleted the
 * interval with the stand-in.
 */
bool osoe_progress_host_wait(_Atomic uint32_t *wake, uint32_t seen);

/*
 * internode-am.c's half of the pass: take whatever arrivals local members have
 * posted to the handoff words (§5.3) and perform them on the engine. Lead's
 * agent only; returns true if it took any.
 */
bool osoe_internode_am_take_arrivals(void);

/*
 * Ask the local lead's agent to run a pass: it seals what writers have
 * committed, sends it, and reaps what has arrived (§3.12's "a flag plus a futex
 * wake", where here the committed bytes are the flag).
 *
 * Called by an application thread after committing into a connection buffer
 * and setting its dirty bit (osoe_connection_area_mark), and again by anything
 * waiting on a peer - a writer out of room, a quiet waiting for an
 * acknowledgement. It makes the futex syscall only if the lead has raised
 * lead_sleeping and this caller claims the wake (§3.10, lead_wake_model.py);
 * otherwise it is one load.
 */
void osoe_progress_host_wake_lead(void);

/*
 * The highest segment sequence `peer` has acknowledged *applying* (§4.6), which
 * is the evidence quiet waits on. Read out of the lead's own connection state,
 * which only the lead writes (§3.12); a reader may see a stale value, and a
 * stale value only ever makes a wait longer.
 */
uint64_t osoe_progress_host_acked(uint32_t peer);

/*
 * The deadlock detector's calls (transport.h's node_idle, node_quiet and
 * deadlock_send; §3.15, deadlock.h), on the lead's agent: the engine's view of
 * whether this node owes the network anything, and a detector message queued
 * toward another node's lead. On a node that is not the lead of a multi-node
 * job, idle and quiet with nothing applied.
 */
struct osoe_deadlock_message;
bool osoe_progress_host_idle(uint64_t *applied);
bool osoe_progress_host_quiet(uint64_t *applied);
void osoe_progress_host_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m);

#endif /* OSOE_PROGRESS_HOST_H */
