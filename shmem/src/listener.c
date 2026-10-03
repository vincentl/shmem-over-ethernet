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
    if (osoe_stall_watch_notice_due(watch))
        osoe_notice("shmem_init on node %u has waited %llu s for node %u %s its part of the "
                    "rendezvous (%llu bytes so far); still waiting", b->node,
                    (unsigned long long)osoe_stall_watch_waited_seconds(watch), peer, what,
                    (unsigned long long)moved);
}

/* Read exactly n bytes from node `peer`, or return false on end of stream, on an
 * error, or once `limit_ns` has passed without them. With no limit (0) it waits
 * as long as the peer takes, sweeping (swept). */
static bool read_all(const struct osoe_bootstrap *b, int fd, void *bytes, size_t n,
                     uint64_t limit_ns, uint32_t peer)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
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
    osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
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
                        uint32_t session, const struct sockaddr_in *address)
{
    *l = (struct osoe_listener){ .fd = -1, .node = b->node, .session = session,
                                 .bootstrap = b };
    callers_open(l);
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
    callers_open(l);
    if (!try_listen(l, address, 0))
        osoe_fatal("node %u could not listen on any port", b->node);
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
                          enum osoe_hello_purpose purpose)
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
    if (a.verdict == ACCEPTED && h->purpose != (uint32_t)purpose)
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
    c->purpose = (uint32_t)purpose;
    c->got = 0;
}

/*
 * A caller whose confirmation has arrived: the connection to return, or -1 if
 * it is no lead of this job. With a secret the hello's claims are judged here,
 * once proved (listener.h).
 */
static int caller_confirmed(struct osoe_listener *l, struct osoe_listener_caller *c,
                            uint32_t *peer)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    if (c->confirmation.magic != HELLO_MAGIC) {
        caller_drop(c);
        return -1;
    }
    /* A caller that cannot prove the secret is not counted, and nothing it
     * claimed is acted on: whatever it is, it is not a lead of this job. */
    if (b->secured && c->confirmation.proof != proof(b, CONFIRM_TAG, &c->hello, c->answer.nonce)) {
        fprintf(stderr, "osoe: node %u refused a caller claiming to be node %u of this "
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
    return fd;
}

int osoe_listener_accept(struct osoe_listener *l, enum osoe_hello_purpose purpose,
                         uint32_t *peer, const char *expecting)
{
    const struct osoe_bootstrap *b = l->bootstrap;
    if (!expecting)
        expecting = "another node";
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct pollfd p[1 + OSOE_LISTENER_CALLERS];
    uint32_t at[1 + OSOE_LISTENER_CALLERS];
    for (;;) {
        /* At the sweep's cadence whatever arrives meanwhile, so that callers
         * that come and go cannot keep the sweep from running. */
        if (osoe_deadline_expired(&sweep)) {
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u is "
                           "waiting for %s to connect to its port %u",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), l->node,
                           expecting, l->port);
            if (osoe_stall_watch_notice_due(&watch)) {
                uint32_t part = 0;
                for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++)
                    part += l->callers[i].fd >= 0;
                osoe_notice("shmem_init on node %u has waited %llu s for %s to call it on "
                            "port %u (%u caller%s part-way through the hello); still waiting",
                            l->node, (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            expecting, l->port, part, part == 1 ? "" : "s");
            }
        }
        /* A caller already confirmed between calls cannot be: each is returned
         * as its confirmation arrives. So poll the listening socket, while a
         * place is free, and every caller part-way through. */
        uint32_t count = 0;
        uint64_t now = now_ns(), wait_ns = OSOE_SWEEP_NS;
        bool room = false;
        for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++) {
            struct osoe_listener_caller *c = &l->callers[i];
            if (c->fd < 0) {
                room = true;
                continue;
            }
            if (!c->answered) {
                if (now >= c->hello_until_ns) {
                    caller_drop(c);     /* a stranger, or a caller that gave up */
                    room = true;
                    continue;
                }
                if (c->hello_until_ns - now < wait_ns)
                    wait_ns = c->hello_until_ns - now;
            } else if (c->purpose != (uint32_t)purpose) {
                continue;               /* answered for another purpose: its call */
            }
            p[count] = (struct pollfd){ .fd = c->fd, .events = POLLIN };
            at[count++] = i;
        }
        if (room) {
            p[count] = (struct pollfd){ .fd = l->fd, .events = POLLIN };
            at[count++] = UINT32_MAX;
        }
        int r = poll(p, count, (int)((wait_ns + 999999) / 1000000));
        if (r < 0 && errno != EINTR)
            osoe_fatal("waiting for connections: %s", strerror(errno));
        if (r <= 0)
            continue;
        for (uint32_t k = 0; k < count; k++) {
            if (!p[k].revents)
                continue;
            if (at[k] == UINT32_MAX) {
                int fd = accept4(l->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                        continue;
                    osoe_fatal("accepting a connection on port %u: %s", l->port,
                               strerror(errno));
                }
                for (uint32_t i = 0; i < OSOE_LISTENER_CALLERS; i++)
                    if (l->callers[i].fd < 0) {
                        l->callers[i] = (struct osoe_listener_caller){
                            .fd = fd, .hello_until_ns = now_ns() + HELLO_NS };
                        break;
                    }
                continue;
            }
            struct osoe_listener_caller *c = &l->callers[at[k]];
            bool gone = false;
            if (!c->answered) {
                if (caller_read(c, &c->hello, sizeof c->hello, &gone))
                    caller_answer(l, c, purpose);
                else if (gone)
                    caller_drop(c);
            } else if (caller_read(c, &c->confirmation, sizeof c->confirmation, &gone)) {
                int fd = caller_confirmed(l, c, peer);
                if (fd >= 0)
                    return fd;
            } else if (gone) {
                caller_drop(c);         /* the caller gave up before our answer came */
            }
        }
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

static void notice_calling(const struct osoe_bootstrap *b, const struct osoe_stall_watch *watch,
                           uint32_t node, const struct sockaddr_in *host, const char *ports,
                           const struct attempt *t)
{
    char entry[256] = "?", ip[INET_ADDRSTRLEN] = "?";
    if (b->node_list)
        osoe_hostlist_entry(b->node_list, node, entry, sizeof entry);
    inet_ntop(AF_INET, &host->sin_addr, ip, sizeof ip);
    uint64_t ago_ms = t->at_ns ? (now_ns() - t->at_ns) / UINT64_C(1000000) : 0;
    osoe_notice("shmem_init on node %u has waited %llu s for node %u (%s, at %s) to answer on "
                "%s: %s, %llu.%llu s ago; still waiting", b->node,
                (unsigned long long)osoe_stall_watch_waited_seconds(watch), node, entry, ip,
                ports, t->at_ns ? t->last : "not tried yet",
                (unsigned long long)(ago_ms / 1000), (unsigned long long)(ago_ms % 1000 / 100));
}

/* A connection to host:port, or -1 if nothing listens there within PROBE_NS,
 * with what went wrong in `t`. */
static int dial(const struct sockaddr_in *host, uint16_t port, struct attempt *t)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        osoe_fatal("cannot create a socket: %s", strerror(errno));
    struct sockaddr_in a = *host;
    a.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno != EINPROGRESS) {
            attempted(t, strerror(errno));
            close(fd);
            return -1;
        }
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int error = 0;
        socklen_t length = sizeof error;
        int r = poll(&p, 1, (int)(PROBE_NS / 1000000));
        if (r != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 ||
            error != 0) {
            attempted(t, r == 0 ? "the connection timed out" : strerror(error ? error : errno));
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
    osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
    struct attempt tried = { "", 0 };
    char ports[32];
    snprintf(ports, sizeof ports, "port %u", port);
    for (;;) {
        struct hello h = { HELLO_MAGIC, HELLO_VERSION, (uint32_t)purpose, b->job, b->step,
                           session, b->node, node, b->node_count, b->world_size,
                           fresh_nonce() };
        int fd = dial(host, port, &tried);
        if (fd >= 0) {
            struct answer a;
            /* Told this port, so whoever answers is the peer or a
             * disagreement; no time limit on the answer, sweeping. */
            if (!write_all(b, fd, &h, sizeof h, node) || !read_all(b, fd, &a, sizeof a, 0, node)) {
                attempted(&tried, "the connection closed before an answer to the hello");
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
                attempted(&tried, "it is busy with another purpose");
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
            if (osoe_stall_watch_notice_due(&watch))
                notice_calling(b, &watch, node, host, ports, &tried);
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
        fprintf(stderr, "osoe: node %u: %s resolves here to %s, a loopback address; "
                "listening at %s, this host's address toward node %u\n", b->node, host,
                self_ip, local_ip, other);
}

/*
 * The calling loop's sweep: the abort word, OSOE_NO_PROGRESS_TIMEOUT and the
 * notice, between dials as well as between rounds. A dial to an address that
 * never answers ARP takes most of a second, and a round 32 of them, so a check
 * once a round put the 30 s notice near 50 s (the tier-3 test of 2026-10-02).
 */
static void calling_sweep(const struct osoe_bootstrap *b, struct osoe_stall_watch *watch,
                          struct osoe_deadline *sweep, const struct sockaddr_in *host,
                          uint32_t node, const char *ports, const struct attempt *tried)
{
    if (!osoe_deadline_expired(sweep))
        return;
    *sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    osoe_sweep_expired("shmem_init");
    if (osoe_stall_watch_stalled(watch, 0))
        osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has "
                   "not answered node %u on any of its %u candidate ports in "
                   "OSOE_PORT_RANGE=%u-%u",
                   (unsigned long long)osoe_stall_watch_seconds(watch), node, b->node,
                   CANDIDATES, b->port_low, b->port_high);
    if (osoe_stall_watch_notice_due(watch))
        notice_calling(b, watch, node, host, ports, tried);
}

int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
    /* The most telling thing any candidate said in the last round: an answer
     * from the node beats a stranger's, which beats a port nobody holds. */
    struct attempt tried = { "", 0 }, round = { "", 0 };
    char ports[64];
    snprintf(ports, sizeof ports, "its candidate ports in OSOE_PORT_RANGE=%u-%u", b->port_low,
             b->port_high);
    for (;;) {
        int telling = 0;        /* 0 nobody, 1 silence, 2 another node's, 3 the node's own */
        round = (struct attempt){ "", 0 };
        for (uint32_t i = 0; i < CANDIDATES; i++) {
            calling_sweep(b, &watch, &sweep, host, node, ports, round.at_ns ? &round : &tried);
            struct hello h = { HELLO_MAGIC, HELLO_VERSION, (uint32_t)purpose, b->job, b->step,
                               session, b->node, node, b->node_count, b->world_size,
                               fresh_nonce() };
            struct attempt dialled = { "", 0 };
            int fd = dial(host, osoe_listener_candidate(b, node, i), &dialled);
            if (fd < 0) {
                if (telling == 0)
                    round = dialled;
                continue;
            }
            struct answer a;
            if (!write_all(b, fd, &h, sizeof h, node) ||
                !read_all(b, fd, &a, sizeof a, PROBE_NS, node) ||
                a.magic != HELLO_MAGIC || a.version != HELLO_VERSION) {
                if (telling < 1) {
                    attempted(&round, "no answer to the hello on a candidate port");
                    telling = 1;
                }
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
            if (a.verdict == NOT_YET) {
                attempted(&round, "it answered, busy with another purpose");
                telling = 3;
                break;                  /* found, and not ready: wait, then again */
            }
            if (telling < 2) {
                attempted(&round, "its candidate ports answered for another node or job");
                telling = 2;
            }
        }
        if (round.at_ns)
            tried = round;
        calling_sweep(b, &watch, &sweep, host, node, ports, &tried);
        nanosleep(&back, NULL);
        if (back.tv_nsec < 10000000)
            back.tv_nsec *= 2;
    }
}
