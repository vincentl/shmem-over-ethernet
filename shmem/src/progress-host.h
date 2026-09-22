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
 * Everything here is below the transport boundary: transport-am.c and
 * internode-am.c call it, and nothing in the SHMEM layer does
 * (cmake/CheckBoundary.cmake).
 *
 * **Only the local lead's agent runs the engine** (§3.12). Every other PE of
 * the node maps the connection area - its application threads reserve into the
 * buffers like any writer (§3.5) - and does nothing else here. A job of one
 * logical node has no engine at all, and every entry point below is a no-op.
 */
#ifndef OSOE_PROGRESS_HOST_H
#define OSOE_PROGRESS_HOST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Open the connection area, the wires and the engine, or do nothing on a job
 * of one logical node. Called by osoe_transport_init, after the job-wide
 * rendezvous and before the agent thread starts: the agent polls what this sets
 * up, so it must be complete before that thread exists.
 */
void osoe_progress_host_start(uint32_t session);

/* Reverse it. Called by osoe_transport_fini, after the agent thread has been
 * joined, so that nothing is polling while the wires close. */
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
 * How long an idle agent may sleep. OSOE_SWEEP_NS for an agent with nothing to
 * poll; **much less for the lead's agent in a multi-node job**, which is the
 * one place step 2 departs from §3.10.
 *
 * §3.10's idle loop blocks on a single io_uring submission carrying both the
 * socket completions and an `IORING_OP_FUTEX_WAIT` on the agent's wake word -
 * which is why tools/probe checks for that opcode. Step 2 has no ring, and a
 * futex wait takes one address, so the lead can wait on its wake word or on a
 * peer's wire but not on both, and with several peers not even on all the
 * wires. Outbound traffic is unaffected: an application thread bumps the wake
 * word, which is an address this agent is waiting on. Only bytes *arriving*
 * have no way to announce themselves, so those are found by looking.
 *
 * §3.10 rejects a sleep loop for the shipping design, and that stands - this is
 * a stand-in for a ring that does not exist yet, not a choice between the two,
 * and step 3 restores the blocking form and deletes this.
 */
uint64_t osoe_progress_host_idle_ns(void);

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
 * Called by an application thread after committing into a connection buffer,
 * and again by anything waiting on a peer - a writer out of room, a quiet
 * waiting for an acknowledgement - because the lead may be asleep and this is
 * the only thing that wakes it for a connection.
 */
void osoe_progress_host_wake_lead(void);

/*
 * The highest segment sequence `peer` has acknowledged *applying* (§4.6), which
 * is the evidence quiet waits on. Read out of the lead's own connection state,
 * which only the lead writes (§3.12); a reader may see a stale value, and a
 * stale value only ever makes a wait longer.
 */
uint64_t osoe_progress_host_acked(uint32_t peer);

#endif /* OSOE_PROGRESS_HOST_H */
