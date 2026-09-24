#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/hostlist.h"
#include "base/siphash.h"
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
#define HELLO_VERSION 3         /* 2: the launch's shape;
                                 * 3: nonces and proofs (OSOE_JOB_SECRET) */

struct hello {
    uint64_t magic;
    uint32_t version;
    uint32_t purpose;           /* enum osoe_hello_purpose */
    uint64_t job;
    uint32_t step;
    uint32_t session;
    uint32_t from_node;
    uint32_t to_node;
    /* The launch's shape, checked by the lead that answers. The node count is
     * the one value that shapes a rendezvous's waits - a lead expects children
     * by it - so a disagreement about it must be refused on the edge where it
     * first appears, before either end waits on it (rendezvous.h). */
    uint32_t node_count;
    uint32_t world_size;
    uint64_t nonce;             /* the caller's, fresh for this connection */
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
    uint32_t secured;           /* it holds an OSOE_JOB_SECRET */
    uint64_t nonce;             /* the answerer's, fresh for this connection */
    uint64_t proof;             /* with a secret: that it knows it (below) */
};

/* The caller's third step (listener.h): only after this does a lead count it. */
struct confirmation {
    uint64_t magic;
    uint64_t proof;             /* with a secret: that the caller knows it */
};

/*
 * OSOE_JOB_SECRET (§6.2; §10's "authenticating the step-3 handshake"). With a
 * secret, each side proves it knows it without sending it: a SipHash-2-4 digest,
 * keyed by the secret, of the hello and both sides' nonces, one tag for the
 * answer and another for the confirmation. The nonces are fresh per connection,
 * so a proof overheard on one connection answers no other; a host that can
 * connect but does not know the secret cannot join a job, read its heaps or
 * write them. What it does not do is protect a connection from a host that can
 * see and inject its packets after the hello - the network is still trusted
 * that far, as Slurm's own is (§3.4).
 */
enum { ANSWER_TAG = 0x616e73, CONFIRM_TAG = 0x636f6e };

static uint64_t proof(const struct osoe_bootstrap *b, uint32_t tag, const struct hello *h,
                      uint64_t answer_nonce)
{
    unsigned char m[4 + sizeof *h + 8];
    memcpy(m, &tag, 4);
    memcpy(m + 4, h, sizeof *h);
    memcpy(m + 4 + sizeof *h, &answer_nonce, 8);
    return osoe_siphash(b->secret, m, sizeof m);
}

static uint64_t fresh_nonce(void)
{
    uint64_t n = 0;
    if (getrandom(&n, sizeof n, 0) != (ssize_t)sizeof n) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        n = (uint64_t)t.tv_nsec ^ ((uint64_t)t.tv_sec << 30) ^ ((uint64_t)getpid() << 48);
    }
    return n;
}

/*
 * The caller's check of an ACCEPTED answer, which came from a node of this job,
 * session and purpose (the answerer checked that before answering): a secret on
 * one side and not the other, or two different ones, is a launch that gave the
 * job's PEs different environments, and ends it. Returns the confirmation.
 */
static struct confirmation confirm(const struct osoe_bootstrap *b, const struct hello *h,
                                   const struct answer *a)
{
    if ((a->secured != 0) != b->secured)
        osoe_fatal("node %u %s OSOE_JOB_SECRET and node %u %s: every PE of a job must be "
                   "given the same", a->node, a->secured ? "has an" : "has no", b->node,
                   b->secured ? "has one" : "has none");
    if (b->secured && a->proof != proof(b, ANSWER_TAG, h, a->nonce))
        osoe_fatal("node %u answered node %u with a proof of a different OSOE_JOB_SECRET: "
                   "every PE of a job must be given the same", a->node, b->node);
    return (struct confirmation){ HELLO_MAGIC,
                                  b->secured ? proof(b, CONFIRM_TAG, h, a->nonce) : 0 };
}

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

/*
 * A wait with no time limit, at its sweep (§3.15): the sweep, then the
 * no-progress rule on `moved`, the bytes that have crossed so far - so a
 * rendezvous that is slow but moving is never stalled, and one whose peer has
 * stopped ends naming it after OSOE_NO_PROGRESS_TIMEOUT (by default, never).
 */
static void swept(const struct osoe_bootstrap *b, struct osoe_stall_watch *watch,
                  uint64_t moved, uint32_t peer, const char *what)
{
    osoe_sweep_expired("shmem_init");
    if (osoe_stall_watch_stalled(watch, moved))
        osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is waiting for "
                   "node %u %s", (unsigned long long)osoe_stall_watch_seconds(watch), b->node,
                   peer, what);
}

/* Read exactly n bytes from node `peer`, or return false on end of stream, on an
 * error, or once `limit_ns` has passed without them. With no limit (0) it waits
 * as long as the peer takes, sweeping (swept). */
static bool read_all(const struct osoe_bootstrap *b, int fd, void *bytes, size_t n,
                     uint64_t limit_ns, uint32_t peer)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    uint64_t start = now_ns(), moved = 0;
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
                swept(b, &watch, moved, peer, "to send");
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
        moved += (uint64_t)k;
    }
    return true;
}

/*
 * Write exactly n bytes to node `peer`, or return false on an error. A hello
 * fits any socket buffer, but the rendezvous's tables are O(N) bytes - over
 * 600 KB at 4,096 nodes - so a write waits for its reader as a read waits for
 * its writer: sweeping, with no limit (swept).
 */
static bool write_all(const struct osoe_bootstrap *b, int fd, const void *bytes, size_t n,
                      uint32_t peer)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    uint64_t moved = 0;
    for (const char *at = bytes; n > 0; ) {
        ssize_t k = send(fd, at, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k >= 0) {
            at += k;
            n -= (size_t)k;
            moved += (uint64_t)k;
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            return false;
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int r = poll(&p, 1, (int)(OSOE_SWEEP_NS / 1000000));
        if (r < 0 && errno != EINTR)
            return false;
        if (r <= 0)
            swept(b, &watch, moved, peer, "to receive");
    }
    return true;
}

/* ---- listening -------------------------------------------------------- */

/* Bind and listen on address:port, or return false if the port is taken. */
static bool try_listen(struct osoe_listener *l, const struct sockaddr_in *address,
                       uint16_t port)
{
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
    if (address)
        a.sin_addr = address->sin_addr;
    /* A backlog for every lower node at once: the eager mesh's worst case
     * (§6.2), capped by the kernel at net.core.somaxconn. */
    const struct osoe_bootstrap *b = l->bootstrap;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
        listen(fd, (int)(b->node_count < 4096 ? b->node_count : 4096)) == 0) {
        socklen_t length = sizeof a;
        getsockname(fd, (struct sockaddr *)&a, &length);
        l->fd = fd;
        l->port = ntohs(a.sin_port);
        return true;
    }
    int error = errno;
    close(fd);
    if (error == EADDRNOTAVAIL) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
        osoe_fatal("node %u cannot listen on %s, the address its host list entry names: it "
                   "is not an address of this host", b->node, ip);
    }
    if (error != EADDRINUSE && error != EACCES)
        osoe_fatal("cannot listen on port %u: %s", port, strerror(error));
    return false;
}

void osoe_listener_open(struct osoe_listener *l, const struct osoe_bootstrap *b,
                        uint32_t session, const struct sockaddr_in *address)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    for (uint32_t i = 0; i < CANDIDATES; i++)
        if (try_listen(l, address, osoe_listener_candidate(b, b->node, i)))
            return;
    osoe_fatal("node %u found none of its %u candidate ports free in OSOE_PORT_RANGE=%u-%u; "
               "choose a range this host does not use", b->node, CANDIDATES,
               b->port_low, b->port_high);
}

void osoe_listener_open_any_port(struct osoe_listener *l, const struct osoe_bootstrap *b,
                                 uint32_t session, const struct sockaddr_in *address)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    if (!try_listen(l, address, 0))
        osoe_fatal("node %u could not listen on any port", b->node);
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
        if (!read_all(b, fd, &h, sizeof h, HELLO_NS, UINT32_MAX) ||
            h.magic != HELLO_MAGIC || h.version != HELLO_VERSION) {
            close(fd);                  /* a stranger, or a caller that gave up */
            continue;
        }
        struct answer a = { HELLO_MAGIC, HELLO_VERSION, ACCEPTED, l->node, b->secured,
                            fresh_nonce(), 0 };
        if (h.job != b->job || h.step != b->step || h.session != l->session ||
            h.to_node != l->node)
            a.verdict = NOT_THIS_NODE;
        else if (h.node_count != b->node_count || h.world_size != b->world_size)
            osoe_fatal("node %u, launched for %u tasks on %u nodes, was called by node %u, "
                       "launched for %u tasks on %u nodes: every lead must be given the same",
                       l->node, b->world_size, b->node_count, h.from_node, h.world_size,
                       h.node_count);
        else if (h.purpose != (uint32_t)purpose)
            a.verdict = NOT_YET;
        else if (h.from_node >= b->node_count || h.from_node == l->node)
            osoe_fatal("node %u was called by a lead claiming to be node %u of a job of %u "
                       "nodes", l->node, h.from_node, b->node_count);
        if (a.verdict == ACCEPTED && b->secured)
            a.proof = proof(b, ANSWER_TAG, &h, a.nonce);
        bool sent = write_all(b, fd, &a, sizeof a, h.from_node);
        if (a.verdict != ACCEPTED || !sent) {
            close(fd);
            continue;
        }
        struct confirmation c;
        if (!read_all(b, fd, &c, sizeof c, 0, h.from_node) || c.magic != HELLO_MAGIC) {
            close(fd);                  /* the caller gave up before our answer came */
            continue;
        }
        /* A caller that cannot prove the secret is not counted, and not
         * answered further: whatever it is, it is not a lead of this job. */
        if (b->secured && c.proof != proof(b, CONFIRM_TAG, &h, a.nonce)) {
            fprintf(stderr, "osoe: node %u refused a caller claiming to be node %u of this "
                            "job: it does not know OSOE_JOB_SECRET\n", l->node, h.from_node);
            close(fd);
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

int osoe_listener_connect_port(const struct osoe_bootstrap *b, uint32_t session,
                               const struct sockaddr_in *host, uint16_t port, uint32_t node,
                               enum osoe_hello_purpose purpose)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
    for (;;) {
        struct hello h = { HELLO_MAGIC, HELLO_VERSION, (uint32_t)purpose, b->job, b->step,
                           session, b->node, node, b->node_count, b->world_size,
                           fresh_nonce() };
        int fd = dial(host, port);
        if (fd >= 0) {
            struct answer a;
            /* Told this port, so whoever answers is the peer or a
             * disagreement; no time limit on the answer, sweeping. */
            if (!write_all(b, fd, &h, sizeof h, node) || !read_all(b, fd, &a, sizeof a, 0, node)) {
                close(fd);
            } else if (a.magic != HELLO_MAGIC || a.version != HELLO_VERSION ||
                       a.verdict == NOT_THIS_NODE || a.node != node) {
                osoe_fatal("node %u reached node %u's advertised port %u and found node %u "
                           "or a stranger there (verdict %u): the nodes disagree about the job",
                           b->node, node, port, a.node, a.verdict);
            } else if (a.verdict == ACCEPTED) {
                struct confirmation c = confirm(b, &h, &a);
                if (write_all(b, fd, &c, sizeof c, node))
                    return fd;
                close(fd);
            } else {
                close(fd);              /* NOT_YET: the peer is busy with another purpose */
            }
        }
        if (osoe_deadline_expired(&sweep)) {
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has "
                           "not answered node %u on its port %u",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), node, b->node,
                           port);
        }
        nanosleep(&back, NULL);
        if (back.tv_nsec < 10000000)
            back.tv_nsec *= 2;
    }
}

bool osoe_listener_read(const struct osoe_bootstrap *b, int fd, void *bytes, size_t n,
                        uint32_t peer)
{
    return read_all(b, fd, bytes, n, 0, peer);
}

bool osoe_listener_write(const struct osoe_bootstrap *b, int fd, const void *bytes, size_t n,
                         uint32_t peer)
{
    return write_all(b, fd, bytes, n, peer);
}

void osoe_listener_node_address(const struct osoe_bootstrap *b, uint32_t node,
                                struct sockaddr_in *out)
{
    char host[256];
    if (!b->node_list || !osoe_hostlist_entry(b->node_list, node, host, sizeof host))
        osoe_fatal("OSOE_STEP_NODELIST='%s' has no usable entry for node %u",
                   b->node_list ? b->node_list : "", node);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found;
    int e = getaddrinfo(host, NULL, &hints, &found);
    if (e != 0)
        osoe_fatal("cannot resolve %s, node %u's host in OSOE_STEP_NODELIST: %s", host, node,
                   gai_strerror(e));
    *out = *(const struct sockaddr_in *)found->ai_addr;
    out->sin_port = 0;
    freeaddrinfo(found);
}

int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
    for (;;) {
        for (uint32_t i = 0; i < CANDIDATES; i++) {
            struct hello h = { HELLO_MAGIC, HELLO_VERSION, (uint32_t)purpose, b->job, b->step,
                               session, b->node, node, b->node_count, b->world_size,
                               fresh_nonce() };
            int fd = dial(host, osoe_listener_candidate(b, node, i));
            if (fd < 0)
                continue;
            struct answer a;
            if (!write_all(b, fd, &h, sizeof h, node) ||
                !read_all(b, fd, &a, sizeof a, PROBE_NS, node) ||
                a.magic != HELLO_MAGIC || a.version != HELLO_VERSION) {
                close(fd);
                continue;
            }
            if (a.verdict == ACCEPTED && a.node == node) {
                struct confirmation c = confirm(b, &h, &a);
                if (write_all(b, fd, &c, sizeof c, node))
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
                           "OSOE_PORT_RANGE=%u-%u",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), node, b->node,
                           CANDIDATES, b->port_low, b->port_high);
        }
        nanosleep(&back, NULL);
        if (back.tv_nsec < 10000000)
            back.tv_nsec *= 2;
    }
}
