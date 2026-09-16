/*
 * Environment probe for the shmem-over-ethernet design assumptions.
 *
 * Checks the kernel facilities the design document relies on (see its §8.5):
 * io_uring ring setup, IORING_OP_FUTEX_WAIT, multishot recv with a provided
 * buffer ring, cross-process futexes, and 64-bit futex support.
 * Run it first on any new machine.
 *
 *   gcc -O2 -Wall -Wextra -std=gnu11 -o probe tools/probe.c -luring && ./probe
 *
 * Requires Linux 6.7+ and liburing. Exits nonzero if any check fails.
 */
#define _GNU_SOURCE
#include <errno.h>
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
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __NR_futex_wake
#define __NR_futex_wake 454
#endif
#ifndef __NR_futex_wait
#define __NR_futex_wait 455
#endif

static int fails;
#define CHECK(name, ok, fmt, ...) do { \
    printf("%-4s %-44s " fmt "\n", (ok) ? "ok" : "FAIL", name, ##__VA_ARGS__); \
    if (!(ok)) fails++; } while (0)

static long futex2_wait(void *addr, uint64_t val, unsigned flags, struct timespec *ts)
{
    return syscall(__NR_futex_wait, addr, val, FUTEX_BITSET_MATCH_ANY, flags, ts, CLOCK_MONOTONIC);
}

static long futex2_wake(void *addr, int n, unsigned flags)
{
    return syscall(__NR_futex_wake, addr, FUTEX_BITSET_MATCH_ANY, n, flags);
}

static void probe_futex2(void)
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

/* Cross-process futex on a shared memfd mapping: the 4.9 / 5.x pattern. */
static void probe_shared_futex(void)
{
    int fd = memfd_create("probe", 0);
    ftruncate(fd, 4096);
    _Atomic uint32_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    atomic_store(w, 0);

    pid_t pid = fork();
    if (pid == 0) {
        /* futex2 futex_wait takes an ABSOLUTE deadline on the given clock. */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 2;
        while (atomic_load(w) == 0) {
            long r = futex2_wait(w, 0, FUTEX2_SIZE_U32, &ts);
            if (r == -1 && errno == ETIMEDOUT) _exit(2);
        }
        _exit(0);
    }
    usleep(100000);            /* let the child block */
    atomic_store(w, 1);
    long woke = futex2_wake(w, 1, FUTEX2_SIZE_U32);
    int status;
    waitpid(pid, &status, 0);
    CHECK("cross-process futex on memfd mapping",
          WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "woke=%ld child_exit=%d", woke, WEXITSTATUS(status));
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
    io_uring_wait_cqe(&ring, &cqe);
    CHECK("uring futex_wait stale value -> -EAGAIN", cqe->res == -EAGAIN, "res=%d", cqe->res);
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
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

    int ncqe = 0, more = 0, enobufs = 0;
    struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) {
        ncqe++;
        if (cqe->flags & IORING_CQE_F_MORE) more++;
        if (cqe->res == -ENOBUFS) enobufs = 1;
        io_uring_cqe_seen(&ring, cqe);
    }
    CHECK("multishot recv: >1 CQE from one SQE", ncqe > 1, "cqes=%d with_F_MORE=%d", ncqe, more);
    CHECK("buffer ring exhaustion -> -ENOBUFS", enobufs, "(re-arm path exists)");

    close(cs); close(ss); close(ls);
    io_uring_queue_exit(&ring);
    free(bufs);
}

int main(void)
{
    struct utsname u;
    uname(&u);
    printf("info kernel %s %s, page size %ld, liburing %d.%d\n", u.release, u.machine,
           sysconf(_SC_PAGESIZE), IO_URING_VERSION_MAJOR, IO_URING_VERSION_MINOR);
    probe_futex2();
    probe_shared_futex();
    probe_uring_setup();
    probe_multishot();
    printf("%s (%d failures)\n", fails ? "PROBE FAILED" : "PROBE PASSED", fails);
    return fails != 0;
}
