/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Each of osoe_abort_end's ends (abort.c) waits for a first word another
 * thread is still writing (fatal.h), as a fatal's does (test-fatal-order.c):
 * a global exit this PE called, which leaves by exit and its handlers with the
 * exit's status; one another PE called, which leaves by _exit with 0; and an
 * abort a fatal declared, whose own "ending" line loses the word, which leaves
 * by _exit with the fatal's status. Each in a child whose standard error is a
 * pipe filled to the brim, so that a thread's first word waits in its write
 * until the parent reads, a second after the fork: an end that did not wait
 * has by then ended the child with the line unwritten - the survivor of
 * shmem.death-in-wait.barrier.L4 that left the job without a word
 * (docs/decisions/2026-10-18.md). The parent requires that line and nothing
 * else, and the end's status.
 *
 * abort.c and fatal.c are compiled in on their own, with stubs for what they
 * reach beyond them: the node's abort word, which says what each case sets; an
 * agent that is not running, so that the thread that calls makes the end; and
 * no node-mates to outlast.
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
#include "liveness.h"
#include "metrics.h"
#include "state.h"

struct osoe_state osoe;
static struct osoe_node_header header;

/* What the node's abort word says, set before each child: a global exit
 * with GLOBAL_STATUS, or a fatal with FATAL_STATUS, declared by PE `declarer`.
 * This PE is PE 7. */
enum { THIS_PE = 7, OTHER_PE = 3, GLOBAL_STATUS = 5, FATAL_STATUS = 4 };
static enum osoe_abort_disposition word;
static uint32_t declarer;

enum osoe_abort_disposition osoe_node_abort_read(const struct osoe_node *n, uint32_t *pe,
                                                 int *status)
{
    (void)n;
    *pe = declarer;
    *status = word == OSOE_ABORT_GLOBAL_EXIT ? GLOBAL_STATUS : FATAL_STATUS;
    return word;
}

/* fatal.c's; nothing here makes a fatal. */
bool osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status,
                             enum osoe_abort_disposition how)
{
    (void)n; (void)pe; (void)status; (void)how;
    return true;
}

bool osoe_agent_speaks(void)
{
    return false;
}

bool osoe_agent_is_this_thread(void)
{
    return false;
}

bool osoe_metrics_thread_unpooled(void)
{
    return false;
}

const char *osoe_blocked_where(char *buf, size_t len)
{
    snprintf(buf, len, "waiting in shmem_barrier_all");
    return buf;
}

void osoe_liveness_outlast(uint32_t besides, const struct osoe_deadline *until)
{
    (void)besides; (void)until;
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

/* The child: stderr filled, a thread taking the first word into it, and then
 * - its write waiting, a tenth of a second on - this thread's end, the
 * global exit's caller if `called`. */
static _Noreturn void held_child(bool called)
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
    if (called)
        osoe_abort_caller();
    osoe_abort_end();
}

/* One case: the child's stderr, read a second after the fork unless the child
 * has ended by then, the fill's NUL bytes left out; the child must leave with
 * `expected` and have written the held line alone. */
static void held_case(const char *what, bool called, int expected)
{
    int fds[2];
    CHECK_FATAL(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK_FATAL(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        osoe.bootstrap.have_rank = true;
        osoe.bootstrap.rank = THIS_PE;
        osoe.bootstrap.node_count = 1;
        osoe.node.hdr = &header;
        held_child(called);
    }
    close(fds[1]);
    int status = 0;
    pid_t ended = 0;
    for (int ms = 0; ms < 1000 && ended == 0; ms += 10) {
        sleep_ms(10);
        ended = waitpid(pid, &status, WNOHANG);
    }
    char out[4096];
    size_t got = 0;
    char chunk[4096];
    ssize_t n;
    while ((n = read(fds[0], chunk, sizeof chunk)) > 0)
        for (ssize_t i = 0; i < n; i++)
            if (chunk[i] != '\0' && got + 1 < sizeof out)
                out[got++] = chunk[i];
    out[got] = '\0';
    close(fds[0]);
    if (ended == 0)
        ended = waitpid(pid, &status, 0);
    bool left = ended == pid && WIFEXITED(status) && WEXITSTATUS(status) == expected;
    if (strcmp(out, held_word) != 0 || !left)
        fprintf(stderr, "%s, beside a first word still being written: the child left %s %d, "
                "where %d was due, and wrote:\n%s\n", what,
                WIFEXITED(status) ? "with status" : "by signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status), expected, out);
    CHECK(strcmp(out, held_word) == 0);
    CHECK(left);
}

int main(void)
{
    word = OSOE_ABORT_GLOBAL_EXIT;
    declarer = THIS_PE;
    held_case("a global exit this PE called", true, GLOBAL_STATUS);
    declarer = OTHER_PE;
    held_case("a global exit another PE called", false, 0);
    word = OSOE_ABORT_FATAL;
    held_case("an abort another PE's fatal declared", false, FATAL_STATUS);
    if (CHECK_DONE() == 0)
        printf("ABORT ENDS WAIT: 3 ends, each its first word out and its status\n");
    return CHECK_DONE();
}
