/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A library line written whole (base/print-line.h; D30). Four processes print
 * 2,000 lines of 200 bytes each through osoe_print_line to one pipe, and then
 * one line of 3,000 bytes, which takes the path that formats into allocated
 * memory: every line the reader gets must be one of theirs, whole. Printed by
 * fprintf(stderr, ...) instead, on glibc 2.37 and later, most of the short
 * lines arrive broken at 128 bytes (round nine's portability read, P9-7). And
 * the call keeps errno.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "base/print-line.h"
#include "check.h"

enum { WRITERS = 4, LINES = 2000, SHORT = 200, LONG = 3000 };

/* One writer's lines: "writer W line NNNN " and its letter to the length, the
 * newline included; the last one LONG bytes long. */
static void write_lines(int writer)
{
    static char fill[LONG];
    memset(fill, 'a' + writer, sizeof fill);
    for (int i = 0; i <= LINES; i++) {
        int length = i < LINES ? SHORT : LONG;
        int head = snprintf(NULL, 0, "writer %d line %04d ", writer, i);
        osoe_print_line("writer %d line %04d %.*s\n", writer, i, length - head - 1, fill);
    }
}

/* Whether `line` (without its newline) is one a writer printed, whole; its
 * writer and number through the pointers. */
static bool whole(const char *line, size_t length, int *writer, int *number)
{
    int head = 0;
    if (sscanf(line, "writer %d line %d %n", writer, number, &head) != 2 || head == 0)
        return false;
    if (*writer < 0 || *writer >= WRITERS || *number < 0 || *number > LINES)
        return false;
    if (length + 1 != (size_t)(*number < LINES ? SHORT : LONG))
        return false;
    for (size_t k = (size_t)head; k < length; k++)
        if (line[k] != 'a' + *writer)
            return false;
    return true;
}

int main(void)
{
    int pipe_fds[2];
    CHECK(pipe(pipe_fds) == 0);
    pid_t pids[WRITERS];
    for (int w = 0; w < WRITERS; w++) {
        pids[w] = fork();
        CHECK(pids[w] >= 0);
        if (pids[w] == 0) {
            dup2(pipe_fds[1], STDERR_FILENO);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            write_lines(w);
            _exit(0);
        }
    }
    close(pipe_fds[1]);

    size_t capacity = (size_t)WRITERS * ((size_t)LINES * SHORT + LONG) + 1, used = 0;
    char *text = malloc(capacity);
    CHECK(text != NULL);
    if (!text)
        return 1;
    for (ssize_t got; used + 1 < capacity &&
                      (got = read(pipe_fds[0], text + used, capacity - 1 - used)) != 0;) {
        if (got < 0 && errno == EINTR)
            continue;
        CHECK(got > 0);
        if (got < 0)
            break;
        used += (size_t)got;
    }
    text[used] = '\0';
    for (int w = 0; w < WRITERS; w++) {
        int status = 0;
        CHECK(waitpid(pids[w], &status, 0) == pids[w] && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0);
    }

    static bool seen[WRITERS][LINES + 1];
    unsigned lines = 0, broken = 0;
    for (char *line = text, *end; *line; line = end + 1) {
        end = strchr(line, '\n');
        if (!end) {
            broken++;                           /* a last line with no newline */
            break;
        }
        *end = '\0';
        int writer = -1, number = -1;
        lines++;
        if (whole(line, (size_t)(end - line), &writer, &number) && !seen[writer][number])
            seen[writer][number] = true;
        else
            broken++;
    }
    unsigned missing = 0;
    for (int w = 0; w < WRITERS; w++)
        for (int i = 0; i <= LINES; i++)
            missing += !seen[w][i];
    printf("%u lines read, %u broken, %u missing of %d\n", lines, broken, missing,
           WRITERS * (LINES + 1));
    CHECK(broken == 0);
    CHECK(missing == 0);

    /* errno as it was, though the write in between may set it. */
    int saved = dup(STDERR_FILENO), null = open("/dev/null", O_WRONLY);
    CHECK(saved >= 0 && null >= 0);
    dup2(null, STDERR_FILENO);
    errno = ERANGE;
    osoe_print_line("a line to /dev/null\n");
    int after = errno;
    dup2(saved, STDERR_FILENO);
    close(null);
    close(saved);
    CHECK(after == ERANGE);

    free(text);
    return CHECK_DONE();
}
