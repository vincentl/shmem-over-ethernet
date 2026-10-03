/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * probe-extra - the checks tools/probe does not make, written by the
 * portability review. Each is something the design relies on (or will in
 * steps 3 and 4) that the existing probe passes without exercising:
 *
 *   1. IORING_OP_FUTEX_WAIT on a *shared* word (no FUTEX2_PRIVATE) in a named
 *      shm_open mapping, woken by futex_wake from *another process*. §3.10's
 *      wake word lives in shared memory and application PEs in other processes
 *      bump it; tools/probe tests only a private word with a stale value.
 *   2. pidfd_open plus IORING_OP_POLL_ADD on the pidfd (§3.15's liveness).
 *   3. MAP_FIXED_NOREPLACE honoured: a second mapping at the same address must
 *      fail with EEXIST rather than replace or relocate (§4.1).
 *   4. shm_open itself, the page size, /dev/shm capacity against a heap size,
 *      and the user address-space width (from the stack address).
 *
 * Build: cc -O1 -o probe-extra probe-extra.c -luring
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef __NR_futex_wake
#define __NR_futex_wake 454
#endif

static int fails;
#define CHECK(name, ok, ...) do { printf("%-4s %-52s ", (ok) ? "ok" : "FAIL", name); \
    printf(__VA_ARGS__); printf("\n"); if (!(ok)) fails++; } while (0)

int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    int local;
    unsigned bits = 0;
    for (uintptr_t a = (uintptr_t)&local; a; a >>= 1) bits++;
    printf("info page size %ld, a stack address needs %u bits\n", page, bits);

    struct statvfs vfs;
    if (statvfs("/dev/shm", &vfs) == 0)
        printf("info /dev/shm: %llu MiB free - heaps are sparse tmpfs files, so L x "
               "SHMEM_SYMMETRIC_SIZE beyond this is SIGBUS on first touch, not an error at init\n",
               (unsigned long long)vfs.f_bavail * vfs.f_frsize >> 20);

    char name[64];
    snprintf(name, sizeof name, "/osoe-probe-extra.%d", (int)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK("shm_open O_EXCL", fd >= 0, "%s", fd < 0 ? strerror(errno) : name);
    if (fd < 0) return 1;
    shm_unlink(name);
    if (ftruncate(fd, page) != 0) return 1;
    _Atomic uint32_t *word = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (word == MAP_FAILED) return 1;

    /* 3. MAP_FIXED_NOREPLACE */
    void *again = mmap(word, (size_t)page, PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    CHECK("MAP_FIXED_NOREPLACE refuses an occupied address", again == MAP_FAILED && errno == EEXIST,
          "%s", again == MAP_FAILED ? strerror(errno) : "mapped: the flag was ignored");

    /* 1. uring futex wait on a shared word, woken across processes */
    struct io_uring ring;
    struct io_uring_params params = { .flags = IORING_SETUP_SINGLE_ISSUER };
    int r = io_uring_queue_init_params(8, &ring, &params);
    CHECK("io_uring ring", r == 0, "r=%d", r);
    if (r) return 1;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_futex_wait(sqe, (uint32_t *)word, 0, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32, 0);
    sqe->user_data = 1;
    io_uring_submit(&ring);

    pid_t child = fork();
    if (child == 0) {
        usleep(100000);
        atomic_store(word, 1);
        long woke = syscall(__NR_futex_wake, word, (unsigned long)FUTEX_BITSET_MATCH_ANY, 1,
                            FUTEX2_SIZE_U32);
        _exit(woke == 1 ? 0 : 3);
    }
    /* 2. pidfd on the ring */
    int pidfd = (int)syscall(SYS_pidfd_open, child, 0);
    CHECK("pidfd_open", pidfd >= 0, "%s", pidfd < 0 ? strerror(errno) : "");
    if (pidfd >= 0) {
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_poll_add(sqe, pidfd, POLLIN);
        sqe->user_data = 2;
        io_uring_submit(&ring);
    }
    int got_futex = -999, got_pidfd = -999;
    struct __kernel_timespec two = { 2, 0 };
    for (int k = 0; k < 2; k++) {
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe_timeout(&ring, &cqe, &two) != 0) break;
        if (cqe->user_data == 1) got_futex = cqe->res; else got_pidfd = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
    }
    int status = 0;
    waitpid(child, &status, 0);
    CHECK("uring FUTEX_WAIT, shared word, woken by another process", got_futex == 0,
          "res=%d, child says woke=%s", got_futex,
          WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "1" : "0");
    CHECK("uring POLL_ADD on a pidfd reports the exit", got_pidfd > 0, "res=%d", got_pidfd);
    printf("%s (%d failures)\n", fails ? "PROBE-EXTRA FAILED" : "PROBE-EXTRA PASSED", fails);
    return fails != 0;
}
