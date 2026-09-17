#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "agent.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "fatal.h"

static pthread_t agent;
static _Atomic uint32_t stop_requested;     /* process-private */

/*
 * Stop protocol. The stopper stores stop_requested, then bumps agent_wake (a
 * locked instruction), then wakes. The agent loads agent_wake before it loads
 * stop_requested, then waits on the value it loaded. If the agent's load of
 * stop_requested misses the store, its load of agent_wake preceded the bump,
 * so the kernel's compare in futex_wait sees the bump (§4.9's rule: load the
 * sequence word first; store the predicate before bumping it).
 */
static void *agent_main(void *arg)
{
    struct osoe_pe_slot *slot = arg;
    for (;;) {
        uint32_t seen = atomic_load_explicit(&slot->agent_wake, memory_order_acquire);
        if (atomic_load_explicit(&stop_requested, memory_order_acquire))
            break;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&slot->agent_wake, seen, &sweep, OSOE_FUTEX_SHARED);
    }
    return NULL;
}

void osoe_agent_start(struct osoe_pe_slot *slot)
{
    atomic_store_explicit(&stop_requested, 0, memory_order_release);

    /* Block everything but the synchronous signals, which cannot be
     * redirected to another thread and are fatal when blocked. */
    sigset_t all, old;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGFPE);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGTRAP);
    sigdelset(&all, SIGSYS);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int r = pthread_create(&agent, NULL, agent_main, slot);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r != 0)
        osoe_fatal("cannot start the agent thread: %s", strerror(r));
}

void osoe_agent_stop(struct osoe_pe_slot *slot)
{
    atomic_store_explicit(&stop_requested, 1, memory_order_release);
    atomic_fetch_add_explicit(&slot->agent_wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&slot->agent_wake, 1, OSOE_FUTEX_SHARED);
    int r = pthread_join(agent, NULL);
    if (r != 0)
        osoe_fatal("cannot join the agent thread: %s", strerror(r));
}
