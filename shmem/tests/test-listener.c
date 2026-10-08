/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The lead's half of OSOE_JOB_SECRET (listener.c; §6.2), in one process
 * (tier 1): a caller that sends a correct hello but cannot prove the secret is
 * refused and not counted, and the lead goes on listening for one that can.
 *
 * shmem.job.secret.missing and .different cover the caller's half - a lead
 * whose peer has no secret, or another one, ends the job. Both ends of those
 * are the library, which does not lie, so the lead's own check - the one that
 * keeps out a host that can reach its port and knows the job's public ids but
 * not the secret - is reached only by a caller that does. This test is that
 * caller. It needs the hello's private layout, so it includes listener.c
 * rather than linking it.
 *
 * And the round-three review's N2, which its investigator found with a fuzzer
 * of hellos: a stranger's hello claiming another launch is not acted on before
 * its proof, and a stranger that says hello and then nothing does not keep the
 * lead from counting the real peer (listener.h).
 */
#include "listener.c"

#include <pthread.h>
#include <stdarg.h>
#include <time.h>

#include "check.h"

_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static struct osoe_listener lead;
static int accepted = -1;
static uint32_t accepted_node = UINT32_MAX;

static void *accept_one(void *unused)
{
    (void)unused;
    accepted = osoe_listener_accept(&lead, OSOE_HELLO_PURPOSE(OSOE_HELLO_DATA_WIRE),
                                    &accepted_node, NULL, "node 1");
    return NULL;
}

/* Wait for the lead's accept at most 10 s, so that a lead held by a caller ends
 * this test as a failure rather than as ctest's timeout. */
static bool joined_in_time(pthread_t t)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    return pthread_timedjoin_np(t, NULL, &until) == 0;
}

/* A caller that knows the job's public ids and not its secret: it connects,
 * sends `h`, and reads the lead's answer. */
static int stranger(const struct osoe_bootstrap *as, const struct sockaddr_in *at,
                    const struct hello *h, struct answer *a)
{
    struct attempt tried;
    int fd = dial(at, lead.port, &tried);
    CHECK_FATAL(fd >= 0);
    CHECK_FATAL(write_all(as, fd, h, sizeof *h, 0) && read_all(as, fd, a, sizeof *a, 0, 0));
    return fd;
}

/* The lead closes `fd` without counting it: a lead that counted it would return
 * it from accept and hold it open, and the poll would expire. */
static void refused(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char byte;
    CHECK_FATAL(poll(&p, 1, 10000) == 1 && recv(fd, &byte, 1, 0) == 0);
    close(fd);
}

/* A lead that knows the secret calls, and is the one counted. */
static void real_peer_joins(const struct osoe_bootstrap *node1, const struct sockaddr_in *at,
                            pthread_t t)
{
    int good = osoe_listener_connect_port(node1, 0, at, lead.port, 0, OSOE_HELLO_DATA_WIRE);
    CHECK_FATAL(joined_in_time(t));
    CHECK_FATAL(accepted >= 0);
    CHECK(accepted_node == 1);
    struct sockaddr_in mine, theirs;
    socklen_t length = sizeof mine;
    CHECK(getsockname(good, (struct sockaddr *)&mine, &length) == 0);
    length = sizeof theirs;
    CHECK(getpeername(accepted, (struct sockaddr *)&theirs, &length) == 0);
    CHECK(mine.sin_port == theirs.sin_port);
    close(good);
    close(accepted);
    accepted = -1;
    accepted_node = UINT32_MAX;
}

int main(void)
{
    struct sockaddr_in loopback = { .sin_family = AF_INET,
                                    .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct osoe_bootstrap node0 = { .world_size = 2, .node_count = 2, .node = 0, .job = 4242,
                                    .port_low = 17000, .port_high = 26999, .secured = true };
    memcpy(node0.secret, "sixteen byte key", 16);
    struct osoe_bootstrap node1 = node0;
    node1.node = 1;
    osoe_listener_open_any_port(&lead, &node0, 0, &loopback);
    pthread_t t;

    /* 1. A stranger: every field of its hello is right, and none of them is
     * secret - the job and step ids are public (squeue). Its best guess at a
     * proof is the one the lead just sent it; the answer and the confirmation
     * are made with different tags, so replaying it fails. */
    CHECK_FATAL(pthread_create(&t, NULL, accept_one, NULL) == 0);
    struct hello h = { HELLO_MAGIC, HELLO_VERSION, OSOE_HELLO_DATA_WIRE, node1.job, node1.step, 0,
                       1, 0, node1.node_count, node1.world_size, fresh_nonce() };
    struct answer a;
    int fd = stranger(&node1, &loopback, &h, &a);
    CHECK(a.verdict == ACCEPTED && a.node == 0 && a.secured);
    struct confirmation guess = { HELLO_MAGIC, a.proof };
    CHECK_FATAL(write_all(&node1, fd, &guess, sizeof guess, 0));
    refused(fd);
    real_peer_joins(&node1, &loopback, t);

    /* 2. The round-three review's N2: a stranger whose hello claims another
     * launch - three nodes - is answered as any caller is, and nothing it
     * claims is acted on before it proves the secret, which it cannot: before,
     * the lead ended the job on the count alone. */
    CHECK_FATAL(pthread_create(&t, NULL, accept_one, NULL) == 0);
    struct hello wrong = h;
    wrong.node_count = 3;
    wrong.world_size = 3;
    wrong.nonce = fresh_nonce();
    fd = stranger(&node1, &loopback, &wrong, &a);
    CHECK(a.verdict == ACCEPTED);
    struct confirmation nonsense = { HELLO_MAGIC, 0x5eed };
    CHECK_FATAL(write_all(&node1, fd, &nonsense, sizeof nonsense, 0));
    refused(fd);
    real_peer_joins(&node1, &loopback, t);

    /* 3. The same for the node a hello claims: one outside the job, which the
     * lead also judges only after the proof - a lead that judged it on arrival
     * ended the job, and no test sent one (the adversarial read of
     * 2026-10-03). */
    CHECK_FATAL(pthread_create(&t, NULL, accept_one, NULL) == 0);
    struct hello far = h;
    far.from_node = 7;
    far.nonce = fresh_nonce();
    fd = stranger(&node1, &loopback, &far, &a);
    CHECK(a.verdict == ACCEPTED);
    CHECK_FATAL(write_all(&node1, fd, &nonsense, sizeof nonsense, 0));
    refused(fd);
    real_peer_joins(&node1, &loopback, t);

    /* 4. N2's other half: a stranger that says hello and then nothing, holding
     * its connection open. Before, the lead read its confirmation with no limit
     * and served no one else; now the real peer is counted beside it. */
    CHECK_FATAL(pthread_create(&t, NULL, accept_one, NULL) == 0);
    struct hello quiet = h;
    quiet.nonce = fresh_nonce();
    int silent = stranger(&node1, &loopback, &quiet, &a);
    CHECK(a.verdict == ACCEPTED);
    real_peer_joins(&node1, &loopback, t);
    close(silent);

    osoe_listener_close(&lead);
    return CHECK_DONE();
}

/* The library's notice (fatal.h), which the listener's waits print once they
 * have lasted (base/stall-watch.h): here, to stderr as it is. */
void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_notice: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
