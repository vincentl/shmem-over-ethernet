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
 * PE, through osoe_abort_end and with its status, after the reason.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "abort.h"
#include "check.h"
#include "fatal.h"
#include "state.h"

struct osoe_state osoe;
static struct osoe_node_header header;

static void mark(const char *what)
{
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

    word = OSOE_ABORT_GLOBAL_EXIT;
    child_output(false, GLOBAL_STATUS, out, sizeof out);
    if (!in_order(out, "THE REASON", "DECLARED") ||
        !in_order(out, "DECLARED", "ENDED AS THE GLOBAL EXIT"))
        fprintf(stderr, "osoe_fatal during a global exit wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "DECLARED"));
    CHECK(in_order(out, "DECLARED", "ENDED AS THE GLOBAL EXIT"));
    return CHECK_DONE();
}
