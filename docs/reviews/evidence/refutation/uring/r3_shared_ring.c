/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* R3: two connections share one 32x4096 pbuf ring (128 KB). Each sends a 128 KB segment in two
 * halves; both first halves are sent before either second half. Policy: hold pbufs until the
 * connection's segment is complete. Then the same with stream-parse (recycle every CQE). */
#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
static void tcp_pair(int *c, int *s) {
    int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a); listen(ls, 1); socklen_t al = sizeof a; getsockname(ls, (void *)&a, &al);
    *c = socket(AF_INET, SOCK_STREAM, 0); connect(*c, (void *)&a, sizeof a); *s = accept(ls, NULL, NULL); close(ls);
}
static int c[2], s[2]; enum { SEG = 131072, NBUF = 32, BUFSZ = 4096 };
static void sendall(int fd, char *m, int n) { for (int o = 0; o < n;) { int r = send(fd, m + o, n - o, MSG_NOSIGNAL); if (r <= 0) return; o += r; } }
static void *snd(void *p) { char *m = calloc(1, SEG); sendall(c[0], m, SEG/2); sendall(c[1], m, SEG/2); usleep(200000); sendall(c[0], m, SEG/2); sendall(c[1], m, SEG/2); return NULL; }
static void arm(struct io_uring *r, int i) { struct io_uring_sqe *q = io_uring_get_sqe(r); io_uring_prep_recv_multishot(q, s[i], NULL, 0, 0); q->flags |= IOSQE_BUFFER_SELECT; q->buf_group = 1; q->user_data = i; io_uring_submit(r); }
static void run(int stream) {
    struct io_uring ring; io_uring_queue_init(64, &ring, 0);
    int ret; struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, NBUF, 1, 0, &ret);
    char *bufs = malloc(NBUF * BUFSZ);
    for (int i = 0; i < NBUF; i++) io_uring_buf_ring_add(br, bufs + i * BUFSZ, BUFSZ, i, io_uring_buf_ring_mask(NBUF), i);
    io_uring_buf_ring_advance(br, NBUF);
    long got[2] = {0}; int held[2] = {0}, hb[2][NBUF], enob = 0, rearm = 0;
    for (int i = 0; i < 2; i++) { tcp_pair(&c[i], &s[i]); int sz = 4096; setsockopt(s[i], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz); arm(&ring, i); }
    pthread_t th; pthread_create(&th, NULL, snd, NULL);
    for (;;) {
        struct io_uring_cqe *cqe; struct __kernel_timespec ts = { 2, 0 };
        if (io_uring_wait_cqe_timeout(&ring, &cqe, (void *)&ts)) break;
        int i = cqe->user_data;
        if (cqe->res > 0) { got[i] += cqe->res; int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            if (stream) { io_uring_buf_ring_add(br, bufs + bid * BUFSZ, BUFSZ, bid, io_uring_buf_ring_mask(NBUF), 0); io_uring_buf_ring_advance(br, 1); }
            else { hb[i][held[i]++] = bid; if (got[i] >= SEG) { for (int k = 0; k < held[i]; k++) io_uring_buf_ring_add(br, bufs + hb[i][k] * BUFSZ, BUFSZ, hb[i][k], io_uring_buf_ring_mask(NBUF), k); io_uring_buf_ring_advance(br, held[i]); held[i] = 0; } }
            if (!(cqe->flags & IORING_CQE_F_MORE)) arm(&ring, i);
        } else if (cqe->res == -ENOBUFS) { enob++; if (rearm++ < 20) arm(&ring, i); }
        io_uring_cqe_seen(&ring, cqe);
        if (got[0] >= SEG && got[1] >= SEG) break;
    }
    printf("%s: conn0 %ld/%d conn1 %ld/%d held=%d+%d ENOBUFS=%d -> %s\n", stream ? "stream-parse, recycle per CQE" : "hold-until-segment-complete ",
           got[0], SEG, got[1], SEG, held[0], held[1], enob, (got[0] >= SEG && got[1] >= SEG) ? "completed" : "STALLED (both conns)");
    for (int i = 0; i < 2; i++) { shutdown(c[i], SHUT_RDWR); shutdown(s[i], SHUT_RDWR); }
    pthread_join(th, NULL);
    for (int i = 0; i < 2; i++) { close(c[i]); close(s[i]); }
    io_uring_queue_exit(&ring);
}
int main(void) { run(0); run(1); return 0; }
