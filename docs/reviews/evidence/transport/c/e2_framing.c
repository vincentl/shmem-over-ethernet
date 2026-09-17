/* E2: multishot recv + provided buffers delivers arbitrary byte boundaries.
 * (a) 100 records of 3000 bytes, 4096-byte pbufs: how many records straddle a CQE boundary.
 * (b) a single 64 KB "segment" with 8 x 4096 pbufs, receiver holds pbufs until the segment
 *     is complete (i.e. zero-copy parse with no reassembly buffer): ENOBUFS, never completes. */
#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
static void tcp_pair(int *c, int *s) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a); listen(ls, 1); socklen_t al = sizeof a; getsockname(ls, (void *)&a, &al);
    *c = socket(AF_INET, SOCK_STREAM, 0); connect(*c, (void *)&a, sizeof a); *s = accept(ls, NULL, NULL); close(ls);
    int one = 1; setsockopt(*c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}
struct sargs { int fd; int nrec; int reclen; int usec; };
static void *sender(void *p) {
    struct sargs *a = p; char *m = malloc(a->reclen); memset(m, 'r', a->reclen);
    for (int i = 0; i < a->nrec; i++) { for (int o = 0; o < a->reclen;) { int r = send(a->fd, m + o, a->reclen - o, 0); if (r <= 0) return NULL; o += r; } if (a->usec) usleep(a->usec); }
    return NULL;
}
int main(void) {
    enum { NBUF = 8, BUFSZ = 4096 };
    struct io_uring ring; struct io_uring_params prm = { .flags = IORING_SETUP_SINGLE_ISSUER };
    io_uring_queue_init_params(32, &ring, &prm);
    int ret; struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, NBUF, 1, 0, &ret);
    char *bufs = malloc(NBUF * BUFSZ);
    for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(br, bufs + i * BUFSZ, BUFSZ, i, io_uring_buf_ring_mask(NBUF), i);
    io_uring_buf_ring_advance(br, NBUF);
    int c, s; tcp_pair(&c, &s);
    /* (a) replenish immediately; count record straddles */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv_multishot(sqe, s, NULL, 0, 0); sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = 1;
    io_uring_submit(&ring);
    struct sargs sa = { c, 100, 3000, 200 }; pthread_t th; pthread_create(&th, NULL, sender, &sa);
    long total = 0; int ncqe = 0, straddle = 0, hist[5] = {0}; int enobufs = 0;
    while (total < 300000) {
        struct io_uring_cqe *cqe; io_uring_wait_cqe(&ring, &cqe);
        if (cqe->res < 0) { if (cqe->res == -ENOBUFS) enobufs++; io_uring_cqe_seen(&ring, cqe);
            sqe = io_uring_get_sqe(&ring); io_uring_prep_recv_multishot(sqe, s, NULL, 0, 0); sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = 1; io_uring_submit(&ring); continue; }
        long before = total; total += cqe->res; ncqe++;
        if (total / 3000 != before / 3000 && total % 3000) straddle++;
        hist[cqe->res <= 1024 ? 0 : cqe->res <= 2048 ? 1 : cqe->res <= 3000 ? 2 : cqe->res < 4096 ? 3 : 4]++;
        int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        io_uring_buf_ring_add(br, bufs + bid * BUFSZ, BUFSZ, bid, io_uring_buf_ring_mask(NBUF), 0); io_uring_buf_ring_advance(br, 1);
        if (!(cqe->flags & IORING_CQE_F_MORE)) { sqe = io_uring_get_sqe(&ring); io_uring_prep_recv_multishot(sqe, s, NULL, 0, 0); sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = 1; io_uring_submit(&ring); }
        io_uring_cqe_seen(&ring, cqe);
    }
    pthread_join(th, NULL);
    printf("(a) 100x3000B records, 4096B pbufs: %d data CQEs, CQE sizes <=1K:%d <=2K:%d <=3000:%d <4096:%d =4096:%d; CQE boundaries falling inside a record: %d; ENOBUFS=%d\n",
           ncqe, hist[0], hist[1], hist[2], hist[3], hist[4], straddle, enobufs);
    /* (b) hold pbufs until a 64 KB segment is complete */
    struct sargs sb = { c, 1, 65536, 0 }; pthread_create(&th, NULL, sender, &sb);
    long got = 0; int held = 0, term = 0, rearms = 0;
    for (;;) {
        struct io_uring_cqe *cqe; struct __kernel_timespec ts = { 1, 0 };
        if (io_uring_wait_cqe_timeout(&ring, &cqe, (void *)&ts)) break;
        if (cqe->res > 0) { got += cqe->res; held++; }
        else { term = cqe->res; if (rearms++ < 3) { sqe = io_uring_get_sqe(&ring); io_uring_prep_recv_multishot(sqe, s, NULL, 0, 0); sqe->flags |= IOSQE_BUFFER_SELECT; sqe->buf_group = 1; io_uring_submit(&ring);} }
        io_uring_cqe_seen(&ring, cqe);
    }
    printf("(b) 64KB segment, pbufs held until segment complete: got %ld/65536 bytes in %d pbufs, last terminal res=%d (%s), re-arms=%d -> segment never completes (receive stalled; TCP window then stalls the sender)\n",
           got, held, term, strerror(-term), rearms);
    return 0;
}
