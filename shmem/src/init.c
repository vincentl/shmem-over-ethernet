/*
 * Initialization lifecycle (§6.4): shmem_init, shmem_finalize,
 * shmem_query_initialized, shmem_my_pe, shmem_n_pes.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>

#include <pshmem.h>
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

/*
 * What SHMEM_INFO prints (§8). The spec says "print helpful text about all
 * these environment variables", so it is the specification's four plus the
 * OSOE_ ones a user of this implementation would otherwise have to read the
 * source to find. Written to stderr, because stdout belongs to the program.
 */
static void print_info(void)
{
    fprintf(stderr,
SHMEM_VENDOR_STRING " environment variables\n"
"\n"
"  From the OpenSHMEM specification (§8). Each also has a deprecated SMA_\n"
"  spelling; when both are set the SHMEM_ one controls.\n"
"    SHMEM_SYMMETRIC_SIZE   symmetric heap bytes per PE, with an optional\n"
"                           k, m, g or t multiplier (default 256m)\n"
"    SHMEM_VERSION          print the library version at start-up\n"
"    SHMEM_INFO             print this text\n"
"    SHMEM_DEBUG            print what the library set up, per PE\n"
"\n"
"  Specific to osoe:\n"
"    OSOE_WATCHDOG          0 or 1: the development watchdog that ends a job\n"
"                           whose PE has died, instead of waiting for the\n"
"                           launcher's timeout. On in debug builds (§3.15)\n"
"    OSOE_MAX_THREADS       counter blocks per PE, one per thread (§3.14)\n"
"    OSOE_BOOTSTRAP_TIMEOUT seconds to wait for the other local PEs (§6.2)\n"
"    OSOE_HEAP_BASE         the address every PE maps its heap at (§4.1)\n"
"\n"
"  The bootstrap variables OSOE_PROCID, OSOE_LOCALID, OSOE_GTIDS,\n"
"  OSOE_STEP_NUM_TASKS, OSOE_NODEID, OSOE_JOB_ID and OSOE_STEP_ID are set by\n"
"  the launcher; see osoe-launch.\n"
"\n"
"  A running or hung job is read with osoe-stat, which maps the control\n"
"  segment read-only and needs no cooperation from the job (§3.14).\n");
}

static void session_start(void)
{
    osoe = (struct osoe_state){ 0 };
    osoe_bootstrap_read(&osoe.bootstrap);

    /*
     * One logical node still, until the AM backend carries internode traffic
     * (§7.1 step 2). The check is here rather than in the bootstrap because
     * what is missing is the *library's* ability to run such a job, not
     * anything wrong with the environment: the rendezvous of §6.2 parses and
     * uses that environment already, and refusing it there would stop it being
     * testable before the backend exists.
     */
    if (osoe.bootstrap.node != 0 || osoe.bootstrap.local_count != osoe.bootstrap.world_size)
        osoe_fatal("this job spans more than one node (OSOE_NODEID=%u, %u of %u ranks "
                   "local); internode traffic needs the AM backend of §7.1 step 2",
                   osoe.bootstrap.node, osoe.bootstrap.local_count,
                   osoe.bootstrap.world_size);

    /*
     * Before the rendezvous, so that a job which fails to start still says
     * which library it was and how to ask it for help.
     */
    if (osoe.bootstrap.print_version)
        fprintf(stderr, "osoe: PE %u: " SHMEM_VENDOR_STRING ", OpenSHMEM %d.%d\n",
                osoe.bootstrap.rank, SHMEM_MAJOR_VERSION, SHMEM_MINOR_VERSION);
    if (osoe.bootstrap.print_info && osoe.bootstrap.local_id == 0)
        print_info();
    /* A re-init after a full teardown is a fresh session (§6.4). Every PE runs
     * the same sequence of sessions, so the numbers agree. */
    osoe_node_join(&osoe.node, &osoe.bootstrap, sessions++);
    osoe_transport_init();
    if (osoe_allocator_init(&osoe.allocator, osoe.node.heap_size) != 0)
        osoe_fatal("out of memory");
    osoe_agent_start(&osoe.node.slots[osoe.bootstrap.local_id]);
    init_pid = getpid();

    /*
     * SHMEM_DEBUG, after the rendezvous, because the two things worth knowing
     * are the ones it produces: the control segment to point osoe-stat at
     * (§3.14), and the heap base every PE agreed on (§4.1). This is the same
     * name §3.15 prints on an abort, said once at start-up instead.
     */
    if (osoe.bootstrap.debug)
        fprintf(stderr, "osoe: PE %u of %u (local %u of %u): segment /dev/shm%s, "
                "heap %llu bytes at %#llx, watchdog %s\n",
                osoe.bootstrap.rank, osoe.bootstrap.world_size, osoe.bootstrap.local_id,
                osoe.bootstrap.local_count, osoe.node.control_name,
                (unsigned long long)osoe.node.heap_size,
                (unsigned long long)osoe.node.hdr->heap_base,
                osoe.bootstrap.watchdog ? "on" : "off");
}

static void session_end(void)
{
    osoe_agent_stop(&osoe.node.slots[osoe.bootstrap.local_id]);
    osoe_allocator_fini(&osoe.allocator);
    osoe_transport_fini();
    osoe_node_leave(&osoe.node, &osoe.bootstrap);
    osoe_bootstrap_free(&osoe.bootstrap);
}

OSOE_API void pshmem_init(void)
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

OSOE_API void pshmem_finalize(void)
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
OSOE_API int pshmem_init_thread(int requested, int *provided)
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
OSOE_API void pshmem_query_thread(int *provided)
{
    if (provided)
        *provided = atomic_load_explicit(&thread_level, memory_order_acquire);
}

OSOE_API void pshmem_query_initialized(int *flag)
{
    *flag = atomic_load_explicit(&initialized, memory_order_acquire);
}

OSOE_API int pshmem_my_pe(void)
{
    return atomic_load_explicit(&initialized, memory_order_acquire) ? (int)osoe.bootstrap.rank : -1;
}

OSOE_API int pshmem_n_pes(void)
{
    return atomic_load_explicit(&initialized, memory_order_acquire)
               ? (int)osoe.bootstrap.world_size : -1;
}

/* The weak `shmem_` aliases (export.h). */
OSOE_ALIAS(init)
OSOE_ALIAS(finalize)
OSOE_ALIAS(init_thread)
OSOE_ALIAS(query_thread)
OSOE_ALIAS(query_initialized)
OSOE_ALIAS(my_pe)
OSOE_ALIAS(n_pes)
