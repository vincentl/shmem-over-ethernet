#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fatal.h"
#include "state.h"

/*
 * One buffer, one write. Every local PE of a failing job usually dies at the
 * same moment, and separate writes interleave inside each other's lines: the
 * §3.15 diagnostic is the whole value of a loud failure, so it must arrive
 * whole. A single write of less than PIPE_BUF bytes is atomic against other
 * writers on a pipe, which is what a test harness gives us, and is one call on
 * a terminal or a file either way.
 */
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    char out[2048];
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    int n;
    if (osoe.boot.have_rank && osoe.node.control_name[0])
        n = snprintf(out, sizeof out, "osoe: PE %u: %s\nosoe: PE %u: control segment "
                     "/dev/shm%s\n", osoe.boot.rank, msg, osoe.boot.rank,
                     osoe.node.control_name);
    else if (osoe.boot.have_rank)
        n = snprintf(out, sizeof out, "osoe: PE %u: %s\n", osoe.boot.rank, msg);
    else
        n = snprintf(out, sizeof out, "osoe: %s\n", msg);

    fflush(stderr);
    if (n > 0) {
        size_t len = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;
        ssize_t ignored = write(STDERR_FILENO, out, len);
        (void)ignored;
    }
    exit(EXIT_FAILURE);
}
