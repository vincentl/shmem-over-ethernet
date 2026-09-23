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
#include "state.h"

static _Atomic uint32_t ending;         /* process-private: a thread is ending it */
static _Thread_local bool this_thread_ends;     /* ...and it is this one */
static _Atomic uint32_t inside_exit;    /* a thread is inside exit (osoe_abort_inside_exit) */
static _Thread_local bool this_thread_inside_exit;      /* ...and it is this one */
static int grace_status;

bool osoe_abort_pending(void)
{
    return osoe.node.hdr &&
           atomic_load_explicit(&osoe.node.hdr->aborting, memory_order_acquire) != 0;
}

void osoe_abort_inside_exit(void)
{
    this_thread_inside_exit = true;
    /* seq_cst: against osoe_abort_end's claim and its load of this (below). */
    atomic_store_explicit(&inside_exit, 1, memory_order_seq_cst);
}

/*
 * The end of a process, on its thread inside exit, once another has claimed
 * the end - the agent, after its goodbyes. That thread's exit would wait on
 * this one for good: since glibc 2.41 (BZ 31997) exit takes a lock and never
 * gives it back, and this thread took it first. So this one ends the process,
 * calling exit again from the handler it is in, which glibc has always run as
 * one exit, ending with the last call's status (stdlib/exit.c): the handlers
 * left run and stdio is flushed. A fatal's end is _exit, which takes no lock,
 * so for a fatal this thread waits like any other.
 */
static _Noreturn void end_inside_exit(void)
{
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    if (osoe_node_abort_read(&osoe.node, &pe, &status) == OSOE_ABORT_GLOBAL_EXIT) {
        this_thread_ends = true;        /* the library's handlers left stand aside */
        exit(status);
    }
    for (;;)
        pause();
}

bool osoe_abort_stand_aside(void)
{
    /* seq_cst, for a thread inside exit: against the claim (osoe_abort_end). */
    if (atomic_load_explicit(&ending, memory_order_seq_cst) == 0)
        return false;
    if (this_thread_ends)
        return true;
    if (this_thread_inside_exit)
        end_inside_exit();
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
    /* An exit still waiting may be waiting on exit's own lock, held by a
     * thread in a program's handler (abort.h): stdout's buffered lines are
     * then what can still be saved - unless a thread holds the stream, which
     * may be what exit waits on, so never waiting for it. */
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
     * word was set, and looks at it every turn. A thread inside exit waits
     * only for the agent's claim, which follows the goodbyes, and then ends
     * the process itself (end_inside_exit). */
    while (osoe_agent_speaks()) {
        if (!this_thread_inside_exit)
            for (;;)
                pause();
        if (atomic_load_explicit(&ending, memory_order_seq_cst) != 0)
            end_inside_exit();
        struct timespec nap = { 0, 1000 * 1000 };
        nanosleep(&nap, NULL);
    }
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    uint32_t idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&ending, &idle, 1, memory_order_seq_cst,
                                                 memory_order_seq_cst)) {
        if (this_thread_inside_exit)
            end_inside_exit();
        for (;;)
            pause();                    /* another thread is ending this process */
    }
    this_thread_ends = true;
    grace_status = status;
    /* With no thread to spare, the bound is SIGALRM's default action instead:
     * the process still ends, though by a signal and not with its status -
     * unless the program handles or blocks SIGALRM, when there is none. */
    pthread_t t;
    if (pthread_create(&t, NULL, grace, NULL) == 0)
        pthread_detach(t);
    else
        alarm(2 * OSOE_ABORT_GRACE_SECONDS);
    if (how == OSOE_ABORT_GLOBAL_EXIT) {
        /* A thread inside exit ends it instead, and exit from here would only
         * wait on that thread. seq_cst, with the claim above, against that
         * thread's store of inside_exit and its load of the claim: one of the
         * two sees the other, so one of them calls exit - both may, and the
         * second then waits (glibc 2.41), or runs beside the first, both with
         * this status (older glibc). */
        if (!this_thread_inside_exit &&
            atomic_load_explicit(&inside_exit, memory_order_seq_cst) != 0)
            for (;;)
                pause();
        exit(status);
    }
    /* A fatal elsewhere on the node (or across the job): say so, once, for a PE
     * that was computing and would otherwise vanish without a word, and leave
     * as a fatal does - flushing, then _exit, running no handler. */
    char msg[160];
    int n = snprintf(msg, sizeof msg, "osoe: PE %u: ending: the job is aborting on behalf "
                     "of PE %u\n", osoe.bootstrap.rank, pe);
    fflush(NULL);
    if (n > 0) {
        ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n < sizeof msg ? (size_t)n
                                                                          : sizeof msg - 1);
        (void)ignored;
    }
    _exit(status);
}
