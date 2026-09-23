#define _GNU_SOURCE
#include <pthread.h>
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
static int grace_status;

bool osoe_abort_pending(void)
{
    return osoe.node.hdr &&
           atomic_load_explicit(&osoe.node.hdr->aborting, memory_order_acquire) != 0;
}

bool osoe_abort_ending(void)
{
    return atomic_load_explicit(&ending, memory_order_acquire) != 0;
}

static void *grace(void *unused)
{
    (void)unused;
    struct timespec t = { OSOE_ABORT_GRACE_SECONDS, 0 };
    nanosleep(&t, NULL);
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
    grace_status = status;
    pthread_t t;
    if (pthread_create(&t, NULL, grace, NULL) == 0)
        pthread_detach(t);
    if (how == OSOE_ABORT_GLOBAL_EXIT)
        exit(status);
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
