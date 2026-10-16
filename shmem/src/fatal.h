/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Loud failure (§3.15). The library's error path is to print what went wrong,
 * naming the PE and, while its name is in /dev/shm, the control segment an
 * investigator points osoe-stat at (§3.14), and to terminate the process.
 */
#ifndef OSOE_FATAL_H
#define OSOE_FATAL_H

#include <stdbool.h>

_Noreturn void osoe_fatal(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/*
 * osoe_fatal with a last act before the abort: the message is written, then
 * `publish(context)` runs - a rendezvous's `failed` word - and then the
 * process leaves as osoe_fatal does.
 *
 * The message first, always. Anything published lets another process end the
 * job: its PEs stop, the launcher sees a failure and sends the rest SIGTERM. A
 * PE that published before writing could be killed with its reason still
 * unwritten, and the job then said only that it "failed on another node" -
 * seen once in 30 runs of shmem.job.duplicate-rank under load.
 */
_Noreturn void osoe_fatal_then(void (*publish)(void *), void *context, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * A last word: what a fatal with nothing of its own to publish does after its
 * message, while one is set - set from the moment a PE knows who it is until
 * its node has joined, so that a PE refusing its own environment, or the
 * kernel, tells its node rather than leaving its node-mates waiting in silence
 * (node.c, osoe_node_tell_refusal; the round-three review's R8). NULL clears
 * it. Process-wide: one thread runs init.
 */
void osoe_fatal_last_word(void (*tell)(void *), void *context);

/*
 * A notice: loud, and nothing more (§3.15). A start-up wait that has lasted 30 s
 * says what it waits for, and again each minute (base/stall-watch.h), in one
 * line written whole as a fatal's is, naming the PE once it is known. It ends
 * nothing: no wait gives up because time has passed.
 */
void osoe_notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * Whether the line of this process's end is this caller's to write: true for
 * the first caller in the process, false after (D18, D20, D21). An end has
 * several writers - a fatal on any thread, a progress helper's among them, the
 * agent's "ending" on another PE's abort (abort.c), a local PE's death seen
 * (liveness.c), an exit without shmem_finalize (init.c) - and a lead with
 * helpers wrote one death twice, a non-lead the abort twice; the first says
 * it, the cause where there is one, and a later writer ends the process all
 * the same, silently.
 */
bool osoe_fatal_first_word(void);

#endif /* OSOE_FATAL_H */
