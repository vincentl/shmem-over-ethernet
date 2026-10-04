/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An LD_PRELOAD shim for one test: hold back every shm_open that creates an
 * object whose name contains OSOE_TEST_SLOW_NAME - until the file
 * OSOE_TEST_SLOW_AFTER names is gone, if it is set (for at most ten seconds),
 * and then half a second more - and say on stderr, as it begins to, that it
 * holds one, so that the test can tell that it ran even when the job ends
 * during the hold. Nothing else is touched.
 *
 * It makes a race deterministic that the order of start-up makes rare. A node
 * lead makes its job tables and then its connection area; a node-mate that
 * has taken the lead's tables opens the area by name at once. A failed start
 * by the same ids left a ready area under that name, and the node-mate must
 * not take it (the release fixes' review, correctness F1). The tables' name
 * goes when the last of the node's PEs has opened them, so a lead whose area
 * waits for that, and a moment more, has a node-mate that meets the leftover
 * every time, where without the shim the lead has nearly always replaced it
 * first (restart-leftovers.sh). A lead that replaces a leftover creates twice,
 * and is held back each time.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int shm_open(const char *name, int flags, mode_t mode)
{
    static int (*real_shm_open)(const char *, int, mode_t);
    if (!real_shm_open)
        real_shm_open = (int (*)(const char *, int, mode_t))dlsym(RTLD_NEXT, "shm_open");
    const char *text = getenv("OSOE_TEST_SLOW_NAME");
    if ((flags & O_CREAT) && text && *text && strstr(name, text)) {
        fprintf(stderr, "slow-create: holding the creation of %s\n", name);
        const char *after = getenv("OSOE_TEST_SLOW_AFTER");
        struct timespec tick = { 0, 5 * 1000 * 1000 };
        for (int i = 0; after && *after && i < 2000 && access(after, F_OK) == 0; i++)
            nanosleep(&tick, NULL);
        struct timespec half = { 0, 500 * 1000 * 1000 };
        nanosleep(&half, NULL);
    }
    return real_shm_open(name, flags, mode);
}
