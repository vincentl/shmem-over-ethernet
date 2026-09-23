/*
 * The node-pair wire between two real processes (§3.1; phase 1 step 3).
 *
 * shmem.wire drives both ends from one thread, which settles the stream
 * arithmetic - short sends, partial reads, buffers that end mid-message - but
 * cannot see what only two processes racing can. This one runs the two ends in
 * the two processes they will really be in, each with its own ring, streaming
 * both ways at once, and makes its connection the way a lead does: node 0
 * finds node 1's listener by the port rule (listener.h) and says hello.
 *
 * It does not call shmem_init: it tests the wire on its own, below the
 * library, and needs only the bootstrap environment osoe-launch sets.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "ring.h"
#include "wire.h"

/* bootstrap.c reads the fan-out's default from the backend (internode.h), which
 * this test does not link; AM's value. */
uint32_t osoe_internode_fanout_default(void);
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

enum { PAYLOAD = 1 << 18, CHUNK = 1024 };

/* Each direction carries a different pattern, so bytes that came back the way
 * they went are not mistaken for bytes that crossed. */
static unsigned char pattern(unsigned node, size_t i)
{
    return (unsigned char)((i * 31 + 7 + node * 101) & 0xff);
}

static uint32_t env_u32(const char *name)
{
    const char *v = getenv(name);
    if (!v) {
        fprintf(stderr, "%s is not set; run this under osoe-launch\n", name);
        exit(1);
    }
    return (uint32_t)strtoul(v, NULL, 10);
}

int main(void)
{
    uint32_t me = env_u32("OSOE_NODEID");
    uint32_t peer = me == 0 ? 1 : 0;

    struct osoe_bootstrap b = {
        .job = strtoull(getenv("OSOE_JOB_ID") ? getenv("OSOE_JOB_ID") : "0", NULL, 10),
        .step = env_u32("OSOE_STEP_ID"),
        .node = me,
        .node_count = 2,
        .no_progress_ns = UINT64_C(30000000000),  /* fail rather than hang */
        .port_low = OSOE_DEFAULT_PORT_LOW,
        .port_high = OSOE_DEFAULT_PORT_HIGH,
    };

    /* One wire, toward the other node, and the ring that carries it: the same
     * connection a lead makes (§6.2: node 0 connects, node 1 accepts). */
    struct osoe_wire w;
    struct osoe_ring ring;
    osoe_wires_open(&w, &ring, &b, 0, NULL);
    osoe_ring_enable(&ring);

    unsigned char out[CHUNK], in[CHUNK];
    size_t sent = 0, got = 0, filled = SIZE_MAX;
    /*
     * A deadline, not a spin budget: an iteration count stands in for a clock
     * badly on a loaded machine, and failed one run in forty under load when
     * the step-2 version of this test used one.
     */
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(20000000000));

    /*
     * Both directions at once: this process sends while the peer receives, and
     * receives while the peer sends. One send in flight at a time, whose bytes
     * stay put until the wire reports them taken (wire.h).
     */
    while (sent < PAYLOAD || got < PAYLOAD) {
        bool moved = false;
        if (sent < PAYLOAD) {
            size_t want = PAYLOAD - sent < CHUNK ? PAYLOAD - sent : CHUNK;
            if (filled != sent) {
                for (size_t i = 0; i < want; i++)
                    out[i] = pattern(me, sent + i);
                filled = sent;
            }
            size_t n = osoe_wire_send(&w, out, want);
            sent += n;
            moved |= n > 0;
        }
        osoe_ring_reap(&ring);
        /* Empty the wire before the next reap, as the engine does (§3.3). */
        for (;;) {
            size_t n = osoe_wire_recv(&w, in, sizeof in);
            if (n == 0)
                break;
            for (size_t i = 0; i < n; i++)
                if (in[i] != pattern(peer, got + i)) {
                    fprintf(stderr, "node %u: byte %zu is 0x%02x, expected 0x%02x "
                                    "(from node %u)\n", me, got + i, in[i],
                            pattern(peer, got + i), peer);
                    CHECK_FATAL(false);
                }
            got += n;
            moved = true;
        }
        if (!moved) {
            /* Sleep on the ring, not a spin: the peer needs a core to send what
             * this node is waiting for (§3.11's rule against spinning). */
            osoe_ring_wait(&ring, UINT64_C(1000000));
            if (osoe_deadline_expired(&give_up)) {
                fprintf(stderr, "node %u stalled: %zu of %d sent, %zu received\n", me,
                        sent, PAYLOAD, got);
                CHECK_FATAL(false);
            }
        }
    }
    CHECK(sent == PAYLOAD);
    CHECK(got == PAYLOAD);

    /*
     * The close: end this side's stream, then read until the peer's ends, and
     * only then close the socket - closing first would make the kernel reset a
     * connection the peer is still sending on (close_model.py). No bytes may
     * arrive after the payload.
     */
    osoe_wire_shutdown(&w);
    struct osoe_deadline d = osoe_deadline_in(UINT64_C(10000000000));
    while (!osoe_wire_ended(&w) && !osoe_deadline_expired(&d)) {
        osoe_ring_reap(&ring);
        CHECK(osoe_wire_recv(&w, in, sizeof in) == 0);
        osoe_ring_wait(&ring, UINT64_C(1000000));
    }
    CHECK(osoe_wire_ended(&w));
    osoe_wire_close(&w);
    osoe_ring_close(&ring);
    return CHECK_DONE();
}
