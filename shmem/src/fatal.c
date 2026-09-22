#include <stdarg.h>
#include <stdatomic.h>
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
static _Noreturn void fatal(void (*publish)(void *), void *context, const char *fmt,
                            va_list ap)
{
    char out[2048];
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);

    int n;
    if (osoe.bootstrap.have_rank && osoe.node.control_name[0])
        n = snprintf(out, sizeof out, "osoe: PE %u: %s\nosoe: PE %u: control segment "
                     "%s (osoe-stat %s, while any PE of this node is alive)\n",
                     osoe.bootstrap.rank, msg, osoe.bootstrap.rank, osoe.node.control_name,
                     osoe.node.control_name);
    else if (osoe.bootstrap.have_rank)
        n = snprintf(out, sizeof out, "osoe: PE %u: %s\n", osoe.bootstrap.rank, msg);
    else
        n = snprintf(out, sizeof out, "osoe: %s\n", msg);

    /* The reason reaches stderr before anything that lets another process end
     * the job (fatal.h). */
    fflush(NULL);
    if (n > 0) {
        size_t len = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;
        ssize_t ignored = write(STDERR_FILENO, out, len);
        (void)ignored;
    }

    if (publish)
        publish(context);
    /* Tell this node's other PEs: each stops at its next sweep rather than
     * waiting on something this PE will now never do (§3.15). Unless an abort
     * is already declared - then this PE is one of the ones stopping, or
     * `publish` declared it for someone else, and the word must keep naming the
     * PE that started it. */
    if (osoe.bootstrap.have_rank && osoe.node.hdr &&
        !atomic_load_explicit(&osoe.node.hdr->aborting, memory_order_acquire))
        osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, EXIT_FAILURE);

    /*
     * _exit, not exit: exit runs atexit handlers, and start_pes registered
     * shmem_finalize as one - a barrier. A PE ending the job must not enter a
     * collective the others will never reach; it hung start_pes programs until
     * the launcher's timeout (second review, conformance F1). stdio was flushed
     * above, which is the one thing exit would have done that is wanted here.
     */
    _exit(EXIT_FAILURE);
}

_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fatal(NULL, NULL, fmt, ap);
}

_Noreturn void osoe_fatal_then(void (*publish)(void *), void *context, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fatal(publish, context, fmt, ap);
}
