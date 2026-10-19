/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A loud failure writes its reason before it lets anyone else end the job
 * (fatal.h). The order is the whole point: publishing a rendezvous's `failed`
 * word, or the node's abort, lets the other PEs exit, the launcher then sends
 * this one SIGTERM, and a reason not yet written is lost. That happened once
 * in 30 runs of shmem.job.duplicate-rank under load.
 *
 * fatal.c is compiled in on its own, with a stub osoe_node_declare_abort, and
 * the state it reads set by hand so that it takes the abort path. A child
 * calls osoe_fatal_then with a `publish` that writes a marker to stderr, and
 * the stub writes another; the parent reads the child's stderr and requires
 * the reason, then the publication, then the abort. And the same for
 * osoe_fatal, which has no publication: the reason, then the abort. And a
 * fatal that finds a global exit declared first ends as that exit ends every
 * PE, through osoe_abort_end and with its status, after the reason. A fatal
 * the agent raises itself says where this PE's thread was waiting; one from any
 * other thread does not.
 *
 * And a fatal that loses the first word (fatal.h) to a thread still writing
 * it does not end the process under that write: the child's stderr is a pipe
 * filled to the brim, so a thread's first word waits in its write until the
 * parent reads, and the parent reads only after a second, by when a fatal
 * that did not wait has ended the child with the line unwritten - the
 * survivor of shmem.death-in-wait.barrier.L4 that left the job without a word
 * (docs/decisions/2026-10-18.md). The stubs write no marker then: theirs
 * would wait in the same pipe and hold the fatal back by themselves.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "abort.h"
#include "agent.h"
#include "blocked.h"
#include "check.h"
#include "fatal.h"
#include "metrics.h"
#include "state.h"

struct osoe_state osoe;
static struct osoe_node_header header;

/* Whether the stubs write their markers: not beside a first word held in a
 * full pipe. */
static bool markers_written = true;

static void mark(const char *what)
{
    if (!markers_written)
        return;
    ssize_t ignored = write(STDERR_FILENO, what, strlen(what));
    (void)ignored;
}

bool osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status,
                             enum osoe_abort_disposition how)
{
    (void)n; (void)pe; (void)status; (void)how;
    mark("DECLARED\n");
    return true;
}

/* What the word says after the stub's declaration: the fatal it declared, or a
 * global exit with status GLOBAL_STATUS that someone declared first. */
enum { GLOBAL_STATUS = 5 };
static enum osoe_abort_disposition word = OSOE_ABORT_FATAL;

enum osoe_abort_disposition osoe_node_abort_read(const struct osoe_node *n, uint32_t *pe,
                                                 int *status)
{
    (void)n;
    *pe = 7;
    *status = word == OSOE_ABORT_GLOBAL_EXIT ? GLOBAL_STATUS : EXIT_FAILURE;
    return word;
}

_Noreturn void osoe_abort_end(void)
{
    mark("ENDED AS THE GLOBAL EXIT\n");
    _exit(GLOBAL_STATUS);
}

/* Whether the fatal is the agent's (agent.c), set before each child, and where
 * this PE's thread waits (metrics.c). */
static bool on_agent;

bool osoe_agent_is_this_thread(void)
{
    return on_agent;
}

/* No progress helper here (metrics.c): a helper's fatal says where its PE was
 * as the agent's does, which on_agent stands for. */
bool osoe_metrics_thread_unpooled(void)
{
    return false;
}

const char *osoe_blocked_where(char *buf, size_t len)
{
    snprintf(buf, len, "waiting in shmem_barrier_all");
    return buf;
}

static void publish(void *context)
{
    (void)context;
    mark("PUBLISHED\n");
}

/* Run `with_publish` ? osoe_fatal_then : osoe_fatal in a child; its stderr. It
 * must leave with `expected`. */
static void child_output(bool with_publish, int expected, char *out, size_t len)
{
    int fds[2];
    CHECK_FATAL(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK_FATAL(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        osoe.bootstrap.have_rank = true;
        osoe.bootstrap.rank = 7;
        osoe.node.hdr = &header;
        if (with_publish)
            osoe_fatal_then(publish, NULL, "THE REASON");
        osoe_fatal("THE REASON");
    }
    close(fds[1]);
    size_t got = 0;
    ssize_t n;
    while (got + 1 < len && (n = read(fds[0], out + got, len - 1 - got)) > 0)
        got += (size_t)n;
    out[got] = '\0';
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

static bool in_order(const char *out, const char *first, const char *second)
{
    const char *a = strstr(out, first), *b = strstr(out, second);
    return a && b && a < b;
}

static const char held_word[] = "THE FIRST WORD\n";
static _Atomic bool taking;         /* the thread is about to take the first word */

static void *say_held_word(void *unused)
{
    (void)unused;
    atomic_store_explicit(&taking, true, memory_order_release);
    osoe_fatal_first_word(held_word, sizeof held_word - 1);
    return NULL;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000 * 1000 };
    nanosleep(&t, NULL);
}

/* The held case's child: stderr filled, a thread taking the first word into
 * it, and then - its write waiting, a tenth of a second on - this thread's
 * fatal, which loses the word. */
static _Noreturn void held_child(void)
{
    int flags = fcntl(STDERR_FILENO, F_GETFL);
    CHECK_FATAL(flags >= 0 && fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK) == 0);
    char fill[4096];                    /* PIPE_BUF: each write all or nothing */
    memset(fill, 0, sizeof fill);
    while (write(STDERR_FILENO, fill, sizeof fill) > 0)
        ;
    CHECK_FATAL(errno == EAGAIN && fcntl(STDERR_FILENO, F_SETFL, flags) == 0);
    pthread_t t;
    CHECK_FATAL(pthread_create(&t, NULL, say_held_word, NULL) == 0);
    pthread_detach(t);
    while (!atomic_load_explicit(&taking, memory_order_acquire))
        sleep_ms(1);
    sleep_ms(100);
    markers_written = false;
    osoe_fatal("THE LOSER");
}

/* The held case: the child's stderr, read a second after the fork unless the
 * child has ended by then, the fill's NUL bytes left out. */
static void held_output(char *out, size_t len)
{
    int fds[2];
    CHECK_FATAL(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK_FATAL(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        osoe.bootstrap.have_rank = true;
        osoe.bootstrap.rank = 7;
        osoe.node.hdr = &header;
        held_child();
    }
    close(fds[1]);
    int status = 0;
    pid_t ended = 0;
    for (int ms = 0; ms < 1000 && ended == 0; ms += 10) {
        sleep_ms(10);
        ended = waitpid(pid, &status, WNOHANG);
    }
    size_t got = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof chunk)) > 0)
        for (ssize_t i = 0; i < n; i++)
            if (chunk[i] != '\0' && got + 1 < len)
                out[got++] = chunk[i];
    out[got] = '\0';
    close(fds[0]);
    if (ended == 0)
        ended = waitpid(pid, &status, 0);
    CHECK(ended == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}

int main(void)
{
    char out[4096];
    child_output(true, EXIT_FAILURE, out, sizeof out);
    if (!in_order(out, "THE REASON", "PUBLISHED") || !in_order(out, "PUBLISHED", "DECLARED"))
        fprintf(stderr, "osoe_fatal_then wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "PUBLISHED"));
    CHECK(in_order(out, "PUBLISHED", "DECLARED"));

    child_output(false, EXIT_FAILURE, out, sizeof out);
    if (!in_order(out, "THE REASON", "DECLARED"))
        fprintf(stderr, "osoe_fatal wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "DECLARED"));
    CHECK(strstr(out, "ENDED AS THE GLOBAL EXIT") == NULL);
    CHECK(strstr(out, "this PE was") == NULL);

    on_agent = true;
    child_output(false, EXIT_FAILURE, out, sizeof out);
    if (!strstr(out, "THE REASON; this PE was waiting in shmem_barrier_all\n"))
        fprintf(stderr, "osoe_fatal on the agent wrote:\n%s", out);
    CHECK(strstr(out, "THE REASON; this PE was waiting in shmem_barrier_all\n") != NULL);
    on_agent = false;

    held_output(out, sizeof out);
    if (strcmp(out, held_word) != 0)
        fprintf(stderr, "a fatal beside a first word still being written left:\n%s\n", out);
    CHECK(strcmp(out, held_word) == 0);

    word = OSOE_ABORT_GLOBAL_EXIT;
    child_output(false, GLOBAL_STATUS, out, sizeof out);
    if (!in_order(out, "THE REASON", "DECLARED") ||
        !in_order(out, "DECLARED", "ENDED AS THE GLOBAL EXIT"))
        fprintf(stderr, "osoe_fatal during a global exit wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "DECLARED"));
    CHECK(in_order(out, "DECLARED", "ENDED AS THE GLOBAL EXIT"));
    return CHECK_DONE();
}
