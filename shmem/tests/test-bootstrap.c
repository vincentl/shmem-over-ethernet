/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The bootstrap against real launcher environments (§6.1, §8.2): each fixture
 * is one task's environment as srun set it - captured from a real step, or
 * written from Slurm's documentation and marked so - and the bootstrap must
 * read it as the task it was, or refuse it by name.
 *
 *   test-bootstrap FIXTURE
 *
 * A fixture is NAME=VALUE lines and "#" comments. One comment line says what
 * the bootstrap must make of it:
 *
 *   # expect: launcher=SLURM_ rank=5 size=16 node=1 nodes=4 local=1/4 gtids=4,5,6,7
 *             job=77 step=0 hosts=pi-[001-004]            (on one line)
 *   # expect-fatal: TEXT        it refuses, and its message contains TEXT
 *
 * The task's environment is exactly the fixture's: nothing of the test's own -
 * a ctest run inside a Slurm allocation has SLURM_ names of its own - leaks in.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bootstrap.h"

/* The library's fatal, as a test sees it: the message, then a failing exit. */
_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* bootstrap.c reads the fan-out's default from the backend (internode.h), which
 * this test does not link; AM's value. */
uint32_t osoe_internode_fanout_default(void);
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

enum { MAX_LINES = 256, LINE = 4096 };

static void summarize(const struct osoe_bootstrap *b, char *out, size_t len)
{
    char gtids[LINE] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < b->local_count && used < sizeof gtids; i++)
        used += (size_t)snprintf(gtids + used, sizeof gtids - used, "%s%u", i ? "," : "",
                                 b->gtids[i]);
    snprintf(out, len, "launcher=%s rank=%u size=%u node=%u nodes=%u local=%u/%u gtids=%s "
             "job=%llu step=%u hosts=%s", b->launcher, b->rank, b->world_size, b->node,
             b->node_count, b->local_id, b->local_count, gtids, (unsigned long long)b->job,
             b->step, b->node_list ? b->node_list : "-");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: test-bootstrap FIXTURE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    static char lines[MAX_LINES][LINE];
    char expect[LINE] = "", expect_fatal[LINE] = "";
    int n = 0;
    char line[LINE];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "# expect: ", 10) == 0)
            snprintf(expect, sizeof expect, "%s", line + 10);
        else if (strncmp(line, "# expect-fatal: ", 16) == 0)
            snprintf(expect_fatal, sizeof expect_fatal, "%s", line + 16);
        else if (line[0] != '#' && strchr(line, '=') && n < MAX_LINES)
            snprintf(lines[n++], LINE, "%s", line);
    }
    fclose(f);
    if (!*expect == !*expect_fatal) {
        fprintf(stderr, "%s: needs exactly one '# expect:' or '# expect-fatal:' line\n",
                argv[1]);
        return 2;
    }

    clearenv();
    for (int i = 0; i < n; i++)
        if (putenv(lines[i]) != 0) {
            perror("putenv");
            return 2;
        }

    if (*expect) {
        struct osoe_bootstrap b;
        osoe_bootstrap_read(&b);        /* a refusal exits here, failing the test */
        char got[LINE];
        summarize(&b, got, sizeof got);
        free(b.gtids);
        if (strcmp(got, expect) != 0) {
            printf("MISMATCH\n  expected %s\n  got      %s\n", expect, got);
            return 1;
        }
        printf("%s\nFIXTURE OK\n", got);
        return 0;
    }

    /* A refusal: the bootstrap exits, so it runs in a child whose stderr is read. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 2;
    }
    pid_t child = fork();
    if (child == 0) {
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        struct osoe_bootstrap b;
        osoe_bootstrap_read(&b);
        _exit(0);                       /* accepted what it should have refused */
    }
    close(pipefd[1]);
    char said[LINE] = "";
    size_t got = 0;
    ssize_t r;
    while (got < sizeof said - 1 && (r = read(pipefd[0], said + got, sizeof said - 1 - got)) > 0)
        got += (size_t)r;
    said[got] = '\0';
    close(pipefd[0]);
    int status;
    waitpid(child, &status, 0);
    bool refused = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    printf("%s", said);
    if (!refused || !strstr(said, expect_fatal)) {
        printf("MISMATCH\n  expected a refusal saying: %s\n", expect_fatal);
        return 1;
    }
    printf("FIXTURE OK\n");
    return 0;
}
