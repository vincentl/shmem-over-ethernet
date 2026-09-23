/*
 * The development watchdog (§3.12, §3.15).
 *
 * A PE blocked in a wait_until or a barrier whose sibling has died waits until
 * the launcher's timeout. In production that is the right answer: the launcher
 * tears the job down (`srun -K1`, §3.15), and §7.1 step 4 adds pidfd liveness
 * and abort propagation to make the library's own detection complete. This is
 * the development aid until then - a loud failure naming the dead PE, the
 * routine, and the control segment, instead of a hang.
 *
 * It runs only when a library wait's safety-sweep deadline expires
 * (base/sweep.h), never on the wake path, so a healthy wait pays nothing: no
 * load, no branch, no syscall. A wait that is simply slow pays one pass over
 * the local slots every OSOE_SWEEP_NS.
 *
 * What it does NOT do, deliberately (§3.15, §7.1 step 4):
 *   - no pidfd polling: liveness is kill(pid, 0) at a sweep, not an event on
 *     the lead's ring, so detection is up to one sweep late and cannot see a PE
 *     that is alive but wedged;
 *   - no cross-node detection: a vanished peer node surfaces through TCP
 *     keepalive, which does not exist yet;
 *   - no shmem_global_exit and no abort flood: the abort word is set and read
 *     within one node, and the PE that sets it is the one that noticed.
 *
 * It is on by default in the debug, asan and tsan presets and off in release,
 * because production must not depend on it. OSOE_WATCHDOG=0 or 1 forces either
 * way.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "abort.h"
#include "base/sweep.h"
#include "fatal.h"
#include "metrics.h"
#include "state.h"

/*
 * A PE is expected to be gone once its last finalize has run, and has no pid to
 * check before it has joined. Between those it must exist. kill(pid, 0) failing
 * with ESRCH is unambiguous: no process has that pid. A recycled pid can only
 * make a dead PE look alive, which is the hang this replaces rather than a
 * false abort on a healthy job.
 */
/* The dead sibling the watchdog names, for declare_for_dead. A PE runs one
 * watchdog check at a time and leaves through it, so one word is enough. */
static uint32_t dead_rank;

static void declare_for_dead(void *context)
{
    (void)context;
    osoe_node_declare_abort(&osoe.node, dead_rank, EXIT_FAILURE, OSOE_ABORT_FATAL);
}

/*
 * If the node has aborted, end this PE as the word says: a global exit with the
 * caller's status, the way its agent would (abort.h), and anything else as a
 * fatal here too.
 */
static void stop_if_aborting(const char *routine)
{
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    if (how == OSOE_ABORT_GLOBAL_EXIT)
        osoe_abort_end();
    if (how != OSOE_ABORT_NONE)
        osoe_fatal("%s: the job is aborting on behalf of PE %u; this PE cannot make "
                   "progress", routine, pe);
}

static bool slot_dead(const struct osoe_pe_slot *s)
{
    uint32_t phase = atomic_load_explicit(&s->phase, memory_order_acquire);
    if (phase == OSOE_PHASE_EMPTY || phase == OSOE_PHASE_FINALIZED)
        return false;
    /* phase was published with release after pid was written (§3.16). */
    return s->pid > 0 && kill((pid_t)s->pid, 0) != 0 && errno == ESRCH;
}

void osoe_sweep_expired(const char *routine)
{
    if (!osoe.node.hdr)
        return;
    /* A blocked PE's liveness signal: it is asleep, but its sweeps still rise.
     * Counted whether or not the watchdog is on, since it costs one add on a
     * path that has just slept for OSOE_SWEEP_NS (§3.14). */
    atomic_fetch_add_explicit(&osoe.node.slots[osoe.bootstrap.local_id].sweeps, 1,
                              memory_order_seq_cst);

    /* The abort word is the library's own and is read in every build: a PE
     * that has ended the job declared it (fatal.c), and every wait must stop
     * at its next sweep (§3.15). Only the liveness polling below is the
     * development aid. It used to be read only with the watchdog on, so a
     * release build ignored it (phase 1 step 2 slice 2). */
    stop_if_aborting(routine);
    if (!osoe.bootstrap.watchdog)
        return;

    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++) {
        if (i == osoe.bootstrap.local_id || !slot_dead(&osoe.node.slots[i]))
            continue;
        /* A PE that an abort ended is dead too, and its death is the abort's
         * own: this sweep may have read the word before it was declared and
         * found the PE gone after. The word was declared before that PE could
         * see it and end, and its death is seen through the kernel after, so
         * reading the word again finds it - and a global exit must not become
         * a fatal here, with a failure status and, on a lead, no one told
         * (abort_exit_model.py, watchdog_unchecked). */
        stop_if_aborting(routine);
        /* Tell the siblings before dying, so they stop at their next sweep
         * rather than each rediscovering it - after the message (fatal.h), and
         * naming the dead PE, which is why the abort is declared here. */
        dead_rank = osoe.node.slots[i].rank;
        osoe_fatal_then(declare_for_dead, NULL, "%s: local PE %u (rank %u, pid %d) has died; this PE would wait for "
                   "it forever. This is the development watchdog (OSOE_WATCHDOG); in "
                   "production the launcher tears the job down instead (srun -K1)",
                   routine, i, dead_rank, osoe.node.slots[i].pid);
    }
}

/* base/sweep.h. The epoch waits of the barriers are base's, so they report a
 * lost wake through this hook rather than naming a counter they cannot see. */
void osoe_missed_wake(const char *routine)
{
    (void)routine;
    osoe_count(OSOE_C_INVARIANT_MISSED_WAKE, 1);
}
