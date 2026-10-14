/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An LD_PRELOAD shim for one test: cut an osoe control segment down to its
 * first page the moment osoe-stat has it, as the object's owner may -
 * `osoe-stat --gc` runs as root over objects any local user can write (D5;
 * round four's portability report, Q4).
 *
 * The cut comes right after the reader has the object: after mmap(2) returns a
 * mapping of it, or after the first pread(2) of it, the magic a copying reader
 * checks before it reads the rest. The page keeps the directory whole, so a
 * reader goes on to the metric table past it: a mapped one faults there
 * (SIGBUS), and a copying one, which has the page alone, finds no table in it
 * and leaves the object alone. Once a process; nothing else is touched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static int cut_already;

/* Cut the object `fd` is open on to a page, if it is in /dev/shm by an osoe
 * name; `after` says which call it followed. */
static void cut(int fd, const char *after)
{
    if (cut_already || fd < 0)
        return;
    char link[64], path[256];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0)
        return;
    path[n] = '\0';
    if (strncmp(path, "/dev/shm/osoe.", strlen("/dev/shm/osoe.")) != 0)
        return;
    int writable = open(path, O_WRONLY);
    if (writable < 0)
        return;
    long page = sysconf(_SC_PAGESIZE);
    if (ftruncate(writable, (off_t)page) == 0) {
        cut_already = 1;
        dprintf(STDERR_FILENO, "shrink-under-read: %s cut to %ld bytes after %s\n", path,
                page, after);
    }
    close(writable);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
    if (!real_mmap)
        real_mmap = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    void *p = real_mmap(addr, len, prot, flags, fd, offset);
    if (p != MAP_FAILED)
        cut(fd, "its mapping");
    return p;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    static ssize_t (*real_pread)(int, void *, size_t, off_t);
    if (!real_pread)
        real_pread = (ssize_t (*)(int, void *, size_t, off_t))dlsym(RTLD_NEXT, "pread");
    ssize_t n = real_pread(fd, buf, count, offset);
    if (n > 0)
        cut(fd, "its first read");
    return n;
}
