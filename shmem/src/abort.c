/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "abort.h"
#include "agent.h"
#include "base/deadline.h"
#include "blocked.h"
#include "fatal.h"
#include "liveness.h"
#include "state.h"

static _Atomic uint32_t ending;         /* process-private: a thread is ending it */
static _Thread_local bool this_thread_ends;     /* ...and it is this one */
static _Atomic uint32_t called;         /* this process called shmem_global_exit */
static int grace_status;
/* What this process's end waits for first, on a global exit (osoe_abort_end):
 * a lead's node, so that its end of stream tells the truth. Written before the
 * grace thread is created, which waits for it too. */
static bool outlasts;
static uint32_t outlast_besides;
static struct osoe_deadline outlast_until;

bool osoe_abort_pending(void)
{
    return osoe.node.hdr &&
           atomic_load_explicit(&osoe.node.hdr->aborting, memory_order_acquire) != 0;
}

void osoe_abort_caller(void)
{
    /* Before the declaration, which publishes it to this PE's agent: the
     * CAS that installs the word is a release, and the agent's load of the
     * word an acquire. A caller whose declaration loses may go unseen, and
     * its PE then ends as another's global exit ends it (abort.h). */
    atomic_store_explicit(&called, 1, memory_order_release);
}

bool osoe_abort_stand_aside(void)
{
    if (atomic_load_explicit(&ending, memory_order_acquire) == 0)
        return false;
    if (this_thread_ends)
        return true;
    for (;;)
        pause();
}

/*
 * The bound on exit (abort.h). Every signal blocked, so none is delivered here
 * and none cuts the sleep short; and an absolute deadline, so an interrupted
 * sleep resumes for what is left rather than starting over or giving up.
 */
static void *grace(void *unused)
{
    (void)unused;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += OSOE_ABORT_GRACE_SECONDS;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR)
        ;
    /* Not before what this end must outlast: the grace bounds a process's own
     * end, not the order of ends a global exit keeps (osoe_abort_end). */
    if (outlasts)
        osoe_liveness_outlast(outlast_besides, &outlast_until);
    /* An end still waiting may be waiting on a lock another thread holds -
     * a stream's, or on the caller's PE exit's own, held by a thread in a
     * program's handler (abort.h): stdout's buffered lines are then what can
     * still be saved, unless a thread holds the stream, so never waiting for
     * it. */
    if (ftrylockfile(stdout) == 0) {
        fflush_unlocked(stdout);
        funlockfile(stdout);
    }
    _exit(grace_status);
}

_Noreturn void osoe_abort_end(void)
{
    /* While this PE's agent runs, it ends the process: it may have the network
     * to tell first (osoe_transport_agent_abort), and a process that another
     * thread ended would take the agent with it, untold. It was woken when the
     * word was set, and looks at it every turn. */
    if (osoe_agent_speaks())
        for (;;)
            pause();
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    uint32_t idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&ending, &idle, 1, memory_order_seq_cst,
                                                 memory_order_acquire))
        for (;;)
            pause();                    /* another thread is ending this process */
    this_thread_ends = true;
    /* SIGPIPE held back in this thread, as the agent holds every signal back
     * in its own (agent.c): a flush into a pipe whose reader is gone then
     * fails with EPIPE, and the PE leaves with the status below rather than by
     * the signal - one end, whichever thread makes it (Part 2's adversarial
     * read, 2026-10-02). */
    sigset_t broken_pipe;
    sigemptyset(&broken_pipe);
    sigaddset(&broken_pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &broken_pipe, NULL);
    /*
     * A global exit's status is the job's and its caller's (spec §9.1.6): one
     * PE leaves with it, the one its node's word names - the carrier, whose
     * declaration won - and every other PE leaves with 0, a caller whose own
     * declaration lost among them. A launcher which ends the step at the first
     * bad exit - srun -K1, KillOnBadExit - kills every task still running then,
     * and a killed task counts 143, which srun's status, the highest of all its
     * tasks', would be; so the carrier also leaves last (abort.h): it waits for
     * every other PE of its node to end - its lead last of them, which has
     * outlasted the other nodes - and only then begins its own end. A lead of
     * a job of more than one node waits for its node's others but the carrier,
     * so that its end of stream, which its end makes, tells the carrier's node
     * that its whole node is done (progress-host.c). A fatal's status is every
     * PE's, and nobody waits. The grace below leaves with the same.
     */
    bool caller = atomic_load_explicit(&called, memory_order_acquire) != 0;
    bool carrier = how == OSOE_ABORT_GLOBAL_EXIT && pe == osoe.bootstrap.rank;
    grace_status = how == OSOE_ABORT_GLOBAL_EXIT && !carrier ? 0 : status;
    /* A caller's wait comes before its grace, which then bounds its own end -
     * its handlers and its flush - as it bounds any caller's: the carrier's,
     * and a lead's that called and lost (its handlers were cut by a grace that
     * outran its wait: Part 2's second adversarial read). Any other lead's
     * comes after its grace starts and its flush: a lead whose flush is held
     * still ends by its grace, with what it waits for ended first (grace). */
    if (carrier) {
        struct osoe_deadline until = osoe_deadline_in(UINT64_C(1000000000) *
                                                      OSOE_ABORT_CARRIER_WAIT_SECONDS);
        osoe_liveness_outlast(UINT32_MAX, &until);
    } else if (how == OSOE_ABORT_GLOBAL_EXIT && osoe.bootstrap.local_id == 0 &&
               osoe.bootstrap.node_count > 1) {
        outlast_besides = UINT32_MAX;               /* none here: another node's */
        for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
            if (osoe.bootstrap.gtids[i] == pe)
                outlast_besides = i;
        outlast_until = osoe_deadline_in(UINT64_C(1000000000) * OSOE_ABORT_LEAD_WAIT_SECONDS);
        if (caller)
            osoe_liveness_outlast(outlast_besides, &outlast_until);
        else
            outlasts = true;
    }
    /* With no thread to spare, the bound is SIGALRM's default action instead:
     * the process still ends, though by a signal and not with its status -
     * unless the program handles or blocks SIGALRM, when there is none. */
    pthread_t t;
    if (pthread_create(&t, NULL, grace, NULL) == 0)
        pthread_detach(t);
    else
        alarm(2 * OSOE_ABORT_GRACE_SECONDS + (outlasts ? OSOE_ABORT_LEAD_WAIT_SECONDS : 0));
    /* A caller's own PE ends as a program does, its handlers and then stdio
     * (spec §9.1.6) - the carrier with the status, a caller whose declaration
     * lost with 0. Every other ends as a fatal does, flushing and then _exit,
     * but with 0 (above): its main thread may be running the program still,
     * and a handler or destructor run beside it would pull state from under it
     * - and _exit takes no lock, where exit waits for good on a thread already
     * inside exit, since glibc 2.41 (BZ 31997). */
    if (how == OSOE_ABORT_GLOBAL_EXIT) {
        if (caller)
            exit(carrier ? status : 0);
        fflush(NULL);
        if (outlasts)
            osoe_liveness_outlast(outlast_besides, &outlast_until);
        _exit(0);
    }
    /* A fatal elsewhere on the node (or across the job): say so, once, for a PE
     * that was computing and would otherwise vanish without a word, and where
     * this PE was - the routine its thread waits in, which the development
     * watchdog named before it was retired (§3.15) - and leave as a fatal
     * does: flushing, then _exit, running no handler. */
    char where[128], msg[256];
    int n = snprintf(msg, sizeof msg, "osoe: PE %u: ending: the job is aborting on behalf "
                     "of PE %u; this PE was %s\n", osoe.bootstrap.rank, pe,
                     osoe_blocked_where(where, sizeof where));
    fflush(NULL);
    /* Unless this process has said its end already - its own thread's fatal on
     * the same abort, say (fatal.h's first word). */
    if (n > 0 && osoe_fatal_first_word()) {
        ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n < sizeof msg ? (size_t)n
                                                                          : sizeof msg - 1);
        (void)ignored;
    }
    _exit(status);
}
