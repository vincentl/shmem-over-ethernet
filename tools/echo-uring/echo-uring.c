/*
 * echo-uring (§8.4): the round-trip and bandwidth floor for step 3, and the
 * smallest reproducer of every io_uring shape the AM backend uses.
 *
 * One TCP connection per measurement. Ping-pong: the client sends 8 bytes,
 * the server echoes them, and the client times the round trip. --stream: the
 * client sends as fast as one in-flight send allows and the server counts what
 * arrives. Both ends use exactly the shapes §3.3 fixes for the library: a
 * SINGLE_ISSUER ring whose completion queue is sized from its provided-buffer
 * ring, multishot recv over that ring with every provided buffer recycled as
 * soon as its bytes are consumed, a re-arm on every CQE without
 * IORING_CQE_F_MORE (-ENOBUFS included), one send in flight per connection
 * with a short send resubmitted, and TCP_NODELAY. A kernel or liburing that
 * breaks one of those shows here first.
 *
 * The same two exchanges are timed over plain blocking send and recv on the
 * same kind of connection. Both measure the same link, so their ratio does not
 * depend on the hardware; that ratio, the median over seven pairs of trials
 * each measured beside the other, is what the ctest pins (§8.4: ratios, not
 * absolute times). A small run verifies the stream word by word through a
 * 4 KiB send buffer and four 1 KiB provided buffers, which forces short sends,
 * -ENOBUFS and re-arms, and words split across buffers.
 *
 * The memory side of §10's assumption is measured beside the wire: copy
 * bandwidth over a working set larger than any cache, and an 8-byte copy
 * followed by a seq_cst fence. The two ratios printed last - one-way latency
 * over the 8-byte copy, and copy bandwidth over stream bandwidth - are the
 * ones §10 asks about ("wire latency dominating memory bandwidth by two orders
 * of magnitude").
 *
 *   echo-uring [--rounds N] [--mebibytes M] [--check LATENCY BANDWIDTH]
 *       both ends on loopback, the server in a forked child
 *   echo-uring --server PORT         one end, on one host
 *   echo-uring --client HOST PORT    the other, on another
 */
#define _GNU_SOURCE
#include <errno.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum mode { PING_URING = 1, PING_BLOCKING, STREAM_URING, STREAM_BLOCKING, STREAM_SMALL, QUIT };

/* The client's first 16 bytes on every connection: what to do, and how much. */
struct hello {
    uint32_t magic;
    uint32_t mode;
    uint64_t count;             /* rounds, or bytes */
};
#define HELLO_MAGIC UINT32_C(0x6f686365)   /* "echo" */

enum { SEND_BYTES = 128 << 10 };           /* §3.4's default segment: 2 x 64 KiB chunk */
enum { GROUP = 1, RECEIVE = 1, SEND = 2 };

static _Noreturn void fail(const char *what, int error)
{
    fprintf(stderr, "echo-uring: %s: %s\n", what, strerror(error));
    exit(2);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- one end of a connection, on a ring ------------------------------- */

struct endpoint {
    struct io_uring ring;
    struct io_uring_buf_ring *buffer_ring;
    unsigned char *buffers;
    unsigned buffer_count, buffer_bytes;
    int fd;
    bool receive_armed, closed;
    const unsigned char *send_at;       /* the one send in flight (§3.3) */
    size_t send_left;
    bool sending;
    void (*arrived)(struct endpoint *e, const unsigned char *bytes, size_t n);
    void *context;
    uint64_t enobufs, rearms, short_sends;
};

static void endpoint_open(struct endpoint *e, int fd, unsigned buffer_count,
                          unsigned buffer_bytes)
{
    memset(e, 0, sizeof *e);
    e->fd = fd;
    e->buffer_count = buffer_count;
    e->buffer_bytes = buffer_bytes;
    /*
     * §3.3's two bounds. SQ: one recv and one send per connection, nothing
     * else here. CQ: one completion per provided buffer the kernel can fill,
     * plus one terminal CQE per armed recv, plus the send - so the CQ can never
     * fill, and nothing relies on the kernel's overflow list.
     */
    struct io_uring_params p = { .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CQSIZE,
                                 .cq_entries = buffer_count + 2 + 1 };
    int r = io_uring_queue_init_params(4, &e->ring, &p);
    if (r < 0)
        fail("io_uring_queue_init_params", -r);
    if (p.sq_entries < 2 || p.cq_entries < buffer_count + 3)
        fail("ring smaller than §3.3's bounds", ERANGE);
    e->buffer_ring = io_uring_setup_buf_ring(&e->ring, buffer_count, GROUP, 0, &r);
    if (!e->buffer_ring)
        fail("io_uring_setup_buf_ring", -r);
    e->buffers = malloc((size_t)buffer_count * buffer_bytes);
    if (!e->buffers)
        fail("provided buffers", ENOMEM);
    for (unsigned i = 0; i < buffer_count; i++)
        io_uring_buf_ring_add(e->buffer_ring, e->buffers + (size_t)i * buffer_bytes,
                              buffer_bytes, (unsigned short)i,
                              io_uring_buf_ring_mask(buffer_count), (int)i);
    io_uring_buf_ring_advance(e->buffer_ring, (int)buffer_count);
}

static void endpoint_close(struct endpoint *e)
{
    io_uring_free_buf_ring(&e->ring, e->buffer_ring, e->buffer_count, GROUP);
    io_uring_queue_exit(&e->ring);
    free(e->buffers);
    close(e->fd);
}

static void arm_receive(struct endpoint *e)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&e->ring);
    io_uring_prep_recv_multishot(sqe, e->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = GROUP;
    io_uring_sqe_set_data64(sqe, RECEIVE);
    e->receive_armed = true;
}

static void submit_send(struct endpoint *e)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&e->ring);
    io_uring_prep_send(sqe, e->fd, e->send_at, e->send_left, 0);
    io_uring_sqe_set_data64(sqe, SEND);
    e->sending = true;
}

/* Start the connection's one send. The caller keeps the bytes unchanged until
 * the send has finished (e->sending is false again). */
static void endpoint_send(struct endpoint *e, const void *bytes, size_t n)
{
    if (e->sending)
        fail("a second send in flight", EBUSY);    /* §3.3, invariant 2 */
    e->send_at = bytes;
    e->send_left = n;
    submit_send(e);
}

/*
 * Submit what is queued, wait for at least one completion, and handle every
 * completion there is. A provided buffer is handed to `arrived` and recycled
 * before the next CQE is looked at: nothing is ever held (§3.3, invariant 23).
 */
static void endpoint_pump(struct endpoint *e)
{
    int r = io_uring_submit_and_wait(&e->ring, 1);
    if (r < 0 && r != -EINTR)
        fail("io_uring_submit_and_wait", -r);
    struct io_uring_cqe *cqe;
    unsigned head, seen = 0;
    io_uring_for_each_cqe(&e->ring, head, cqe) {
        seen++;
        if (io_uring_cqe_get_data64(cqe) == SEND) {
            if (cqe->res < 0)
                fail("send", -cqe->res);
            if ((size_t)cqe->res < e->send_left)
                e->short_sends++;
            e->send_at += cqe->res;
            e->send_left -= (size_t)cqe->res;
            e->sending = false;
            if (e->send_left > 0)
                submit_send(e);                 /* advance past a short send */
            continue;
        }
        if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
            unsigned id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            e->arrived(e, e->buffers + (size_t)id * e->buffer_bytes, (size_t)cqe->res);
            io_uring_buf_ring_add(e->buffer_ring, e->buffers + (size_t)id * e->buffer_bytes,
                                  e->buffer_bytes, (unsigned short)id,
                                  io_uring_buf_ring_mask(e->buffer_count), 0);
            io_uring_buf_ring_advance(e->buffer_ring, 1);
        } else if (cqe->res == 0) {
            e->closed = true;                   /* the peer's FIN */
        } else if (cqe->res == -ENOBUFS) {
            e->enobufs++;                       /* normal under load: re-armed below */
        } else if (cqe->res < 0) {
            fail("recv", -cqe->res);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE))
            e->receive_armed = false;
    }
    io_uring_cq_advance(&e->ring, seen);
    if (!e->receive_armed && !e->closed) {      /* every CQE without F_MORE re-arms */
        arm_receive(e);
        e->rearms++;
    }
}

/* ---- sockets ---------------------------------------------------------- */

static void tune(int fd, bool small)
{
    /* The application layer owns coalescing (§3.3, invariant 14). */
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0)
        fail("TCP_NODELAY", errno);
    /*
     * §8.2: make short writes happen. Only the send side: a receive buffer this
     * small turns every window update into a delayed-ACK stall, which costs
     * about a hundred seconds per 4 MiB and tests TCP rather than io_uring. The
     * receiving side is squeezed by its provided buffers instead.
     */
    if (small) {
        int bytes = 4096;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
    }
}

static void write_all(int fd, const void *bytes, size_t n)
{
    for (const char *at = bytes; n > 0; ) {
        ssize_t k = send(fd, at, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            fail("send", errno);
        }
        at += k;
        n -= (size_t)k;
    }
}

static void read_all(int fd, void *bytes, size_t n)
{
    for (char *at = bytes; n > 0; ) {
        ssize_t k = recv(fd, at, n, 0);
        if (k == 0)
            fail("recv", ECONNRESET);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            fail("recv", errno);
        }
        at += k;
        n -= (size_t)k;
    }
}

/* ---- the server ------------------------------------------------------- */

struct echo_state { unsigned char pending[64]; size_t have; };

static void echo_arrived(struct endpoint *e, const unsigned char *bytes, size_t n)
{
    struct echo_state *s = e->context;
    if (s->have + n > sizeof s->pending)
        fail("echo overran its buffer", EOVERFLOW);
    memcpy(s->pending + s->have, bytes, n);
    s->have += n;
}

/* Stream verification: every 8-byte word holds its own offset in words, so a
 * byte lost, repeated or reordered anywhere is caught, including a word split
 * across two provided buffers. */
struct sink_state { uint64_t received, expected; bool verify; unsigned char carry[8]; unsigned carried; };

static void sink_arrived(struct endpoint *e, const unsigned char *bytes, size_t n)
{
    struct sink_state *s = e->context;
    for (size_t i = 0; s->verify && i < n; i++) {
        s->carry[s->carried++] = bytes[i];
        if (s->carried < 8)
            continue;
        uint64_t word, offset = (s->received + i + 1) / 8 - 1;
        memcpy(&word, s->carry, 8);
        if (word != offset) {
            fprintf(stderr, "echo-uring: stream word %llu holds %llu\n",
                    (unsigned long long)offset, (unsigned long long)word);
            exit(3);
        }
        s->carried = 0;
    }
    s->received += n;
}

static void serve(int fd, const struct hello *h)
{
    bool small = h->mode == STREAM_SMALL;
    tune(fd, small);
    if (h->mode == PING_BLOCKING || h->mode == STREAM_BLOCKING) {
        static unsigned char buffer[64 << 10];
        uint64_t total = 0;
        if (h->mode == PING_BLOCKING)
            for (uint64_t i = 0; i < h->count; i++) {
                read_all(fd, buffer, 8);
                write_all(fd, buffer, 8);
            }
        else
            while (total < h->count) {
                ssize_t k = recv(fd, buffer, sizeof buffer, 0);
                if (k <= 0)
                    fail("recv", k == 0 ? ECONNRESET : errno);
                total += (uint64_t)k;
            }
        write_all(fd, &total, sizeof total);
        close(fd);
        return;
    }

    struct endpoint e;
    endpoint_open(&e, fd, small ? 4 : 64, small ? 1024 : 16 << 10);
    struct echo_state echo = { .have = 0 };
    struct sink_state sink = { .expected = h->count, .verify = small };
    if (h->mode == PING_URING) {
        e.arrived = echo_arrived;
        e.context = &echo;
    } else {
        e.arrived = sink_arrived;
        e.context = &sink;
    }
    arm_receive(&e);
    static unsigned char outgoing[64];
    for (uint64_t echoed = 0; h->mode == PING_URING ? echoed < h->count * 8
                                                    : sink.received < sink.expected; ) {
        endpoint_pump(&e);
        if (e.closed)
            fail("client closed early", ECONNRESET);
        if (h->mode == PING_URING && !e.sending && echo.have > 0) {
            memcpy(outgoing, echo.pending, echo.have);
            endpoint_send(&e, outgoing, echo.have);
            echoed += echo.have;
            echo.have = 0;
        }
    }
    static uint64_t total;
    total = sink.received;
    while (e.sending)
        endpoint_pump(&e);
    if (h->mode != PING_URING) {
        endpoint_send(&e, &total, sizeof total);
        while (e.sending)
            endpoint_pump(&e);
    }
    if (small && (e.short_sends + e.enobufs + e.rearms == 0))
        fprintf(stderr, "echo-uring: note: the small run forced no -ENOBUFS\n");
    if (small)
        printf("small enobufs=%llu rearms=%llu\n", (unsigned long long)e.enobufs,
               (unsigned long long)e.rearms);
    fflush(stdout);
    endpoint_close(&e);
}

static void server_loop(int listener)
{
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            fail("accept", errno);
        struct hello h;
        read_all(fd, &h, sizeof h);
        if (h.magic != HELLO_MAGIC)
            fail("hello", EPROTO);
        if (h.mode == QUIT) {
            close(fd);
            return;
        }
        serve(fd, &h);
    }
}

/* ---- the client ------------------------------------------------------- */

struct client_state { size_t awaited; };

static void ping_arrived(struct endpoint *e, const unsigned char *bytes, size_t n)
{
    (void)bytes;
    struct client_state *s = e->context;
    s->awaited = n > s->awaited ? 0 : s->awaited - n;
}

static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median_of(double *values, int n)
{
    qsort(values, (size_t)n, sizeof *values, compare_doubles);
    return values[n / 2];
}

static struct sockaddr_storage peer;
static socklen_t peer_length;

static int dial(enum mode mode, uint64_t count)
{
    int fd = socket(peer.ss_family, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&peer, peer_length) != 0)
        fail("connect", errno);
    tune(fd, mode == STREAM_SMALL);
    struct hello h = { HELLO_MAGIC, (uint32_t)mode, count };
    write_all(fd, &h, sizeof h);
    return fd;
}

/* Median round trip in ns over `rounds`, after a warm-up of a tenth. */
static double ping(enum mode mode, uint64_t rounds)
{
    uint64_t warm = rounds / 10, total = warm + rounds;
    uint64_t *sample = malloc(rounds * sizeof *sample);
    int fd = dial(mode, total);
    uint64_t word = 0x0123456789abcdef;
    if (mode == PING_BLOCKING) {
        for (uint64_t i = 0; i < total; i++) {
            uint64_t t0 = now_ns();
            write_all(fd, &word, 8);
            read_all(fd, &word, 8);
            if (i >= warm)
                sample[i - warm] = now_ns() - t0;
        }
        read_all(fd, &word, 8);                /* the server's count */
        close(fd);
    } else {
        struct endpoint e;
        struct client_state s;
        endpoint_open(&e, fd, 8, 4096);
        e.arrived = ping_arrived;
        e.context = &s;
        arm_receive(&e);
        for (uint64_t i = 0; i < total; i++) {
            uint64_t t0 = now_ns();
            s.awaited = 8;
            endpoint_send(&e, &word, 8);
            while (s.awaited > 0 || e.sending)
                endpoint_pump(&e);
            if (i >= warm)
                sample[i - warm] = now_ns() - t0;
        }
        endpoint_close(&e);
    }
    qsort(sample, rounds, sizeof *sample, compare);
    printf("%s round_trip_ns min=%llu median=%llu p99=%llu rounds=%llu\n",
           mode == PING_URING ? "uring" : "blocking", (unsigned long long)sample[0],
           (unsigned long long)sample[rounds / 2], (unsigned long long)sample[rounds * 99 / 100],
           (unsigned long long)rounds);
    double median = (double)sample[rounds / 2];
    free(sample);
    return median;
}

/* Bytes per second from the first send to the server's count coming back. */
static double stream(enum mode mode, uint64_t bytes)
{
    static uint64_t chunk[SEND_BYTES / 8];
    int fd = dial(mode, bytes);
    uint64_t t0 = now_ns(), sent = 0, count = 0;
    if (mode == STREAM_BLOCKING) {
        for (; sent < bytes; sent += SEND_BYTES)
            write_all(fd, chunk, bytes - sent < SEND_BYTES ? bytes - sent : SEND_BYTES);
        read_all(fd, &count, sizeof count);
        close(fd);
    } else {
        struct endpoint e;
        struct client_state s = { .awaited = 8 };
        endpoint_open(&e, fd, 8, 4096);
        e.arrived = ping_arrived;
        e.context = &s;
        arm_receive(&e);
        while (sent < bytes || e.sending || s.awaited > 0) {
            if (!e.sending && sent < bytes) {
                size_t n = bytes - sent < SEND_BYTES ? bytes - sent : SEND_BYTES;
                if (mode == STREAM_SMALL)       /* words carry their own offset */
                    for (size_t i = 0; i < n / 8; i++)
                        chunk[i] = sent / 8 + i;
                endpoint_send(&e, chunk, n);
                sent += n;
            }
            endpoint_pump(&e);
        }
        if (mode == STREAM_SMALL)
            printf("small short_sends=%llu (client)\n", (unsigned long long)e.short_sends);
        endpoint_close(&e);
    }
    double seconds = (double)(now_ns() - t0) / 1e9;
    printf("%s stream_mib_per_s=%.1f mebibytes=%llu\n",
           mode == STREAM_BLOCKING ? "blocking" : mode == STREAM_URING ? "uring" : "small",
           (double)bytes / seconds / (1 << 20), (unsigned long long)(bytes >> 20));
    return (double)bytes / seconds;
}

/* ---- the memory side of §10's ratio ------------------------------------ */

static double copy_bandwidth(void)
{
    size_t bytes = (size_t)64 << 20;            /* past any last-level cache */
    unsigned char *a = malloc(bytes), *b = malloc(bytes);
    if (!a || !b)
        fail("copy buffers", ENOMEM);
    memset(a, 1, bytes);
    memset(b, 2, bytes);
    double best = 0;
    for (int pass = 0; pass < 5; pass++) {
        uint64_t t0 = now_ns();
        for (size_t at = 0; at < bytes; at += 64 << 10)   /* in §3.2's chunks */
            memcpy(b + at, a + at, 64 << 10);
        double rate = (double)bytes / ((double)(now_ns() - t0) / 1e9);
        if (rate > best)
            best = rate;
    }
    free(a);
    free(b);
    return best;
}

static volatile uint64_t keep;                 /* so the copies are not dead stores */

static double copy_and_fence_ns(void)
{
    static _Alignas(64) unsigned char target[64];
    uint64_t value = 1, rounds = 10000000;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < rounds; i++) {
        value += i;
        memcpy(target + (i & 7) * 8, &value, 8);
        atomic_thread_fence(memory_order_seq_cst);   /* what quiet adds (§3.16) */
    }
    double each = (double)(now_ns() - t0) / (double)rounds;
    uint64_t sum = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t word;
        memcpy(&word, target + i * 8, 8);
        sum += word;
    }
    keep = sum;
    return each;
}

int main(int argc, char **argv)
{
    uint64_t rounds = 20000, mebibytes = 1024;
    double max_latency_ratio = 0, min_bandwidth_ratio = 0;
    const char *host = NULL, *port = NULL;
    bool server = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
            rounds = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--mebibytes") && i + 1 < argc)
            mebibytes = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--check") && i + 2 < argc) {
            max_latency_ratio = strtod(argv[++i], NULL);
            min_bandwidth_ratio = strtod(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--server") && i + 1 < argc) {
            server = true;
            port = argv[++i];
        } else if (!strcmp(argv[i], "--client") && i + 2 < argc) {
            host = argv[++i];
            port = argv[++i];
        } else {
            fprintf(stderr, "usage: echo-uring [--rounds N] [--mebibytes M] "
                            "[--check LATENCY BANDWIDTH] [--server PORT | --client HOST PORT]\n");
            return 2;
        }
    }
    if (rounds < 10 || mebibytes < 1)
        fail("--rounds is at least 10 and --mebibytes at least 1", EINVAL);

    int listener = -1;
    pid_t child = -1;
    if (host) {
        struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found;
        int r = getaddrinfo(host, port, &hints, &found);
        if (r != 0) {
            fprintf(stderr, "echo-uring: %s: %s\n", host, gai_strerror(r));
            return 2;
        }
        memcpy(&peer, found->ai_addr, found->ai_addrlen);
        peer_length = found->ai_addrlen;
        freeaddrinfo(found);
    } else {
        struct sockaddr_in a = { .sin_family = AF_INET,
                                 .sin_port = htons(port ? (uint16_t)atoi(port) : 0),
                                 .sin_addr.s_addr = htonl(server ? INADDR_ANY : INADDR_LOOPBACK) };
        listener = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (listener < 0 || bind(listener, (void *)&a, sizeof a) != 0 || listen(listener, 8) != 0)
            fail("listen", errno);
        if (server) {
            server_loop(listener);
            return 0;
        }
        peer_length = sizeof a;
        getsockname(listener, (struct sockaddr *)&peer, &peer_length);
        fflush(stdout);
        child = fork();
        if (child < 0)
            fail("fork", errno);
        if (child == 0) {
            /* A hung client is killed by its ctest timeout; take the server with it. */
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() == 1)
                _exit(2);
            server_loop(listener);
            _exit(0);
        }
    }

    /*
     * Correctness first: a failure here is an io_uring bug, not a slow link.
     * Then TRIALS pairs of trials, each io_uring measurement beside its blocking
     * one, and the pinned ratio is the median over the pairs of the ratio within
     * each. On an oversubscribed machine (the load loop: 16 busy loops on 4 CPUs;
     * or ctest -j4 beside other tests) a round trip falls into one of three
     * modes, about 4, 15 or 25 us by where the two processes land and whether a
     * CPU must wake, for io_uring and blocking alike. Comparing the best of
     * each kind failed when one blocking trial caught the fastest mode and no
     * io_uring trial did (4.3x); a pair measured together shares its mode, and
     * the median of seven pairs ignores the odd pair that does not. The best of
     * each kind is still what the report prints.
     */
    enum { TRIALS = 7 };
    stream(STREAM_SMALL, (uint64_t)1 << 20);
    double uring = 1e300, blocking = 1e300, uring_rate = 0, blocking_rate = 0;
    double latency_ratio[TRIALS], bandwidth_ratio[TRIALS];
    for (int trial = 0; trial < TRIALS; trial++) {
        double u = ping(PING_URING, rounds), b = ping(PING_BLOCKING, rounds);
        double ur = stream(STREAM_URING, mebibytes << 20);
        double br = stream(STREAM_BLOCKING, mebibytes << 20);
        latency_ratio[trial] = u / b;
        bandwidth_ratio[trial] = ur / br;
        uring = u < uring ? u : uring;
        blocking = b < blocking ? b : blocking;
        uring_rate = ur > uring_rate ? ur : uring_rate;
        blocking_rate = br > blocking_rate ? br : blocking_rate;
    }
    close(dial(QUIT, 0));
    int status = 0;
    if (child > 0 && (waitpid(child, &status, 0) != child || status != 0))
        fail("the server", ECHILD);

    double copy = copy_bandwidth(), fence = copy_and_fence_ns();
    printf("memory copy_mib_per_s=%.1f copy8_seq_cst_fence_ns=%.2f\n", copy / (1 << 20), fence);
    printf("ratio one_way_latency_over_copy8=%.0f copy_bandwidth_over_stream=%.2f\n",
           uring / 2 / fence, copy / uring_rate);
    double latency = median_of(latency_ratio, TRIALS);
    double bandwidth = median_of(bandwidth_ratio, TRIALS);
    printf("ratio uring_over_blocking round_trip=%.3f stream=%.3f (median of %d pairs)\n",
           latency, bandwidth, TRIALS);

    /*
     * The pins measure the io_uring path, so they hold only where the harness's
     * own code runs as it will ship. Under asan or tsan, or unoptimized, the
     * io_uring side - whose loop is mostly the harness's code - slows and the
     * blocking side - mostly the kernel's - does not: 6.4x and 3.9x round trips
     * in the first sanitizer runs, measuring the instrumentation.
     */
#if defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    bool pinned = true;
#else
    bool pinned = false;
#endif
    if (!pinned && (max_latency_ratio > 0 || min_bandwidth_ratio > 0)) {
        printf("pins not checked: an instrumented or unoptimized build measures itself\n");
        return 0;
    }
    if (max_latency_ratio > 0 && latency > max_latency_ratio) {
        printf("FAIL: io_uring round trip %.3fx blocking, pinned at most %.3fx\n", latency,
               max_latency_ratio);
        return 1;
    }
    if (min_bandwidth_ratio > 0 && bandwidth < min_bandwidth_ratio) {
        printf("FAIL: io_uring stream %.3fx blocking, pinned at least %.3fx\n", bandwidth,
               min_bandwidth_ratio);
        return 1;
    }
    return 0;
}
