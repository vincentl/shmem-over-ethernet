/*
 * osoe-launch - start L processes of one job on this machine, the way a Slurm
 * step would, for tests. Not installed.
 *
 *   osoe-launch -n L [-t SECONDS] [-e RANK:NAME=VALUE]... -- PROGRAM [ARGS]...
 *
 * Each process gets the OSOE_ bootstrap variables of design §6.2 for a job with
 * one node and L tasks: rank i is local id i. The job id is this launcher's pid,
 * so concurrent launches never share a shared-memory name. -e sets or
 * overrides one variable for one rank, for tests that need PEs to disagree.
 *
 * Fails (exit 1) if any process exits nonzero or dies on a signal, if the
 * timeout (default 60 s) expires, or if the job left shared-memory objects
 * named /osoe.<job>.* behind after every process exited successfully: heap
 * names are removed once mapped and the control segment at the last finalize
 * (§6.2), so a clean job leaves none. Leftovers are removed in every case.
 * After the first failure the remaining processes get SIGTERM, then SIGKILL
 * two seconds later, since they are usually blocked waiting for the one that
 * failed.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MAX_PROCS = 1024, MAX_OVERRIDES = 64 };

struct override {
    long rank;
    char *assignment;       /* NAME=VALUE */
};

static void usage(void)
{
    fprintf(stderr, "usage: osoe-launch -n L [-t SECONDS] [-e RANK:NAME=VALUE]... "
                    "-- PROGRAM [ARGS]...\n");
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

/* Runs in the child between fork and exec. */
static void exec_rank(long rank, long n, const char *job, const struct override *ov,
                      int nov, char **argv)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", rank);
    setenv_or_die("OSOE_PROCID", buf);
    setenv_or_die("OSOE_LOCALID", buf);
    snprintf(buf, sizeof buf, "%ld", n);
    setenv_or_die("OSOE_STEP_NUM_TASKS", buf);
    setenv_or_die("OSOE_NODEID", "0");
    setenv_or_die("OSOE_JOB_ID", job);
    setenv_or_die("OSOE_STEP_ID", "0");

    char host[HOST_NAME_MAX + 1] = "localhost";
    gethostname(host, sizeof host);
    host[sizeof host - 1] = '\0';
    setenv_or_die("OSOE_STEP_NODELIST", host);

    size_t len = (size_t)n * 12 + 1;
    char *gtids = malloc(len);
    if (!gtids)
        _exit(127);
    size_t off = 0;
    for (long i = 0; i < n; i++)
        off += (size_t)snprintf(gtids + off, len - off, i ? ",%ld" : "%ld", i);
    setenv_or_die("OSOE_GTIDS", gtids);

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
    struct override ov[MAX_OVERRIDES];
    int nov = 0;

    int opt;
    while ((opt = getopt(argc, argv, "+n:t:e:")) != -1) {
        switch (opt) {
        case 'n':
            n = parse_long(optarg, 1, MAX_PROCS, "process count");
            break;
        case 't':
            timeout = parse_long(optarg, 1, 86400, "timeout");
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
            exec_rank(i, n, job, ov, nov, prog);
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
                    for (long j = 0; j < n; j++)
                        if (!done[j])
                            kill(pids[j], SIGTERM);
                    kill_at = now() + 2.0;
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

    int left = remove_leftovers(job);
    if (left && !failed) {
        fprintf(stderr, "osoe-launch: every process succeeded but %d shared-memory "
                        "object(s) were left behind\n", left);
        failed = true;
    }
    return failed ? 1 : 0;
}
