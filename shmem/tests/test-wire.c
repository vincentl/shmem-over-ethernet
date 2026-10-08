/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The node-pair wire (§3.1, §3.3; phase 1 step 3): a byte stream over a real
 * socket, not a message queue.
 *
 * Both ends live in this one process - the two ends of a TCP loopback
 * connection, carried by one ring, which one thread driving both may do - so
 * this is tier 1 and needs no launcher. It checks what §3.3's receive path
 * depends on and §8.2 warns loopback will never exercise by itself: bytes
 * arrive exactly once, in order, across short sends, partial reads, and receive
 * buffers that end in the middle of whatever was sent.
 *
 * The receive buffers are 64 bytes and there are four of them, and
 * OSOE_WIRE_SPLIT is turned up, so the kernel splits every transfer across
 * buffers, the ring runs dry and re-arms, and the injection truncates half of
 * what is left. The test then asserts that each of those happened: a wire that
 * never split would pass every other assertion while proving nothing about the
 * path it exists for.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "ring.h"
#include "wire.h"
#include "wire-settings.h"

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

enum { PAYLOAD = 1 << 16, CHUNK = 300 };

static unsigned char pattern(size_t i)
{
    return (unsigned char)((i * 31 + 7) & 0xff);
}

/* How many sends the kernel or the injection cut short, and how many receives
 * returned less than was waiting. */
static int short_sends, short_recvs;

/* Two connected TCP sockets on loopback. */
static void pair(int fd[2])
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t length = sizeof a;
    CHECK_FATAL(listener >= 0 && bind(listener, (struct sockaddr *)&a, sizeof a) == 0 &&
                listen(listener, 1) == 0 &&
                getsockname(listener, (struct sockaddr *)&a, &length) == 0);
    fd[0] = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_FATAL(fd[0] >= 0 && connect(fd[0], (struct sockaddr *)&a, sizeof a) == 0);
    fd[1] = accept(listener, NULL, NULL);
    CHECK_FATAL(fd[1] >= 0);
    close(listener);
}

/*
 * The limit on the kernel's unsent bytes (v0.2 step 4, wire-settings.h): the
 * link's 128 µs, at least 16 KiB and at most 1 GiB, or as set; the option on a
 * real TCP socket, which a limit of 0 leaves alone; and lifted again, as an
 * aborting node lifts it.
 */
static void unsent_limit(struct osoe_wire *w, const struct osoe_bootstrap *b)
{
    struct osoe_wire_settings s;
    osoe_wire_settings_read(&s, b, false);
    CHECK(!s.unsent_set);
    CHECK(osoe_wire_unsent_bytes(&s, 0) == 16384);                 /* no link: the floor */
    CHECK(osoe_wire_unsent_bytes(&s, 116000000) == 16384);         /* the Pi: 14,848, the floor */
    CHECK(osoe_wire_unsent_bytes(&s, 1000000000) == 128000);
    CHECK(osoe_wire_unsent_bytes(&s, 4440000000u) == 568320);
    CHECK(osoe_wire_unsent_bytes(&s, UINT64_C(10000000000000)) == UINT32_C(1) << 30);
    setenv("OSOE_WIRE_UNSENT_BYTES", "65536", 1);
    osoe_wire_settings_read(&s, b, false);
    CHECK(s.unsent_set && osoe_wire_unsent_bytes(&s, 1000000000) == 65536);
    setenv("OSOE_WIRE_UNSENT_BYTES", "0", 1);
    osoe_wire_settings_read(&s, b, false);
    CHECK(s.unsent_set && osoe_wire_unsent_bytes(&s, 1000000000) == 0);
    unsetenv("OSOE_WIRE_UNSENT_BYTES");

    CHECK(osoe_wire_limit_unsent(w, 65536));
    int v = 0;
    socklen_t length = sizeof v;
    CHECK(getsockopt(w->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, &length) == 0 && v == 65536);
    CHECK(osoe_wire_limit_unsent(w, 0));
    CHECK(getsockopt(w->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, &length) == 0 && v == 65536);
    osoe_wire_lift_unsent(w);
    CHECK(getsockopt(w->fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &v, &length) == 0 && v == INT_MAX);
    CHECK(osoe_wire_limit_unsent(w, 16384));
    struct osoe_wire closed = { .fd = -1 };
    CHECK(!osoe_wire_limit_unsent(&closed, 16384));   /* not open: not counted */
}

/* Take everything waiting on `rx`, checking every byte against where it falls
 * in the stream. The wire must be emptied before the next reap, as the engine
 * empties every readable wire in its pass (§3.3, invariant 23). */
static bool drain(struct osoe_wire *rx, size_t *got)
{
    unsigned char in[CHUNK];
    bool moved = false;
    for (;;) {
        size_t n = osoe_wire_recv(rx, in, sizeof in);
        if (n == 0)
            return moved;
        if (n < sizeof in && rx->queued > 0)
            short_recvs++;
        for (size_t i = 0; i < n; i++)
            if (in[i] != pattern(*got + i)) {
                fprintf(stderr, "byte %zu is 0x%02x, expected 0x%02x\n", *got + i, in[i],
                        pattern(*got + i));
                CHECK_FATAL(false);
            }
        *got += n;
        moved = true;
    }
}

/*
 * Stream `total` bytes from `tx` to `rx`, checking every byte. One send is in
 * flight at a time and its bytes stay where they are until the wire reports
 * them taken (wire.h), so `out` is refilled only once the stream has moved on.
 */
static void stream(struct osoe_ring *ring, struct osoe_wire *tx, struct osoe_wire *rx,
                   size_t total)
{
    unsigned char out[CHUNK];
    size_t sent = 0, got = 0, filled = SIZE_MAX;
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(20000000000));
    while (sent < total || got < total) {
        bool moved = false;
        if (sent < total) {
            size_t want = total - sent < CHUNK ? total - sent : CHUNK;
            if (filled != sent) {
                for (size_t i = 0; i < want; i++)
                    out[i] = pattern(sent + i);
                filled = sent;
            }
            size_t n = osoe_wire_send(tx, out, want);
            CHECK(n <= want);
            if (n > 0 && n < want)
                short_sends++;
            sent += n;
            moved |= n > 0;
        }
        osoe_ring_reap(ring);
        moved |= drain(rx, &got);
        if (!moved) {
            osoe_ring_wait(ring, UINT64_C(1000000));
            if (osoe_deadline_expired(&give_up)) {
                fprintf(stderr, "stream stalled: %zu of %zu sent, %zu received\n", sent,
                        total, got);
                CHECK_FATAL(false);
            }
        }
    }
    CHECK(sent == total && got == total);
}

int main(void)
{
    setenv("OSOE_WIRE_BYTES", "64", 1);     /* every transfer spans buffers */
    setenv("OSOE_WIRE_BUFFERS", "4", 1);    /* and the ring runs dry: -ENOBUFS, re-arm */
    setenv("OSOE_WIRE_SPLIT", "5000", 1);   /* and half the rest is truncated besides */
    /*
     * A fixed seed. Without it the seed comes from the job id, which is this
     * pid, so every run would inject a different pattern of truncations - and
     * the checks at the end, which assert that short transfers actually
     * happened, would pass or fail by luck. §3.14 asks for exactly this: same
     * seed, same decisions, and a failing seed committed as a permanent
     * regression case.
     */
    setenv("OSOE_WIRE_SEED", "20260919", 1);

    struct osoe_bootstrap lo = { .job = (uint64_t)getpid(), .node = 0, .node_count = 2,
                                 .no_progress_ns = UINT64_C(10000000000) };
    struct osoe_bootstrap hi = lo;
    hi.node = 1;

    int fd[2];
    pair(fd);
    static struct osoe_wire wires[2];
    struct osoe_ring ring;
    osoe_wire_ring_open(&ring, wires, 2, &lo);
    osoe_wire_attach(&wires[0], &ring, 0, fd[0], 1, &lo);  /* node 0's end */
    osoe_wire_attach(&wires[1], &ring, 1, fd[1], 0, &hi);  /* node 1's end */
    osoe_ring_enable(&ring);

    unsent_limit(&wires[1], &hi);
    stream(&ring, &wires[0], &wires[1], PAYLOAD);
    stream(&ring, &wires[1], &wires[0], PAYLOAD);

    /*
     * The directions are separate: what node 0 wrote must not come back to it.
     * Two wires that shared a socket end would pass every check above - each
     * would read its own bytes back in order - so this is the one that
     * catches it.
     */
    unsigned char probe[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, back[8];
    size_t sent = 0, got = 0;
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(10000000000));
    while ((sent < sizeof probe || got < sizeof probe) && !osoe_deadline_expired(&give_up)) {
        if (sent < sizeof probe)
            sent += osoe_wire_send(&wires[0], probe + sent, sizeof probe - sent);
        osoe_ring_reap(&ring);
        CHECK(osoe_wire_recv(&wires[0], back, sizeof back) == 0);   /* not at the sender */
        got += osoe_wire_recv(&wires[1], back + got, sizeof back - got);
        osoe_ring_wait(&ring, UINT64_C(1000000));
    }
    CHECK(got == sizeof probe && memcmp(probe, back, sizeof probe) == 0);

    /* The paths this test exists for must actually have been taken. */
    CHECK(short_sends > 0);
    CHECK(short_recvs > 0);
    CHECK(ring.counts.enobufs > 0);
    CHECK(ring.counts.rearms >= ring.counts.enobufs);
    if (short_sends == 0 || short_recvs == 0 || ring.counts.enobufs == 0)
        fprintf(stderr, "short sends %d, short receives %d, -ENOBUFS %llu: the split "
                        "paths were not exercised\n", short_sends, short_recvs,
                (unsigned long long)ring.counts.enobufs);

    /*
     * The close: each end sends its end of stream, and each sees the other's
     * once every byte before it has been taken (wire.h).
     */
    osoe_wire_shutdown(&wires[0]);
    osoe_wire_shutdown(&wires[1]);
    give_up = osoe_deadline_in(UINT64_C(10000000000));
    while (!(osoe_wire_ended(&wires[0]) && osoe_wire_ended(&wires[1])) &&
           !osoe_deadline_expired(&give_up)) {
        osoe_ring_reap(&ring);
        CHECK(osoe_wire_recv(&wires[0], back, sizeof back) == 0);
        CHECK(osoe_wire_recv(&wires[1], back, sizeof back) == 0);
        osoe_ring_wait(&ring, UINT64_C(1000000));
    }
    CHECK(osoe_wire_ended(&wires[0]) && osoe_wire_ended(&wires[1]));
    osoe_wire_close(&wires[0]);
    osoe_wire_close(&wires[1]);
    osoe_ring_close(&ring);
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
