#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "agent.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "blocked.h"
#include "fatal.h"
#include "metrics.h"
#include "progress-host.h"

static pthread_t agent;
static _Atomic uint32_t stop_requested;     /* process-private */
/*
 * Set once the agent has claimed its counter block, and waited for by
 * osoe_agent_start before it returns.
 *
 * Without it, shmem_init returns as soon as pthread_create does and the agent
 * may not have reached its first statement - so a job that hangs early shows
 * **no agent at all** in osoe-stat, which is exactly when §3.14 wants the
 * agent's liveness most. It also made shmem.stat.L1 assert something the
 * library did not promise, and that assertion failed about one run in
 * twenty-five under load. Making the guarantee real is better than weakening
 * the test, because "after shmem_init, every thread this PE started is in the
 * table" is what a reader of the table needs.
 */
static _Atomic uint32_t agent_ready;        /* process-private */

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
    struct osoe_counter_block *b = osoe_metrics_block();
    /* Published before any work, so the block is in the table by the time
     * osoe_agent_start returns (§3.14). */
    atomic_store_explicit(&agent_ready, 1, memory_order_release);
    osoe_futex_wake(&agent_ready, INT_MAX, OSOE_FUTEX_PRIVATE);
    for (;;) {
        /* §3.14's progress heartbeat: two reads showing the same loop count and
         * the same last-wake time mean this agent is stalled, not merely idle. */
        osoe_counter_add(b, OSOE_C_AGENT_LOOPS, 1);
        uint32_t seen = atomic_load_explicit(&slot->agent_wake, memory_order_acquire);
        if (atomic_load_explicit(&stop_requested, memory_order_acquire))
            break;
        /*
         * §3.12: the local lead's agent is the node's progress engine; every
         * other agent is answered with false and sleeps as before.
         *
         * `seen` is loaded **before** the pass, and that ordering is the whole
         * of the wake protocol (§4.9). An arrival posted or an operation
         * committed while the pass runs bumps the wake word past `seen`, so the
         * futex compare below fails and the wait returns at once rather than
         * sleeping on work already waiting. Loading it after the pass instead
         * is the `wake_after` case of arrival_model.py, and it hangs.
         *
         * A pass that moved something goes round again without sleeping: while
         * there is work, latency beats power (§3.10).
         */
        if (osoe_progress_host_poll())
            continue;
        struct osoe_deadline sweep = osoe_deadline_in(osoe_progress_host_idle_ns());
        if (osoe_futex_wait(&slot->agent_wake, seen, &sweep, OSOE_FUTEX_SHARED) == 0) {
            osoe_counter_add(b, OSOE_C_AGENT_WAKEUPS, 1);
            osoe_counter_set(b, OSOE_C_AGENT_LAST_WAKE_NS, osoe_now_ns());
        }
    }
    return NULL;
}

void osoe_agent_start(struct osoe_pe_slot *slot)
{
    atomic_store_explicit(&stop_requested, 0, memory_order_release);
    atomic_store_explicit(&agent_ready, 0, memory_order_release);

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

    /* Wait for it to be in the metric table before anyone can look (above). */
    struct osoe_deadline until = osoe_deadline_in(OSOE_SWEEP_NS * 100);
    while (atomic_load_explicit(&agent_ready, memory_order_acquire) == 0) {
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&agent_ready, 0, &sweep, OSOE_FUTEX_PRIVATE);
        if (osoe_deadline_expired(&until) &&
            atomic_load_explicit(&agent_ready, memory_order_acquire) == 0)
            osoe_fatal("the agent thread did not start");
    }
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
