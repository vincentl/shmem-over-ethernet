/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An LD_PRELOAD shim for one test: delay, by a second, every write(2) whose
 * bytes contain OSOE_TEST_SLOW_TEXT. Nothing else is touched.
 *
 * It makes a race deterministic that load makes rare. A PE that refuses a job
 * must get its reason onto stderr before it publishes anything that lets the
 * others end the job; the launcher sends the rest SIGTERM on the first failure,
 * and a reason not yet written is lost (fatal.h). With the reason's write held
 * back a second, a PE that publishes first is killed inside that second every
 * time, and one that writes first never is.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int, const void *, size_t);
    if (!real_write)
        real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    const char *text = getenv("OSOE_TEST_SLOW_TEXT");
    if (text && *text && memmem(buf, count, text, strlen(text))) {
        struct timespec second = { 1, 0 };
        nanosleep(&second, NULL);
    }
    return real_write(fd, buf, count);
}
