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

/* How many sends the kernel or the injection cut short, and how many peeks
 * the injection cut short of what their provided buffer held. */
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

/* Take everything waiting on `rx`, checking every byte where it arrived against
 * where it falls in the stream. The wire must be emptied before the next reap,
 * as the engine empties every readable wire in its pass (§3.3, invariant 23).
 * A short receive is a peek that showed less than its provided buffer holds. */
static bool drain(struct osoe_wire *rx, size_t *got)
{
    bool moved = false;
    for (;;) {
        const unsigned char *in;
        size_t whole = rx->queued ? rx->ring->filled[rx->first] - rx->offset : 0;
        size_t n = osoe_wire_peek(rx, &in);
        if (n == 0)
            return moved;
        if (n < whole)
            short_recvs++;
        for (size_t i = 0; i < n; i++)
            if (in[i] != pattern(*got + i)) {
                fprintf(stderr, "byte %zu is 0x%02x, expected 0x%02x\n", *got + i, in[i],
                        pattern(*got + i));
                CHECK_FATAL(false);
            }
        osoe_wire_consume(rx, n);
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

/*
 * The zero-copy send (wire.h): `total` bytes from `tx` to `rx` through the two
 * regions a sender lends the kernel, sent alternately by reference, each
 * refilled only once the kernel owes nothing for it - the rule the engine
 * keeps for a connection's two buffers - and every byte checked where it lands.
 * Then the wait for the last notifications, which every lent region must see.
 */
static void stream_lent(struct osoe_ring *ring, struct osoe_wire *tx, struct osoe_wire *rx,
                        size_t total)
{
    static unsigned char region[OSOE_WIRE_LENT_REGIONS][CHUNK];
    size_t length[OSOE_WIRE_LENT_REGIONS] = { 0 }, placed = 0, sent = 0, got = 0, offset = 0;
    uint32_t k = 0;
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(20000000000));
    /* Until the sender has been told of every byte too: the last send's
     * completion is collected by offering its bytes once more (wire.h). */
    while (sent < total || got < total) {
        bool moved = false;
        if (length[k] == 0 && placed < total && osoe_wire_owed(tx, k) == 0) {
            length[k] = total - placed < CHUNK ? total - placed : CHUNK;
            for (size_t i = 0; i < length[k]; i++)
                region[k][i] = pattern(placed + i);
            placed += length[k];
            offset = 0;
        }
        if (length[k] != 0) {
            size_t n = osoe_wire_send_lent(tx, region[k] + offset, length[k] - offset, k);
            CHECK(n <= length[k] - offset);
            if (n > 0 && n < length[k] - offset)
                short_sends++;
            offset += n;
            sent += n;
            moved |= n > 0;
            if (offset == length[k]) {
                length[k] = 0;          /* sent whole: lent until it owes nothing */
                k ^= 1;
            }
        }
        osoe_ring_reap(ring);
        moved |= drain(rx, &got);
        if (!moved) {
            osoe_ring_wait(ring, UINT64_C(1000000));
            if (osoe_deadline_expired(&give_up)) {
                fprintf(stderr, "zero-copy stream stalled: %zu of %zu placed, %zu received, "
                                "%u and %u notifications owed\n", placed, total, got,
                        osoe_wire_owed(tx, 0), osoe_wire_owed(tx, 1));
                CHECK_FATAL(false);
            }
        }
    }
    while ((osoe_wire_owed(tx, 0) || osoe_wire_owed(tx, 1)) && !osoe_deadline_expired(&give_up)) {
        osoe_ring_reap(ring);
        if (osoe_wire_owed(tx, 0) || osoe_wire_owed(tx, 1))
            osoe_ring_wait(ring, UINT64_C(1000000));
    }
    CHECK(osoe_wire_owed(tx, 0) == 0 && osoe_wire_owed(tx, 1) == 0);
    CHECK(placed == total && sent == total && got == total);
}

/*
 * The peek's misuses (wire.h), one a run, each expected to end in its message
 * (CMakeLists.txt): a consume past what the last peek showed, a second consume
 * of one peek, and - in the ASan preset, which poisons a provided buffer once it
 * is the kernel's again (ring.c) - a read of a buffer after its consume. A
 * misuse that went unremarked would print the last line and fail the match.
 */
static int misuse(const char *what)
{
    struct osoe_bootstrap lo = { .job = (uint64_t)getpid(), .node = 0, .node_count = 2,
                                 .no_progress_ns = UINT64_C(10000000000) };
    struct osoe_bootstrap hi = lo;
    hi.node = 1;
    int fd[2];
    pair(fd);
    static struct osoe_wire wires[2];
    struct osoe_ring ring;
    osoe_wire_ring_open(&ring, wires, 2, &lo);
    osoe_wire_attach(&wires[0], &ring, 0, fd[0], 1, &lo);
    osoe_wire_attach(&wires[1], &ring, 1, fd[1], 0, &hi);
    osoe_ring_enable(&ring);
    unsigned char out[48];
    for (size_t i = 0; i < sizeof out; i++)
        out[i] = pattern(i);
    size_t sent = 0;
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(10000000000));
    while (wires[1].queued == 0 && !osoe_deadline_expired(&give_up)) {
        if (sent < sizeof out)
            sent += osoe_wire_send(&wires[0], out + sent, sizeof out - sent);
        osoe_ring_reap(&ring);
        if (wires[1].queued == 0)
            osoe_ring_wait(&ring, UINT64_C(1000000));
    }
    CHECK_FATAL(wires[1].queued > 0);
    const unsigned char *bytes;
    size_t n = osoe_wire_peek(&wires[1], &bytes);
    CHECK_FATAL(n > 0 && bytes != NULL);
    if (strcmp(what, "consume-past-peek") == 0) {
        osoe_wire_consume(&wires[1], n + 1);
    } else if (strcmp(what, "consume-twice") == 0) {
        osoe_wire_consume(&wires[1], n);
        osoe_wire_consume(&wires[1], 1);
    } else if (strcmp(what, "read-after-consume") == 0) {
        /* The whole buffer, so that the consume gives it back (no split here). */
        CHECK_FATAL(n == wires[1].ring->filled[wires[1].first] - wires[1].offset);
        osoe_wire_consume(&wires[1], n);
        volatile unsigned char after = bytes[0];
        (void)after;
    } else {
        fprintf(stderr, "no misuse named %s\n", what);
        return 2;
    }
    printf("the misuse %s went unremarked\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    bool zero_copy = argc > 1 && strcmp(argv[1], "zero-copy") == 0;
    if (argc > 1 && !zero_copy) {
        setenv("OSOE_WIRE_BYTES", "64", 1);
        setenv("OSOE_WIRE_SPLIT", "0", 1);
        return misuse(argv[1]);
    }
    if (zero_copy) {
        /* Every send of 64 bytes or more by reference (wire-settings.h). */
        setenv("OSOE_WIRE_ZERO_COPY", "on", 1);
        setenv("OSOE_WIRE_ZERO_COPY_BYTES", "64", 1);
    }
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
    if (zero_copy) {
        stream_lent(&ring, &wires[0], &wires[1], PAYLOAD);
        stream_lent(&ring, &wires[1], &wires[0], PAYLOAD);
        /* Sends by reference happened, and every one was given back - over
         * loopback, copied after all, which the notification says. */
        CHECK(ring.counts.zero_copy_sends > 0);
        CHECK(ring.counts.zero_copy_notifications > 0);
        CHECK(ring.counts.zero_copy_notifications <= ring.counts.zero_copy_sends);
        CHECK(ring.counts.zero_copy_copied <= ring.counts.zero_copy_notifications);
        fprintf(stderr, "zero copy: %llu sends by reference, %llu notifications, %llu copied\n",
                (unsigned long long)ring.counts.zero_copy_sends,
                (unsigned long long)ring.counts.zero_copy_notifications,
                (unsigned long long)ring.counts.zero_copy_copied);
    } else {
        stream(&ring, &wires[0], &wires[1], PAYLOAD);
        stream(&ring, &wires[1], &wires[0], PAYLOAD);
    }

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
        const unsigned char *bytes;
        CHECK(osoe_wire_peek(&wires[0], &bytes) == 0);              /* not at the sender */
        for (size_t n; (n = osoe_wire_peek(&wires[1], &bytes)) > 0; got += n) {
            CHECK_FATAL(n <= sizeof back - got);
            memcpy(back + got, bytes, n);
            osoe_wire_consume(&wires[1], n);
        }
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
        const unsigned char *bytes;
        CHECK(osoe_wire_peek(&wires[0], &bytes) == 0);
        CHECK(osoe_wire_peek(&wires[1], &bytes) == 0);
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
