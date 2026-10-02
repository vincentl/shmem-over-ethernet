/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "abort.h"
#include "agent.h"
#include "blocked.h"
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
static void (*last_word)(void *);
static void *last_word_context;

void osoe_fatal_last_word(void (*tell)(void *), void *context)
{
    last_word = tell;
    last_word_context = context;
}

static _Noreturn void fatal(void (*publish)(void *), void *context, const char *fmt,
                            va_list ap)
{
    char out[2048];
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    /* A fatal this PE's agent raises itself - another node's death, a local
     * PE's - ends a PE whose own thread may be waiting in the library, so it
     * says where, as the agent does when it ends the process on another PE's
     * abort (abort.c): what the development watchdog named before it was
     * retired (§3.15). A fatal on any other thread is that thread's own, and
     * names its routine where it has one. */
    if (osoe_agent_is_this_thread()) {
        const char *routine = osoe_blocked_routine();
        size_t used = strlen(msg);
        snprintf(msg + used, sizeof msg - used, "; this PE was %s%s",
                 routine ? "waiting in " : "not waiting in the library",
                 routine ? routine : "");
    }

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

    if (publish) {
        publish(context);
    } else if (last_word) {
        /* Once: a fatal inside it would otherwise come back here for ever. */
        void (*tell)(void *) = last_word;
        last_word = NULL;
        tell(last_word_context);
    }
    /* Tell this node's other PEs: each stops at its next sweep rather than
     * waiting on something this PE will now never do (§3.15). Unless an abort
     * is already declared - then this PE is one of the ones stopping, or
     * `publish` declared it for someone else, and the word must keep naming the
     * PE that started it. */
    if (osoe.bootstrap.have_rank && osoe.node.hdr) {
        osoe_node_declare_abort(&osoe.node, osoe.bootstrap.rank, EXIT_FAILURE,
                                OSOE_ABORT_FATAL);
        /* A global exit declared first ends this PE as it ends every other
         * that did not call it, with 0, leaving the job the caller's status:
         * what went wrong here may be that exit's own doing - a peer gone, a
         * reply that will not come - and §9.1.6 gives the job the caller's
         * status, which a launcher reads from the one task that leaves
         * nonzero (abort.h). */
        uint32_t pe;
        int status;
        if (osoe_node_abort_read(&osoe.node, &pe, &status) == OSOE_ABORT_GLOBAL_EXIT)
            osoe_abort_end();
    }

    /*
     * _exit, not exit: exit runs atexit handlers, and start_pes registered
     * shmem_finalize as one - a barrier. A PE ending the job must not enter a
     * collective the others will never reach; it hung start_pes programs until
     * the launcher's timeout (second review, conformance F1). stdio was flushed
     * above, which is the one thing exit would have done that is wanted here.
     */
    _exit(EXIT_FAILURE);
}

void osoe_notice(const char *fmt, ...)
{
    char msg[1024], out[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    int n = osoe.bootstrap.have_rank
            ? snprintf(out, sizeof out, "osoe: PE %u: %s\n", osoe.bootstrap.rank, msg)
            : snprintf(out, sizeof out, "osoe: %s\n", msg);
    if (n > 0) {
        size_t len = (size_t)n < sizeof out ? (size_t)n : sizeof out - 1;
        ssize_t ignored = write(STDERR_FILENO, out, len);
        (void)ignored;
    }
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
