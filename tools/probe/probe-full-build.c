/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The full build's half of the probe (probe.c has the checks both builds make):
 * the futex2 calls, which are Linux 6.7's, and every io_uring check, which
 * compiles against liburing 2.6. The single-node build uses neither, and
 * compiles probe-single-node.c in this file's place.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "base/ring-check.h"
#include "probe.h"

#ifndef __NR_futex_wake
#define __NR_futex_wake 454
#endif
#ifndef __NR_futex_wait
#define __NR_futex_wait 455
#endif

void probe_build_info(void)
{
    printf("info full build: liburing %d.%d\n", IO_URING_VERSION_MAJOR, IO_URING_VERSION_MINOR);
    /* What io_uring's rings, on Linux 6.14 and later, and the zero-copy send
     * count against (base/ring-check.h): a limit too low for them says only
     * "Cannot allocate memory" where it is met (the review of v0.2 step 4's
     * merge point, portability P1). */
    long long kib = osoe_locked_memory_kib();
    if (kib < 0)
        printf("info locked-memory limit (ulimit -l): none\n");
    else
        printf("info locked-memory limit (ulimit -l): %lld KiB\n", kib);
}

static long futex2_wait(void *word, uint64_t value, unsigned flags,
                        const struct timespec *absolute_deadline)
{
    return syscall(__NR_futex_wait, word, value, FUTEX_BITSET_MATCH_ANY, flags,
                   absolute_deadline, CLOCK_MONOTONIC);
}

static long futex2_wake(void *word, int n, unsigned flags)
{
    return syscall(__NR_futex_wake, word, FUTEX_BITSET_MATCH_ANY, n, flags);
}

/* The two calls probe.c's cross-process futex check is written against, over
 * futex2: a 32-bit word, no FUTEX2_PRIVATE, an absolute CLOCK_MONOTONIC deadline. */
long probe_futex_wait_shared(_Atomic uint32_t *word, uint32_t value,
                             const struct timespec *absolute_deadline)
{
    return futex2_wait(word, value, FUTEX2_SIZE_U32, absolute_deadline);
}

long probe_futex_wake_shared(_Atomic uint32_t *word, int n)
{
    return futex2_wake(word, n, FUTEX2_SIZE_U32);
}

static void probe_futex_calls(void)
{
    uint64_t w64 = 7;
    uint32_t w32 = 7;
    struct timespec ts = { 0, 1000000 };

    long r = futex2_wait(&w32, 8, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, &ts);
    CHECK("futex2 U32 wait (value mismatch)", r == -1 && errno == EAGAIN,
          "r=%ld errno=%s", r, strerror(errno));

    r = futex2_wait(&w64, 8, FUTEX2_SIZE_U64 | FUTEX2_PRIVATE, &ts);
    int e = errno;
    printf("info %-44s r=%ld errno=%s -> %s\n", "futex2 U64 wait", r, strerror(e),
           e == EINVAL ? "U64 NOT implemented (design 4.9 must change)"
                       : "U64 accepted");
}

/*
 * probe.c's cross-process futex check (§3.10, §6.2) with the waiter sleeping in
 * an IORING_OP_FUTEX_WAIT in place of the futex wait: a word in a *named*
 * shared-memory object, which the waiter opens by name having inherited nothing
 * of it, woken by a futex wake from another process - without FUTEX2_PRIVATE,
 * since the word is shared. The two checks are the same but for the waiter; keep
 * them so.
 *
 * The wake must report one waiter woken, so a waiter that never got to sleep
 * does not pass for one that was woken.
 */
static void probe_shared_futex_uring(void)
{
    const char *what = "uring futex_wait, named shm, woken by process";
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
        struct io_uring ring;
        if (io_uring_queue_init(4, &ring, 0) != 0)
            _exit(5);
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_futex_wait(sqe, (uint32_t *)&cw[0], 0, FUTEX_BITSET_MATCH_ANY,
                                 FUTEX2_SIZE_U32, 0);
        if (io_uring_submit(&ring) != 1)
            _exit(5);
        atomic_store_explicit(&cw[1], 1, memory_order_release);
        struct __kernel_timespec two = { 2, 0 };
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe_timeout(&ring, &cqe, &two) != 0)
            _exit(2);
        _exit(cqe->res == 0 && atomic_load_explicit(&cw[0], memory_order_acquire) == 1 ? 0 : 4);
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
    /* The name goes before the line is printed, as in probe.c. */
    munmap((void *)w, 4096);
    close(fd);
    shm_unlink(name);
    CHECK(what, woke == 1 && code == 0,
          "woke=%ld child_exit=%d (2: never woken, 3: open by name, 4: wait error, "
          "5: ring)", woke, code);
}

static void probe_uring_setup(void)
{
    struct io_uring ring;
    struct io_uring_params p = { 0 };
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE;
    p.cq_entries = 4096;
    int r = io_uring_queue_init_params(64, &ring, &p);
    CHECK("SINGLE_ISSUER + CQSIZE", r == 0, "r=%d sq=%u cq=%u", r, p.sq_entries, p.cq_entries);
    if (r) return;

    struct io_uring_probe *pr = io_uring_get_probe_ring(&ring);
    CHECK("opcode FUTEX_WAIT", pr && io_uring_opcode_supported(pr, IORING_OP_FUTEX_WAIT), "");
    CHECK("opcode RECV", pr && io_uring_opcode_supported(pr, IORING_OP_RECV), "");
    io_uring_free_probe(pr);

    /* IORING_OP_FUTEX_WAIT with a stale value must complete with -EAGAIN (3.10). */
    uint32_t w = 5;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_futex_wait(sqe, &w, 6, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
    io_uring_submit(&ring);
    struct io_uring_cqe *cqe;
    /* A ring that cannot be entered completes nothing: say so, rather than
     * read a completion that never came (Part 6's adversarial read). */
    int waited = io_uring_wait_cqe(&ring, &cqe);
    CHECK("uring futex_wait stale value -> -EAGAIN", waited == 0 && cqe->res == -EAGAIN,
          "res=%d", waited == 0 ? cqe->res : waited);
    if (waited == 0)
        io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
}

/* shmem_init's own check, through the same function, rather than a copy of
 * it here that could fall behind: a ring set up R_DISABLED, enabled and
 * entered, the SEND opcode among the others, and a provided-buffer ring (§1). */
static void probe_init_ring_check(void)
{
    const char *facility = "";
    int e = osoe_ring_check(&facility);
    CHECK("shmem_init's io_uring check", e == 0, "%s%s%s", e ? facility : "",
          e ? ": " : "", e ? strerror(e) : "");
}

/* Multishot recv with a provided buffer ring over real TCP loopback (3.3). */
static void probe_multishot(void)
{
    enum { BGID = 1, NBUF = 8, BUFSZ = 4096, NMSG = 64 };
    struct io_uring ring;
    struct io_uring_params p = { .flags = IORING_SETUP_SINGLE_ISSUER };
    io_uring_queue_init_params(32, &ring, &p);

    int ret;
    struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, NBUF, BGID, 0, &ret);
    CHECK("register provided buffer ring", br != NULL, "ret=%d", ret);
    if (!br) return;
    char *bufs = malloc(NBUF * BUFSZ);
    for (int i = 0; i < NBUF; i++)
        io_uring_buf_ring_add(br, bufs + i * BUFSZ, BUFSZ, i, io_uring_buf_ring_mask(NBUF), i);
    io_uring_buf_ring_advance(br, NBUF);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a);
    listen(ls, 1);
    socklen_t al = sizeof a;
    getsockname(ls, (void *)&a, &al);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    connect(cs, (void *)&a, sizeof a);
    int ss = accept(ls, NULL, NULL);
    int one = 1;
    int nd = setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    CHECK("TCP_NODELAY", nd == 0, "");

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv_multishot(sqe, ss, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BGID;
    io_uring_submit(&ring);

    /* Send more data than the buffer ring can hold, without replenishing:
     * expect several CQEs from one SQE, then termination with -ENOBUFS. */
    char msg[BUFSZ];
    memset(msg, 'x', sizeof msg);
    for (int i = 0; i < NMSG; i++)
        if (send(cs, msg, sizeof msg, 0) < 0) break;
    usleep(200000);

    int ncqe = 0, more = 0, data = 0, enobufs = 0, last = 0;
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
        ncqe++;
        if (cqe->flags & IORING_CQE_F_MORE) more++;
        if (cqe->res > 0) data++;
        if (cqe->res == -ENOBUFS) enobufs++;
        last = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
    }
    CHECK("multishot recv: one CQE per provided buffer", data == NBUF && more == NBUF,
          "data=%d with_F_MORE=%d buffers=%d", data, more, NBUF);
    CHECK("buffer ring exhaustion -> one terminal -ENOBUFS",
          enobufs == 1 && last == -ENOBUFS && ncqe == NBUF + 1, "cqes=%d last=%d", ncqe, last);

    close(cs); close(ss); close(ls);
    io_uring_queue_exit(&ring);
    free(bufs);
}

void probe_build_checks(void)
{
    probe_futex_calls();
    probe_shared_futex_uring();
    probe_uring_setup();
    probe_init_ring_check();
    probe_multishot();
}
