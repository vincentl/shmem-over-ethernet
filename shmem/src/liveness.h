/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Liveness (§3.15; phase 1 step 4 slice 1, part C).
 *
 * A PE that dies without finalizing leaves the rest of the job waiting for
 * something it will never do, and no wait gives up because time passed. So
 * each PE's agent watches the deaths it can see: the lead's agent one pidfd for
 * every other local PE, and every other agent one for its lead. A pidfd turns
 * readable when its process has ended, and a PE that ended before its last
 * finalize is a death: the agent that sees it declares the node's abort as a
 * fatal naming it, which the abort then carries to every PE of the job
 * (abort.h). A declaration that loses to an abort already declared does
 * nothing more - a PE the abort ended is dead too (abort_exit_model.py) - and
 * neither does a death found once the word is set.
 *
 * A pidfd names the process, not the number, so a recycled pid cannot hide a
 * death, as it can from kill(pid, 0). Only a pid recycled before its pidfd was
 * opened could, and they are opened at the first check after the node's
 * rendezvous, which every PE's pid is published before. Where no pidfd can be
 * had - out of descriptors, or a kernel or sandbox refusing the call - the PE
 * is watched by kill(pid, 0) instead, after one line saying so, rather than the
 * job ending for it. Like the reclaim of a dead job's objects (§6.2), this
 * assumes the PEs of a node share a pid namespace.
 */
#ifndef OSOE_LIVENESS_H
#define OSOE_LIVENESS_H

/* From the node's rendezvous, once every local PE has published its pid, to
 * the end of the session. */
void osoe_liveness_start(void);
void osoe_liveness_stop(void);

/*
 * At most once every OSOE_SWEEP_NS, one poll(2) of the pidfds this PE holds,
 * with no wait: O(L) descriptors on the lead, one on any other PE. Called on
 * one thread at a time - the agent at each turn of its loop, and until the
 * agent runs, whatever thread is sweeping init's waits (osoe_sweep_expired),
 * so that a death during init is found in every build too.
 */
void osoe_liveness_check(void);

#endif /* OSOE_LIVENESS_H */
