/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An LD_PRELOAD shim for one test: end this process with SIGKILL, a fifth of a
 * second after it creates a shared-memory object whose name contains
 * OSOE_TEST_KILL_NAME, and say on stderr, as it does, which object, so that the
 * test can tell that the process was ended there and not elsewhere. Nothing
 * else is touched.
 *
 * It ends a node's lead where start-up leaves it least time to be ended. The
 * lead makes and sizes the node's control segment, then creates its own heap -
 * an object whose name contains ".heap." - and only then stores `ready`; a lead
 * killed at the heap's creation has left its node-mates a segment they have
 * mapped and a `ready` that cannot come (node.c, lead_ended_before_ready; the
 * release fixes' review, correctness F2; lead-ends-before-ready.sh). No signal
 * the launcher sends can be timed to that window, which lasts a few
 * milliseconds. The fifth of a second is for the node-mates, which map the
 * segment within milliseconds: they are waiting on `ready` when the lead ends,
 * as the review found them, and not arriving after it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

int shm_open(const char *name, int flags, mode_t mode)
{
    static int (*real_shm_open)(const char *, int, mode_t);
    if (!real_shm_open)
        real_shm_open = (int (*)(const char *, int, mode_t))dlsym(RTLD_NEXT, "shm_open");
    const char *text = getenv("OSOE_TEST_KILL_NAME");
    if ((flags & O_CREAT) && text && *text && strstr(name, text)) {
        fprintf(stderr, "kill-on-create: ending this process at its creation of %s\n", name);
        fflush(stderr);
        struct timespec fifth = { 0, 200 * 1000 * 1000 };
        nanosleep(&fifth, NULL);
        raise(SIGKILL);
    }
    return real_shm_open(name, flags, mode);
}
