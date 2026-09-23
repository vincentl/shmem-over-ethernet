#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "fatal.h"
#include "listener.h"

/*
 * How many candidate ports each lead has. A host using all of them for
 * something else is misconfigured, and binding fails naming the range.
 */
enum { CANDIDATES = 32 };

/*
 * How long a caller waits for the answer to a hello, and a lead for the hello
 * on a connection it accepted, before treating the other end as a stranger and
 * moving on. Not a time bound on the job (§3.15): a caller that gives up tries
 * the next candidate and then the whole sequence again, and the three-step hello
 * is what makes giving up safe (listener.h).
 */
#define PROBE_NS  UINT64_C(1000000000)          /* 1 s for an answer */
#define HELLO_NS  UINT64_C(100000000)           /* 0.1 s for a hello */

#define HELLO_MAGIC   UINT64_C(0x6f6c6568656f736f)   /* "osoehelo", little-endian */
#define HELLO_VERSION 1

struct hello {
    uint64_t magic;
    uint32_t version;
    uint32_t purpose;           /* enum osoe_hello_purpose */
    uint64_t job;
    uint32_t step;
    uint32_t session;
    uint32_t from_node;
    uint32_t to_node;
};

enum verdict {
    ACCEPTED = 1,
    NOT_THIS_NODE,              /* another job, another node: try the next candidate */
    NOT_YET,                    /* the right node, busy with another purpose: back off */
};

struct answer {
    uint64_t magic;
    uint32_t version;
    uint32_t verdict;           /* enum verdict */
    uint32_t node;              /* who answered */
    uint32_t reserved;
};

/* The caller's third step (listener.h): only after this does a lead count it. */
struct confirmation {
    uint64_t magic;
};

/* ---- the port rule --------------------------------------------------- */

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

uint16_t osoe_listener_candidate(const struct osoe_bootstrap *b, uint32_t node, uint32_t i)
{
    uint64_t span = (uint64_t)b->port_high - b->port_low + 1;
    uint64_t h = mix(b->job ^ mix(((uint64_t)b->step << 32) | node) ^
                     mix(i + UINT64_C(0x9E3779B97F4A7C15)));
    return (uint16_t)(b->port_low + h % span);
}

/* ---- small blocking I/O with a limit --------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* Read exactly n bytes, or return false on end of stream, on an error, or once
 * `limit_ns` has passed without them (0: no limit, sweeping). */
static bool read_all(int fd, void *bytes, size_t n, uint64_t limit_ns, const char *routine)
{
    uint64_t start = now_ns();
    for (char *at = bytes; n > 0; ) {
        uint64_t wait_ns = OSOE_SWEEP_NS;
        if (limit_ns) {
            uint64_t spent = now_ns() - start;
            if (spent >= limit_ns)
                return false;
            if (limit_ns - spent < wait_ns)
                wait_ns = limit_ns - spent;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)((wait_ns + 999999) / 1000000));
        if (r < 0 && errno != EINTR)
            return false;
        if (r <= 0) {
            if (!limit_ns)
                osoe_sweep_expired(routine);
            continue;
        }
        ssize_t k = recv(fd, at, n, MSG_DONTWAIT);
        if (k == 0)
            return false;
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return false;
        }
        at += k;
        n -= (size_t)k;
    }
    return true;
}

/* A small write on a fresh connection: the socket buffer takes it whole. */
static bool write_all(int fd, const void *bytes, size_t n)
{
    for (const char *at = bytes; n > 0; ) {
        ssize_t k = send(fd, at, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        at += k;
        n -= (size_t)k;
    }
    return true;
}

/* ---- listening -------------------------------------------------------- */

void osoe_listener_open(struct osoe_listener *l, const struct osoe_bootstrap *b,
                        uint32_t session)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    for (uint32_t i = 0; i < CANDIDATES; i++) {
        uint16_t port = osoe_listener_candidate(b, b->node, i);
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            osoe_fatal("cannot create a listening socket: %s", strerror(errno));
        /*
         * No SO_REUSEADDR: a candidate still in TIME_WAIT from an earlier job is
         * skipped like one in use, which is what the sequence is for, and a
         * listener that could share a port with a stranger's would receive its
         * connections.
         */
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        /* A backlog for every lower node at once: the eager mesh's worst case
         * (§6.2), capped by the kernel at net.core.somaxconn. */
        if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
            listen(fd, (int)(b->node_count < 4096 ? b->node_count : 4096)) == 0) {
            l->fd = fd;
            l->port = port;
            return;
        }
        int error = errno;
        close(fd);
        if (error != EADDRINUSE && error != EACCES)
            osoe_fatal("cannot listen on port %u: %s", port, strerror(error));
    }
    osoe_fatal("node %u found none of its %u candidate ports free in OSOE_PORT_RANGE=%u-%u; "
               "choose a range this host does not use", b->node, CANDIDATES,
               b->port_low, b->port_high);
}

void osoe_listener_close(struct osoe_listener *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
}

int osoe_listener_accept(struct osoe_listener *l, enum osoe_hello_purpose purpose,
                         uint32_t *peer)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    for (;;) {
        struct pollfd p = { .fd = l->fd, .events = POLLIN };
        int r = poll(&p, 1, (int)(OSOE_SWEEP_NS / 1000000));
        if (r < 0 && errno != EINTR)
            osoe_fatal("waiting for connections: %s", strerror(errno));
        if (r <= 0) {
            osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is "
                           "waiting for another node to connect to its port %u",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), l->node,
                           l->port);
            continue;
        }
        int fd = accept4(l->fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                continue;
            osoe_fatal("accepting a connection on port %u: %s", l->port, strerror(errno));
        }
        struct hello h;
        if (!read_all(fd, &h, sizeof h, HELLO_NS, "shmem_init") ||
            h.magic != HELLO_MAGIC || h.version != HELLO_VERSION) {
            close(fd);                  /* a stranger, or a caller that gave up */
            continue;
        }
        struct answer a = { HELLO_MAGIC, HELLO_VERSION, ACCEPTED, l->node, 0 };
        if (h.job != b->job || h.step != b->step || h.session != l->session ||
            h.to_node != l->node)
            a.verdict = NOT_THIS_NODE;
        else if (h.purpose != (uint32_t)purpose)
            a.verdict = NOT_YET;
        else if (h.from_node >= b->node_count || h.from_node == l->node)
            osoe_fatal("node %u was called by a lead claiming to be node %u of a job of %u "
                       "nodes", l->node, h.from_node, b->node_count);
        bool sent = write_all(fd, &a, sizeof a);
        if (a.verdict != ACCEPTED || !sent) {
            close(fd);
            continue;
        }
        struct confirmation c;
        if (!read_all(fd, &c, sizeof c, 0, "shmem_init") || c.magic != HELLO_MAGIC) {
            close(fd);                  /* the caller gave up before our answer came */
            continue;
        }
        *peer = h.from_node;
        return fd;
    }
}

/* ---- calling ---------------------------------------------------------- */

/* A connection to host:port, or -1 if nothing listens there within PROBE_NS. */
static int dial(const struct sockaddr_in *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        osoe_fatal("cannot create a socket: %s", strerror(errno));
    struct sockaddr_in a = *host;
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int error = 0;
        socklen_t length = sizeof error;
        if (poll(&p, 1, (int)(PROBE_NS / 1000000)) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            close(fd);
            return -1;
        }
    }
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
    struct hello h = { HELLO_MAGIC, HELLO_VERSION, (uint32_t)purpose, b->job, b->step,
                       session, b->node, node };
    for (;;) {
        for (uint32_t i = 0; i < CANDIDATES; i++) {
            int fd = dial(host, osoe_listener_candidate(b, node, i));
            if (fd < 0)
                continue;
            struct answer a;
            if (!write_all(fd, &h, sizeof h) ||
                !read_all(fd, &a, sizeof a, PROBE_NS, "shmem_init") ||
                a.magic != HELLO_MAGIC || a.version != HELLO_VERSION) {
                close(fd);
                continue;
            }
            if (a.verdict == ACCEPTED && a.node == node) {
                struct confirmation c = { HELLO_MAGIC };
                if (write_all(fd, &c, sizeof c))
                    return fd;
                close(fd);
                continue;
            }
            close(fd);
            if (a.verdict == NOT_YET)
                break;                  /* found, and not ready: wait, then again */
        }
        if (osoe_deadline_expired(&sweep)) {
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has "
                           "not answered node %u on any of its %u candidate ports in "
                           "OSOE_PORT_RANGE=%u-%u", (unsigned long long)osoe_stall_watch_seconds(&watch),
                           node, b->node, CANDIDATES, b->port_low, b->port_high);
        }
        nanosleep(&back, NULL);
        if (back.tv_nsec < 10000000)
            back.tv_nsec *= 2;
    }
}
