/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe-launch - start L processes of one job on this machine, the way a Slurm
 * step would: for tests, and for running a job where there is no Slurm, as on a
 * desktop with the single-node build. Installed, in both builds (v0.2 step 1).
 *
 *   osoe-launch -n L [-m MAP] [-t SECONDS] [-k] [-s] [-e RANK:NAME=VALUE]...
 *               [-N NAMESPACE_PREFIX] [-H HOST_LIST] -- PROGRAM [ARGS]...
 *   osoe-launch --version
 *
 * Each process gets the OSOE_ bootstrap variables of design §6.2 - or, with
 * -s, the same variables named as srun names them, SLURM_ in place of OSOE_,
 * so that the job takes the bootstrap's Slurm path (§6.1) the way a real step
 * does, on one machine and at every tier. Without -m
 * the job has one node and L tasks: rank i is local id i. The job id is this
 * launcher's pid, so concurrent launches never share a shared-memory name. -e
 * sets or overrides one variable for one rank, for tests that need PEs to
 * disagree.
 *
 * -m MAP, or OSOE_NODE_MAP in this launcher's environment, is §8.1's logical
 * node map: one logical node id per rank, so that one machine runs a job of
 * several nodes. The launcher derives each rank's OSOE_NODEID, OSOE_LOCALID and
 * OSOE_GTIDS from it, which is what Slurm computes from a distribution on a
 * real step - so the library sees the same environment in a test as in
 * production and reads no test-only variable (base/node-map.h).
 *
 * Fails if any process exits nonzero or dies on a signal, if the timeout
 * (default 60 s) expires, or if the job left shared-memory objects named
 * /osoe.<job>.* behind after every process exited successfully: heap names are
 * removed once mapped and the control segment at the last finalize (§6.2), so
 * a clean job leaves none. Leftovers are removed in every case. Its status is
 * srun's (design §3.15): the highest exit code of any rank, 128 plus the signal
 * for a rank killed by a signal this launcher did not send itself, and 1 for
 * the launcher's own findings - a timeout, leftovers - when no rank reported
 * more; so a job ended by shmem_global_exit(status) exits with that status.
 * After the first failure the remaining processes get SIGTERM, then SIGKILL
 * two seconds later, since they are usually blocked waiting for the one that
 * failed. -k leaves them alone instead, so that what ends the job is the
 * library's own reaction to a dead PE rather than the launcher's; the timeout
 * still applies. It is the launcher's counterpart to Slurm's KillOnBadExit
 * (design §3.15).
 *
 * **A job never outlives its launcher.** SIGTERM or SIGINT to the launcher -
 * what ctest's timeout, a kill, or ^C sends - is forwarded to every process
 * still running, followed by SIGKILL two seconds later, and the launcher then
 * exits 128 + that signal once they are gone, removing leftovers as always.
 * A launcher that dies without the chance (SIGKILL) takes the job with it:
 * each child asks for SIGKILL when its parent dies (PR_SET_PDEATHSIG). Before
 * both, a killed launcher left its PEs running and their objects in /dev/shm
 * (review round two, omissions 6).
 *
 * **Tier 3 (design §8.2): -N and -H.** Every logical node's ranks normally run
 * in this machine's one network namespace, each lead on its own loopback
 * address (§8.1). -N PREFIX starts logical node i's ranks in the network
 * namespace PREFIX<i> instead - /run/netns/PREFIX<i>, made and joined by veth
 * links with netem on them by tools/launch/tier3.sh - and -H names the host list
 * those namespaces' addresses make, in place of 127.0.0.[1-N]. Entering a
 * namespace needs root, so tier 3 runs this launcher under sudo; each rank
 * enters its namespace between fork and exec and then returns to the user sudo
 * was run by (SUDO_USER, SUDO_UID, SUDO_GID), so that the job itself runs as it
 * would anywhere else.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base/node-map.h"

#ifndef OSOE_VERSION_STRING
#error "OSOE_VERSION_STRING comes from CMake: see cmake/OsoeVersion.cmake"
#endif

enum { MAX_PROCS = 1024, MAX_OVERRIDES = 64 };

struct override {
    long rank;
    char *assignment;       /* NAME=VALUE */
};

static void usage(FILE *out, int status)
{
    fprintf(out, "usage: osoe-launch -n L [-m NODE_MAP] [-t SECONDS] [-k] [-s] "
                 "[-e RANK:NAME=VALUE]... [-N NAMESPACE_PREFIX] [-H HOST_LIST] "
                 "-- PROGRAM [ARGS]...\n"
                 "       osoe-launch --version\n");
    if (out == stdout) {
        fprintf(out, "\n"
                     "  -n L            L processes, ranks 0 to L - 1, one job\n"
                     "  -m NODE_MAP     a logical node for each rank, as 0,0,1,1: a job of\n"
                     "                  several nodes on this one machine (OSOE_NODE_MAP sets it\n"
                     "                  too); without it, one node. The single-node build refuses\n"
                     "                  a job of more than one\n"
                     "  -t SECONDS      end the job after this long, 1 to 86400; default 60\n"
                     "  -k              after a rank fails, leave the others to the library\n"
                     "                  rather than ending them\n"
                     "  -s              name the variables as srun does, SLURM_ for OSOE_\n"
                     "  -e RANK:NAME=VALUE  set or override one variable for one rank\n"
                     "  -N PREFIX       start logical node i's ranks in network namespace\n"
                     "                  PREFIX<i> (tier 3, as root; tools/launch/tier3.sh)\n"
                     "  -H HOST_LIST    the host list those namespaces' addresses make\n");
        fprintf(out, "\nExit status, as srun's: the highest exit code of any rank; 128 plus "
                     "the signal for a\nrank killed by a signal the launcher did not send "
                     "itself; 1 for a timeout or\nshared memory left behind when no rank "
                     "reported more; 128 plus the signal when the\nlauncher itself is "
                     "interrupted; 2 for a bad argument.\n");
    }
    exit(status);
}

static long parse_long(const char *s, long lo, long hi, const char *what)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        fprintf(stderr, "osoe-launch: invalid %s '%s'\n", what, s);
        exit(2);
    }
    return v;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void setenv_or_die(const char *name, const char *value)
{
    if (setenv(name, value, 1) != 0) {
        perror("osoe-launch: setenv");
        _exit(127);
    }
}

/*
 * What one rank's bootstrap environment says about where it runs. Derived in
 * the parent before any fork, so the child between fork and exec only calls
 * setenv and execvp.
 */
struct placement {
    uint32_t node;
    uint32_t local_id;
    const char *gtids;      /* its node's ranks, in local-id order */
};

static uint32_t node_count_for_children;
static pid_t launcher_pid;
static const char *namespace_prefix;    /* -N: tier 3's per-node network namespaces */
static const char *host_list;           /* -H: their addresses, as OSOE_STEP_NODELIST */
static const char *launcher_prefix = "OSOE_";  /* -s: "SLURM_", srun's names (§6.1) */
static char launch_text[24];            /* OSOE_LAUNCH: which launch of this job id */

/* One of the bootstrap's launcher variables, under this launch's names. */
static void set_launcher_variable(const char *suffix, const char *value)
{
    char name[40];
    snprintf(name, sizeof name, "%s%s", launcher_prefix, suffix);
    setenv_or_die(name, value);
}

/*
 * Tier 3 (-N): put this rank in its logical node's network namespace, then give
 * up root for the user sudo ran the launcher for - that user's uid, gid and
 * groups. In the child between fork and exec, like everything else here;
 * initgroups reads the group database, which is safe there because the launcher
 * has one thread. The parent-death signal is cleared by the change of user
 * (prctl(2)), so it is asked for again.
 */
static void enter_namespace(uint32_t node)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "/run/netns/%s%u", namespace_prefix, node);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || setns(fd, CLONE_NEWNET) != 0) {
        fprintf(stderr, "osoe-launch: cannot enter %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(fd);
    const char *user = getenv("SUDO_USER");
    const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
    if (geteuid() != 0 || !uid || !gid)
        return;
    int groups = user ? initgroups(user, (gid_t)atol(gid)) : setgroups(0, NULL);
    if (groups != 0 || setgid((gid_t)atol(gid)) != 0 || setuid((uid_t)atol(uid)) != 0) {
        perror("osoe-launch: return to the invoking user");
        _exit(127);
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != launcher_pid)
        _exit(127);
}

/* Runs in the child between fork and exec. */
static void exec_rank(long rank, long n, const char *job, const struct placement *p,
                      const struct override *ov, int nov, char **argv)
{
    /*
     * Die with the launcher. The setting survives exec. A launcher that died
     * before the prctl has already re-parented this child, which the getppid
     * check sees; one that dies after it delivers the signal.
     */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != launcher_pid)
        _exit(127);
    if (namespace_prefix)
        enter_namespace(p->node);

    /* Under srun's names, nothing of ours may be left for the bootstrap to
     * prefer: OSOE_PROCID decides which names it reads (§6.1). */
    if (strcmp(launcher_prefix, "OSOE_") != 0)
        unsetenv("OSOE_PROCID");
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", rank);
    set_launcher_variable("PROCID", buf);
    snprintf(buf, sizeof buf, "%u", p->local_id);
    set_launcher_variable("LOCALID", buf);
    snprintf(buf, sizeof buf, "%ld", n);
    set_launcher_variable("STEP_NUM_TASKS", buf);
    snprintf(buf, sizeof buf, "%u", p->node);
    set_launcher_variable("NODEID", buf);
    snprintf(buf, sizeof buf, "%u", node_count_for_children);
    set_launcher_variable("STEP_NUM_NODES", buf);
    set_launcher_variable("JOB_ID", job);
    set_launcher_variable("STEP_ID", "0");
    /* Under srun's names a first run has no restart count, as under srun. */
    if (strcmp(launcher_prefix, "OSOE_") == 0)
        setenv_or_die("OSOE_LAUNCH", launch_text);
    set_launcher_variable("GTIDS", p->gtids);

    /*
     * The step's hosts, in node order (§6.1). One node: this host's name. More:
     * one loopback address per logical node - all of 127.0.0.0/8 is loopback on
     * Linux - so each node's lead listens on its own address, as on a cluster,
     * and the rendezvous resolves a host list rather than a special case (§8.1).
     */
    char host[HOST_NAME_MAX + 1] = "localhost";
    gethostname(host, sizeof host);
    host[sizeof host - 1] = '\0';
    if (node_count_for_children > 1)
        snprintf(host, sizeof host, "127.0.0.[1-%u]", node_count_for_children);
    set_launcher_variable("STEP_NODELIST", host_list ? host_list : host);

    for (int i = 0; i < nov; i++)
        if (ov[i].rank == rank && putenv(ov[i].assignment) != 0)
            _exit(127);

    execvp(argv[0], argv);
    fprintf(stderr, "osoe-launch: exec %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

/* A control segment of this job: <prefix><step>.<node>.<session>, three numbers. */
static bool is_control_segment(const char *rest)
{
    unsigned step, node, session;
    char tail;
    return sscanf(rest, "%u.%u.%u%c", &step, &node, &session, &tail) == 3;
}

/*
 * Remove /dev/shm/osoe.<job>.*; return how many there were. After a job that
 * got through init there are none - the library removes every name as soon as
 * everyone who needs it has it open (§3.14) - so anything here is a job that
 * died during init, or a library defect. With OSOE_KEEP_CONTROL_SEGMENT=1 the
 * control segments are kept on purpose for post-mortem reading: they are
 * reported, and left for osoe-stat --gc, since removing them here would throw
 * away exactly what the setting asked to keep.
 */
static int remove_leftovers(const char *job)
{
    char prefix[64];
    snprintf(prefix, sizeof prefix, "osoe.%s.", job);
    const char *keep = getenv("OSOE_KEEP_CONTROL_SEGMENT");
    bool kept = keep && strcmp(keep, "1") == 0;
    DIR *d = opendir("/dev/shm");
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
            continue;
        if (kept && is_control_segment(e->d_name + strlen(prefix))) {
            fprintf(stderr, "osoe-launch: kept for post-mortem reading: /dev/shm/%s\n",
                    e->d_name);
            continue;
        }
        fprintf(stderr, "osoe-launch: left behind: /dev/shm/%s\n", e->d_name);
        char path[PATH_MAX];
        snprintf(path, sizeof path, "/dev/shm/%s", e->d_name);
        unlink(path);
        count++;
    }
    closedir(d);
    return count;
}

int main(int argc, char **argv)
{
    long n = 0;
    long timeout = 60;
    bool keep = false;          /* -k: do not terminate the others on a failure */
    const char *map_text = getenv("OSOE_NODE_MAP");
    struct override ov[MAX_OVERRIDES];
    int nov = 0;

    /* --version alone, as every osoe tool answers it; getopt knows no long option. */
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("osoe-launch (osoe) " OSOE_VERSION_STRING "\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--help") == 0)
        usage(stdout, 0);
    int opt;
    while ((opt = getopt(argc, argv, "+n:m:t:kse:N:H:")) != -1) {
        switch (opt) {
        case 'n':
            n = parse_long(optarg, 1, MAX_PROCS, "process count");
            break;
        case 'm':
            map_text = optarg;
            break;
        case 't':
            timeout = parse_long(optarg, 1, 86400, "timeout");
            break;
        case 'k':
            keep = true;
            break;
        case 's':
            launcher_prefix = "SLURM_";
            break;
        case 'e': {
            char *colon = strchr(optarg, ':');
            if (!colon || !strchr(colon, '=') || nov == MAX_OVERRIDES)
                usage(stderr, 2);
            *colon = '\0';
            ov[nov].rank = parse_long(optarg, 0, MAX_PROCS - 1, "rank");
            ov[nov].assignment = colon + 1;
            nov++;
            break;
        }
        case 'N':
            namespace_prefix = optarg;
            break;
        case 'H':
            host_list = optarg;
            break;
        default:
            usage(stderr, 2);
        }
    }
    if (n == 0 || optind >= argc)
        usage(stderr, 2);
    char **prog = argv + optind;

    char job[32];
    snprintf(job, sizeof job, "%ld", (long)getpid());
    /* Which launch of that id this is (bootstrap.h): the id is this process's
     * pid, which a later launcher can have again, and a leftover of this one
     * must not read as that one's. */
    uint64_t launch = 0;
    if (getrandom(&launch, sizeof launch, 0) != (ssize_t)sizeof launch)
        launch = (uint64_t)time(NULL) << 22 ^ (uint64_t)getpid();
    snprintf(launch_text, sizeof launch_text, "%llu", (unsigned long long)launch);

    /*
     * Where each rank runs. Without a map the job is one logical node and rank
     * i is local id i, which is what every step-1 test launches. With one, the
     * ranks of a node are its gtids list in increasing rank order (§8.1).
     *
     * The map is parsed before anything is allocated, so a bad one exits
     * holding nothing; everything allocated here is freed before main returns.
     * This is the parent, which lives as long as the job, so what a child used
     * to leak up to its exec LeakSanitizer now sees under the asan preset.
     */
    struct osoe_node_map map = { 0 };
    uint32_t node_count = 1;
    if (map_text) {
        if (osoe_node_map_parse(map_text, (uint32_t)n, &map) != 0) {
            fprintf(stderr, "osoe-launch: node map '%s' is not %ld logical node "
                            "ids using every id from 0 upward (design §8.1)\n",
                    map_text, n);
            return 2;
        }
        node_count = map.node_count;
    }

    struct placement *place = calloc((size_t)n, sizeof *place);
    char **gtids_of_node = calloc(node_count, sizeof *gtids_of_node);
    uint32_t *ranks = malloc(sizeof *ranks * (size_t)n);
    if (!place || !gtids_of_node || !ranks) {
        fprintf(stderr, "osoe-launch: out of memory\n");
        return 1;
    }
    for (uint32_t node = 0; node < node_count; node++) {
        uint32_t count;
        if (map_text) {
            count = osoe_node_map_gtids(&map, node, ranks, (uint32_t)n);
        } else {
            count = (uint32_t)n;
            for (uint32_t i = 0; i < count; i++)
                ranks[i] = i;
        }
        size_t len = (size_t)count * 12 + 1;
        char *text = malloc(len);
        if (!text) {
            fprintf(stderr, "osoe-launch: out of memory\n");
            return 1;
        }
        size_t off = 0;
        for (uint32_t i = 0; i < count; i++)
            off += (size_t)snprintf(text + off, len - off, i ? ",%u" : "%u", ranks[i]);
        gtids_of_node[node] = text;
        for (uint32_t i = 0; i < count; i++)
            place[ranks[i]] = (struct placement){ node, i, text };
    }
    free(ranks);
    osoe_node_map_free(&map);
    node_count_for_children = node_count;
    /* One loopback address per logical node (exec_rank): 127.0.0.1 to .254. */
    if (node_count > 254 && !host_list) {
        fprintf(stderr, "osoe-launch: %u logical nodes; at most 254 get a loopback address "
                        "each\n", node_count);
        return 1;
    }

    /*
     * SIGCHLD, SIGTERM and SIGINT are consumed with sigtimedwait, so block them
     * before any fork. A signal that is blocked is queued even if its
     * disposition is to ignore it - a shell starts a background job with SIGINT
     * ignored - so the launcher still hears it. The children get both back at
     * their default, or a forwarded signal would be ignored too.
     */
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigaddset(&chld, SIGTERM);
    sigaddset(&chld, SIGINT);
    sigprocmask(SIG_BLOCK, &chld, NULL);
    launcher_pid = getpid();

    pid_t pids[MAX_PROCS];
    bool done[MAX_PROCS] = { false };
    for (long i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("osoe-launch: fork");
            for (long j = 0; j < i; j++)
                kill(pids[j], SIGKILL);
            return 1;
        }
        if (pids[i] == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            sigprocmask(SIG_UNBLOCK, &chld, NULL);
            exec_rank(i, n, job, &place[i], ov, nov, prog);
        }
    }

    double deadline = now() + (double)timeout;
    double kill_at = 0;             /* SIGKILL time once a failure is seen */
    bool failed = false, timed_out = false;
    /* The status, srun's way: the highest code a rank reported, and a signal
     * as 128 plus it - unless this launcher sent that signal, after a failure,
     * at the timeout or forwarding its own, when the rank's end is the
     * launcher's. Which signals were sent is kept a rank, not only whether: a
     * rank that died of its own signal before the launcher's reached it, and
     * was reaped after, still counts (the adversarial read of 2026-10-02). */
    int highest = 0;
    uint64_t sent[MAX_PROCS] = { 0 };       /* bit s: signal s was sent */
    int interrupted = 0;            /* SIGTERM or SIGINT received, once */
    long remaining = n;

    while (remaining > 0) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            long r = 0;
            while (r < n && pids[r] != pid)
                r++;
            if (r == n || done[r])
                continue;           /* not one of ours: a grandchild re-parented? */
            done[r] = true;
            remaining--;
            bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            int code = WIFEXITED(status) ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) &&
                                 !(sent[r] & UINT64_C(1) << (WTERMSIG(status) & 63))
                           ? 128 + WTERMSIG(status) : 0;
            if (code > highest)
                highest = code;
            if (!ok) {
                if (WIFSIGNALED(status))
                    fprintf(stderr, "osoe-launch: rank %ld killed by signal %d\n", r,
                            WTERMSIG(status));
                else if (!failed || !timed_out)
                    fprintf(stderr, "osoe-launch: rank %ld exited with status %d\n", r,
                            WEXITSTATUS(status));
                if (!failed) {
                    failed = true;
                    if (!keep) {
                        for (long j = 0; j < n; j++)
                            if (!done[j]) {
                                kill(pids[j], SIGTERM);
                                sent[j] |= UINT64_C(1) << SIGTERM;
                            }
                        kill_at = now() + 2.0;
                    }
                }
            }
        }
        if (remaining == 0)
            break;

        double t = now();
        if (!timed_out && t >= deadline) {
            timed_out = true;
            failed = true;
            fprintf(stderr, "osoe-launch: timeout after %ld s; killing %ld process(es); -t "
                    "sets the timeout\n",
                    timeout, remaining);
            for (long j = 0; j < n; j++)
                if (!done[j]) {
                    kill(pids[j], SIGKILL);
                    sent[j] |= UINT64_C(1) << SIGKILL;
                }
        } else if (kill_at > 0 && t >= kill_at) {
            for (long j = 0; j < n; j++)
                if (!done[j]) {
                    kill(pids[j], SIGKILL);
                    sent[j] |= UINT64_C(1) << SIGKILL;
                }
            kill_at = 0;
        }

        double until = timed_out ? t + 1.0 : (kill_at > 0 ? kill_at : deadline);
        double wait = until - t;
        if (wait < 0.01)
            wait = 0.01;
        struct timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
        int sig = sigtimedwait(&chld, NULL, &ts);
        if ((sig == SIGTERM || sig == SIGINT) && !interrupted) {
            interrupted = sig;
            failed = true;
            fprintf(stderr, "osoe-launch: %s received; forwarded to %ld process(es)\n",
                    sig == SIGTERM ? "SIGTERM" : "SIGINT", remaining);
            for (long j = 0; j < n; j++)
                if (!done[j]) {
                    kill(pids[j], sig);
                    sent[j] |= UINT64_C(1) << sig;
                }
            if (kill_at == 0)
                kill_at = now() + 2.0;
        }
    }

    /* Test programs' own coordination tables (shmem/tests/arrivals.h) are not
     * the library's: remove them quietly. */
    char table[64];
    snprintf(table, sizeof table, "/dev/shm/osoetest.%s", job);
    unlink(table);

    for (uint32_t node = 0; node < node_count; node++)
        free(gtids_of_node[node]);
    free(gtids_of_node);
    free(place);

    int left = remove_leftovers(job);
    if (left && !failed) {
        fprintf(stderr, "osoe-launch: every process succeeded but %d shared-memory "
                        "object(s) were left behind\n", left);
        failed = true;
    }
    if (interrupted)
        return 128 + interrupted;
    if (highest == 0 && failed)
        return 1;
    return highest;
}
