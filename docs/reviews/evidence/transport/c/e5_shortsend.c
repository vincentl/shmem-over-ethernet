/* E5: io_uring send of 4 MB on TCP loopback with small SO_SNDBUF and a slow reader:
 * does the CQE report a short send (no MSG_WAITALL)? */
#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
static int S;
static void *reader(void *p) { char b[4096]; long t = 0; int r; usleep(200000); while ((r = recv(S, b, sizeof b, 0)) > 0) { t += r; usleep(100); } return NULL; }
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(ls, (void *)&a, sizeof a); listen(ls, 1); socklen_t al = sizeof a; getsockname(ls, (void *)&a, &al);
    int c = socket(AF_INET, SOCK_STREAM, 0); int sz = 16384; setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    connect(c, (void *)&a, sizeof a); S = accept(ls, NULL, NULL); setsockopt(S, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    pthread_t th; pthread_create(&th, NULL, reader, NULL);
    struct io_uring ring; io_uring_queue_init(8, &ring, 0);
    size_t len = 4 << 20; char *m = malloc(len); memset(m, 'z', len);
    for (int flags = 0; flags < 2; flags++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_send(sqe, c, m, len, flags ? MSG_WAITALL : 0);
        io_uring_submit(&ring); struct io_uring_cqe *cqe; io_uring_wait_cqe(&ring, &cqe);
        printf("send %zu bytes, flags=%s: CQE res=%d%s\n", len, flags ? "MSG_WAITALL" : "0", cqe->res, (size_t)cqe->res < len ? "  <-- SHORT SEND" : "");
        io_uring_cqe_seen(&ring, cqe);
    }
    shutdown(c, SHUT_WR); pthread_join(th, NULL); return 0;
}
