/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The library against its floor (§1, §8.4; phase 1 step 3 slice 2): the pins §1
 * asks for, measured inside one job of two logical nodes, one PE on each.
 *
 *   test-floor [MEBIBYTES [ROUNDS]]
 *
 * §1 states the library's targets against a local floor - an 8-byte round trip
 * against a trivial io_uring TCP echo on the same link, and a stream against a
 * stream - and §8.4 found which comparisons hold still enough to pin: the
 * fastest round trip, which is the length of the path, measured side by side in
 * pairs, the ratio taken within each pair and its median over the pairs. So each
 * of seven pairs here times the floor - 8-byte round trips and a stream over
 * io_uring on a raw TCP connection between the job's two processes, PE 0
 * dialling the address node 1's host-list entry names, so that it crosses the
 * link the library crosses: loopback at tier 2, a shaped veth link at tier 3 -
 * and the library's get, put and quiet, fetch-add and stream, so that whatever
 * load the machine carries falls on both. The four round trips are interleaved
 * within every round, in an order that rotates, since trips timed one kind after
 * another met different load: a CPU asleep for one kind's two thousand trips and
 * awake for the next moved a pair's ratio anywhere from 0.5x to 12x.
 *
 * Pinned, each the median over the pairs, from twelve runs - four idle, four
 * beside a `ctest -j4`, four under 16 busy loops on four CPUs - and nine with
 * step 2's poll put back into quiet's and a writer's waits (§8.4):
 *   - a get's, a fetch-add's, and a put and quiet's fastest trip over the
 *     floor's, at most FASTEST: the library's path against the floor's.
 *     Measured 1.4-2.2x. The poll measured 2.1-2.7x idle, which this cannot
 *     tell from 1.9-2.2x: its 20 us sleep is about an idle round trip;
 *   - a put and quiet's, and a fetch-add's, median over a get's, at most
 *     INTERNAL. All three cost the same four wakes, so they hold together
 *     under any load - measured 0.98-1.00x - and this is the pin a wait that
 *     polls fails: the poll measured 1.8-2.1x idle and up to 14x loaded;
 *   - the library's stream over the floor's, at least STREAM, at the default
 *     connection buffer. There a 64 KiB put fills half the buffer and every
 *     other one waits for a turnover (§8.4), so this is the pin a writer's wait
 *     that polls fails: measured 0.21-0.43x, and 0.037-0.055x with the poll.
 *     At 1 MiB a writer seldom waits, and the poll measured within the noise.
 * Reported and not pinned: each median trip over the floor's median. §8.4 found
 * those move with the load's mix, not with the library.
 *
 * Pins are checked in an optimized, uninstrumented build only, as echo-uring's
 * are: under a sanitizer a program measures its own instrumentation. There the
 * stream is 64 MiB unless an argument says otherwise, since only the paths are
 * being run. Every build ends, as every tier-2 test does, by requiring each
 * invariant counter to be zero - invariant.missed_wake among them, which a wait
 * that lost its wake and found its value on a sweep counts (§3.11).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "base/hostlist.h"
#include "check.h"
#include "invariants.h"

#if defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#define PINNED 1
enum { DEFAULT_MEBIBYTES = 1024 };
#else
#define PINNED 0
enum { DEFAULT_MEBIBYTES = 64 };
#endif

enum { PAIRS = 7, WARMUP = 200, PIECE = 64 << 10, AREA = 1 << 20 };

#define FASTEST 3.0
#define INTERNAL 1.5
#define STREAM 0.10

/* The four round trips each pair times. */
enum kind { FLOOR_TRIP, GET, PUT_QUIET, FETCH_ADD, KINDS };
static const char *const kind_name[KINDS] = { "floor", "get", "put_quiet", "fetch_add" };

struct trip {
    uint64_t fastest, median;
};

static struct io_uring ring;
static int link_fd = -1;

static _Noreturn void fail(const char *what, int error)
{
    fprintf(stderr, "test-floor: %s: %s\n", what, strerror(error));
    exit(2);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static struct trip summarize(uint64_t *t, size_t n)
{
    qsort(t, n, sizeof *t, compare_u64);
    return (struct trip){ t[0], t[n / 2] };
}

/* The median over the pairs, and the range, of one ratio. */
struct spread {
    double median, low, high;
};

static struct spread spread_of(const double *ratio)
{
    double v[PAIRS];
    memcpy(v, ratio, sizeof v);
    qsort(v, PAIRS, sizeof *v, compare_double);
    return (struct spread){ v[PAIRS / 2], v[0], v[PAIRS - 1] };
}

/* One send or receive of exactly `len` bytes on the ring, one submission and
 * wait at a time, as the floor's trivial echo does. */
static void ring_transfer(bool send, void *bytes, size_t len)
{
    for (size_t done = 0; done < len; ) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (send)
            io_uring_prep_send(sqe, link_fd, (char *)bytes + done, len - done, MSG_NOSIGNAL);
        else
            io_uring_prep_recv(sqe, link_fd, (char *)bytes + done, len - done, 0);
        int r = io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe *cqe;
        if (r < 0 || (r = io_uring_wait_cqe(&ring, &cqe)) != 0)
            fail("io_uring", -r);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        if (res <= 0)
            fail(send ? "send" : "recv", res < 0 ? -res : EPIPE);
        done += (size_t)res;
    }
}

/* The floor's connection: PE 1 listens, PE 0 dials node 1's host-list address. */
static void connect_floor(int me, int *port)
{
    if (me == 1) {
        int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
        socklen_t length = sizeof a;
        if (listener < 0 || bind(listener, (struct sockaddr *)&a, sizeof a) != 0 ||
            listen(listener, 1) != 0 || getsockname(listener, (struct sockaddr *)&a, &length) != 0)
            fail("listen", errno);
        *port = ntohs(a.sin_port);
        shmem_barrier_all();
        link_fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (link_fd < 0)
            fail("accept", errno);
        close(listener);
    } else {
        shmem_barrier_all();
        int there = 0;
        shmem_getmem(&there, port, sizeof there, 1);
        /* The step's hosts under the bootstrap's rule (§6.1): osoe-launch's
         * names when OSOE_PROCID is set, srun's otherwise. */
        const char *list = getenv(getenv("OSOE_PROCID") ? "OSOE_STEP_NODELIST"
                                                        : "SLURM_STEP_NODELIST");
        char host[256];
        if (!list || !osoe_hostlist_entry(list, 1, host, sizeof host))
            fail("the step's node list names no node 1", EINVAL);
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found;
        if (getaddrinfo(host, NULL, &hints, &found) != 0)
            fail("getaddrinfo", EHOSTUNREACH);
        struct sockaddr_in a = *(struct sockaddr_in *)found->ai_addr;
        freeaddrinfo(found);
        a.sin_port = htons((uint16_t)there);
        link_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (link_fd < 0 || connect(link_fd, (struct sockaddr *)&a, sizeof a) != 0)
            fail("connect", errno);
    }
    int one = 1;
    setsockopt(link_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int r = io_uring_queue_init(8, &ring, 0);
    if (r != 0)
        fail("io_uring_queue_init", -r);
}

/* One round trip of `kind`: the floor's through the echo on its connection, the
 * library's against PE 1's word. */
static uint64_t one_trip(enum kind kind, uint64_t *word)
{
    uint64_t v = 0, a = now_ns();
    switch (kind) {
    case FLOOR_TRIP:
        ring_transfer(true, &v, 8);
        ring_transfer(false, &v, 8);
        break;
    case GET:
        shmem_getmem(&v, word, 8, 1);
        break;
    case PUT_QUIET:
        shmem_putmem(word, &v, 8, 1);
        shmem_quiet();
        break;
    default:
        v = shmem_uint64_atomic_fetch_add(word, 1, 1);
        break;
    }
    return now_ns() - a;
}

/* PE 0: `rounds` rounds of the four trips, interleaved and rotating, then each
 * kind's fastest and median; PE 1 echoes the floor's trips meanwhile, and its
 * agent serves the library's. */
static void round_trips(int me, uint64_t *word, uint64_t *t[KINDS], int rounds,
                        struct trip out[KINDS])
{
    uint64_t echo = 0;
    for (int r = 0; r < WARMUP + rounds; r++) {
        if (me == 1) {
            ring_transfer(false, &echo, 8);
            ring_transfer(true, &echo, 8);
            continue;
        }
        for (int i = 0; i < KINDS; i++) {
            enum kind k = (enum kind)((r + i) % KINDS);
            uint64_t took = one_trip(k, word);
            if (r >= WARMUP)
                t[k][r - WARMUP] = took;
        }
    }
    for (int k = 0; me == 0 && k < KINDS; k++)
        out[k] = summarize(t[k], (size_t)rounds);
}

/* PE 0: the floor's stream in bytes per second, until PE 1 says it has them all. */
static double floor_stream(int me, unsigned char *piece, uint64_t bytes)
{
    uint64_t done = 0;
    if (me == 1) {
        for (uint64_t got = 0; got < bytes; got += PIECE)
            ring_transfer(false, piece, PIECE);
        ring_transfer(true, &done, 8);
        return 0;
    }
    uint64_t a = now_ns();
    for (uint64_t sent = 0; sent < bytes; sent += PIECE)
        ring_transfer(true, piece, PIECE);
    ring_transfer(false, &done, 8);
    return (double)bytes / ((double)(now_ns() - a) / 1e9);
}

/* PE 0: the library's stream of 64 KiB puts into PE 1's area, then one quiet. */
static double library_stream(unsigned char *area, const unsigned char *piece, uint64_t bytes)
{
    uint64_t a = now_ns();
    for (uint64_t sent = 0; sent < bytes; sent += PIECE)
        shmem_putmem(area + sent % AREA, piece, PIECE, 1);
    shmem_quiet();
    return (double)bytes / ((double)(now_ns() - a) / 1e9);
}

static bool report(struct trip trips[PAIRS][KINDS], const double streams[2][PAIRS])
{
    double fastest[KINDS][PAIRS], median[KINDS][PAIRS], internal[KINDS][PAIRS], stream[PAIRS];
    for (int p = 0; p < PAIRS; p++) {
        printf("pair %d fastest_us", p);
        for (int k = 0; k < KINDS; k++)
            printf(" %s=%.1f", kind_name[k], (double)trips[p][k].fastest / 1e3);
        printf(" median_us");
        for (int k = 0; k < KINDS; k++)
            printf(" %s=%.1f", kind_name[k], (double)trips[p][k].median / 1e3);
        printf(" stream_mib_per_s floor=%.0f library=%.0f\n", streams[0][p] / (1 << 20),
               streams[1][p] / (1 << 20));
        for (int k = GET; k < KINDS; k++) {
            fastest[k][p] = (double)trips[p][k].fastest / (double)trips[p][FLOOR_TRIP].fastest;
            median[k][p] = (double)trips[p][k].median / (double)trips[p][FLOOR_TRIP].median;
            internal[k][p] = (double)trips[p][k].median / (double)trips[p][GET].median;
        }
        stream[p] = streams[1][p] / streams[0][p];
    }

    bool ok = true;
    printf("ratio, the median of %d pairs (range):\n", PAIRS);
    for (int k = GET; k < KINDS; k++) {
        struct spread s = spread_of(fastest[k]);
        printf("  fastest %s/floor=%.3f (%.2f-%.2f)\n", kind_name[k], s.median, s.low, s.high);
        if (PINNED && s.median > FASTEST) {
            printf("FAIL: a %s's fastest trip is %.2fx the floor's, pinned at most %.1fx\n",
                   kind_name[k], s.median, FASTEST);
            ok = false;
        }
    }
    for (int k = PUT_QUIET; k < KINDS; k++) {
        struct spread s = spread_of(internal[k]);
        printf("  median %s/get=%.3f (%.2f-%.2f)\n", kind_name[k], s.median, s.low, s.high);
        if (PINNED && s.median > INTERNAL) {
            printf("FAIL: a %s costs %.2fx a get, pinned at most %.1fx: a wait that polls, "
                   "or a wake too many\n", kind_name[k], s.median, INTERNAL);
            ok = false;
        }
    }
    struct spread s = spread_of(stream);
    printf("  stream library/floor=%.3f (%.3f-%.3f)\n", s.median, s.low, s.high);
    if (PINNED && s.median < STREAM) {
        printf("FAIL: the stream runs at %.3fx the floor's, pinned at least %.2fx\n", s.median,
               STREAM);
        ok = false;
    }
    for (int k = GET; k < KINDS; k++) {
        s = spread_of(median[k]);
        printf("  reported: median %s/floor=%.3f (%.2f-%.2f)\n", kind_name[k], s.median, s.low,
               s.high);
    }
    if (!PINNED)
        printf("pins not checked: an instrumented or unoptimized build measures itself\n");
    return ok;
}

int main(int argc, char **argv)
{
    uint64_t mebibytes = argc > 1 ? strtoull(argv[1], NULL, 10) : DEFAULT_MEBIBYTES;
    int rounds = argc > 2 ? atoi(argv[2]) : 2000;
    if (mebibytes < 1 || rounds < 10) {
        fprintf(stderr, "usage: test-floor [MEBIBYTES [ROUNDS]], at least 1 and 10\n");
        exit(2);
    }
    uint64_t bytes = mebibytes << 20;

    shmem_init();
    int me = shmem_my_pe();
    if (shmem_n_pes() != 2) {
        fprintf(stderr, "test-floor: needs two PEs, one on each of two logical nodes\n");
        exit(2);                        /* the launcher ends the other */
    }
    int *port = shmem_malloc(sizeof *port);
    uint64_t *word = shmem_calloc(1, sizeof *word);
    unsigned char *area = shmem_malloc(AREA);
    unsigned char *piece = malloc(PIECE);
    uint64_t *t[KINDS];
    bool allocated = port && word && area && piece;
    for (int k = 0; k < KINDS; k++)
        allocated = (t[k] = malloc((size_t)rounds * sizeof *t[k])) != NULL && allocated;
    if (!allocated)
        fail("allocation", ENOMEM);
    memset(piece, 7, PIECE);
    connect_floor(me, port);

    struct trip trips[PAIRS][KINDS];
    double streams[2][PAIRS];
    for (int p = 0; p < PAIRS; p++) {
        shmem_barrier_all();
        round_trips(me, word, t, rounds, trips[p]);
        streams[0][p] = floor_stream(me, piece, bytes);
        shmem_barrier_all();
        if (me == 0)
            streams[1][p] = library_stream(area, piece, bytes);
    }
    shmem_barrier_all();

    CHECK(me != 0 || report(trips, streams));
    io_uring_queue_exit(&ring);
    close(link_fd);
    free(piece);
    for (int k = 0; k < KINDS; k++)
        free(t[k]);
    CHECK(osoe_invariants_ok());
    shmem_free(area);
    shmem_free(word);
    shmem_free(port);
    shmem_finalize();
    return CHECK_DONE();
}
