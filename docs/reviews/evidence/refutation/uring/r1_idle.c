/* R1: §3.10 idle loop with a REAL multishot TCP recv as the competing CQE source.
 * mode 0 = literal doc loop (arm futex wait every idle iteration)
 * mode 1 = armed flag (arm only if no futex wait outstanding)
 * After N messages, check overflow flag BEFORE reaping, then wake(INT_MAX) and wake(1) behaviour. */
#define _GNU_SOURCE
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#define __NR_futex_wake 454
enum { NMSG = 2000, NBUF = 16, BUFSZ = 4096, CQ = 64 };
static _Atomic uint32_t wake;
static int cfd;
static void *sender(void *p) { for (int i = 0; i < NMSG; i++) { send(cfd, "12345678", 8, 0); usleep(300); } return NULL; }
int main(int argc, char **argv) {
    int mode = atoi(argv[1]);
    int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a); listen(ls, 1); socklen_t al = sizeof a; getsockname(ls, (void *)&a, &al);
    cfd = socket(AF_INET, SOCK_STREAM, 0); connect(cfd, (void *)&a, sizeof a); int s = accept(ls, NULL, NULL);
    int one = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct io_uring ring; struct io_uring_params prm = { .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE, .cq_entries = CQ };
    if (io_uring_queue_init_params(16, &ring, &prm)) { perror("init"); return 1; }
    int ret; struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, NBUF, 1, 0, &ret);
    char *bufs = malloc(NBUF * BUFSZ);
    for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(br, bufs + i * BUFSZ, BUFSZ, i, io_uring_buf_ring_mask(NBUF), i);
    io_uring_buf_ring_advance(br, NBUF);
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv_multishot(sqe, s, NULL, 0, 0); sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = 1; sqe->user_data = 2;
    io_uring_submit(&ring);
    pthread_t th; pthread_create(&th, NULL, sender, NULL);
    long got = 0; int armed = 0, submitted = 0, fcqe = 0, iters = 0, max_armed = 0;
    while (got < NMSG * 8) {
        uint32_t v = atomic_load(&wake);
        struct io_uring_cqe *cqe; unsigned head, n = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            n++;
            if (cqe->user_data == 1) { fcqe++; armed--; }
            else if (cqe->res > 0) { got += cqe->res; int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                io_uring_buf_ring_add(br, bufs + bid * BUFSZ, BUFSZ, bid, io_uring_buf_ring_mask(NBUF), 0); io_uring_buf_ring_advance(br, 1); }
        }
        io_uring_cq_advance(&ring, n);
        if (got >= NMSG * 8) break;
        iters++;
        if (mode == 0 || armed == 0) {
            sqe = io_uring_get_sqe(&ring);
            io_uring_prep_futex_wait(sqe, (uint32_t *)&wake, v, FUTEX_BITSET_MATCH_ANY, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE, 0);
            sqe->user_data = 1; armed++; submitted++;
        }
        if (armed > max_armed) max_armed = armed;
        io_uring_submit_and_wait(&ring, 1);
    }
    pthread_join(th, NULL);
    printf("mode=%s: idle iterations=%d futex SQEs=%d futex CQEs=%d armed now=%d (max %d)\n",
           mode ? "armed-flag" : "literal", iters, submitted, fcqe, armed, max_armed);
    /* single PE wake(1): does the loop still get a CQE? */
    atomic_fetch_add(&wake, 1);
    long w1 = syscall(__NR_futex_wake, &wake, FUTEX_BITSET_MATCH_ANY, 1, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE);
    long wall = syscall(__NR_futex_wake, &wake, FUTEX_BITSET_MATCH_ANY, INT_MAX, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE);
    usleep(50000);
    printf("  wake(1) woke %ld; then wake(INT_MAX) woke %ld; CQ ring overflow flag before reaping: %s, cq_ready=%u (CQ size %d)\n",
           w1, wall, io_uring_cq_has_overflow(&ring) ? "SET" : "clear", io_uring_cq_ready(&ring), CQ);
    int total = 0; struct io_uring_cqe *cqe;
    while (io_uring_peek_cqe(&ring, &cqe) == 0) { total++; io_uring_cqe_seen(&ring, cqe); }
    printf("  CQEs eventually reaped via peek (liburing flushes overflow list): %d\n", total);
    return 0;
}
