/*
 * osoe-launch - start L processes of one job on this machine, the way a Slurm
 * step would, for tests. Not installed.
 *
 *   osoe-launch -n L [-m MAP] [-t SECONDS] [-k] [-e RANK:NAME=VALUE]...
 *               -- PROGRAM [ARGS]...
 *
 * Each process gets the OSOE_ bootstrap variables of design §6.2. Without -m
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
 * production and reads no test-only variable (base/nodemap.h).
 *
 * Fails (exit 1) if any process exits nonzero or dies on a signal, if the
 * timeout (default 60 s) expires, or if the job left shared-memory objects
 * named /osoe.<job>.* behind after every process exited successfully: heap
 * names are removed once mapped and the control segment at the last finalize
 * (§6.2), so a clean job leaves none. Leftovers are removed in every case.
 * After the first failure the remaining processes get SIGTERM, then SIGKILL
 * two seconds later, since they are usually blocked waiting for the one that
 * failed. -k leaves them alone instead, so that what ends the job is the
 * library's own reaction to a dead PE rather than the launcher's; the timeout
 * still applies. It is the launcher's counterpart to Slurm's KillOnBadExit
 * (design §3.15).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base/nodemap.h"

enum { MAX_PROCS = 1024, MAX_OVERRIDES = 64 };

struct override {
    long rank;
    char *assignment;       /* NAME=VALUE */
};

static void usage(void)
{
    fprintf(stderr, "usage: osoe-launch -n L [-m NODE_MAP] [-t SECONDS] [-k] "
                    "[-e RANK:NAME=VALUE]... -- PROGRAM [ARGS]...\n");
    exit(2);
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

/* Runs in the child between fork and exec. */
static void exec_rank(long rank, long n, const char *job, const struct placement *p,
                      const struct override *ov, int nov, char **argv)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", rank);
    setenv_or_die("OSOE_PROCID", buf);
    snprintf(buf, sizeof buf, "%u", p->local_id);
    setenv_or_die("OSOE_LOCALID", buf);
    snprintf(buf, sizeof buf, "%ld", n);
    setenv_or_die("OSOE_STEP_NUM_TASKS", buf);
    snprintf(buf, sizeof buf, "%u", p->node);
    setenv_or_die("OSOE_NODEID", buf);
    setenv_or_die("OSOE_JOB_ID", job);
    setenv_or_die("OSOE_STEP_ID", "0");
    setenv_or_die("OSOE_GTIDS", p->gtids);

    char host[HOST_NAME_MAX + 1] = "localhost";
    gethostname(host, sizeof host);
    host[sizeof host - 1] = '\0';
    setenv_or_die("OSOE_STEP_NODELIST", host);

    for (int i = 0; i < nov; i++)
        if (ov[i].rank == rank && putenv(ov[i].assignment) != 0)
            _exit(127);

    execvp(argv[0], argv);
    fprintf(stderr, "osoe-launch: exec %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

/* Remove /dev/shm/osoe.<job>.*; return how many there were. */
static int remove_leftovers(const char *job)
{
    char prefix[64];
    snprintf(prefix, sizeof prefix, "osoe.%s.", job);
    DIR *d = opendir("/dev/shm");
    if (!d)
        return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
            continue;
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

    int opt;
    while ((opt = getopt(argc, argv, "+n:m:t:ke:")) != -1) {
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
        case 'e': {
            char *colon = strchr(optarg, ':');
            if (!colon || !strchr(colon, '=') || nov == MAX_OVERRIDES)
                usage();
            *colon = '\0';
            ov[nov].rank = parse_long(optarg, 0, MAX_PROCS - 1, "rank");
            ov[nov].assignment = colon + 1;
            nov++;
            break;
        }
        default:
            usage();
        }
    }
    if (n == 0 || optind >= argc)
        usage();
    char **prog = argv + optind;

    char job[32];
    snprintf(job, sizeof job, "%ld", (long)getpid());

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

    /* SIGCHLD is consumed with sigtimedwait, so block it before any fork. */
    sigset_t chld;
    sigemptyset(&chld);
    sigaddset(&chld, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld, NULL);

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
            sigprocmask(SIG_UNBLOCK, &chld, NULL);
            exec_rank(i, n, job, &place[i], ov, nov, prog);
        }
    }

    double deadline = now() + (double)timeout;
    double kill_at = 0;             /* SIGKILL time once a failure is seen */
    bool failed = false, timed_out = false;
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
                            if (!done[j])
                                kill(pids[j], SIGTERM);
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
            fprintf(stderr, "osoe-launch: timeout after %ld s; killing %ld process(es)\n",
                    timeout, remaining);
            for (long j = 0; j < n; j++)
                if (!done[j])
                    kill(pids[j], SIGKILL);
        } else if (kill_at > 0 && t >= kill_at) {
            for (long j = 0; j < n; j++)
                if (!done[j])
                    kill(pids[j], SIGKILL);
            kill_at = 0;
        }

        double until = timed_out ? t + 1.0 : (kill_at > 0 ? kill_at : deadline);
        double wait = until - t;
        if (wait < 0.01)
            wait = 0.01;
        struct timespec ts = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };
        sigtimedwait(&chld, NULL, &ts);
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
    return failed ? 1 : 0;
}
