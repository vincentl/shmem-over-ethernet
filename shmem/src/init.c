/*
 * Initialization lifecycle (§6.4): shmem_init, shmem_finalize,
 * shmem_query_initialized, shmem_my_pe, shmem_n_pes.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <shmem.h>

#include "agent.h"
#include "barrier.h"
#include "export.h"
#include "fatal.h"
#include "state.h"
#include "transport.h"

struct osoe_state osoe;

/*
 * The reference count is per process. The mutex is the once-gate: a first
 * shmem_init runs the whole bootstrap exactly once, and a thread arriving
 * meanwhile waits for it. `initialized` mirrors the count for
 * shmem_query_initialized, which the spec lets any thread call at any time, so
 * it is published with a release store and read without the mutex.
 */
static pthread_mutex_t lifecycle = PTHREAD_MUTEX_INITIALIZER;
static unsigned refcount;
static _Atomic int initialized;
static pid_t init_pid;
static uint32_t sessions;       /* completed or current full sessions */

static void session_start(void)
{
    osoe = (struct osoe_state){ 0 };
    osoe_boot_read(&osoe.boot);
    /* A re-init after a full teardown is a fresh session (§6.4). Every PE runs
     * the same sequence of sessions, so the numbers agree. */
    osoe_node_join(&osoe.node, &osoe.boot, sessions++);
    osoe_tp_init();
    if (osoe_alloc_init(&osoe.alloc, osoe.node.heap_size) != 0)
        osoe_fatal("out of memory");
    osoe_agent_start(&osoe.node.slots[osoe.boot.local_id]);
    init_pid = getpid();
}

static void session_end(void)
{
    osoe_agent_stop(&osoe.node.slots[osoe.boot.local_id]);
    osoe_alloc_fini(&osoe.alloc);
    osoe_tp_fini();
    osoe_node_leave(&osoe.node, &osoe.boot);
    osoe_boot_free(&osoe.boot);
}

OSOE_API void shmem_init(void)
{
    pthread_mutex_lock(&lifecycle);
    if (refcount == 0)
        session_start();
    refcount++;
    atomic_store_explicit(&initialized, 1, memory_order_release);
    pthread_mutex_unlock(&lifecycle);
}

OSOE_API void shmem_finalize(void)
{
    pthread_mutex_lock(&lifecycle);
    /* A child forked after init inherits the mappings but no agent, and must
     * not tear down what its parent still uses (§3.12, §6.4). */
    if (refcount == 0 || getpid() != init_pid) {
        pthread_mutex_unlock(&lifecycle);
        return;
    }
    /* Every finalize is a barrier (spec §9.1.4, invariant 26): a non-last one
     * only that, the last one before tearing down. Skipping it would let the
     * world team's epoch counts diverge across PEs. */
    osoe_barrier_all();
    refcount--;
    if (refcount == 0) {
        session_end();
        atomic_store_explicit(&initialized, 0, memory_order_release);
    }
    pthread_mutex_unlock(&lifecycle);
}

OSOE_API void shmem_query_initialized(int *flag)
{
    *flag = atomic_load_explicit(&initialized, memory_order_acquire);
}

OSOE_API int shmem_my_pe(void)
{
    return atomic_load_explicit(&initialized, memory_order_acquire) ? (int)osoe.boot.rank : -1;
}

OSOE_API int shmem_n_pes(void)
{
    return atomic_load_explicit(&initialized, memory_order_acquire)
               ? (int)osoe.boot.world_size : -1;
}
