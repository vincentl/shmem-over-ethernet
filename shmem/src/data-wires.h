/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A node's data wires made on demand (H23; OSOE_MESH_DATA_WIRES=on-demand; the
 * author's decisions of 2026-10-25, docs/decisions/2026-10-24.md, "The
 * author's answers", item 4): the lead's half, on its agent - owner 0's
 * thread (owner-map.h).
 *
 * The job starts with its control wires alone, eager to every node as they
 * always were, so start-up still proves every node reachable, and the barrier,
 * the deadlock detector, the abort, keepalive and osoe-stat keep their wires.
 * A node pair's data wire is made the first time a PE of either node has
 * something for the other: the connection's owner finds it has no wire at the
 * visit the writer's mark brings (progress.c, attach_or_ask) and asks for one
 * here, once. Nothing changes on the writer's side, whose connection keeps both
 * buffers sealed meanwhile, as every connection starts (§3.5).
 *
 * **The schedule's caller always dials** (mesh-schedule.h). Where this node is
 * the caller of the pair the need is a call, made as start-up's are - the
 * three-step hello, at most OSOE_MESH_CALLS_IN_FLIGHT at once, each retried
 * until made (listener.h's calls part-way). Where it is not, it asks the peer
 * to dial - one OSOE_AM_DATA_WIRE_WANTED on their control wire - and its
 * listener, open for the job's life in this mode alone, answers the call. No
 * two calls cross, so nothing has to resolve a crossing.
 *
 * **The socket made goes to its connection's owner**: stored, then the
 * connection marked in the owner's dirty bitmap and the owner woken - a
 * writer's mark and wake, lead_wake_model.py's protocol - so that the owner's
 * next visit attaches it to its own ring, whose only submitter it is (ring.h),
 * opens a buffer, and with the turnover releases the writers waiting.
 *
 * **The listener and the calls are watched by the ring** (ring.h's watch): one
 * epoll set holds the listening socket, the callers part-way through the hello
 * and the calls' sockets, as listener.h's poll asks for them, and one poll on
 * owner 0's ring watches the set, so that the agent's idle wait ends when any of
 * them moves, and by the earliest of their deadlines at the latest. listener.c's
 * set of sixteen callers, its hello's limits and its calls' retries are
 * start-up's, unchanged; a multishot accept, which design §6.2 once sketched,
 * would take connections that the full set has no place for.
 *
 * **An abort waits for no wire** (data_wire_model.py). The agent's abort closes
 * the hand-over, stops answering - its listening socket closed and every caller
 * not yet answered dropped, since nobody has attached an end of theirs - and
 * abandons its calls before their confirmation. But it serves a caller it has
 * answered to the confirmation or its end, since that caller attaches its end
 * the moment it has sent the confirmation, and sends the abortive goodbye on the
 * socket so made itself: an end of stream without one would be a peer's death
 * to the caller (`abandon_answered`). A helper finishes its own abort only after
 * a pass begun once the hand-over is closed, so every socket handed to it before
 * is attached and told (progress-host.c's abort_wires).
 *
 * **A stalled data wire is told as any wait is** (§3.15): a notice once a call,
 * or a request to the peer, has waited 30 s, and every minute after, naming the
 * node, its address and the last call's error, as start-up's calls say;
 * OSOE_NO_PROGRESS_TIMEOUT ends the job if it is set. A writer waiting for the
 * wire is in its turnover's wait meanwhile, and the deadlock detector finds a
 * node making a wire neither idle nor quiet.
 *
 * All of it is the agent's but osoe_data_wires_want, which any owner calls, and
 * osoe_data_wires_take and osoe_data_wires_handover_closed, an owner's.
 * Outside on-demand mode each is a no-op, and says so.
 */
#ifndef OSOE_DATA_WIRES_H
#define OSOE_DATA_WIRES_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "bootstrap.h"
#include "listener.h"
#include "ring.h"

/*
 * Begin, on the lead, after the mesh's start-up (osoe_wires_open, which in this
 * mode made the control wires alone and left `listener` open): `peers`, every
 * node's address and port as the rendezvous carried them, is copied.
 */
void osoe_data_wires_start(const struct osoe_bootstrap *b, uint32_t session,
                           struct osoe_listener *listener, const struct sockaddr_in *peers);

/* Whether this node makes its data wires on demand: started, and not stopped. */
bool osoe_data_wires_on(void);

/*
 * A writer's need of peer slot `slot`'s data wire, which its connection's owner
 * met (progress.h's want_wire): marked for the agent, on any owner's thread, by
 * a seq_cst read-modify-write - the store side of lead_wake_model.py's protocol,
 * of which osoe_data_wires_wanted, after the agent raises its flag, is the
 * load side; the owner then wakes the agent, unless it is the agent.
 */
void osoe_data_wires_want(uint32_t slot);
bool osoe_data_wires_wanted(void);

/* A peer's request to dial their data wire (OSOE_AM_DATA_WIRE_WANTED), on the
 * agent: fatal where the schedule does not have this node call the peer. */
void osoe_data_wires_requested(uint32_t node);

/*
 * The agent's turn, after its engine's pass: the needs and the requests taken,
 * calls begun and taken on, callers served, sockets handed over, notices said,
 * and the watch armed on `ring`, owner 0's. True if anything moved.
 */
bool osoe_data_wires_poll(struct osoe_ring *ring);

/* When the agent must turn again at the latest (CLOCK_MONOTONIC): a dial's
 * limit, a back-off's end, a hello's time, the next notice; or UINT64_MAX. */
uint64_t osoe_data_wires_due_ns(void);

/* Whether a data wire is being made: a call to come or in flight, or a request
 * not yet answered by a call - a node neither idle nor quiet then (§3.15). */
bool osoe_data_wires_making(void);

/* The socket made for peer slot `slot`, taken once, on its owner's thread, at
 * the visit the hand-over's mark brings; or -1. */
int osoe_data_wires_take(uint32_t slot);

/* Whether the agent's abort has closed the hand-over - an acquire load, after
 * which the owner's next pass sees every socket handed before it (above). True
 * outside on-demand mode. */
bool osoe_data_wires_handover_closed(void);

/* The agent's abort, with the abortive goodbye's words (progress.h's
 * osoe_progress_abort): the hand-over closed, the listener answering no one
 * new, the calls not yet made abandoned. */
void osoe_data_wires_abort(uint32_t pe, int32_t status, uint32_t disposition, bool keep_stream);

/* Whether the agent's part of the abort is done: no caller it answered still
 * to confirm, and the abortive goodbye on every socket it made since sent and
 * acknowledged by the peer's kernel. True outside on-demand mode. */
bool osoe_data_wires_aborted(void);

/* Close what is left - the listener, a call, a socket never taken - and free
 * everything: the lead's stop, its wires closed. */
void osoe_data_wires_stop(void);

#endif /* OSOE_DATA_WIRES_H */
