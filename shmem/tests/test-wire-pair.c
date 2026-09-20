/*
 * The node-pair wire between two real processes (§3.1, §7.1 step 2).
 *
 * shmem.wire drives both ends from one thread, which settles the stream
 * arithmetic - wrap, short writes, partial reads - but cannot see a
 * publication-ordering defect, because nothing races. This one runs the two
 * ends in the two processes they will really be in, streaming both ways at
 * once, so the writer is filling the ring while the reader is draining it.
 *
 * That is what catches a reader publishing `consumed` before the bytes are out
 * of the ring, or a writer publishing `produced` before the bytes are in it:
 * both are invisible without a concurrent peer, and both corrupt the stream
 * under load. Under the tsan preset the same race is reported directly, with
 * the caveat §8.2 states - ThreadSanitizer instruments one process, so it sees
 * this PE's own threads and not the peer's, and the cross-process race is
 * caught here by the payload check rather than by tsan.
 *
 * It does not call shmem_init: a job of two logical nodes is still refused
 * until the backend lands (§7.1 step 2), and the wire needs only the bootstrap
 * environment osoe-launch already sets.
 */
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
#include "wire.h"

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
        .timeout_ns = UINT64_C(30000000000),
    };

    struct osoe_wire w;
    osoe_wire_open(&w, &b, peer, 0);

    unsigned char out[CHUNK], in[CHUNK];
    size_t sent = 0, got = 0;
    bool stalled = false;
    /*
     * A deadline, not a spin budget. This was `idle > 100000` iterations, which
     * looks like a timeout and is not one: with this node's own receive already
     * complete the loop below skips the recv block entirely - and with it the
     * futex wait - so it spins on send alone at roughly ten million iterations
     * a second. A hundred thousand of them is about ten milliseconds, and on a
     * loaded machine the peer simply has not been scheduled in that time. It
     * failed about one run in forty under load and never otherwise, which is
     * exactly the shape of a spin count standing in for a clock.
     */
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(20000000000));

    /*
     * Both directions at once: this process fills its ring while the peer
     * drains it, and drains the peer's while the peer fills it. A one-way
     * version would leave one side's publication order untested.
     */
    while (sent < PAYLOAD || got < PAYLOAD) {
        size_t sent_was = sent, got_was = got;

        if (sent < PAYLOAD) {
            size_t want = PAYLOAD - sent;
            if (want > CHUNK)
                want = CHUNK;
            for (size_t i = 0; i < want; i++)
                out[i] = pattern(me, sent + i);
            sent += osoe_wire_send(&w, out, want);
        }

        if (got < PAYLOAD) {
            /* The wake value before draining (§4.6), then sleep on it if the
             * peer has sent nothing - with a deadline, so a lost wake costs
             * latency and not the job (§3.10). */
            uint32_t seq = osoe_wire_seq(&w);
            size_t want = PAYLOAD - got;
            if (want > CHUNK)
                want = CHUNK;
            size_t n = osoe_wire_recv(&w, in, want);
            for (size_t i = 0; i < n; i++)
                if (in[i] != pattern(peer, got + i)) {
                    fprintf(stderr, "node %u: byte %zu is 0x%02x, expected 0x%02x "
                                    "(from node %u)\n", me, got + i, in[i],
                            pattern(peer, got + i), peer);
                    CHECK(false);
                    osoe_wire_close(&w);
                    return CHECK_DONE();
                }
            got += n;
            if (n == 0 && sent == sent_was) {
                struct osoe_deadline d = osoe_deadline_in(OSOE_SWEEP_NS);
                osoe_wire_wait(&w, seq, &d);
            }
        }

        if (sent == sent_was && got == got_was) {
            /*
             * Yield rather than spin. The peer needs a core to drain the ring
             * this node is waiting on, and on a machine with more runnable
             * threads than cores, spinning here is what keeps it from getting
             * one - the test was creating the stall it then reported.
             */
            struct timespec pause = { 0, 100000 };      /* 100 us */
            nanosleep(&pause, NULL);
            if (osoe_deadline_expired(&give_up)) {
                fprintf(stderr, "node %u stalled: %zu of %d sent, %zu received\n", me,
                        sent, PAYLOAD, got);
                CHECK(false);
                stalled = true;
                break;
            }
        }
    }

    CHECK(sent == PAYLOAD);
    CHECK(got == PAYLOAD);

    /*
     * **Stop here if the exchange failed, rather than carrying on.** CHECK only
     * counts; it does not return. So a node that gave up above used to fall
     * through into the acknowledgement below and put 0xA5 into a ring the peer
     * was still reading as payload - which the peer duly reported as a wrong
     * byte, at the exact index this node had stopped sending at. That made a
     * one-sided timeout look like a stream corruption on the other node, and it
     * is the reason this took three reproductions to read correctly. Closing
     * rather than returning bare also lets the peer's own close finish at once
     * instead of waiting out its timeout.
     */
    if (stalled || CHECK_DONE()) {
        osoe_wire_close(&w);
        return CHECK_DONE();
    }

    /*
     * Both sides must finish before either unmaps: the creator removes the
     * name on close, and a peer still mapped would keep streaming into a
     * segment nobody can open again. One more exchange is enough, since each
     * side only gets here having received everything.
     */
    unsigned char bye = 0xA5, ack = 0;
    while (osoe_wire_send(&w, &bye, 1) == 0)
        ;
    struct osoe_deadline d = osoe_deadline_in(UINT64_C(10000000000));
    while (osoe_wire_recv(&w, &ack, 1) == 0) {
        uint32_t seq = osoe_wire_seq(&w);
        if (osoe_wire_recv(&w, &ack, 1) == 1)
            break;
        if (!osoe_wire_wait(&w, seq, &d) && osoe_deadline_expired(&d)) {
            fprintf(stderr, "node %u: peer never acknowledged\n", me);
            CHECK(false);
            break;
        }
    }
    CHECK(ack == 0xA5);

    osoe_wire_close(&w);
    return CHECK_DONE();
}
