/*
 * Environment probe for the shmem-over-ethernet design assumptions.
 *
 * Checks the kernel facilities the design document relies on (see its §8.5):
 * io_uring ring setup, IORING_OP_FUTEX_WAIT, multishot recv with a provided
 * buffer ring, cross-process futexes on a named shared-memory object (by
 * futex_wait and by io_uring), and 64-bit futex support.
 * Run it first on any new machine.
 *
 *   cmake --workflow --preset debug        (runs it as the env.probe test)
 *   build/debug/tools/probe/probe          (runs it directly)
 *
 * Requires Linux 6.7+ and liburing 2.6+. Exits nonzero if any check fails.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdbool.h>
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

/*
 * Cross-process futexes in the form the library uses (§3.10, §6.2): a word in a
 * *named* shared-memory object, which the waiter opens by name having inherited
 * nothing of it, woken by futex_wake from another process. The waiter sleeps in
 * futex_wait, or with `uring` in an IORING_OP_FUTEX_WAIT - both without
 * FUTEX2_PRIVATE, since the word is shared. A kernel could pass a private,
 * same-process check and fail this one (review round two, portability F4).
 *
 * The wake must report one waiter woken, so a waiter that never got to sleep
 * does not pass for one that was woken.
 */
static void probe_shared_futex(bool uring)
{
    const char *what = uring ? "uring futex_wait, named shm, woken by process"
                             : "futex_wait, named shm, woken by other process";
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
        if (!uring) {
            atomic_store_explicit(&cw[1], 1, memory_order_release);
            while (atomic_load_explicit(&cw[0], memory_order_acquire) == 0) {
                long r = futex2_wait(&cw[0], 0, FUTEX2_SIZE_U32, &deadline);
                if (r == -1 && errno == ETIMEDOUT)
                    _exit(2);
                if (r == -1 && errno != EAGAIN && errno != EINTR)
                    _exit(4);
            }
            _exit(0);
        }
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
    long woke = futex2_wake(&w[0], 1, FUTEX2_SIZE_U32);
    int status;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    CHECK(what, woke == 1 && code == 0,
          "woke=%ld child_exit=%d (2: never woken, 3: open by name, 4: wait error, "
          "5: ring)", woke, code);
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
    probe_shared_futex(false);
    probe_shared_futex(true);
    probe_unlinked_reopen();
    probe_uring_setup();
    probe_multishot();
    printf("%s (%d failures)\n", fails ? "PROBE FAILED" : "PROBE PASSED", fails);
    return fails != 0;
}
