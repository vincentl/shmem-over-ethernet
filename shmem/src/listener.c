/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/hostlist.h"
#include "base/print-line.h"
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
#define HELLO_VERSION 4         /* 2: the launch's shape;
                                 * 3: nonces and proofs (OSOE_JOB_SECRET);
                                 * 4: a control wire beside each pair's data wire */

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
                  uint64_t moved, uint32_t peer, const char *what, int fd)
{
    osoe_sweep_expired("shmem_init");
    if (osoe_stall_watch_stalled(watch, moved))
        osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is waiting for "
                   "node %u %s", (unsigned long long)osoe_stall_watch_seconds(watch), b->node,
                   peer, what);
    if (osoe_stall_watch_notice_due(watch)) {
        /* Where the connection goes, as the calling loop's notice says it:
         * the README promises the address and port of a node not answering. */
        struct sockaddr_in at;
        socklen_t at_bytes = sizeof at;
        char where[64] = "";
        char address[INET_ADDRSTRLEN];
        if (getpeername(fd, (struct sockaddr *)&at, &at_bytes) == 0 &&
            at.sin_family == AF_INET &&
            inet_ntop(AF_INET, &at.sin_addr, address, sizeof address))
            snprintf(where, sizeof where, ", at %s port %u,", address, ntohs(at.sin_port));
        osoe_notice("shmem_init on node %u has waited %llu s for node %u%s %s its part of "
                    "the rendezvous (%llu bytes so far); still waiting", b->node,
                    (unsigned long long)osoe_stall_watch_waited_seconds(watch), peer, where,
                    what, (unsigned long long)moved);
    }
}

/* Read exactly n bytes from node `peer`, or return false on end of stream, on an
 * error, or once `limit_ns` has passed without them. With no limit (0) it waits
 * as long as the peer takes, sweeping (swept). */
static bool read_all(const struct osoe_bootstrap *b, int fd, void *bytes, size_t n,
                     uint64_t limit_ns, uint32_t peer)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
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
                swept(b, &watch, moved, peer, "to send", fd);
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
    osoe_stall_watch_begin_noticed(&watch, 0, osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
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
            swept(b, &watch, moved, peer, "to receive", fd);
    }
    return true;
}

/* ---- listening -------------------------------------------------------- */

/*
 * What a socket that could not be had for want of descriptors means here, to
 * follow the kernel's "Too many open files" (D14): a node's lead holds two
 * sockets for every other node and a descriptor for every other PE of its node
 * beside the program's own, and shmem_init has raised the soft limit to the
 * hard one (init.c), so the hard limit is the one to raise. Nothing for any
 * other error.
 */
static const char *descriptor_hint(int error, char *buf, size_t len)
{
    struct rlimit files;
    if (error == EMFILE && getrlimit(RLIMIT_NOFILE, &files) == 0 &&
        files.rlim_cur != RLIM_INFINITY) {
        snprintf(buf, len, ": this process may hold %llu descriptors, which shmem_init raised "
                 "to the hard limit, and a node's lead holds two sockets for every other node "
                 "and a descriptor for every other PE of its node beside the program's own - "
                 "raise the hard limit on open files, ulimit -Hn",
                 (unsigned long long)files.rlim_cur);
        return buf;
    }
    if (error == ENFILE)
        return ": the system's limit on open files, fs.file-max, is reached";
    return "";
}

/* What the end of a wait on a node that has not come says beside it (D15): a
 * node takes part in the rendezvous once every PE of its own has called
 * shmem_init, and that node's own end names any that has not - first, since
 * this wait's limit is longer by a hop's grace (base/stall-watch.h). */
#define NOT_COME " - a node comes to the rendezvous once all its PEs have called " \
                 "shmem_init, and names any that has not"

/* Bind and listen on every address at `port`, or return false if the port is
 * taken. Every address, the rendezvous's as the mesh's (P20): a peer that
 * resolves this node's name to another address of this host - a host with
 * several, which a site's names may give differently from node to node -
 * reaches it there, where a listener on the one address its own entry names
 * left that peer calling a port nothing listened on (the release review's
 * portability N4, measured). */
static bool try_listen(struct osoe_listener *l, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        char hint[320];
        osoe_fatal("cannot create a listening socket: %s%s", strerror(e),
                   descriptor_hint(e, hint, sizeof hint));
    }
    /*
     * SO_REUSEADDR (P19): a candidate whose connections an earlier session left
     * in TIME_WAIT is listened on all the same. Without it a program that
     * finalized and initialized again found none of its 32 candidates free at
     * the 33rd session within a minute, its own earlier sessions holding them
     * (round four's portability Q7), though the specification lets it
     * initialize again. On Linux it shares no port with a live listener - a
     * second gets EADDRINUSE, measured (the release review's portability N4) -
     * so a stranger's connections cannot reach this one.
     */
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) != 0)
        osoe_fatal("cannot set SO_REUSEADDR on a listening socket: %s", strerror(errno));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
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
    if (error != EADDRINUSE && error != EACCES)
        osoe_fatal("cannot listen on port %u: %s", port, strerror(error));
    return false;
}

/*
 * A caller part-way through the hello. Before its answer the hello is read
 * within HELLO_NS of its accept; after an ACCEPTED answer, its confirmation is
 * waited for as long as it takes (listener.h).
 */
struct osoe_listener_caller {
    int fd;                     /* -1: the place is free */
    bool answered;              /* ACCEPTED sent; its confirmation is what comes */
    uint32_t purpose;           /* the purpose it was answered for */
    uint64_t hello_until_ns;    /* before the answer: when its hello is late */
    size_t got;                 /* bytes of the hello, then of the confirmation */
    struct hello hello;
    struct answer answer;
    struct confirmation confirmation;
};

static void callers_open(struct osoe_listener *l)
{
    l->callers = calloc(OSOE_LISTENER_CALLERS, sizeof *l->callers);
    if (!l->callers)
        osoe_fatal("out of memory");
    for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++)
        l->callers[i].fd = -1;
}

void osoe_listener_open(struct osoe_listener *l, const struct osoe_bootstrap *b,
                        uint32_t session)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    callers_open(l);
    for (uint32_t i = 0; i < CANDIDATES; i++)
        if (try_listen(l, osoe_listener_candidate(b, b->node, i)))
            return;
    osoe_fatal("node %u found none of its %u candidate ports free in OSOE_PORT_RANGE=%u-%u; "
               "choose a range this host does not use", b->node, CANDIDATES,
               b->port_low, b->port_high);
}

void osoe_listener_open_told_port(struct osoe_listener *l, const struct osoe_bootstrap *b,
                                  uint32_t session)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    callers_open(l);
    if (b->mesh_port_low == 0) {
        if (!try_listen(l, 0))
            osoe_fatal("node %u could not listen on any port", b->node);
        return;
    }
    /* OSOE_MESH_PORT_RANGE (P18): every port of the range, from one this
     * node's ids pick, so that one host's logical nodes - and jobs - do not
     * all try the same ports first. */
    uint32_t span = (uint32_t)b->mesh_port_high - b->mesh_port_low + 1;
    uint32_t first = (uint32_t)(mix(b->job ^ mix(((uint64_t)b->step << 32) | b->node)) % span);
    for (uint32_t i = 0; i < span; i++)
        if (try_listen(l, (uint16_t)(b->mesh_port_low + (first + i) % span)))
            return;
    osoe_fatal("node %u found no port of OSOE_MESH_PORT_RANGE=%u-%u free for its mesh; choose a "
               "range this host does not use", b->node, b->mesh_port_low, b->mesh_port_high);
}

void osoe_listener_check_own_address(const struct osoe_bootstrap *b,
                                     const struct sockaddr_in *address)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        char hint[320];
        osoe_fatal("cannot create a socket to check this node's address: %s%s", strerror(e),
                   descriptor_hint(e, hint, sizeof hint));
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0, .sin_addr = address->sin_addr };
    int bound = bind(fd, (struct sockaddr *)&a, sizeof a);
    int error = errno;
    close(fd);
    if (bound == 0)
        return;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    if (error == EADDRNOTAVAIL)
        osoe_fatal("node %u cannot listen on %s, the address its host list entry names: it "
                   "is not an address of this host", b->node, ip);
    osoe_fatal("node %u cannot check %s, the address its host list entry names: %s", b->node, ip,
               strerror(error));
}

void osoe_listener_close(struct osoe_listener *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
    if (l->callers) {
        for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++)
            if (l->callers[i].fd >= 0)
                close(l->callers[i].fd);
        free(l->callers);
        l->callers = NULL;
    }
}

/* A place's caller is done with, one way or the other. */
static void caller_drop(struct osoe_listener_caller *c)
{
    close(c->fd);
    c->fd = -1;
}

/* Read what has arrived of the hello or the confirmation; true once it is all
 * there. A caller that closed or failed is dropped (`*gone`). */
static bool caller_read(struct osoe_listener_caller *c, void *into, size_t n, bool *gone)
{
    while (c->got < n) {
        ssize_t k = recv(c->fd, (char *)into + c->got, n - c->got, MSG_DONTWAIT);
        if (k > 0) {
            c->got += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN)
            return false;
        *gone = true;                   /* end of stream, or an error */
        return false;
    }
    return true;
}

/*
 * What a hello claims about the launch, judged once it may be acted on: as it
 * arrives without a secret, after its caller's proof with one (listener.h).
 * Fatal on a count that differs - the one value that shapes the rendezvous's
 * waits (rendezvous.h) - or, from a caller for this purpose, a node that cannot
 * be this job's.
 */
static void judge_counts(const struct osoe_listener *l, const struct hello *h)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    if (h->node_count != b->node_count || h->world_size != b->world_size)
        osoe_fatal("node %u, launched for %u tasks on %u nodes, was called by node %u, "
                   "launched for %u tasks on %u nodes: every lead must be given the same",
                   l->node, b->world_size, b->node_count, h->from_node, h->world_size,
                   h->node_count);
}

static void judge_claim(const struct osoe_listener *l, const struct hello *h)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    if (h->from_node >= b->node_count || h->from_node == l->node)
        osoe_fatal("node %u was called by a lead claiming to be node %u of a job of %u "
                   "nodes", l->node, h->from_node, b->node_count);
}

/*
 * A caller whose hello has arrived: answered, and either kept for its
 * confirmation (ACCEPTED) or dropped.
 */
static void caller_answer(struct osoe_listener *l, struct osoe_listener_caller *c,
                          uint32_t purposes)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    const struct hello *h = &c->hello;
    if (h->magic != HELLO_MAGIC || h->version != HELLO_VERSION) {
        caller_drop(c);                 /* a stranger */
        return;
    }
    struct answer a = { HELLO_MAGIC, HELLO_VERSION, ACCEPTED, l->node, b->secured,
                        fresh_nonce(), 0 };
    /* Without a secret nothing can be proved, and a hello is judged as it
     * arrives, in the order it always was; with one, its claims wait for the
     * proof (caller_confirmed). */
    if (h->job != b->job || h->step != b->step || h->session != l->session ||
        h->to_node != l->node)
        a.verdict = NOT_THIS_NODE;
    else if (!b->secured)
        judge_counts(l, h);
    if (a.verdict == ACCEPTED &&
        (h->purpose >= 32 || !(purposes & OSOE_HELLO_PURPOSE(h->purpose))))
        a.verdict = NOT_YET;
    else if (a.verdict == ACCEPTED && !b->secured)
        judge_claim(l, h);
    if (a.verdict == ACCEPTED && b->secured)
        a.proof = proof(b, ANSWER_TAG, h, a.nonce);
    /* The answer fits any socket buffer: one send, and a caller that is not
     * there to take it is dropped. */
    bool sent = send(c->fd, &a, sizeof a, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof a;
    if (a.verdict != ACCEPTED || !sent) {
        caller_drop(c);
        return;
    }
    c->answer = a;
    c->answered = true;
    c->purpose = h->purpose;
    c->got = 0;
}

/*
 * A caller whose confirmation has arrived: the connection to return, or -1 if
 * it is no lead of this job. With a secret the hello's claims are judged here,
 * once proved (listener.h).
 */
static int caller_confirmed(struct osoe_listener *l, struct osoe_listener_caller *c,
                            uint32_t *peer, enum osoe_hello_purpose *purpose)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    if (c->confirmation.magic != HELLO_MAGIC) {
        caller_drop(c);
        return -1;
    }
    /* A caller that cannot prove the secret is not counted, and nothing it
     * claimed is acted on: whatever it is, it is not a lead of this job. */
    if (b->secured && c->confirmation.proof != proof(b, CONFIRM_TAG, &c->hello, c->answer.nonce)) {
        osoe_print_line("osoe: node %u refused a caller claiming to be node %u of this "
                        "job: it does not know OSOE_JOB_SECRET\n", l->node, c->hello.from_node);
        caller_drop(c);
        return -1;
    }
    if (b->secured) {
        judge_counts(l, &c->hello);
        judge_claim(l, &c->hello);
    }
    int fd = c->fd;
    c->fd = -1;
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    *peer = c->hello.from_node;
    if (purpose)
        *purpose = (enum osoe_hello_purpose)c->purpose;
    return fd;
}

static void describe_fixed(const void *context, char *text, size_t len)
{
    snprintf(text, len, "%s", (const char *)context);
}

int osoe_listener_accept(struct osoe_listener *l, uint32_t purposes, uint32_t *peer,
                         enum osoe_hello_purpose *purpose, const char *expecting)
{
    return osoe_listener_accept_described(l, purposes, peer, purpose, describe_fixed,
                                          expecting ? expecting : "another node");
}

uint32_t osoe_listener_part_way(const struct osoe_listener *l)
{
    uint32_t part = 0;
    for (uint32_t i = 0; l->callers && i < OSOE_LISTENER_CALLERS; i++)
        part += l->callers[i].fd >= 0;
    return part;
}

uint32_t osoe_listener_answered(const struct osoe_listener *l)
{
    uint32_t answered = 0;
    for (uint32_t i = 0; l->callers && i < OSOE_LISTENER_CALLERS; i++)
        answered += l->callers[i].fd >= 0 && l->callers[i].answered;
    return answered;
}

void osoe_listener_stop_answering(struct osoe_listener *l)
{
    if (l->fd >= 0)
        close(l->fd);
    l->fd = -1;
    for (uint32_t i = 0; l->callers && i < OSOE_LISTENER_CALLERS; i++)
        if (l->callers[i].fd >= 0 && !l->callers[i].answered)
            caller_drop(&l->callers[i]);
}

uint32_t osoe_listener_poll(struct osoe_listener *l, uint32_t purposes, struct pollfd *p,
                            uint64_t *serve_by_ns)
{
    /* A caller already confirmed between calls cannot be: each is returned as
     * its confirmation arrives. So poll the listening socket, while a place is
     * free, and every caller part-way through. */
    uint32_t count = 0;
    uint64_t now = now_ns();
    bool room = false;
    for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++) {
        struct osoe_listener_caller *c = &l->callers[i];
        if (c->fd < 0) {
            room = true;
            continue;
        }
        if (!c->answered) {
            if (now >= c->hello_until_ns) {
                caller_drop(c);         /* a stranger, or a caller that gave up */
                room = true;
                continue;
            }
            if (c->hello_until_ns < *serve_by_ns)
                *serve_by_ns = c->hello_until_ns;
        } else if (!(purposes & OSOE_HELLO_PURPOSE(c->purpose))) {
            continue;                   /* answered for another purpose: its call */
        }
        p[count] = (struct pollfd){ .fd = c->fd, .events = POLLIN };
        l->polled[count++] = i;
    }
    if (room && l->fd >= 0) {
        p[count] = (struct pollfd){ .fd = l->fd, .events = POLLIN };
        l->polled[count++] = UINT32_MAX;
    }
    l->polled_count = count;
    return count;
}

int osoe_listener_serve(struct osoe_listener *l, uint32_t purposes, struct pollfd *p,
                        uint32_t *peer, enum osoe_hello_purpose *purpose)
{
    for (uint32_t k = 0; k < l->polled_count; k++) {
        if (!p[k].revents)
            continue;
        p[k].revents = 0;               /* taken: a second call goes on past it */
        if (l->polled[k] == UINT32_MAX) {
            int fd = accept4(l->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                    continue;
                int e = errno;
                char hint[320];
                osoe_fatal("accepting a connection on port %u: %s%s", l->port, strerror(e),
                           descriptor_hint(e, hint, sizeof hint));
            }
            for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++)
                if (l->callers[i].fd < 0) {
                    l->callers[i] = (struct osoe_listener_caller){
                        .fd = fd, .hello_until_ns = now_ns() + HELLO_NS };
                    break;
                }
            continue;
        }
        struct osoe_listener_caller *c = &l->callers[l->polled[k]];
        if (c->fd != p[k].fd)
            continue;                   /* its place taken by another since */
        bool gone = false;
        if (!c->answered) {
            if (caller_read(c, &c->hello, sizeof c->hello, &gone))
                caller_answer(l, c, purposes);
            else if (gone)
                caller_drop(c);
        } else if (caller_read(c, &c->confirmation, sizeof c->confirmation, &gone)) {
            int fd = caller_confirmed(l, c, peer, purpose);
            if (fd >= 0)
                return fd;
        } else if (gone) {
            caller_drop(c);             /* the caller gave up before our answer came */
        }
    }
    return -1;
}

/* Wait in poll(2) until `until_ns` at the latest, to the nanosecond: a
 * back-off of 0.2 ms is not a millisecond's. */
static int poll_until(struct pollfd *p, nfds_t count, uint64_t until_ns)
{
    uint64_t now = now_ns(), wait_ns = until_ns > now ? until_ns - now : 0;
    struct timespec t = { (time_t)(wait_ns / UINT64_C(1000000000)),
                          (long)(wait_ns % UINT64_C(1000000000)) };
    return ppoll(p, count, &t, NULL);
}

int osoe_listener_accept_described(struct osoe_listener *l, uint32_t purposes, uint32_t *peer,
                                   enum osoe_hello_purpose *purpose,
                                   void (*describe)(const void *context, char *text, size_t len),
                                   const void *context)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    char expecting[200];
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct pollfd p[OSOE_LISTENER_POLLED];
    for (;;) {
        /* At the sweep's cadence whatever arrives meanwhile, so that callers
         * that come and go cannot keep the sweep from running. */
        if (osoe_deadline_expired(&sweep)) {
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_sweep_expired("shmem_init");
            bool stalled = osoe_stall_watch_stalled(&watch, 0);
            bool notice = !stalled && osoe_stall_watch_notice_due(&watch);
            if (stalled || notice)
                describe(context, expecting, sizeof expecting);
            if (stalled)
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is "
                           "waiting for %s to connect to its port %u" NOT_COME,
                           (unsigned long long)osoe_stall_watch_seconds(&watch), l->node,
                           expecting, l->port);
            if (notice) {
                uint32_t part = osoe_listener_part_way(l);
                osoe_notice("shmem_init on node %u has waited %llu s for %s to call it on "
                            "port %u (%u caller%s part-way through the hello); still waiting",
                            l->node, (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            expecting, l->port, part, part == 1 ? "" : "s");
            }
        }
        uint64_t serve_by = now_ns() + OSOE_SWEEP_NS;
        uint32_t count = osoe_listener_poll(l, purposes, p, &serve_by);
        int r = poll_until(p, count, serve_by);
        if (r < 0 && errno != EINTR)
            osoe_fatal("waiting for connections: %s", strerror(errno));
        if (r <= 0)
            continue;
        int fd = osoe_listener_serve(l, purposes, p, peer, purpose);
        if (fd >= 0)
            return fd;
    }
}

/* ---- calling ---------------------------------------------------------- */

/*
 * What a caller last met, for its notice (listener.h): the node it calls, as
 * its host list entry and the address dialled, what happened, and when.
 */
struct attempt {
    char last[96];
    uint64_t at_ns;
};

static void attempted(struct attempt *t, const char *what)
{
    snprintf(t->last, sizeof t->last, "%s", what);
    t->at_ns = now_ns();
}

/* "node 5 (pi5, at 10.0.0.5) to answer on port 40213: Connection refused, 0.2 s
 * ago" - the README promises the address and port of a node not answering. */
static void describe_calling(const struct osoe_bootstrap *b, uint32_t node,
                             const struct sockaddr_in *host, const char *ports,
                             const struct attempt *t, char *text, size_t len)
{
    char entry[256] = "?", ip[INET_ADDRSTRLEN] = "?";
    if (b->node_list)
        osoe_hostlist_entry(b->node_list, node, entry, sizeof entry);
    inet_ntop(AF_INET, &host->sin_addr, ip, sizeof ip);
    char last[sizeof t->last + 48] = "not tried yet";
    if (t->at_ns) {
        uint64_t ago_ms = (now_ns() - t->at_ns) / UINT64_C(1000000);
        snprintf(last, sizeof last, "%s, %llu.%llu s ago", t->last,
                 (unsigned long long)(ago_ms / 1000), (unsigned long long)(ago_ms % 1000 / 100));
    }
    snprintf(text, len, "node %u (%s, at %s) to answer on %s: %s", node, entry, ip, ports, last);
}

/*
 * A call, a step at a time (listener.h; H23). With a told port, a dial that
 * fails, an answer that does not come, or a peer busy with another purpose
 * waits a back-off and dials again; anything but that node of this job
 * answering is fatal. With the port rule, each candidate is dialled in turn -
 * the answer within PROBE_NS, since a stranger's port may say nothing - and a
 * round of them ends in a back-off, or early where the node answered busy.
 */
enum call_state {
    CALL_IDLE,                  /* not begun, or done with */
    CALL_WAITING,               /* to dial at until_ns */
    CALL_DIALLING,              /* connecting, until until_ns */
    CALL_HELLO,                 /* writing the hello */
    CALL_ANSWER,                /* reading the answer: on a candidate until until_ns */
    CALL_CONFIRMING,            /* writing the confirmation */
};

struct osoe_listener_call {
    const struct osoe_bootstrap *bootstrap;
    uint32_t session, node;
    enum osoe_hello_purpose purpose;
    struct sockaddr_in host;
    uint16_t port;              /* as told, or 0: the port rule's candidates */
    uint32_t candidate;         /* with the port rule, the one dialled */
    int telling;                /* the round's most telling answer: 0 nobody, 1 silence,
                                 * 2 another node's or job's, 3 the node's own */
    enum call_state state;
    int fd;
    uint64_t until_ns;
    uint64_t back_ns;           /* 0.2 ms, doubling past 10 ms once */
    uint64_t asked_ns;          /* when the hello went, for the notice */
    size_t done;                /* bytes of the hello, the answer or the confirmation */
    struct hello hello;
    struct answer answer;
    struct confirmation confirmation;
    /* What it met: with the port rule, the last whole round's most telling
     * (tried) and this round's so far (round). */
    struct attempt tried, round;
};

struct osoe_listener_call *osoe_listener_call_new(void)
{
    struct osoe_listener_call *c = calloc(1, sizeof *c);
    if (!c)
        osoe_fatal("out of memory");
    c->fd = -1;
    return c;
}

static void call_close(struct osoe_listener_call *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

void osoe_listener_call_free(struct osoe_listener_call *c)
{
    if (!c)
        return;
    call_close(c);
    free(c);
}

void osoe_listener_call_begin(struct osoe_listener_call *c, const struct osoe_bootstrap *b,
                              uint32_t session, const struct sockaddr_in *host, uint16_t port,
                              uint32_t node, enum osoe_hello_purpose purpose)
{
    call_close(c);
    *c = (struct osoe_listener_call){ .bootstrap = b, .session = session, .node = node,
                                      .purpose = purpose, .host = *host, .port = port,
                                      .state = CALL_WAITING, .fd = -1,
                                      .back_ns = UINT64_C(200000) };
}

/* Wait before dialling again: a told port after a failed attempt, the
 * candidates after a round. */
static void call_back_off(struct osoe_listener_call *c)
{
    call_close(c);
    c->state = CALL_WAITING;
    c->until_ns = now_ns() + c->back_ns;
    if (c->back_ns < UINT64_C(10000000))
        c->back_ns *= 2;
}

/* With the port rule, the round's end: its most telling answer kept for the
 * notice, and a back-off before the next. */
static void call_end_round(struct osoe_listener_call *c)
{
    if (c->round.at_ns)
        c->tried = c->round;
    c->round = (struct attempt){ "", 0 };
    c->telling = 0;
    c->candidate = 0;
    call_back_off(c);
}

static void call_next_candidate(struct osoe_listener_call *c)
{
    call_close(c);
    if (++c->candidate == CANDIDATES) {
        call_end_round(c);
        return;
    }
    c->state = CALL_WAITING;
    c->until_ns = 0;
}

static void call_dial_failed(struct osoe_listener_call *c, const char *what)
{
    if (c->port) {
        attempted(&c->tried, what);
        call_back_off(c);
        return;
    }
    if (c->telling == 0)
        attempted(&c->round, what);
    call_next_candidate(c);
}

/* The hello's answer did not come: the connection ended first, or, on a
 * candidate, its second did. */
static void call_unanswered(struct osoe_listener_call *c)
{
    if (c->port) {
        attempted(&c->tried, "the connection closed before an answer to the hello");
        call_back_off(c);
        return;
    }
    if (c->telling < 1) {
        attempted(&c->round, "no answer to the hello on a candidate port");
        c->telling = 1;
    }
    call_next_candidate(c);
}

/* Connected: the hello, with a nonce fresh for this connection. */
static void call_hello(struct osoe_listener_call *c)
{
    const struct osoe_bootstrap *b = c->bootstrap;
    c->hello = (struct hello){ HELLO_MAGIC, HELLO_VERSION, (uint32_t)c->purpose, b->job, b->step,
                               c->session, b->node, c->node, b->node_count, b->world_size,
                               fresh_nonce() };
    c->state = CALL_HELLO;
    c->done = 0;
}

static void call_dial(struct osoe_listener_call *c)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        int e = errno;
        char hint[320];
        osoe_fatal("cannot create a socket: %s%s", strerror(e),
                   descriptor_hint(e, hint, sizeof hint));
    }
    struct sockaddr_in a = c->host;
    a.sin_port = htons(c->port ? c->port
                               : osoe_listener_candidate(c->bootstrap, c->node, c->candidate));
    c->fd = fd;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        call_hello(c);
        return;
    }
    int e = errno;
    if (e != EINPROGRESS) {
        call_dial_failed(c, strerror(e));
        return;
    }
    c->state = CALL_DIALLING;
    c->until_ns = now_ns() + PROBE_NS;
}

/* The answer has come: judged as osoe_listener_connect_port and
 * osoe_listener_connect always did. */
static void call_answered(struct osoe_listener_call *c)
{
    const struct osoe_bootstrap *b = c->bootstrap;
    const struct answer *a = &c->answer;
    if (c->port) {
        /* Told this port, so whoever answers is the peer or a disagreement. */
        if (a->magic != HELLO_MAGIC || a->version != HELLO_VERSION ||
            a->verdict == NOT_THIS_NODE || a->node != c->node)
            osoe_fatal("node %u reached node %u's advertised port %u and found node %u "
                       "or a stranger there (verdict %u): the nodes disagree about the job",
                       b->node, c->node, c->port, a->node, a->verdict);
        if (a->verdict != ACCEPTED) {
            attempted(&c->tried, "it is busy with another purpose");
            call_back_off(c);           /* NOT_YET: the peer is busy with another purpose */
            return;
        }
    } else if (a->magic != HELLO_MAGIC || a->version != HELLO_VERSION) {
        call_unanswered(c);
        return;
    } else if (a->verdict != ACCEPTED || a->node != c->node) {
        if (a->verdict == NOT_YET) {
            attempted(&c->round, "it answered, busy with another purpose");
            c->telling = 3;
            call_end_round(c);          /* found, and not ready: wait, then again */
            return;
        }
        if (c->telling < 2) {
            attempted(&c->round, "its candidate ports answered for another node or job");
            c->telling = 2;
        }
        call_next_candidate(c);
        return;
    }
    c->confirmation = confirm(b, &c->hello, a);
    c->state = CALL_CONFIRMING;
    c->done = 0;
}

/* Move what is left of `n` bytes, without waiting: true once all have gone or
 * come, false while the socket has no room or nothing more (`*failed` at an
 * end of stream or an error). */
static bool call_move(struct osoe_listener_call *c, void *bytes, size_t n, bool sending,
                      bool *failed)
{
    while (c->done < n) {
        ssize_t k = sending
            ? send(c->fd, (char *)bytes + c->done, n - c->done, MSG_NOSIGNAL | MSG_DONTWAIT)
            : recv(c->fd, (char *)bytes + c->done, n - c->done, MSG_DONTWAIT);
        if (k > 0) {
            c->done += (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN)
            return false;
        *failed = true;
        return false;
    }
    return true;
}

int osoe_listener_call_poll(const struct osoe_listener_call *c, short *events,
                            uint64_t *step_by_ns)
{
    switch (c->state) {
    case CALL_WAITING:
        if (c->until_ns < *step_by_ns)
            *step_by_ns = c->until_ns;
        return -1;
    case CALL_DIALLING:
    case CALL_HELLO:
    case CALL_CONFIRMING:
        *events = POLLOUT;
        break;
    case CALL_ANSWER:
        *events = POLLIN;
        break;
    default:
        return -1;
    }
    if ((c->state == CALL_DIALLING || c->state == CALL_ANSWER) && c->until_ns &&
        c->until_ns < *step_by_ns)
        *step_by_ns = c->until_ns;
    return c->fd;
}

int osoe_listener_call_step(struct osoe_listener_call *c, short revents)
{
    bool failed = false;
    if (c->state == CALL_WAITING) {
        if (now_ns() < c->until_ns)
            return -1;
        call_dial(c);
    } else if (c->state == CALL_DIALLING) {
        if (!revents) {
            if (now_ns() >= c->until_ns)
                call_dial_failed(c, "the connection timed out");
            return -1;
        }
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            call_dial_failed(c, strerror(error ? error : errno));
            return -1;
        }
        call_hello(c);
    }
    if (c->state == CALL_HELLO) {
        if (!call_move(c, &c->hello, sizeof c->hello, true, &failed)) {
            if (failed)
                call_unanswered(c);
            return -1;
        }
        c->state = CALL_ANSWER;
        c->done = 0;
        c->asked_ns = now_ns();
        /* No limit on the answer from a told port, which is the peer's; a
         * candidate's may be a stranger's that says nothing. */
        c->until_ns = c->port ? 0 : c->asked_ns + PROBE_NS;
    }
    if (c->state == CALL_ANSWER) {
        if (!call_move(c, &c->answer, sizeof c->answer, false, &failed)) {
            if (failed || (c->until_ns && now_ns() >= c->until_ns))
                call_unanswered(c);
            return -1;
        }
        call_answered(c);
    }
    if (c->state == CALL_CONFIRMING) {
        if (!call_move(c, &c->confirmation, sizeof c->confirmation, true, &failed)) {
            if (failed && c->port)
                call_back_off(c);
            else if (failed)
                call_next_candidate(c);
            return -1;
        }
        int fd = c->fd;
        c->fd = -1;
        c->state = CALL_IDLE;
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        return fd;
    }
    return -1;
}

void osoe_listener_call_abandon(struct osoe_listener_call *c)
{
    call_close(c);
    c->state = CALL_IDLE;
}

void osoe_listener_call_describe(const struct osoe_listener_call *c, char *text, size_t len)
{
    const struct osoe_bootstrap *b = c->bootstrap;
    char ports[64];
    if (c->port)
        snprintf(ports, sizeof ports, "port %u", c->port);
    else
        snprintf(ports, sizeof ports, "its candidate ports in OSOE_PORT_RANGE=%u-%u",
                 b->port_low, b->port_high);
    struct attempt asked = { "connected, and its hello not yet answered", c->asked_ns };
    const struct attempt *t = c->state == CALL_ANSWER && c->port ? &asked
                              : !c->port && c->round.at_ns   ? &c->round
                                                             : &c->tried;
    describe_calling(b, c->node, &c->host, ports, t, text, len);
}

/*
 * A call waited for alone: osoe_listener_connect's and
 * osoe_listener_connect_port's loop. The sweep (§3.15) - the abort word,
 * OSOE_NO_PROGRESS_TIMEOUT and the notice - at its cadence, between dials as
 * well as within them: a dial to an address that never answers ARP takes most
 * of a second, and a round 32 of them, so a check once a round put the 30 s
 * notice near 50 s (the tier-3 test of 2026-10-02).
 */
static int call_alone(struct osoe_listener_call *c)
{
    const struct osoe_bootstrap *b = c->bootstrap;
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, osoe_stall_watch_limit_beyond(b->no_progress_ns, 1));
    uint64_t sweep_ns = now_ns() + OSOE_SWEEP_NS;
    for (;;) {
        if (now_ns() >= sweep_ns) {
            sweep_ns = now_ns() + OSOE_SWEEP_NS;
            osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0)) {
                if (c->port)
                    osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has "
                               "not answered node %u on its port %u" NOT_COME,
                               (unsigned long long)osoe_stall_watch_seconds(&watch), c->node,
                               b->node, c->port);
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has "
                           "not answered node %u on any of its %u candidate ports in "
                           "OSOE_PORT_RANGE=%u-%u" NOT_COME,
                           (unsigned long long)osoe_stall_watch_seconds(&watch), c->node,
                           b->node, CANDIDATES, b->port_low, b->port_high);
            }
            if (osoe_stall_watch_notice_due(&watch)) {
                char what[512];
                osoe_listener_call_describe(c, what, sizeof what);
                osoe_notice("shmem_init on node %u has waited %llu s for %s; still waiting",
                            b->node, (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            what);
            }
        }
        uint64_t step_by = sweep_ns;
        short events = 0;
        struct pollfd p = { .fd = osoe_listener_call_poll(c, &events, &step_by) };
        p.events = events;
        int r = poll_until(&p, p.fd >= 0, step_by);
        if (r < 0 && errno != EINTR)
            osoe_fatal("calling node %u: %s", c->node, strerror(errno));
        int fd = osoe_listener_call_step(c, r > 0 ? p.revents : 0);
        if (fd >= 0)
            return fd;
    }
}

int osoe_listener_connect_port(const struct osoe_bootstrap *b, uint32_t session,
                               const struct sockaddr_in *host, uint16_t port, uint32_t node,
                               enum osoe_hello_purpose purpose)
{
    struct osoe_listener_call c = { .fd = -1 };
    osoe_listener_call_begin(&c, b, session, host, port, node, purpose);
    return call_alone(&c);
}

int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose)
{
    struct osoe_listener_call c = { .fd = -1 };
    osoe_listener_call_begin(&c, b, session, host, 0, node, purpose);
    return call_alone(&c);
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
        osoe_fatal("%sSTEP_NODELIST='%s' has no usable entry for node %u",
                   b->launcher ? b->launcher : "OSOE_", b->node_list ? b->node_list : "",
                   node);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found;
    int e = getaddrinfo(host, NULL, &hints, &found);
    if (e != 0)
        osoe_fatal("cannot resolve %s, node %u's host in %sSTEP_NODELIST: %s", host, node,
                   b->launcher ? b->launcher : "OSOE_",
                   gai_strerror(e));
    *out = *(const struct sockaddr_in *)found->ai_addr;
    out->sin_port = 0;
    freeaddrinfo(found);
}

void osoe_listener_own_address(const struct osoe_bootstrap *b, struct sockaddr_in *out)
{
    osoe_listener_node_address(b, b->node, out);
    char host[256];
    if (!osoe_hostlist_entry(b->node_list, b->node, host, sizeof host) ||
        !osoe_listener_self_is_loopback(host, out, b->node_count))
        return;
    /* Another node, and the address this host would send to it from: a UDP
     * socket connected to it, which sends nothing. */
    uint32_t other = b->node == 0 ? 1 : 0;
    struct sockaddr_in toward, local;
    osoe_listener_node_address(b, other, &toward);
    if ((ntohl(toward.sin_addr.s_addr) >> 24) == 127)
        return;                         /* every node here: one host's logical nodes */
    toward.sin_port = htons(9);
    socklen_t len = sizeof local;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    bool found = fd >= 0 && connect(fd, (struct sockaddr *)&toward, sizeof toward) == 0 &&
                 getsockname(fd, (struct sockaddr *)&local, &len) == 0;
    if (fd >= 0)
        close(fd);
    char self_ip[INET_ADDRSTRLEN], local_ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &out->sin_addr, self_ip, sizeof self_ip);
    if (!found)
        osoe_fatal("node %u's host %s resolves here to %s, a loopback address its peers "
                   "cannot reach, and no route toward node %u was found: map the name to "
                   "this host's address in /etc/hosts (design §6.2)", b->node, host, self_ip,
                   other);
    out->sin_addr = local.sin_addr;
    inet_ntop(AF_INET, &local.sin_addr, local_ip, sizeof local_ip);
    if (b->debug)
        osoe_print_line("osoe: node %u: %s resolves here to %s, a loopback address; "
                        "listening at %s, this host's address toward node %u\n", b->node, host,
                        self_ip, local_ip, other);
}
