/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "abort.h"
#include "agent.h"
#include "base/deadline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "blocked.h"
#include "deadlock.h"
#include "fatal.h"
#include "liveness.h"
#include "metrics.h"
#include "state.h"
#include "transport.h"

static pthread_t agent;
static _Atomic uint32_t stop_requested;     /* process-private */
static _Atomic uint32_t running;            /* process-private: started, not yet joined */
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
 * A short scheduler slice for the agent (§3.11; the night of 2026-09-27, item
 * 1). Where Slurm binds each of a node's tasks to one core, the node's
 * progress thread - the lead's agent - shares its core with the lead's main
 * thread, and every message of the node waits while that thread runs out its
 * slice: measured on the Pi, the lead's agent spent 29% of its time runnable
 * and not running while three cores idled. Linux 6.12's EEVDF lets a
 * SCHED_OTHER thread ask for a shorter slice (sched_attr's sched_runtime,
 * 0.1 to 100 ms, no privilege needed), which lets it preempt sooner when it
 * wakes: the same fair share, no priority over anyone (so not §3.11's
 * real-time priority, which stays ruled out). At 0.1 ms a bound mailbox ran
 * 1.6 times as fast, and an unbound one 2% faster. OSOE_AGENT_SLICE_US sets
 * it, 100 to 100000 microseconds, and 0 leaves the kernel's own; a kernel
 * without custom slices ignores or refuses the request, and either is fine.
 * Applied to every agent, since the service threads wake rarely and briefly.
 */
enum { AGENT_SLICE_DEFAULT_US = 100, AGENT_SLICE_MIN_US = 100, AGENT_SLICE_MAX_US = 100000 };

/* The kernel's struct sched_attr, as far as SCHED_ATTR_SIZE_VER1: glibc
 * declares neither it nor a wrapper. */
struct scheduling_attributes {
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t utilization_minimum, utilization_maximum;
};

static void ask_for_short_slice(uint32_t microseconds)
{
    if (microseconds == 0)
        return;
    struct scheduling_attributes attributes;
    memset(&attributes, 0, sizeof attributes);
    if (syscall(SYS_sched_getattr, 0, &attributes, sizeof attributes, 0) != 0 ||
        attributes.policy != SCHED_OTHER)
        return;                         /* a policy the application chose is its own */
    attributes.size = sizeof attributes;
    attributes.flags = 0;
    attributes.runtime = (uint64_t)microseconds * 1000;
    (void)syscall(SYS_sched_setattr, 0, &attributes, 0);
}

static uint32_t agent_slice_us(void)
{
    uint32_t microseconds = AGENT_SLICE_DEFAULT_US;
    enum osoe_environment_status status = osoe_environment_u32("OSOE_AGENT_SLICE_US",
                                                               &microseconds);
    if (status == OSOE_ENVIRONMENT_INVALID ||
        (status == OSOE_ENVIRONMENT_OK && microseconds != 0 &&
         (microseconds < AGENT_SLICE_MIN_US || microseconds > AGENT_SLICE_MAX_US)))
        osoe_fatal("OSOE_AGENT_SLICE_US=%s: 0, or %d to %d microseconds",
                   getenv("OSOE_AGENT_SLICE_US"), AGENT_SLICE_MIN_US, AGENT_SLICE_MAX_US);
    return microseconds;
}

static uint32_t slice_us;           /* read once, before the agent starts */

/*
 * Where the agent may run (the author's question of 2026-09-27, "can't we
 * reset the cpu mask on all spawned background threads?"). An agent is born
 * with its PE's binding, and where Slurm binds each of a node's tasks to one
 * core the node's progress thread waits behind its PE's main thread there
 * while the job's other cores idle. OSOE_AGENT_CPUS=node lets every agent run
 * on any CPU any PE of its node may - the union of the tasks' bindings,
 * published in their slots (node.h) - which is the job's own allocation on
 * this node, never a CPU the job was not given; the PEs' threads keep their
 * bindings, and nothing of the system's configuration changes.
 *
 * It is not the default, because the short slice does its work better where
 * the slice exists. On the Pi, 16 PEs bound one a core, a mailbox ran 42,900
 * messages a second with neither, 51,300 widened, 57,500 with the slice and
 * 58,500 with both - but beside the slice, widened, every small collective
 * was slower in both of two alternating runs of osoe-bench: an 8-byte reduce
 * 223 against 200 us, a broadcast 221 against 197, a barrier 191 against 178,
 * a 64 KiB reduce 2,819 against 2,385. So `task`, the default, keeps each
 * agent on its PE's CPUs, and `node` is for a kernel before Linux 6.12, which
 * ignores the slice, running a program bound by point-to-point messages.
 */
static bool cpus_of_node;           /* OSOE_AGENT_CPUS: task, the default, or node */

static bool agent_cpus_of_node(void)
{
    const char *value = getenv("OSOE_AGENT_CPUS");
    if (!value || strcmp(value, "task") == 0)
        return false;
    if (strcmp(value, "node") == 0)
        return true;
    osoe_fatal("OSOE_AGENT_CPUS=%s: task, for its own PE's CPUs, or node, for any CPU of "
               "this node's PEs", value);
}

/* The CPUs of every local PE's task, set on the calling thread; nothing where
 * that adds none, or the kernel refuses (a cpuset that has since shrunk). */
static void run_on_node_cpus(void)
{
    cpu_set_t node, current;
    CPU_ZERO(&node);
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        for (int cpu = 0; cpu < CPU_SETSIZE && cpu < OSOE_TASK_CPU_WORDS * 64; cpu++)
            if (osoe.node.slots[i].task_cpus[cpu / 64] & (UINT64_C(1) << (cpu % 64)))
                CPU_SET(cpu, &node);
    if (CPU_COUNT(&node) == 0 || sched_getaffinity(0, sizeof current, &current) != 0)
        return;
    CPU_OR(&node, &node, &current);
    if (!CPU_EQUAL(&node, &current))
        (void)sched_setaffinity(0, sizeof node, &node);
}

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
    osoe_agent_place_this_thread();
    /* So top -H, ps and a debugger name the library's thread (15 characters). */
    (void)pthread_setname_np(pthread_self(), "osoe-agent");
    /* For the deadlock detector, which counts this PE's threads but this one
     * (deadlock.h). */
    atomic_store_explicit(&slot->agent_tid, (int32_t)syscall(SYS_gettid), memory_order_release);
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
         * Liveness (liveness.h): at most once a sweep
         * interval, a death this agent can see - the lead's of any other local
         * PE, any other PE's of its lead - declares the node's abort, which the
         * check below then acts on in this same turn. Every wait of this loop
         * ends by a sweep interval, so the check runs at least that often.
         */
        osoe_liveness_check();
        /*
         * The node's abort ends this process whatever its main thread is doing
         * (§3.15, abort.h): a PE computing outside the library never sweeps, so
         * its agent is the only part of it that can. Loaded after `seen`, as
         * stop_requested is and for the same reason - a declarer stores the
         * word, then bumps every agent's wake word, then wakes (node.c).
         *
         * The lead's agent first wakes every local agent again. A declarer
         * wakes them in local-id order, the lead's first, but its own agent can
         * see the word and end its process before it has woken the rest; the
         * lead's agent, woken first, is the one that closes that gap for all of
         * them, at O(L) once per node. Then the backend tells the network what
         * only it can - on AM, the lead's abortive goodbye to every peer - and
         * only then does the process end, since its end closes the sockets.
         */
        if (osoe_abort_pending()) {
            if (slot == &osoe.node.slots[0])
                osoe_node_wake_agents(&osoe.node);
            osoe_transport_agent_abort();
            osoe_abort_end();
        }
        /*
         * §3.12: the thread is lent to the backend, through the two calls
         * transport.h declares for it and nothing else. On AM the local lead's
         * agent runs the node's progress engine; every other agent is answered
         * with false and sleeps as before.
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
         *
         * Before the pass, the deadlock detector's look, on the node's lead
         * (§3.15, deadlock.h): at most one a sweep interval while nothing is
         * sampled, and a sample's two halves on two turns with this pass
         * between them - the agent goes round without sleeping while one is
         * half taken.
         */
        bool sampling = slot == &osoe.node.slots[0] && osoe_deadlock_tick();
        if (osoe_transport_agent_pass() || sampling)
            continue;
        /* The backend's wait, on the word and on whatever else it waits for
         * (transport.h): on the lead of a multi-node job, its ring. */
        if (osoe_transport_agent_wait(&slot->agent_wake, seen)) {
            osoe_counter_add(b, OSOE_C_AGENT_WAKEUPS, 1);
            osoe_counter_set(b, OSOE_C_AGENT_LAST_WAKE_NS, osoe_now_ns());
        }
    }
    /* The thread goes back to the library: the backend finishes on it what only
     * this thread can (transport.h), which on the lead is the node's last bytes
     * and the close of every connection. */
    osoe_transport_agent_exit();
    return NULL;
}

void osoe_agent_place_this_thread(void)
{
    ask_for_short_slice(slice_us);
    if (cpus_of_node)
        run_on_node_cpus();
}

bool osoe_agent_running(void)
{
    return atomic_load_explicit(&running, memory_order_acquire) != 0;
}

bool osoe_agent_speaks(void)
{
    return atomic_load_explicit(&running, memory_order_acquire) &&
           !pthread_equal(pthread_self(), agent);
}

bool osoe_agent_is_this_thread(void)
{
    return atomic_load_explicit(&running, memory_order_acquire) &&
           pthread_equal(pthread_self(), agent);
}

void osoe_agent_start(struct osoe_pe_slot *slot)
{
    slice_us = agent_slice_us();
    cpus_of_node = agent_cpus_of_node();
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
    atomic_store_explicit(&running, 1, memory_order_release);

    /* Wait for it to be in the metric table before anyone can look (above).
     * Nothing on its way there waits for anything, so this ends once the
     * thread runs, and has no bound: no wait gives up because time has passed
     * (§3.15). It had one, 10 s, which on a machine too loaded to run a new
     * thread in that time would have ended the job (the README's check of
     * 2026-10-03). A start-up wait like the others (Part 6's adversarial
     * reads): it sweeps for the abort word - the local PEs' liveness being the
     * agent's from here, since `running` says it runs (sweep.c) - ends at
     * OSOE_NO_PROGRESS_TIMEOUT where that is set, and on the node's lead, whose
     * agent its node-mates wait for in turn (init.c), says what it waits for
     * once it has lasted 30 s: one line a node. */
    struct osoe_stall_watch watch;
    if (osoe.bootstrap.local_id == 0)
        osoe_stall_watch_begin_noticed(&watch, 0, osoe.bootstrap.no_progress_ns);
    else
        osoe_stall_watch_begin(&watch, 0, osoe.bootstrap.no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    while (atomic_load_explicit(&agent_ready, memory_order_acquire) == 0) {
        osoe_futex_wait(&agent_ready, 0, &sweep, OSOE_FUTEX_PRIVATE);
        if (!osoe_deadline_expired(&sweep))
            continue;
        if (osoe_stall_watch_stalled(&watch, 0))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): this PE's agent "
                       "thread, created, has not started",
                       (unsigned long long)osoe_stall_watch_seconds(&watch));
        if (osoe_stall_watch_notice_due(&watch))
            osoe_notice("shmem_init on node %u has waited %llu s for its lead's agent thread, "
                        "created and not yet run by the system, to start; still waiting",
                        osoe.bootstrap.node,
                        (unsigned long long)osoe_stall_watch_waited_seconds(&watch));
        osoe_sweep_expired("shmem_init");
        sweep = osoe_deadline_in(OSOE_SWEEP_NS);
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
    atomic_store_explicit(&running, 0, memory_order_release);
}
