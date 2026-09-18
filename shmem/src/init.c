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

/*
 * The thread level (§9.2, §4.6). osoe provides up to SHMEM_THREAD_SERIALIZED;
 * SHMEM_THREAD_MULTIPLE is §4.11 and is not implemented, so a request for it is
 * answered with SERIALIZED rather than refused - the interface exists precisely
 * so that a program can ask for more than it gets and find out.
 *
 * The level is fixed by the *first* initialization of a session and does not
 * change afterwards, which spec §9.2.1 permits in as many words ("may not be
 * able to change the level of threading support provided after the first
 * initialization call has been made"). shmem_query_thread may be called at any
 * time, including before any init, where the answer is the level a
 * shmem_init would provide.
 */
#define OSOE_THREAD_MAX SHMEM_THREAD_SERIALIZED
static _Atomic int thread_level = OSOE_THREAD_MAX;

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
    if (refcount == 0) {
        /* Initialized by shmem_init, the implementation chooses the level
         * (§9.2.2), and osoe chooses the most it has. */
        atomic_store_explicit(&thread_level, OSOE_THREAD_MAX, memory_order_release);
        session_start();
    }
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
    osoe_barrier_all("shmem_finalize");
    refcount--;
    if (refcount == 0) {
        session_end();
        atomic_store_explicit(&initialized, 0, memory_order_release);
        /* A fresh session may be initialized at a different level. Until it
         * is, the answer is the one a shmem_init would give. */
        atomic_store_explicit(&thread_level, OSOE_THREAD_MAX, memory_order_release);
    }
    pthread_mutex_unlock(&lifecycle);
}

/*
 * spec §9.2.1. Returns 0 on success, which is about whether the library came
 * up, not about whether the requested level was granted: a lower `provided` is
 * the interface working as designed, not a failure. A `requested` outside the
 * four defined values is undefined by the spec and is clamped here, so a
 * program that passes garbage gets a defined level rather than a level derived
 * from its garbage.
 */
OSOE_API int shmem_init_thread(int requested, int *provided)
{
    if (requested < SHMEM_THREAD_SINGLE)
        requested = SHMEM_THREAD_SINGLE;
    if (requested > OSOE_THREAD_MAX)
        requested = OSOE_THREAD_MAX;

    pthread_mutex_lock(&lifecycle);
    if (refcount == 0) {
        atomic_store_explicit(&thread_level, requested, memory_order_release);
        session_start();
    }
    refcount++;
    atomic_store_explicit(&initialized, 1, memory_order_release);
    int level = atomic_load_explicit(&thread_level, memory_order_acquire);
    pthread_mutex_unlock(&lifecycle);

    if (provided)
        *provided = level;
    return 0;
}

/* Callable at any time and from any thread, so it takes no lock (§9.2.2). */
OSOE_API void shmem_query_thread(int *provided)
{
    if (provided)
        *provided = atomic_load_explicit(&thread_level, memory_order_acquire);
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
