/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* R2: is the B4 stall intrinsic or a pbuf-sizing artefact of "hold until segment complete"?
 * One 64 KB segment over TCP loopback, 4096 B pbufs.
 *  (a) ring = 8 pbufs, hold-until-complete  (reviewer's case)
 *  (b) ring = 32 pbufs, hold-until-complete (ring >= segment)
 *  (c) ring = 8 pbufs, stream-parse: copy each CQE's bytes onward and recycle immediately
 *  (d) two connections sharing a 32-pbuf ring, each sending a 128 KB segment, hold-until-complete */
#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
static void tcp_pair(int *c, int *s) {
    int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a); listen(ls, 1); socklen_t al = sizeof a; getsockname(ls, (void *)&a, &al);
    *c = socket(AF_INET, SOCK_STREAM, 0); connect(*c, (void *)&a, sizeof a); *s = accept(ls, NULL, NULL); close(ls);
}
struct sa { int fd; int len; };
static void *snd(void *p) { struct sa *a = p; char *m = calloc(1, a->len); for (int o = 0; o < a->len;) { int r = send(a->fd, m + o, a->len - o, 0); if (r <= 0) break; o += r; } return NULL; }
static void arm(struct io_uring *r, int s, int ud) { struct io_uring_sqe *q = io_uring_get_sqe(r); io_uring_prep_recv_multishot(q, s, NULL, 0, 0); q->flags |= IOSQE_BUFFER_SELECT; q->buf_group = 1; q->user_data = ud; io_uring_submit(r); }
static void run(const char *name, int nbuf, int nconn, int seglen, int stream) {
    enum { BUFSZ = 4096 };
    struct io_uring ring; io_uring_queue_init(64, &ring, 0);
    int ret; struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&ring, nbuf, 1, 0, &ret);
    char *bufs = malloc(nbuf * BUFSZ);
    for (int i = 0; i < nbuf; i++) io_uring_buf_ring_add(br, bufs + i * BUFSZ, BUFSZ, i, io_uring_buf_ring_mask(nbuf), i);
    io_uring_buf_ring_advance(br, nbuf);
    int c[4], s[4]; pthread_t th[4]; struct sa args[4]; long got[4] = {0}; int held[4] = {0}, heldbid[4][64], enob = 0;
    for (int i = 0; i < nconn; i++) { tcp_pair(&c[i], &s[i]); arm(&ring, s[i], i); }
    for (int i = 0; i < nconn; i++) { args[i] = (struct sa){ c[i], seglen }; pthread_create(&th[i], NULL, snd, &args[i]); }
    for (;;) {
        struct io_uring_cqe *cqe; struct __kernel_timespec ts = { 1, 0 };
        if (io_uring_wait_cqe_timeout(&ring, &cqe, (void *)&ts)) break;
        int i = cqe->user_data;
        if (cqe->res > 0) {
            got[i] += cqe->res; int bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            if (stream) { io_uring_buf_ring_add(br, bufs + bid * BUFSZ, BUFSZ, bid, io_uring_buf_ring_mask(nbuf), 0); io_uring_buf_ring_advance(br, 1); }
            else heldbid[i][held[i]++] = bid;
            if (!stream && got[i] >= seglen) { /* segment complete: dispatch, recycle all */
                for (int k = 0; k < held[i]; k++) io_uring_buf_ring_add(br, bufs + heldbid[i][k] * BUFSZ, BUFSZ, heldbid[i][k], io_uring_buf_ring_mask(nbuf), k);
                io_uring_buf_ring_advance(br, held[i]); held[i] = 0; }
        } else if (cqe->res == -ENOBUFS) { enob++; if (enob < 50) arm(&ring, s[i], i); }
        if (cqe->res > 0 && !(cqe->flags & IORING_CQE_F_MORE)) arm(&ring, s[i], i);
        io_uring_cqe_seen(&ring, cqe);
        int done = 1; for (int k = 0; k < nconn; k++) if (got[k] < seglen) done = 0;
        if (done) break;
    }
    printf("%s: ", name);
    for (int i = 0; i < nconn; i++) printf("conn%d %ld/%d ", i, got[i], seglen);
    printf("ENOBUFS=%d -> %s\n", enob, ({ int d = 1; for (int k = 0; k < nconn; k++) if (got[k] < seglen) d = 0; d ? "completed" : "STALLED"; }));
    for (int i = 0; i < nconn; i++) { shutdown(c[i], SHUT_RDWR); pthread_join(th[i], NULL); close(c[i]); close(s[i]); }
    io_uring_queue_exit(&ring);
}
int main(void) {
    run("(a) 8 pbufs, hold-until-complete, 1 conn, 64KB ", 8, 1, 65536, 0);
    run("(b) 32 pbufs, hold-until-complete, 1 conn, 64KB", 32, 1, 65536, 0);
    run("(c) 8 pbufs, stream-parse/recycle, 1 conn, 64KB", 8, 1, 65536, 1);
    run("(d) 32 pbufs, hold-until-complete, 2 conns, 128KB each", 32, 2, 131072, 0);
    run("(e) 8 pbufs, stream-parse/recycle, 2 conns, 128KB each", 8, 2, 131072, 1);
    return 0;
}
