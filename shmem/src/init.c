/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Initialization lifecycle (§6.4): shmem_init, shmem_finalize,
 * shmem_query_initialized, shmem_my_pe, shmem_n_pes.
 */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <pshmem.h>
#include <shmem.h>

#include "abort.h"
#include "agent.h"
#include "base/deadline.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "barrier.h"
#include "context.h"
#include "deadlock.h"
#include "export.h"
#include "fatal.h"
#include "internode.h"
#include "job.h"
#include "link-measure.h"
#include "liveness.h"
#include "lock.h"
#include "metrics.h"
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
 * The thread level (§9.2, §4.6, §4.11). osoe provides every level, and since
 * phase 1 step 8 slice 2 SHMEM_THREAD_MULTIPLE to a program that asks for it
 * with shmem_init_thread. shmem_init lets the implementation choose (§9.2.2),
 * and osoe chooses SERIALIZED, OSOE_INIT_LEVEL: what MULTIPLE adds - a mutex
 * around every call on a shareable context, until §4.11's lock-free shared
 * mode (slice 3) - is a cost only a program with threads should pay.
 *
 * The level is fixed by the *first* initialization of a session and does not
 * change afterwards, which spec §9.2.1 permits in as many words ("may not be
 * able to change the level of threading support provided after the first
 * initialization call has been made"). shmem_query_thread may be called at any
 * time, including before any init, where the answer is the level a
 * shmem_init would provide.
 */
#define OSOE_THREAD_MAX SHMEM_THREAD_MULTIPLE
#define OSOE_INIT_LEVEL SHMEM_THREAD_SERIALIZED
static _Atomic int thread_level = OSOE_INIT_LEVEL;

/*
 * What the library calls itself: the vendor string (shmem.h) and the project's
 * version, which CMake gives libosoe as OSOE_VERSION_STRING from the one value
 * that names it (cmake/OsoeVersion.cmake) - "osoe 0.1.0". SHMEM_VERSION's line
 * and the first line of SHMEM_INFO's text begin with it.
 */
#ifndef OSOE_VERSION_STRING
#error "OSOE_VERSION_STRING comes from CMake: see cmake/OsoeVersion.cmake"
#endif
#define OSOE_NAME_AND_VERSION SHMEM_VENDOR_STRING " " OSOE_VERSION_STRING

/*
 * What SHMEM_INFO prints (§8). The spec says "print helpful text about all
 * these environment variables", so it is the specification's four plus the
 * OSOE_ ones a user of this implementation would otherwise have to read the
 * source to find. Written to stderr, because stdout belongs to the program.
 */
#ifndef OSOE_BUILD_NOTE
#error "OSOE_BUILD_NOTE comes from CMake: see cmake/OsoeVersion.cmake"
#endif

static void print_info(void)
{
    fprintf(stderr,
OSOE_NAME_AND_VERSION " environment variables\n"
"\n"
OSOE_BUILD_NOTE
"  From the OpenSHMEM specification (§8). Each also has a deprecated SMA_\n"
"  spelling; when both are set the SHMEM_ one controls.\n"
"    SHMEM_SYMMETRIC_SIZE   symmetric heap bytes per PE, with an optional\n"
"                           k, m, g or t multiplier (default 256m)\n"
"    SHMEM_VERSION          print the library version at start-up\n"
"    SHMEM_INFO             print this text\n"
"    SHMEM_DEBUG            print what the library set up, per PE\n"
"\n"
"  Specific to osoe:\n"
"    OSOE_MAX_THREADS       live threads of a PE that have called the library,\n"
"                           the main thread included: a thread takes its place\n"
"                           at its first call after init that does more than\n"
"                           report on the job, and gives it back when it exits.\n"
"                           1 to 1023, the same on every PE of a node; default\n"
"                           16 (§3.14)\n"
"    OSOE_MAX_TEAMS         teams one PE may be in at once, the three\n"
"                           predefined teams included: a destroyed team gives\n"
"                           its number back. 4 to 1024, the same on every node;\n"
"                           default 256 (§5.7)\n"
"    OSOE_TEAM_BLOCKS       teams a node can hold at once, the three\n"
"                           predefined teams' included: its pool of team\n"
"                           blocks. 8 to 65536, the same on every PE of a\n"
"                           node; default 1024 (§5.7)\n"
"    OSOE_MAX_CONTEXTS      contexts a PE may have at once, the default one\n"
"                           included: 1 to 4096, the same on every PE of a\n"
"                           node; default 64 (§4.11)\n"
"    OSOE_GET_LANDING_BYTES bytes of each PE's landing zone for its gets'\n"
"                           replies, a multiple of 1024 from 1024 to\n"
"                           67107840, the same on every PE of a node;\n"
"                           default 262144 (§4.6, §4.11)\n"
"    OSOE_NO_PROGRESS_TIMEOUT\n"
"                           seconds a wait may see no progress before it ends\n"
"                           the job; 0, the default, means never (§3.15)\n"
"    OSOE_HEAP_BASE         the address every PE maps its heap at (§4.1)\n"
"    OSOE_HEAP_SHIFT        a test mode: each PE maps its own heap this many\n"
"                           bytes times its rank past that address, so no two\n"
"                           PEs share a symmetric address (§8.2)\n"
"    OSOE_KEEP_CONTROL_SEGMENT\n"
"                           1: keep the control segment's name until finalize,\n"
"                           so a crash leaves it for post-mortem reading and\n"
"                           osoe-stat --gc; by default no name survives init\n"
"                           (§3.14)\n"
"    OSOE_COLLECTIVE_FANOUT\n"
"                           k of the internode tree collectives run over,\n"
"                           2 to 32, the same on every node; the default is\n"
"                           the backend's, 4 on AM (§5.3)\n"
"    OSOE_COLLECTIVE_PIPELINE\n"
"                           pieces of a large data collective each node keeps\n"
"                           in flight up and down that tree, 1 to 8, the same\n"
"                           on every node; default 2 (§5.8)\n"
"    OSOE_SMALL_COLLECTIVE_BYTES\n"
"                           the largest reduction, broadcast or scan that\n"
"                           moves through a team's own slots in one piece, 128\n"
"                           to 16384, the same on every node; a larger one goes\n"
"                           through the lanes of the threads that make it;\n"
"                           default 1024\n"
"                           (§5.8)\n"
"    OSOE_COLLECTIVE_SHAPE  link, the default, or fixed, the same on every\n"
"                           node: link is to choose a large reduction's or\n"
"                           broadcast's fan-out and window from the link,\n"
"                           fixed keeps those above; today both run as fixed\n"
"    OSOE_LINK_HOP_NS, OSOE_LINK_MESSAGE_NS, OSOE_LINK_BYTES_PER_SECOND\n"
"                           the link's hop and message cost in nanoseconds\n"
"                           and its bandwidth, all three or none, the same on\n"
"                           every node: for tests and fabrics known in advance\n"
"    OSOE_SHM_CAPACITY_CHECK\n"
"                           0: report a request larger than /dev/shm's free\n"
"                           space and continue, for testing on a small\n"
"                           /dev/shm; by default init refuses it (§6.2)\n"
"    OSOE_PORT_RANGE        LOW-HIGH: the ports a node's lead may listen on,\n"
"                           within 1024-65535; default 17000-26999 (§6.2)\n"
"    OSOE_JOB_SECRET        32 hexadecimal digits: a secret every node of the\n"
"                           job proves it knows when it joins, never sent\n"
"                           (§6.2)\n"
"    OSOE_KEEPALIVE         IDLE,INTERVAL,COUNT: TCP keepalive between nodes,\n"
"                           in seconds, seconds and probes; default 60,10,6;\n"
"                           0 for none (§3.15)\n"
"    OSOE_AGENT_SLICE_US    the scheduler slice each PE's agent thread asks\n"
"                           for, 100 to 100000 microseconds, or 0 for the\n"
"                           kernel's own; default 100, which lets a node's\n"
"                           progress thread preempt sooner on a shared core\n"
"                           (Linux 6.12 and later; §3.11)\n"
"    OSOE_AGENT_CPUS        task, the default: each PE's agent thread runs on\n"
"                           its PE's CPUs; or node: on any CPU any PE of its\n"
"                           node may, which helps a bound job's messages on a\n"
"                           kernel that ignores OSOE_AGENT_SLICE_US (§3.11)\n"
"    OSOE_DEADLOCK_SECONDS  how long a job whose every PE waits in the library,\n"
"                           nothing moving on any node and nothing in flight,\n"
"                           is watched before it ends as deadlocked, naming\n"
"                           where each PE waits: 1 to 86400, or 0 for never;\n"
"                           default 10 (§3.15)\n"
"\n"
"  The bootstrap variables - PROCID, LOCALID, GTIDS, STEP_NUM_TASKS, NODEID,\n"
"  STEP_NUM_NODES, STEP_NODELIST, JOB_ID and STEP_ID - are srun's, with SLURM_\n"
"  before each, or osoe-launch's, with OSOE_; OSOE_PROCID decides which\n"
"  (design §6.1).\n"
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
 * D14). So the job ends here, loudly: this node's abort word ends its other
 * PEs, computing or not (abort.h), and the status is made nonzero for the
 * launcher. The other nodes take this node's end of stream for a death, or,
 * if its lead's agent speaks first, read its abortive goodbye (§3.15).
 *
 * start_pes programs are the exception the specification makes - they are
 * finalized implicitly at exit (§9.1, Annex F.2.3) - and deprecated.c
 * registers that finalize after this handler, so it runs first and this one
 * then finds nothing initialized.
 */
static void exit_without_finalize(void)
{
    /* osoe_initialized, not refcount, which the mutex guards: this may run on
     * the agent, ending the process while the main thread is still finishing
     * shmem_init; and init_pid only once that acquire load has found it set,
     * after init_pid was written. */
    if (atomic_load_explicit(&osoe_initialized, memory_order_acquire) == 0 ||
        getpid() != init_pid || osoe_abort_stand_aside())
        return;
    /* A global exit already declared is what this exit is part of: its end
     * is this PE's end, with its status (abort.h). */
    uint32_t pe;
    int status;
    if (osoe_node_abort_read(&osoe.node, &pe, &status) == OSOE_ABORT_GLOBAL_EXIT)
        osoe_abort_end();
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
    /* A global exit declared since the read above wins the word, and this PE
     * then ends as it does, with its status - as fatal.c routes a fatal that
     * loses (review of the watchdog's retirement). */
    if (!osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, EXIT_FAILURE,
                                 OSOE_ABORT_FATAL) &&
        osoe_node_abort_read(&osoe.node, &pe, &status) == OSOE_ABORT_GLOBAL_EXIT)
        osoe_abort_end();
    _exit(EXIT_FAILURE);
}

/* From the moment this PE knows who it is until its node has joined, a fatal
 * of its own tells the node (node.h): the session is the one about to start. */
static void identified(const struct osoe_bootstrap *b)
{
    osoe_node_refusal_arm(&osoe.node, b, sessions);
}

static void session_start(void)
{
    osoe = (struct osoe_state){ 0 };
    osoe_bootstrap_identified = identified;
    osoe_bootstrap_read(&osoe.bootstrap);

    /*
     * Before the rendezvous, so that a job which fails to start still says
     * which library it was and how to ask it for help.
     */
    if (osoe.bootstrap.print_version)
        fprintf(stderr, "osoe: PE %u: " OSOE_NAME_AND_VERSION ", OpenSHMEM %d.%d\n",
                osoe.bootstrap.rank, SHMEM_MAJOR_VERSION, SHMEM_MINOR_VERSION);
    if (osoe.bootstrap.print_info && osoe.bootstrap.local_id == 0)
        print_info();
    /*
     * §6.2: init raises the soft RLIMIT_NOFILE to the hard limit, before any
     * socket or pidfd is opened. Slurm passes on the submitting shell's
     * limits, often a soft 1,024, and a lead holds a socket per peer node and
     * a pidfd per other local PE (liveness.h): at 256 PEs a node, 1,024 runs
     * out short of 800 nodes. A program that ran before cannot fail for it:
     * only a descriptor it could not have had lands above the old limit.
     */
    struct rlimit files;
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max) {
        files.rlim_cur = files.rlim_max;
        (void)setrlimit(RLIMIT_NOFILE, &files);
    }
    /*
     * §1's start-up check of the running kernel, before anything is created:
     * an old kernel is refused by name, rather than aborting mid-rendezvous
     * with the control segment and heaps already in /dev/shm (review round two,
     * portability F3). The futex calls are what every job uses; what a job of
     * more than one node also needs - io_uring, on AM - the transport checks
     * with its settings below, in the same way (internode-backend.h).
     *
     * Told to nobody: the kernel is every local PE's, so each refuses it by
     * itself, and a refusal's last word (node.h) would leave the node a
     * tombstone in /dev/shm for nothing - and wake its waiters with the very
     * futex call that fails here.
     */
    osoe_node_refusal_disarm();
    int futex_error = osoe_futex_check();
    if (futex_error) {
        struct utsname u;
        const char *release = uname(&u) == 0 ? u.release : "unknown";
        osoe_fatal("osoe needs %s (they fail with \"%s\"); this kernel is %s",
                   osoe_futex_requirement(), strerror(futex_error), release);
    }
    osoe_node_refusal_arm(&osoe.node, &osoe.bootstrap, sessions);

    /*
     * Every setting the node's layout and the wire depend on is checked by
     * every PE before its node joins (the round-three review's R8): until then
     * the AM layer's (OSOE_AM_*, OSOE_GET_LANDING_BYTES) were read by the lead
     * alone as it laid out the segment, and the wire's by leads alone after the
     * rendezvous, so a bad value ended one PE and left the rest waiting, or was
     * never read. The backend checks its own (transport.h), values only, after
     * what it needs of the kernel for a job of this many nodes, which it
     * refuses telling no one, as the futex check above; their users read the
     * settings again where they use them. A few are read only after the
     * join - the deadlock detector's, the agent's CPUs - and a bad one there
     * ends the whole job, as any failure after the join does (the release
     * fixes' review, Q5-6).
     */
    osoe_transport_settings_check(&osoe.bootstrap);

    /* A re-init after a full teardown is a fresh session (§6.4). Every PE runs
     * the same sequence of sessions, so the numbers agree. */
    uint32_t session = sessions++;
    osoe_node_join(&osoe.node, &osoe.bootstrap, session);
    /* Joined: from here a fatal tells the node by its abort word, which every
     * wait of the node reads. */
    osoe_node_refusal_disarm();
    /* Every local PE has published its pid: from here a death is watched for,
     * by the sweeps of init's own waits until the agent takes over (liveness.h). */
    osoe_liveness_start();
    /*
     * Then the job-wide rendezvous (§6.2): only node leads take part, and it is
     * what produces the rank -> node map that every later addressing decision
     * reads. It comes after the node's own rendezvous because a lead publishes
     * its node's rank list, which it learns there.
     */
    osoe_job_join(&osoe.job, &osoe.bootstrap, session, osoe.node.hdr->creator_pid);
    /*
     * The backend, then the allocator, then the teams, then the thread that
     * runs the backend - each needs the one before it. osoe_transport_init
     * starts whatever the backend needs (on AM, the connection area, the wires
     * and the engine), so it is complete before the agent that polls it
     * exists; the internode backend rides on it (internode.h); and the node's
     * lead writes the teams' records, which that backend learns them from,
     * before its agent - their reader - exists.
     */
    osoe_transport_init();
    /* The level was set before session_start; under MULTIPLE the default
     * context and what every context shares take several threads at once. */
    bool concurrent = atomic_load_explicit(&thread_level, memory_order_acquire) ==
                      SHMEM_THREAD_MULTIPLE;
    osoe_transport_threads(concurrent);
    osoe_metrics_threads(concurrent);
    osoe_context_start();
    if (osoe_allocator_init(&osoe.allocator, osoe.node.heap_size) != 0)
        osoe_fatal("out of memory");
    osoe_internode_init();
    osoe_team_start();
    osoe_deadlock_init();
    osoe_agent_start(&osoe.node.slots[osoe.bootstrap.local_id]);
    /*
     * No PE returns from init before its node's lead can speak for the node
     * (§3.15). A non-lead is done once the connection area is open, while its
     * lead may still be connecting to the other nodes, with no agent - and an
     * abort started then, a shmem_global_exit straight after init, would end
     * the lead from its main thread, telling no node: those connected would
     * take its end of stream for a death, those still dialling would retry
     * forever. So the lead says its agent runs, and every other PE waits for
     * that, sweeping, so that a lead that dies meanwhile is found and ends the
     * wait (liveness.h). A futex word that is its own predicate (§3.16). O(1)
     * per PE, once a session.
     *
     * The same word carries the node's names (§6.2). The lead removed its
     * heaps' and control segment's names in osoe_node_join, before this store,
     * so a PE whose acquire load reads the store returns from init with them
     * gone - what shmem.fork.* checks. The removal is a system call, which
     * C11 does not see; what orders it is that shm_unlink had returned before
     * the release store was made, the load that reads the store comes after
     * it, and a name an unlink has removed is gone for every lookup begun
     * after the call returned.
     */
    _Atomic uint32_t *speaks = &osoe.node.hdr->lead_agent_running;
    if (osoe.bootstrap.local_id == 0) {
        atomic_store_explicit(speaks, 1, memory_order_release);
        osoe_futex_wake(speaks, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        /* Silent, as job.c's follow is: the lead speaks for the node, and what
         * it is doing meanwhile - meeting the other nodes, starting its agent -
         * has notices of its own. The deadline outlives a wake, so a signal
         * cannot hold the sweep off, and OSOE_NO_PROGRESS_TIMEOUT ends the wait
         * as it ends the node's others (Part 6's adversarial reads). */
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, osoe.bootstrap.no_progress_ns);
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        while (atomic_load_explicit(speaks, memory_order_acquire) == 0) {
            osoe_futex_wait(speaks, 0, &sweep, OSOE_FUTEX_SHARED);
            if (!osoe_deadline_expired(&sweep))
                continue;
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead, "
                           "rank %u, has not started its agent thread",
                           (unsigned long long)osoe_stall_watch_seconds(&watch),
                           osoe.bootstrap.gtids[0]);
            osoe_sweep_expired("shmem_init");
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        }
    }
    /*
     * The link the large collectives' shapes are chosen from (v0.2 step 2):
     * measured now, through the data path with every agent running - the hop
     * a collective pays - and handed to every PE before any collective can
     * choose; or as set, or none (link-measure.h).
     */
    osoe_link_establish();
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
    if (osoe.bootstrap.debug) {
        /* And the link, as the shapes will see it. One buffer, one write, as
         * fatal.c's: every PE says this at once, and separate writes would
         * interleave inside each other's lines. */
        char link[192] = "", line[544];
        const struct osoe_link *l = &osoe.job.link;
        if (osoe.job.link_source != OSOE_LINK_NONE) {
            int used = snprintf(link, sizeof link, "; link %s: a hop of %llu ns, a message cost "
                                "of %llu ns, %llu bytes a second",
                                osoe.job.link_source == OSOE_LINK_MEASURED ? "measured" : "set",
                                (unsigned long long)l->hop_ns, (unsigned long long)l->message_ns,
                                (unsigned long long)l->bytes_per_second);
            if (osoe.job.link_source == OSOE_LINK_MEASURED && used > 0 &&
                (size_t)used < sizeof link)
                snprintf(link + used, sizeof link - (size_t)used, " (%llu us of this start)",
                         (unsigned long long)(osoe.job.link_ns / 1000));
        }
        int n = snprintf(line, sizeof line, "osoe: PE %u of %u (local %u of %u): segment "
                         "/dev/shm%s, heap %llu bytes at %#llx, node %u of %u%s%s\n",
                         osoe.bootstrap.rank, osoe.bootstrap.world_size, osoe.bootstrap.local_id,
                         osoe.bootstrap.local_count, osoe.node.control_name,
                         (unsigned long long)osoe.node.heap_size,
                         (unsigned long long)(uintptr_t)osoe.node.heap_of[osoe.bootstrap.local_id],
                         osoe.job.node, osoe.job.node_count,
                         osoe.job.force_remote ? ", force-remote" : "", link);
        if (n > 0) {
            size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
            ssize_t ignored = write(STDERR_FILENO, line, len);
            (void)ignored;
        }
    }
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
    osoe_liveness_stop();
    osoe_internode_fini();
    osoe_team_stop();
    osoe_allocator_fini(&osoe.allocator);
    osoe_lock_session_end();
    osoe_context_stop();
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
         * (§9.2.2): SERIALIZED, which costs no program anything (above). */
        atomic_store_explicit(&thread_level, OSOE_INIT_LEVEL, memory_order_release);
        session_start();
    }
    refcount++;
    atomic_store_explicit(&osoe_initialized, 1, memory_order_release);
    pthread_mutex_unlock(&lifecycle);
}

OSOE_API void pshmem_finalize(void)
{
    /* A process ending on an abort runs no collective on the way out: a
     * program's own atexit(shmem_finalize) would wait for PEs that are
     * exiting (abort.h; the rule of conformance F1). Only the thread ending the
     * process returns; any other waits for that end here. */
    if (osoe_abort_stand_aside())
        return;
    pthread_mutex_lock(&lifecycle);
    /* A child forked after init inherits the mappings but no agent, and must
     * not tear down what its parent still uses (§3.12, §6.4). */
    if (refcount == 0 || getpid() != init_pid) {
        pthread_mutex_unlock(&lifecycle);
        return;
    }
    if (refcount == 1) {
        osoe_lock_warn_held();
        /* Every context the program left goes, with its implicit quiet, while
         * the transport is up to complete it (context.h). */
        osoe_context_finalize();
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
        atomic_store_explicit(&thread_level, OSOE_INIT_LEVEL, memory_order_release);
    }
    pthread_mutex_unlock(&lifecycle);
}

/*
 * spec §9.1.6: end every PE of the job, whatever each is doing, and the job
 * with `status` (design §3.15, phase 1 step 4). This PE declares its node's
 * abort as a global exit - the first declarer on a node wins, and the
 * specification allows any caller's status - which wakes every local agent to
 * end its process (abort.h), and then ends its own: exit(status), so its
 * handlers run and standard I/O is flushed. Every other PE leaves with 0, so
 * that the job's status is this one's (abort.h says why). Before init, or in a
 * child forked after it, there is no job to end but this process.
 */
OSOE_API void pshmem_global_exit(int status)
{
    /* Unlocked, like exit_without_finalize: osoe_initialized, then init_pid. */
    if (atomic_load_explicit(&osoe_initialized, memory_order_acquire) == 0 ||
        getpid() != init_pid)
        exit(status);
    osoe_abort_caller();
    osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, status, OSOE_ABORT_GLOBAL_EXIT);
    osoe_abort_end();
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
OSOE_ALIAS(global_exit)
OSOE_ALIAS(init_thread)
OSOE_ALIAS(query_thread)
OSOE_ALIAS(query_initialized)
OSOE_ALIAS(my_pe)
OSOE_ALIAS(n_pes)
