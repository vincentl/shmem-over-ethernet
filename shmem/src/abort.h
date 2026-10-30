/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Ending the job (§3.15; phase 1 step 4): declaring the node's abort, and ending
 * a process because of one, whatever the process was doing.
 *
 * Within a node the abort word already stopped every library wait at its next
 * sweep. What it did not stop was a PE computing outside the library, which
 * never sweeps, and on a multi-node job that includes the lead - whose sockets
 * are the only way the rest of the job learns anything. So every PE's agent
 * now reads the word at each turn of its loop, and a PE whose node aborts ends:
 *
 *   a global exit   on the PE its node's word names - the carrier, the caller
 *                   whose declaration won - exit(status): its handlers, then
 *                   standard I/O, as §9.1.6 asks; on a caller whose
 *                   declaration lost, exit(0). The library's own atexit
 *                   handlers stand aside while it ends this way - one is a
 *                   barrier (start_pes's finalize), the other declares an
 *                   abort that already is. On every other PE, fflush then
 *                   _exit(0), as a fatal ends but with 0: its main thread may
 *                   still be running the program, and no handler or destructor
 *                   may run beside it; and _exit takes no lock, where exit,
 *                   since glibc 2.41 (BZ 31997), waits for good on a thread
 *                   already inside exit - a main thread that returned from
 *                   main. The status is the job's and the carrier's, not each
 *                   task's (the author's decision after the round-three review,
 *                   C2), and the carrier leaves last (the author's decision of
 *                   2026-10-02, after Part 2's adversarial read): a launcher
 *                   that ends the step at the first bad exit (srun -K1,
 *                   KillOnBadExit) kills every task still running then, a
 *                   killed task counts 143, and srun returns the highest code
 *                   of all its tasks. So the carrier waits for every other PE
 *                   of its node before its end; a lead of a job of more than
 *                   one node waits for its node's others but the carrier before
 *                   its own, whose end of stream then tells the carrier's node
 *                   that its node is done; and the carrier's node's lead waits
 *                   for every other node's end of stream after its goodbyes
 *                   (progress-host.c). With callers on several nodes there is
 *                   one carrier for the job, the lowest-ranked of the nodes'
 *                   (P1): a node whose lead reads a goodbye naming a caller
 *                   ranked below its own carrier is outranked (node.h's
 *                   `outranked`) - it waits for no other node, its lead waits
 *                   for every PE of it, and its carrier, rereading the word as
 *                   it waits, leaves with 0 by exit - and a carrier's node
 *                   tells such a peer even where the peer's goodbye arrived
 *                   first, so that it learns it is outranked.
 *   a fatal         _exit with the declarer's status, as a fatal does.
 *
 * Either way the end can wait on a lock another thread holds - a stream's, or
 * on the caller's PE exit's own, held by a thread already inside exit: a
 * program's handler in a library wait, or main returned into the library's
 * handler while another thread called shmem_global_exit - so _exit follows
 * after OSOE_ABORT_GRACE_SECONDS, with the status the PE would have left with,
 * flushing stdout first if no thread holds it: a process already ending,
 * which is not a wait that gives up because time passed. A caller whose own
 * handlers outlast it is ended so too. Exactly one thread of a process ends
 * it; any other that gets here waits for that to happen.
 *
 * The goodbyes take OSOE_ABORT_GRACE_SECONDS at most (progress-host.c) before
 * the grace starts, so on a fatal a process ends within twice that of the
 * abort. A global exit's waits add theirs (OSOE_ABORT_*_WAIT_SECONDS): a lead of
 * the carrier's node ends within five graces of it - its goodbyes, the other
 * nodes, its own node beside its grace - any other lead within three, and the
 * carrier within four. They keep the order of ends where a PE's end is held,
 * which its grace ends; where processes are held at several places at once,
 * or stopped outright, a bound can run out first, and the order with it.
 */
#ifndef OSOE_ABORT_H
#define OSOE_ABORT_H

#include <stdbool.h>
#include <stdint.h>

#include "node.h"

enum { OSOE_ABORT_GRACE_SECONDS = 10 };

/*
 * How long, on a global exit, a PE waits for others to end before its own end
 * begins (abort.c): a lead for its node's other PEs and, on the node whose PE
 * carries the status, for the other nodes' ends of stream (progress-host.c);
 * the carrier for every other PE of its node, its lead last of them. Each
 * outlasts the grace of what it waits for, so that a PE whose flush is held
 * still ends before it, by its own grace; only a process stopped outright is
 * left behind. The waits come before the grace, which then bounds the end.
 */
enum {
    OSOE_ABORT_LEAD_WAIT_SECONDS = 2 * OSOE_ABORT_GRACE_SECONDS,
    OSOE_ABORT_CARRIER_WAIT_SECONDS = 3 * OSOE_ABORT_GRACE_SECONDS,
};

/* Whether the node has aborted: one acquire load, for the agent's loop. */
bool osoe_abort_pending(void);

/*
 * For the library's own exit paths - shmem_finalize, and its atexit handlers -
 * while an abort ends this process: they run no collective then, since the
 * other PEs are ending too (the rule of conformance F1). On the thread ending
 * the process, which is running exit's handlers, this returns true, and the
 * caller returns. Any other thread waits here for the end: returning, it would
 * go on into a second exit - main's own, beside the agent's - which C leaves
 * undefined and which could end the process with main's status instead of the
 * abort's (abort_exit_model.py, finalize_returns). False when no abort is
 * ending the process.
 */
bool osoe_abort_stand_aside(void);

/* Says that this process's program called shmem_global_exit: its PE ends with
 * exit(status), running its handlers, where every other PE ends with fflush
 * and _exit(0). Called before the declaration. */
void osoe_abort_caller(void);

/* End this process as the node's abort says. */
_Noreturn void osoe_abort_end(void);

#endif /* OSOE_ABORT_H */
