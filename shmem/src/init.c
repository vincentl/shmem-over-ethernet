/*
 * Initialization lifecycle (§6.4): shmem_init, shmem_finalize,
 * shmem_query_initialized, shmem_my_pe, shmem_n_pes.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <pshmem.h>
#include <shmem.h>

#include "agent.h"
#include "base/futex.h"
#include "barrier.h"
#include "export.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "state.h"
#include "team.h"
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
_Atomic int osoe_initialized;       /* state.h */
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
"    OSOE_NO_PROGRESS_TIMEOUT\n"
"                           seconds a wait may see no progress before it ends\n"
"                           the job; 0, the default, means never (§3.15)\n"
"    OSOE_HEAP_BASE         the address every PE maps its heap at (§4.1)\n"
"    OSOE_KEEP_CONTROL_SEGMENT\n"
"                           1: keep the control segment's name until finalize,\n"
"                           so a crash leaves it for post-mortem reading and\n"
"                           osoe-stat --gc; by default no name survives init\n"
"                           (§3.14)\n"
"    OSOE_COLLECTIVE_FANOUT\n"
"                           k of the internode tree collectives run over,\n"
"                           2 to 32, the same on every node; the default is\n"
"                           the backend's, 4 on AM (§5.3)\n"
"    OSOE_SHM_CAPACITY_CHECK\n"
"                           0: report a request larger than /dev/shm's free\n"
"                           space and continue, for testing on a small\n"
"                           /dev/shm; by default init refuses it (§6.2)\n"
"\n"
"  The bootstrap variables OSOE_PROCID, OSOE_LOCALID, OSOE_GTIDS,\n"
"  OSOE_STEP_NUM_TASKS, OSOE_NODEID, OSOE_JOB_ID and OSOE_STEP_ID are set by\n"
"  the launcher; see osoe-launch.\n"
"\n"
"  A running or hung job is read with osoe-stat, which maps the control\n"
"  segment read-only and needs no cooperation from the job (§3.14).\n");
}

/*
 * A process that leaves with the library still initialized (§6.4).
 *
 * An shmem_init program ends its use of the library with shmem_finalize or
 * shmem_global_exit (spec §4); leaving without either is outside the
 * specification, and the other PEs would wait for this one at their next
 * collective until the launcher gives up - and the launcher gives up only on
 * a nonzero status, so an exit(0) hung a release-build job (second review,
 * D14). So the job ends here, loudly: this node's abort word stops its other
 * PEs at their next sweep, and the status is made nonzero for the launcher.
 * Remote nodes see it when their own PEs on this node's side stop answering;
 * propagating it to them is §7.1 step 4's abort flood.
 *
 * start_pes programs are the exception the specification makes - they are
 * finalized implicitly at exit (§9.1, Annex F.2.3) - and deprecated.c
 * registers that finalize after this handler, so it runs first and this one
 * then finds nothing initialized.
 */
static void exit_without_finalize(void)
{
    if (refcount == 0 || getpid() != init_pid)
        return;
    fflush(NULL);
    char msg[256];
    int n = snprintf(msg, sizeof msg, "osoe: PE %u: the process exited without calling "
                     "shmem_finalize; ending the job (spec §4, §9.1.4)\n",
                     osoe.bootstrap.rank);
    if (n > 0) {
        ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n < sizeof msg ? (size_t)n
                                                                          : sizeof msg - 1);
        (void)ignored;
    }
    if (osoe.node.hdr && !atomic_load_explicit(&osoe.node.hdr->aborting, memory_order_acquire))
        osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, EXIT_FAILURE);
    _exit(EXIT_FAILURE);
}

static void session_start(void)
{
    osoe = (struct osoe_state){ 0 };
    osoe_bootstrap_read(&osoe.bootstrap);

    /*
     * Before the rendezvous, so that a job which fails to start still says
     * which library it was and how to ask it for help.
     */
    if (osoe.bootstrap.print_version)
        fprintf(stderr, "osoe: PE %u: " SHMEM_VENDOR_STRING ", OpenSHMEM %d.%d\n",
                osoe.bootstrap.rank, SHMEM_MAJOR_VERSION, SHMEM_MINOR_VERSION);
    if (osoe.bootstrap.print_info && osoe.bootstrap.local_id == 0)
        print_info();
    /*
     * §1's start-up check of the running kernel, before anything is created:
     * an old kernel is refused by name, rather than aborting mid-rendezvous
     * with the control segment and heaps already in /dev/shm (review round two,
     * portability F3). futex2 is all the library uses today; io_uring's checks
     * join it at step 3, when the library first opens a ring.
     */
    int futex_error = osoe_futex_check();
    if (futex_error) {
        struct utsname u;
        const char *release = uname(&u) == 0 ? u.release : "unknown";
        osoe_fatal("osoe needs Linux 6.7 or later, for the futex2 calls (they fail with "
                   "\"%s\"); this kernel is %s", strerror(futex_error), release);
    }

    /* A re-init after a full teardown is a fresh session (§6.4). Every PE runs
     * the same sequence of sessions, so the numbers agree. */
    uint32_t session = sessions++;
    osoe_node_join(&osoe.node, &osoe.bootstrap, session);
    /*
     * Then the job-wide rendezvous (§6.2): only node leads take part, and it is
     * what produces the rank -> node map that every later addressing decision
     * reads. It comes after the node's own rendezvous because a lead publishes
     * its node's rank list, which it learns there.
     */
    osoe_job_join(&osoe.job, &osoe.bootstrap, session);
    /*
     * The backend, then the allocator, then the teams, then the thread that
     * runs the backend - each needs the one before it. osoe_transport_init
     * starts whatever the backend needs (on AM, the connection area, the wires
     * and the engine), so it is complete before the agent that polls it
     * exists; the internode backend rides on it (internode.h); and the teams
     * hand their node sets to that backend while nothing else can read them.
     */
    osoe_transport_init();
    if (osoe_allocator_init(&osoe.allocator, osoe.node.heap_size) != 0)
        osoe_fatal("out of memory");
    osoe_internode_init();
    osoe_team_start();
    osoe_agent_start(&osoe.node.slots[osoe.bootstrap.local_id]);
    init_pid = getpid();
    static bool exit_handler;
    if (!exit_handler) {
        exit_handler = true;
        atexit(exit_without_finalize);
    }

    /*
     * SHMEM_DEBUG, after the rendezvous, because the two things worth knowing
     * are the ones it produces: the control segment to point osoe-stat at
     * (§3.14), and the heap base every PE agreed on (§4.1). This is the same
     * name §3.15 prints on an abort, said once at start-up instead.
     */
    if (osoe.bootstrap.debug)
        fprintf(stderr, "osoe: PE %u of %u (local %u of %u): segment /dev/shm%s, "
                "heap %llu bytes at %#llx, watchdog %s, node %u of %u%s\n",
                osoe.bootstrap.rank, osoe.bootstrap.world_size, osoe.bootstrap.local_id,
                osoe.bootstrap.local_count, osoe.node.control_name,
                (unsigned long long)osoe.node.heap_size,
                (unsigned long long)osoe.node.hdr->heap_base,
                osoe.bootstrap.watchdog ? "on" : "off",
                osoe.job.node, osoe.job.node_count,
                osoe.job.force_remote ? ", force-remote" : "");
}

static void session_end(void)
{
    /*
     * The agent first: its last act is the backend's exit (transport.h), which
     * on AM drains what the node still owes the network and closes every wire
     * on the thread that owns them. osoe_transport_fini then frees what is left,
     * with nothing polling it.
     */
    osoe_agent_stop(&osoe.node.slots[osoe.bootstrap.local_id]);
    osoe_internode_fini();
    osoe_allocator_fini(&osoe.allocator);
    osoe_transport_fini();
    osoe_job_leave(&osoe.job);
    osoe_node_leave(&osoe.node, &osoe.bootstrap);
    osoe_bootstrap_free(&osoe.bootstrap);
    /*
     * Otherwise osoe.bootstrap.world_size and the heap table stay exactly as
     * they were with the mappings behind them gone, so any routine that reads
     * `osoe` without going through osoe_require_initialized() first - or a
     * call from a process that never re-initializes - dereferences a stale
     * table instead of a null one (conformance review F5). session_start()
     * zeros it again at the next shmem_init, so this only matters for the
     * window after the last one.
     */
    osoe = (struct osoe_state){ 0 };
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
    atomic_store_explicit(&osoe_initialized, 1, memory_order_release);
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
     * only that, the last one before tearing down. On the finalize team's own
     * counters, so it can never be matched by an application's barrier. */
    osoe_barrier_finalize();
    refcount--;
    if (refcount == 0) {
        session_end();
        atomic_store_explicit(&osoe_initialized, 0, memory_order_release);
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
    atomic_store_explicit(&osoe_initialized, 1, memory_order_release);
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
    *flag = atomic_load_explicit(&osoe_initialized, memory_order_acquire);
}

/* See state.h: the cold half of osoe_require_initialized. */
_Noreturn void osoe_not_initialized(const char *routine)
{
    osoe_fatal("%s: called before shmem_init (or shmem_init_thread), or after "
               "the last shmem_finalize", routine);
}

OSOE_API int pshmem_my_pe(void)
{
    return atomic_load_explicit(&osoe_initialized, memory_order_acquire) ? (int)osoe.bootstrap.rank : -1;
}

OSOE_API int pshmem_n_pes(void)
{
    return atomic_load_explicit(&osoe_initialized, memory_order_acquire)
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
