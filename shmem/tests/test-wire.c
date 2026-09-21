/*
 * The node-pair wire (§3.1, §7.1 step 2): a byte stream, not a message queue.
 *
 * Both ends live in this one process - a wire handle is one side of a pair, and
 * nothing stops one process holding both - so this is tier 1 and needs no
 * launcher. What it checks is the property §3.3's receive path depends on and
 * §8.2 warns loopback will never exercise: bytes arrive exactly once, in order,
 * across short writes, partial reads and wrap-around.
 *
 * The capacity is set far below the payload and OSOE_WIRE_SPLIT is turned up,
 * so short transfers are the rule here rather than a rare interleaving. The
 * test then asserts that they actually happened. Without that last check a
 * wire that never truncated would pass every assertion above it while proving
 * nothing about the path this exists to exercise.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "wire.h"

/* wire.c's error path, replaced here so a failure is a test failure. */
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

enum { PAYLOAD = 4096, CHUNK = 300 };

static unsigned char pattern(size_t i)
{
    return (unsigned char)((i * 31 + 7) & 0xff);
}

/* How many transfers were short, and whether the ring ever wrapped. */
static int short_sends, short_recvs, zero_sends;

/*
 * Stream `total` bytes one way while draining the other, and check every byte.
 * Single-threaded, so send and receive alternate: a loop that only sent would
 * fill the ring and stop, which is precisely what a short write means.
 */
static void stream(struct osoe_wire *tx, struct osoe_wire *rx, size_t total)
{
    unsigned char out[CHUNK], in[CHUNK];
    size_t sent = 0, got = 0;
    int idle = 0;

    while (got < total) {
        size_t sent_was = sent, got_was = got;
        if (sent < total) {
            size_t want = total - sent;
            if (want > CHUNK)
                want = CHUNK;
            for (size_t i = 0; i < want; i++)
                out[i] = pattern(sent + i);
            size_t n = osoe_wire_send(tx, out, want);
            CHECK(n <= want);
            if (n == 0)
                zero_sends++;
            else if (n < want)
                short_sends++;
            sent += n;
        }

        /* The wake value BEFORE draining, as the protocol requires (§4.6).
         * Nothing sleeps here - one thread drives both ends - but reading it in
         * the right order is what the real loop does, so the test uses the same
         * shape rather than a convenient one. */
        uint32_t seq = osoe_wire_seq(rx);
        (void)seq;
        size_t want = total - got;
        if (want > CHUNK)
            want = CHUNK;
        size_t n = osoe_wire_recv(rx, in, want);
        CHECK(n <= want);
        if (n > 0 && n < want && sent > got + n)
            short_recvs++;
        for (size_t i = 0; i < n; i++)
            if (in[i] != pattern(got + i)) {
                fprintf(stderr, "byte %zu is 0x%02x, expected 0x%02x\n", got + i, in[i],
                        pattern(got + i));
                CHECK(false);
                return;
            }
        got += n;

        /*
         * Something must move on every pass: one side always can, since a full
         * ring means there is something to receive and an empty one means there
         * is room to send. A pass where neither moved is a stalled stream, and
         * it must fail here with what it was doing rather than run to the ctest
         * timeout - a mis-paired wire otherwise costs a minute and says only
         * "Timeout". The allowance is for an injected zero-length transfer,
         * which makes no progress on purpose.
         */
        idle = (sent == sent_was && got == got_was) ? idle + 1 : 0;
        if (idle > 10000) {
            fprintf(stderr, "stream stalled: %zu of %zu sent, %zu received\n", sent,
                    total, got);
            CHECK(false);
            return;
        }
    }
    CHECK(sent == total && got == total);
}

int main(void)
{
    char job[32];
    snprintf(job, sizeof job, "%ld", (long)getpid());
    setenv("OSOE_WIRE_BYTES", "64", 1);     /* far below CHUNK: forces short writes */
    setenv("OSOE_WIRE_SPLIT", "5000", 1);   /* and truncate half the rest besides */
    /*
     * A fixed seed. Without it the seed comes from the job id, which is this
     * pid, so every run would inject a different pattern of truncations - and
     * the checks at the end, which assert that short and zero-length transfers
     * actually happened, would pass or fail by luck. §3.14 asks for exactly
     * this: same seed, same decisions, and a failing seed committed as a
     * permanent regression case.
     */
    setenv("OSOE_WIRE_SEED", "20260919", 1);

    struct osoe_bootstrap lo = { .job = (uint64_t)getpid(), .step = 0, .node = 0,
                            .no_progress_ns = UINT64_C(10000000000) };
    struct osoe_bootstrap hi = lo;
    hi.node = 1;

    struct osoe_wire a, b;
    osoe_wire_open(&a, &lo, 1, 0);          /* node 0: creates */
    osoe_wire_open(&b, &hi, 0, 0);          /* node 1: opens */
    CHECK(osoe_wire_capacity(&a) == 64);
    CHECK(osoe_wire_capacity(&b) == 64);

    /* Far more than the ring holds, so it wraps many times over. */
    stream(&a, &b, PAYLOAD);
    stream(&b, &a, PAYLOAD);

    /*
     * The directions are separate: what node 0 wrote must not come back to it.
     * A wire that paired tx and rx the same way on both sides would pass every
     * check above - each side would read its own bytes back in order - so this
     * is the one that catches it.
     */
    unsigned char probe[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, back[8];
    size_t n = osoe_wire_send(&a, probe, sizeof probe);
    CHECK(n > 0);
    CHECK(osoe_wire_recv(&a, back, sizeof back) == 0);   /* not readable at the sender */
    size_t drained = 0;
    while (drained < n)
        drained += osoe_wire_recv(&b, back + drained, n - drained);
    CHECK(memcmp(probe, back, n) == 0);

    /* The path this test exists for must actually have been taken. */
    CHECK(short_sends > 0);
    CHECK(zero_sends > 0);
    CHECK(short_recvs > 0);
    if (short_sends == 0 || zero_sends == 0 || short_recvs == 0)
        fprintf(stderr, "short sends %d, zero sends %d, short receives %d: the "
                        "short-transfer path was not exercised\n",
                short_sends, zero_sends, short_recvs);

    /* One process holds both ends, so both must announce before either waits
     * (wire.h): a close blocks until the other side has announced, and the
     * other side here is the next statement. */
    osoe_wire_shutdown(&a);
    osoe_wire_shutdown(&b);
    osoe_wire_close(&b);
    osoe_wire_close(&a);
    return CHECK_DONE();
}
