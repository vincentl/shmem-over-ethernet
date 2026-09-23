#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
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

/* Per local id: an open pidfd, or one of these. */
enum { UNOPENED = -1, SETTLED = -2 };

static int *watched;
static struct pollfd *polled;
static uint32_t *polled_id;
static uint64_t next_check_ns;

void osoe_liveness_start(void)
{
    if (osoe.bootstrap.liveness_off)
        return;                         /* OSOE_LIVENESS=0: nothing is watched */
    uint32_t n = osoe.bootstrap.local_count;
    watched = malloc(n * sizeof *watched);
    polled = malloc(n * sizeof *polled);
    polled_id = malloc(n * sizeof *polled_id);
    if (!watched || !polled || !polled_id)
        osoe_fatal("out of memory for %u pidfds", n);
    for (uint32_t i = 0; i < n; i++)
        watched[i] = UNOPENED;
    next_check_ns = 0;
}

void osoe_liveness_stop(void)
{
    for (uint32_t i = 0; watched && i < osoe.bootstrap.local_count; i++)
        if (watched[i] >= 0)
            close(watched[i]);
    free(watched);
    free(polled);
    free(polled_id);
    watched = NULL;
    polled = NULL;
    polled_id = NULL;
}

/* The lead watches every other local PE; every other PE, its lead. */
static bool watches(uint32_t i)
{
    uint32_t me = osoe.bootstrap.local_id;
    return i != me && (me == 0 || i == 0);
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

void osoe_liveness_check(void)
{
    if (!watched)
        return;
    uint64_t now = osoe_now_ns();
    if (now < next_check_ns)
        return;
    next_check_ns = now + OSOE_SWEEP_NS;

    nfds_t n = 0;
    for (uint32_t i = 0; i < osoe.bootstrap.local_count; i++) {
        if (!watches(i) || watched[i] == SETTLED)
            continue;
        if (watched[i] == UNOPENED) {
            /* The pid is published before the release store of the phase. */
            const struct osoe_pe_slot *s = &osoe.node.slots[i];
            if (atomic_load_explicit(&s->phase, memory_order_acquire) == OSOE_PHASE_EMPTY ||
                s->pid <= 0)
                continue;               /* not joined yet */
            /* The raw call: glibc wraps it only since 2.36. */
            int fd = (int)syscall(SYS_pidfd_open, (pid_t)s->pid, 0);
            if (fd < 0) {
                if (errno != ESRCH)
                    osoe_fatal("cannot watch local PE %u (pid %d): pidfd_open: %s", i,
                               s->pid, strerror(errno));
                watched[i] = SETTLED;   /* already gone */
                ended(i);
                continue;
            }
            watched[i] = fd;
        }
        polled[n] = (struct pollfd){ .fd = watched[i], .events = POLLIN };
        polled_id[n++] = i;
    }
    if (n == 0 || poll(polled, n, 0) <= 0)
        return;
    for (nfds_t k = 0; k < n; k++) {
        if (!polled[k].revents)
            continue;
        uint32_t i = polled_id[k];
        close(watched[i]);
        watched[i] = SETTLED;
        ended(i);
    }
}
