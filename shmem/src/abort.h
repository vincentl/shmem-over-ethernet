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
 *   a global exit   on the caller's PE, exit(status): its handlers, then
 *                   standard I/O, as §9.1.6 asks. The library's own atexit
 *                   handlers stand aside while it ends this way - one is a
 *                   barrier (start_pes's finalize), the other declares an
 *                   abort that already is. On every other PE, fflush then
 *                   _exit(status), as a fatal ends: its main thread may still
 *                   be running the program, and no handler or destructor may
 *                   run beside it; and _exit takes no lock, where exit, since
 *                   glibc 2.41 (BZ 31997), waits for good on a thread already
 *                   inside exit - a main thread that returned from main.
 *   a fatal         _exit with the declarer's status, as a fatal does.
 *
 * Either way the end can wait on a lock another thread holds - a stream's, or
 * on the caller's PE exit's own, held by a thread already inside exit: a
 * program's handler in a library wait, or main returned into the library's
 * handler while another thread called shmem_global_exit - so _exit(status)
 * follows after OSOE_ABORT_GRACE_SECONDS, flushing stdout first if no thread
 * holds it: a process already ending, which is not a
 * wait that gives up because time passed. Exactly one thread of a process ends
 * it; any other that gets here waits for that to happen.
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

/* Says that this process's program called shmem_global_exit: its PE ends with
 * exit, running its handlers, where every other PE ends with fflush and _exit.
 * Called before the declaration. */
void osoe_abort_caller(void);

/* End this process as the node's abort says. */
_Noreturn void osoe_abort_end(void);

#endif /* OSOE_ABORT_H */
