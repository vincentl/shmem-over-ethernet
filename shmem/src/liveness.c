#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "abort.h"
#include "base/deadline.h"
#include "blocked.h"
#include "fatal.h"
#include "liveness.h"
#include "state.h"

/*
 * What this PE watches: the lead every other local PE, every other PE its
 * lead. Each has an open pidfd, or is watched by its pid (PID) when no pidfd
 * could be opened, or needs nothing more (SETTLED).
 */
enum { UNOPENED = -1, SETTLED = -2, PID = -3 };

static uint32_t watched_count;
static uint32_t *watched_id;        /* local ids */
static int *watched_fd;
static struct pollfd *polled;
static uint32_t *polled_index;      /* into watched_id */
static uint64_t next_check_ns;
static bool said_no_pidfd;

void osoe_liveness_start(void)
{
    watched_count = 0;
    if (osoe.bootstrap.liveness_off || osoe.bootstrap.local_count < 2)
        return;                         /* OSOE_LIVENESS=0, or nobody to watch */
    uint32_t me = osoe.bootstrap.local_id, n = me == 0 ? osoe.bootstrap.local_count - 1 : 1;
    watched_id = malloc(n * sizeof *watched_id);
    watched_fd = malloc(n * sizeof *watched_fd);
    polled = malloc(n * sizeof *polled);
    polled_index = malloc(n * sizeof *polled_index);
    if (!watched_id || !watched_fd || !polled || !polled_index)
        osoe_fatal("out of memory for %u pidfds", n);
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++)
        if (i != me && (me == 0 || i == 0)) {
            watched_id[watched_count] = i;
            watched_fd[watched_count++] = UNOPENED;
        }
    next_check_ns = 0;
    said_no_pidfd = false;
}

void osoe_liveness_stop(void)
{
    for (uint32_t k = 0; k < watched_count; k++)
        if (watched_fd[k] >= 0)
            close(watched_fd[k]);
    free(watched_id);
    free(watched_fd);
    free(polled);
    free(polled_index);
    watched_id = NULL;
    watched_fd = NULL;
    polled = NULL;
    polled_index = NULL;
    watched_count = 0;
}

/*
 * Local PE i's process has ended. Before its last finalize, that is a death:
 * say so, in one write and before anything that lets the other PEs end
 * (fatal.h), then declare the node's abort naming it. Not if the word is set -
 * the abort may be what ended it - and a declaration that loses changes
 * nothing.
 */
static void ended(uint32_t i)
{
    struct osoe_pe_slot *s = &osoe.node.slots[i];
    if (atomic_load_explicit(&s->phase, memory_order_acquire) == OSOE_PHASE_FINALIZED ||
        osoe_abort_pending())
        return;
    char msg[256];
    int n = snprintf(msg, sizeof msg, "osoe: PE %u: local PE %u (rank %u, pid %d) has "
                     "ended without finalizing; ending the job (§3.15)\n",
                     osoe.bootstrap.rank, i, s->rank, s->pid);
    if (n > 0) {
        ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n < sizeof msg ? (size_t)n
                                                                          : sizeof msg - 1);
        (void)ignored;
    }
    osoe_node_declare_abort(&osoe.node, s->rank, EXIT_FAILURE, OSOE_ABORT_FATAL);
}

/*
 * A pidfd for local PE i, or PID if none can be had - out of descriptors, or a
 * kernel or sandbox that refuses the call - after saying so once: a job is not
 * ended because it cannot be watched well, and kill(pid, 0) still sees a death
 * unless a recycled pid hides it. SETTLED, after ended(), if the PE is gone.
 */
static int open_watch(uint32_t i, pid_t pid)
{
    /* The raw call: glibc wraps it only since 2.36. */
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd >= 0)
        return fd;
    if (errno == ESRCH) {
        ended(i);
        return SETTLED;
    }
    if (!said_no_pidfd) {
        said_no_pidfd = true;
        fprintf(stderr, "osoe: PE %u: cannot open a pidfd for local PE %u (pid %d): %s; "
                "watching local PEs by pid instead (§3.15)\n", osoe.bootstrap.rank, i,
                (int)pid, strerror(errno));
    }
    return PID;
}

void osoe_liveness_check(void)
{
    if (watched_count == 0)
        return;
    uint64_t now = osoe_now_ns();
    if (now < next_check_ns)
        return;
    next_check_ns = now + OSOE_SWEEP_NS;

    nfds_t n = 0;
    for (uint32_t k = 0; k < watched_count; k++) {
        uint32_t i = watched_id[k];
        const struct osoe_pe_slot *s = &osoe.node.slots[i];
        if (watched_fd[k] == UNOPENED) {
            /* The pid is published before the release store of the phase. */
            if (atomic_load_explicit(&s->phase, memory_order_acquire) == OSOE_PHASE_EMPTY ||
                s->pid <= 0)
                continue;               /* not joined yet */
            watched_fd[k] = open_watch(i, (pid_t)s->pid);
        }
        if (watched_fd[k] == PID) {
            if (kill((pid_t)s->pid, 0) != 0 && errno == ESRCH) {
                watched_fd[k] = SETTLED;
                ended(i);
            }
        } else if (watched_fd[k] >= 0) {
            polled[n] = (struct pollfd){ .fd = watched_fd[k], .events = POLLIN };
            polled_index[n++] = k;
        }
    }
    if (n == 0 || poll(polled, n, 0) <= 0)
        return;
    for (nfds_t j = 0; j < n; j++) {
        if (!polled[j].revents)
            continue;
        uint32_t k = polled_index[j];
        close(watched_fd[k]);
        watched_fd[k] = SETTLED;
        ended(watched_id[k]);
    }
}
