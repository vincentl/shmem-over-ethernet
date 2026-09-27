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
 */
#include "listener.c"

#include <pthread.h>
#include <stdarg.h>

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
    accepted = osoe_listener_accept(&lead, OSOE_HELLO_MESH, &accepted_node);
    return NULL;
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
    CHECK_FATAL(pthread_create(&t, NULL, accept_one, NULL) == 0);

    /* A stranger: every field of its hello is right, and none of them is
     * secret - the job and step ids are public (squeue). */
    int fd = dial(&loopback, lead.port);
    CHECK_FATAL(fd >= 0);
    struct hello h = { HELLO_MAGIC, HELLO_VERSION, OSOE_HELLO_MESH, node1.job, node1.step, 0,
                       1, 0, node1.node_count, node1.world_size, fresh_nonce() };
    struct answer a;
    CHECK_FATAL(write_all(&node1, fd, &h, sizeof h, 0) &&
                read_all(&node1, fd, &a, sizeof a, 0, 0));
    CHECK(a.verdict == ACCEPTED && a.node == 0 && a.secured);
    /* Its best guess at a proof is the one the lead just sent it; the answer
     * and the confirmation are made with different tags, so replaying it fails. */
    struct confirmation guess = { HELLO_MAGIC, a.proof };
    CHECK_FATAL(write_all(&node1, fd, &guess, sizeof guess, 0));
    /* Refused: the lead closes it without counting it. A lead that counted it
     * would return it from accept and hold it open, and this poll would expire. */
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char byte;
    CHECK_FATAL(poll(&p, 1, 10000) == 1 && recv(fd, &byte, 1, 0) == 0);
    close(fd);

    /* A lead that knows the secret is counted, and it is the one counted. */
    int good = osoe_listener_connect_port(&node1, 0, &loopback, lead.port, 0, OSOE_HELLO_MESH);
    CHECK_FATAL(pthread_join(t, NULL) == 0);
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
    osoe_listener_close(&lead);
    return CHECK_DONE();
}
