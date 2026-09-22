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
 * osoe_fatal, which has no publication: the reason, then the abort.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

void osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status)
{
    (void)n; (void)pe; (void)status;
    mark("DECLARED\n");
}

static void publish(void *context)
{
    (void)context;
    mark("PUBLISHED\n");
}

/* Run `with_publish` ? osoe_fatal_then : osoe_fatal in a child; its stderr. */
static void child_output(bool with_publish, char *out, size_t len)
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
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}

static bool in_order(const char *out, const char *first, const char *second)
{
    const char *a = strstr(out, first), *b = strstr(out, second);
    return a && b && a < b;
}

int main(void)
{
    char out[4096];
    child_output(true, out, sizeof out);
    if (!in_order(out, "THE REASON", "PUBLISHED") || !in_order(out, "PUBLISHED", "DECLARED"))
        fprintf(stderr, "osoe_fatal_then wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "PUBLISHED"));
    CHECK(in_order(out, "PUBLISHED", "DECLARED"));

    child_output(false, out, sizeof out);
    if (!in_order(out, "THE REASON", "DECLARED"))
        fprintf(stderr, "osoe_fatal wrote, in this order:\n%s", out);
    CHECK(in_order(out, "THE REASON", "DECLARED"));
    return CHECK_DONE();
}
