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
 *   a global exit   exit(status), so standard I/O is flushed as §9.1.6 asks.
 *                   The library's own atexit handlers stand aside while the
 *                   process ends this way - one is a barrier (start_pes's
 *                   finalize), the other declares an abort that already is.
 *                   exit from a thread that is not main can block on a stdio
 *                   lock the main thread holds, so _exit(status) follows after
 *                   OSOE_ABORT_GRACE_SECONDS: a process already ending, which
 *                   is not a wait that gives up because time passed.
 *   a fatal         _exit with the declarer's status, as a fatal does.
 *
 * Exactly one thread of a process ends it; any other that gets here waits for
 * that to happen - but for a thread already inside exit, running one of the
 * library's handlers. Since glibc 2.41 (BZ 31997) exit takes a lock it never
 * gives back, so a second thread's exit waits for good: the agent's exit(s)
 * would wait on a main thread that returned from main and is in start_pes's
 * finalize, or in the handler for a program that left without finalizing. Such
 * a thread ends the process itself, calling exit(s) again once the agent has
 * sent its goodbyes, and the agent leaves exit to it (osoe_abort_inside_exit).
 * A program's own handler that is in a library wait when a global exit comes
 * is not known to be inside exit: its process ends at the grace instead, with
 * the status, flushing stdout if no thread holds it.
 *
 * The goodbyes take OSOE_ABORT_GRACE_SECONDS at most (progress-host.c) before
 * the grace starts, so a process ends within twice that of the abort.
 */
#ifndef OSOE_ABORT_H
#define OSOE_ABORT_H

#include <stdbool.h>
#include <stdint.h>

#include "node.h"

enum { OSOE_ABORT_GRACE_SECONDS = 10 };

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

/*
 * Says that this thread is inside exit: called first by the library's atexit
 * handlers, whose thread, if an abort ends the process, ends it itself rather
 * than wait for a thread whose exit would wait on it.
 */
void osoe_abort_inside_exit(void);

/* End this process as the node's abort says. */
_Noreturn void osoe_abort_end(void);

#endif /* OSOE_ABORT_H */
