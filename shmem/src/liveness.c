/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
#include <time.h>
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
    if (osoe.bootstrap.local_count < 2)
        return;                         /* nobody to watch */
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

/* What osoe_liveness_outlast waits on at once: a batch of pidfds opened
 * together, so that a PE reaped while an earlier one is waited for is not
 * looked for later by a pid the kernel may have given another process (Part
 * 2's second adversarial read). On the stack: no allocation on this path. */
enum { OUTLAST_BATCH = 64 };

void osoe_liveness_outlast(uint32_t besides, const struct osoe_deadline *until)
{
    struct pollfd watch[OUTLAST_BATCH];
    pid_t unwatched[OUTLAST_BATCH];         /* where no pidfd could be had */
    uint32_t i = 0;
    while (i < osoe.bootstrap.local_count) {
        nfds_t n = 0;
        uint32_t k = 0;
        for (; i < osoe.bootstrap.local_count && n + k < OUTLAST_BATCH; i++) {
            if (i == osoe.bootstrap.local_id || i == besides)
                continue;
            const struct osoe_pe_slot *s = &osoe.node.slots[i];
            /* The pid is published before the release store of the phase. */
            if (atomic_load_explicit(&s->phase, memory_order_acquire) == OSOE_PHASE_EMPTY ||
                s->pid <= 0)
                continue;
            int fd = (int)syscall(SYS_pidfd_open, (pid_t)s->pid, 0);
            if (fd >= 0)
                watch[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
            else if (errno != ESRCH)            /* ended already: nothing to wait for */
                unwatched[k++] = (pid_t)s->pid;
        }
        /* Every pidfd of the batch, until each has said its process ended. */
        nfds_t open_count = n;
        while (open_count && !osoe_deadline_expired(until)) {
            int r = poll(watch, n, 100);
            if (r < 0 && errno != EINTR)
                break;                          /* cannot wait: as for an expired bound */
            for (nfds_t j = 0; r > 0 && j < n; j++)
                if (watch[j].fd >= 0 && watch[j].revents) {
                    close(watch[j].fd);
                    watch[j].fd = -1;           /* poll skips it from here */
                    open_count--;
                }
        }
        for (nfds_t j = 0; j < n; j++)
            if (watch[j].fd >= 0)
                close(watch[j].fd);
        /* And by kill(pid, 0) every millisecond those it had no pidfd for. */
        struct timespec millisecond = { 0, 1000 * 1000 };
        for (uint32_t j = 0; j < k; j++)
            while (!(kill(unwatched[j], 0) != 0 && errno == ESRCH) &&
                   !osoe_deadline_expired(until))
                nanosleep(&millisecond, NULL);
        if (osoe_deadline_expired(until))
            return;
    }
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
