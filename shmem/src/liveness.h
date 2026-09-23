/*
 * Liveness without the watchdog (§3.15; phase 1 step 4 slice 1, part C).
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
 * opened could, and they are opened at the agent's first turns, once each PE
 * has published its pid.
 *
 * OSOE_LIVENESS=0 turns it off, for the tests of what else finds a death: the
 * development watchdog and a wait's sweep, which the agent would otherwise
 * race and, ending the PE itself, pre-empt.
 */
#ifndef OSOE_LIVENESS_H
#define OSOE_LIVENESS_H

/* Before the agent starts, and after it has been joined: the descriptors are
 * the agent thread's alone in between. */
void osoe_liveness_start(void);
void osoe_liveness_stop(void);

/* On the agent, each turn of its loop: at most once every OSOE_SWEEP_NS, one
 * poll(2) of the pidfds it holds, with no wait. O(L) descriptors on the lead,
 * one on any other PE. */
void osoe_liveness_check(void);

#endif /* OSOE_LIVENESS_H */
