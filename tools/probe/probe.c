/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Environment probe for the shmem-over-ethernet design assumptions.
 *
 * Checks the kernel facilities the design document relies on (see its §8.5),
 * each build's probe those its own library relies on. The build chooses the
 * files, as it does the library's (OSOE_SINGLE_NODE, the top-level
 * CMakeLists.txt), so there is no #ifdef in them. Run it first on any new
 * machine.
 *
 *   probe.c              both builds: main; a cross-process futex on a named
 *                        shared-memory object; an unlinked shared-memory object
 *                        reopened through /proc.
 *   probe-full-build.c   the full build: the futex2 calls, and 64-bit futex
 *                        support; io_uring ring setup, IORING_OP_FUTEX_WAIT (also
 *                        as the cross-process futex's waiter), multishot recv with
 *                        a provided buffer ring over TCP loopback; and the io_uring
 *                        check shmem_init makes (base/ring-check.c), so that a
 *                        kernel this passes is one a job of several nodes starts
 *                        on.
 *   probe-single-node.c  the single-node build: the classic futex(2) calls,
 *                        FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET. It has no
 *                        io_uring and no network to check.
 *
 *   cmake --workflow --preset debug        (runs it as the env.probe test)
 *   build/debug/tools/probe/probe          (runs it directly)
 *
 * The full build's probe requires Linux 6.7+ and liburing 2.6+; the single-node
 * build's needs neither. Exits nonzero if any check fails.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"

int probe_failures;

/*
 * Cross-process futexes in the form the library uses (§3.10, §6.2): a word in a
 * *named* shared-memory object, which the waiter opens by name having inherited
 * nothing of it, woken by a futex wake from another process. The waiter sleeps
 * in the build's futex wait (probe_futex_wait_shared) - without the private
 * flag, since the word is shared. A kernel could pass a private, same-process
 * check and fail this one (review round two, portability F4). The full build
 * makes the check again with the waiter in an IORING_OP_FUTEX_WAIT
 * (probe-full-build.c).
 *
 * The wake must report one waiter woken, so a waiter that never got to sleep
 * does not pass for one that was woken.
 */
static void probe_shared_futex(void)
{
    const char *what = "futex_wait, named shm, woken by other process";
    char name[64];
    snprintf(name, sizeof name, "/osoe-probe-futex.%d", (int)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        CHECK(what, 0, "shm_open/ftruncate: %s", strerror(errno));
        if (fd >= 0) { close(fd); shm_unlink(name); }
        return;
    }
    _Atomic uint32_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (w == MAP_FAILED) {
        CHECK(what, 0, "mmap: %s", strerror(errno));
        close(fd); shm_unlink(name);
        return;
    }
    /* w[0] is the futex word; w[1] says the waiter has mapped it. */

    pid_t pid = fork();
    if (pid == 0) {
        /* Forget everything inherited: this stands for an unrelated process. */
        munmap((void *)w, 4096);
        close(fd);
        int cfd = shm_open(name, O_RDWR, 0);
        if (cfd < 0)
            _exit(3);
        _Atomic uint32_t *cw = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        if (cw == MAP_FAILED)
            _exit(3);
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += 2;
        atomic_store_explicit(&cw[1], 1, memory_order_release);
        while (atomic_load_explicit(&cw[0], memory_order_acquire) == 0) {
            long r = probe_futex_wait_shared(&cw[0], 0, &deadline);
            if (r == -1 && errno == ETIMEDOUT)
                _exit(2);
            if (r == -1 && errno != EAGAIN && errno != EINTR)
                _exit(4);
        }
        _exit(0);
    }

    /* Wait until the waiter has mapped the word, then give it time to sleep. */
    for (int i = 0; i < 200 && atomic_load_explicit(&w[1], memory_order_acquire) == 0; i++)
        usleep(10000);
    usleep(100000);
    /* Single-writer publication (§3.16): this process stores, the waiter only
     * loads, and the kernel's compare in the wait closes the race. */
    atomic_store_explicit(&w[0], 1, memory_order_release);
    long woke = probe_futex_wake_shared(&w[0], 1);
    int status;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(what, woke == 1 && code == 0,
          "woke=%ld child_exit=%d (2: never woken, 3: open by name, 4: wait error)",
          woke, code);
    munmap((void *)w, 4096);
    close(fd);
    shm_unlink(name);
}

/*
 * The premise osoe-stat stands on (§3.14): a shared memory object whose name
 * has been removed can still be opened, by another process of the same user,
 * through /proc/<pid>/fd/<n> of a process that holds it - and what it maps is
 * the live object, not a copy. The library removes every name during init; if
 * this fails, a running job cannot be read from outside at all.
 */
static void probe_unlinked_reopen(void)
{
    char name[64];
    snprintf(name, sizeof name, "/osoe-probe.%d", (int)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        CHECK("unlinked shm object reopened via /proc", 0, "shm_open: %s", strerror(errno));
        return;
    }
    _Atomic uint32_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    atomic_store_explicit(w, 1, memory_order_release);
    shm_unlink(name);                               /* from here it has no name */
    pid_t parent = getpid();

    pid_t pid = fork();
    if (pid == 0) {
        /* Forget everything inherited: this stands for an unrelated process. */
        munmap((void *)w, 4096);
        close(fd);
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)parent, fd);
        int rfd = open(path, O_RDONLY);
        if (rfd < 0)
            _exit(3);
        const _Atomic uint32_t *r = mmap(NULL, 4096, PROT_READ, MAP_SHARED, rfd, 0);
        if (r == MAP_FAILED || atomic_load_explicit(r, memory_order_acquire) != 1)
            _exit(4);
        /* Live, not a copy: the parent's next store is seen here. */
        for (int i = 0; i < 200 && atomic_load_explicit(r, memory_order_acquire) != 2; i++)
            usleep(10000);
        _exit(atomic_load_explicit(r, memory_order_acquire) == 2 ? 0 : 5);
    }
    usleep(50000);
    atomic_store_explicit(w, 2, memory_order_release);
    int status;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK("unlinked shm object reopened via /proc", code == 0,
          "child_exit=%d (3: open /proc/<pid>/fd, 4: map or read, 5: not live)", code);
    munmap((void *)w, 4096);
    close(fd);
}

int main(void)
{
    /* A line at a time, so that what was checked survives a crash when stdout
     * is a pipe, as under srun. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct utsname u;
    uname(&u);
    printf("info kernel %s %s, page size %ld\n", u.release, u.machine,
           sysconf(_SC_PAGESIZE));
    probe_build_info();
    probe_shared_futex();
    probe_unlinked_reopen();
    probe_build_checks();
    printf("%s (%d failures)\n", probe_failures ? "PROBE FAILED" : "PROBE PASSED",
           probe_failures);
    return probe_failures != 0;
}
