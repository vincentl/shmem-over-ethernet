#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base/cacheline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "fatal.h"
#include "wire.h"

/*
 * One direction. produced and consumed are monotone byte counts, never reduced
 * modulo the capacity: their difference is the bytes outstanding, which is what
 * both sides test, and a wrapped pair could not tell "empty" from "full". They
 * are 64-bit and never wrap in any job that could finish.
 *
 * Each is on its own cache line: the writer stores produced while the reader
 * stores consumed, and sharing a line would put a coherence miss on every byte.
 */
struct osoe_wire_direction {
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t produced;
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t consumed;
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t wake;   /* futex word, the reader's */
};

struct osoe_wire_header {
    _Alignas(OSOE_CACHELINE) uint64_t capacity;            /* bytes per direction */
    uint64_t magic;
    /*
     * Published by the creator once capacity and magic are written, and waited on by
     * the opener - §6.2's `ready`, for the same reason it exists there. The
     * name appears at shm_open, which is before the header has been filled in
     * and before ftruncate has given the object its length, so a peer that
     * trusted the name alone would read a zero capacity or take SIGBUS off the
     * end of a zero-length mapping.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t ready;
    /*
     * How many of the two sides have stopped writing (osoe_wire_close). A side
     * unmaps only once this reads 2, because unmapping while the peer still
     * holds the ring is a store into an address that is no longer mapped -
     * SIGSEGV in the peer, blamed on the peer.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t leaving;
    _Alignas(OSOE_CACHELINE) struct osoe_wire_direction direction[2];
};

#define OSOE_WIRE_MAGIC UINT64_C(0x65726977656f736f)   /* "osoewire", little-endian */

static uint64_t data_offset(void)
{
    return (sizeof(struct osoe_wire_header) + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

static unsigned char *ring(const struct osoe_wire *w, int which)
{
    return (unsigned char *)w->map + data_offset() + (uint64_t)which * w->capacity;
}

/*
 * §3.14's fault injection, stateless and hash-derived, so the same seed gives
 * the same decision for the same transfer however the threads interleave. A
 * shared generator would make the split each site sees depend on arrival order
 * and destroy reproducibility, which is the whole value.
 */
static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    return x;
}

/*
 * How much of `avail` this transfer will actually move. Truncating to zero is
 * deliberately allowed for a send - a socket may accept nothing - but not for a
 * receive, where returning 0 with bytes waiting would make the caller sleep on
 * a wake that has already happened. The caller's loop tolerates it either way;
 * this keeps the injected behaviour inside what a socket can do.
 */
static size_t truncate_to(const struct osoe_wire *w, uint64_t site, uint64_t n, bool allow_zero)
{
    if (w->split == 0 || n == 0)
        return (size_t)n;
    uint64_t h = hash64(w->seed ^ site ^ (n * UINT64_C(0x9E3779B97F4A7C15)));
    if (h % 10000 >= w->split)
        return (size_t)n;
    uint64_t keep = (h >> 20) % (n + (allow_zero ? 1 : 0));
    if (!allow_zero && keep == 0)
        keep = 1;
    return (size_t)keep;
}

static void read_env(struct osoe_wire *w, const struct osoe_bootstrap *b)
{
    uint64_t capacity = OSOE_WIRE_DEFAULT_BYTES;
    if (osoe_environment_u64("OSOE_WIRE_BYTES", &capacity) == OSOE_ENVIRONMENT_INVALID ||
        capacity < OSOE_WIRE_MIN_BYTES || capacity > OSOE_WIRE_MAX_BYTES)
        osoe_fatal("OSOE_WIRE_BYTES='%s' is not a byte count between %llu and %llu",
                   getenv("OSOE_WIRE_BYTES"), (unsigned long long)OSOE_WIRE_MIN_BYTES,
                   (unsigned long long)OSOE_WIRE_MAX_BYTES);
    /* A power of two, so the wrap is a mask and the ring has no dead bytes. */
    if ((capacity & (capacity - 1)) != 0)
        osoe_fatal("OSOE_WIRE_BYTES=%llu is not a power of two",
                   (unsigned long long)capacity);
    w->capacity = capacity;

    uint32_t split = 0;
    if (osoe_environment_u32("OSOE_WIRE_SPLIT", &split) == OSOE_ENVIRONMENT_INVALID || split > 10000)
        osoe_fatal("OSOE_WIRE_SPLIT='%s' is not a rate per myriad (0 to 10000)",
                   getenv("OSOE_WIRE_SPLIT"));
    w->split = split;

    /*
     * §3.14: the injection is stateless and hash-derived so that the same seed
     * gives the same decision for the same transfer however the threads
     * interleave - and the seed must be **overridable by environment**, with a
     * failing one committed as a permanent regression case. Without the
     * override the seed here would come from the job id, which is the
     * launcher's pid, so every run would inject a different pattern and a case
     * that failed could not be run again. A test that sets OSOE_WIRE_SPLIT
     * should set this too.
     */
    w->seed = hash64(b->job ^ ((uint64_t)b->step << 32));
    uint64_t seed;
    switch (osoe_environment_u64("OSOE_WIRE_SEED", &seed)) {
    case OSOE_ENVIRONMENT_OK:
        w->seed = seed;
        break;
    case OSOE_ENVIRONMENT_UNSET:
        break;
    default:
        osoe_fatal("OSOE_WIRE_SEED='%s' is not a decimal number",
                   getenv("OSOE_WIRE_SEED"));
    }
    /* Print it whenever injection is on, so a failure names what to reproduce
     * with. Off by default, so a normal run says nothing. */
    if (w->split != 0)
        fprintf(stderr, "osoe: wire split %u per myriad, OSOE_WIRE_SEED=%llu\n",
                w->split, (unsigned long long)w->seed);
}

void osoe_wire_open(struct osoe_wire *w, const struct osoe_bootstrap *b, uint32_t peer,
                    uint32_t session)
{
    *w = (struct osoe_wire){ 0 };
    if (peer == b->node)
        osoe_fatal("a node cannot open a wire to itself (node %u)", b->node);
    read_env(w, b);
    /* The close handshake waits as long as the bootstrap does: both are "the
     * other end of this job should be here by now" (§6.2). */
    w->close_no_progress_ns = b->no_progress_ns;

    /* §6.2: the lower node index connects to the higher, so exactly one wire
     * exists per pair and exactly one side creates it. */
    uint32_t lo = peer < b->node ? peer : b->node;
    uint32_t hi = peer < b->node ? b->node : peer;
    w->creator = b->node == lo;
    snprintf(w->name, sizeof w->name, "/osoe.%llu.%u.wire.%u.%u.%u",
             (unsigned long long)b->job, b->step, lo, hi, session);

    uint64_t bytes = data_offset() + 2 * w->capacity;
    int fd;
    if (w->creator) {
        fd = shm_open(w->name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
            osoe_fatal("cannot create wire /dev/shm%s: %s%s", w->name, strerror(errno),
                       errno == EEXIST ? "; probably left by a crashed job with the same "
                                         "job and step ids" : "");
        if (ftruncate(fd, (off_t)bytes) != 0) {
            close(fd);
            shm_unlink(w->name);
            osoe_fatal("cannot size wire %s: %s", w->name, strerror(errno));
        }
    } else {
        /*
         * Wait for the creator. There is nothing to wait *on* before the name
         * exists, so this polls with backoff exactly as §6.2's non-lead PEs do
         * for the control segment, and then waits on `ready` for the contents.
         * Both halves are needed: shm_open succeeds the moment the name exists,
         * which is before ftruncate has sized the object and before the header
         * has been written.
         */
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        struct timespec back = { 0, 200000 };            /* 0.2 ms, doubling to 10 ms */
        for (;;) {
            fd = shm_open(w->name, O_RDWR, 0);
            if (fd >= 0) {
                /* Mapping past the end of a still-unsized object is allowed and
                 * faults with SIGBUS on the first touch, so wait for the length
                 * too, not only the name. */
                struct stat st;
                if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= bytes)
                    break;
                close(fd);
                fd = -1;
            } else if (errno != ENOENT) {
                osoe_fatal("cannot open wire %s: %s", w->name, strerror(errno));
            }
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has not created the wire %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), lo, w->name);
            nanosleep(&back, NULL);
            if (back.tv_nsec < 10000000)
                back.tv_nsec *= 2;
        }
    }

    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        if (w->creator)
            shm_unlink(w->name);
        osoe_fatal("cannot map wire %s: %s", w->name, strerror(errno));
    }
    w->map = p;
    w->bytes = bytes;

    struct osoe_wire_header *h = p;
    if (w->creator) {
        /* ftruncate zeroed it: every counter and futex word starts at 0. The
         * header is written before `ready`, and `ready` is what the peer waits
         * on - the store is seq_cst because this is the publishing half of a
         * two-sided protocol (§3.16). */
        h->capacity = w->capacity;
        h->magic = OSOE_WIRE_MAGIC;
        atomic_store_explicit(&h->ready, 1, memory_order_seq_cst);
        osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
            struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_futex_wait(&h->ready, 0, &sweep, OSOE_FUTEX_SHARED);
            if (osoe_stall_watch_stalled(&watch, 0)) {
                munmap(p, bytes);
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): node %u has not finished creating the wire %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), lo, w->name);
            }
        }
        /* Both sides must agree on the capacity, or their index arithmetic
         * disagrees and the stream silently tears. */
        if (h->magic != OSOE_WIRE_MAGIC || h->capacity != w->capacity)
            osoe_fatal("wire %s was created with a different OSOE_WIRE_BYTES (%llu, not "
                       "%llu): every PE of a job must set it the same", w->name,
                       (unsigned long long)h->capacity, (unsigned long long)w->capacity);
    }

    /*
     * Direction 0 carries lo -> hi and direction 1 hi -> lo, so each side's tx
     * is the other's rx without either needing to know which created the object.
     */
    w->tx = &h->direction[w->creator ? 0 : 1];
    w->rx = &h->direction[w->creator ? 1 : 0];
}

/*
 * Teardown is two-sided, and the reason is that a barrier orders *arrival*,
 * not the last byte.
 *
 * A lead that has applied its release may still owe an acknowledgement for the
 * segment that carried it (§3.4), so a peer can legitimately write here after
 * this side has finished every collective it will ever run. Unmapping first
 * leaves that peer storing into an address it no longer holds - a SIGSEGV on
 * the node that did nothing wrong, and the backtrace names the innocent side.
 *
 * So it is the same shape as `ready` at the other end of the object's life:
 * publish that this side is done, wake, and wait for the count to reach two.
 * This is the third object in this slice to need the rule §6.2 states for the
 * control segment - do not take the thing away while anyone still holds it -
 * after arrivals_close() and the job segment. There the shared resource was
 * the *name*; here it is the mapping, and the name is fine either way.
 *
 * **Announcing and waiting are separate calls**, which is not tidiness. A lead
 * with several peers must tell all of them before waiting on any, or teardown
 * costs one peer's timeout after another. And a single process holding both
 * ends - which every tier-1 test of this file does - cannot close them one
 * after the other if closing blocks: the first close waits for a side that has
 * not been reached yet. Announcing to both first makes that work, and finding
 * it out is what the tier-1 test is for.
 */
void osoe_wire_shutdown(struct osoe_wire *w)
{
    if (!w->map || w->left)
        return;
    struct osoe_wire_header *h = w->map;
    w->left = true;
    /* Both stores of a two-sided protocol are seq_cst (§3.16), and a locked
     * add is one: each side adds its own and reads the other's. */
    atomic_fetch_add_explicit(&h->leaving, 1, memory_order_seq_cst);
    osoe_futex_wake(&h->leaving, INT_MAX, OSOE_FUTEX_SHARED);
}

void osoe_wire_close(struct osoe_wire *w)
{
    if (!w->map)
        return;
    osoe_wire_shutdown(w);
    struct osoe_wire_header *h = w->map;

    /*
     * Waits for the other end, sweeping, and gives up only by the no-progress
     * rule (§3.15): by default a peer that died mid-teardown leaves this waiting
     * until the launcher tears the job down, which is what it is for. Remote
     * liveness arrives with step 4.
     */
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, atomic_load_explicit(&h->leaving, memory_order_acquire),
                           w->close_no_progress_ns);
    for (;;) {
        uint32_t left = atomic_load_explicit(&h->leaving, memory_order_acquire);
        if (left >= 2)
            break;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        if (osoe_futex_wait(&h->leaving, left, &sweep, OSOE_FUTEX_SHARED) == ETIMEDOUT)
            osoe_sweep_expired("shmem_finalize");
        if (osoe_stall_watch_stalled(&watch, atomic_load_explicit(&h->leaving,
                                                                  memory_order_acquire)))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the other end of the wire %s has not finished; that node is "
                       "still running or has died mid-teardown",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), w->name);
    }

    munmap(w->map, w->bytes);
    if (w->creator)
        shm_unlink(w->name);
    *w = (struct osoe_wire){ 0 };
}

uint64_t osoe_wire_capacity(const struct osoe_wire *w)
{
    return w->capacity;
}

size_t osoe_wire_send(struct osoe_wire *w, const void *buffer, size_t len)
{
    /*
     * The reader's progress first, then our own count. Both are ours to
     * compare; only `consumed` is written by the peer, and reading it early
     * only ever understates the room, which costs a short write and never
     * correctness.
     */
    uint64_t consumed = atomic_load_explicit(&w->tx->consumed, memory_order_acquire);
    uint64_t produced = atomic_load_explicit(&w->tx->produced, memory_order_acquire);
    uint64_t room = w->capacity - (produced - consumed);
    if (room > len)
        room = len;
    size_t n = truncate_to(w, w->sends++, room, true);
    if (n == 0)
        return 0;

    /* Copy in, wrapping once at most. The mask is valid because the capacity
     * is a power of two, checked at open. */
    uint64_t mask = w->capacity - 1;
    uint64_t at = produced & mask;
    size_t first = n;
    if (at + first > w->capacity)
        first = (size_t)(w->capacity - at);
    memcpy(ring(w, w->creator ? 0 : 1) + at, buffer, first);
    if (first < n)
        memcpy(ring(w, w->creator ? 0 : 1), (const unsigned char *)buffer + first, n - first);

    /*
     * Publish the bytes, then the wake. This order is the protocol: a reader
     * that has loaded the wake word already will find the bytes, and one that
     * has not will find the bump. Reversing it, or reversing the reader's two
     * loads, loses the wake (wire_model.py, the `wake_after` case).
     */
    atomic_store_explicit(&w->tx->produced, produced + n, memory_order_seq_cst);
    atomic_fetch_add_explicit(&w->tx->wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&w->tx->wake, 1, OSOE_FUTEX_SHARED);
    return n;
}

size_t osoe_wire_recv(struct osoe_wire *w, void *buffer, size_t len)
{
    uint64_t produced = atomic_load_explicit(&w->rx->produced, memory_order_acquire);
    uint64_t consumed = atomic_load_explicit(&w->rx->consumed, memory_order_acquire);
    uint64_t have = produced - consumed;
    if (have > len)
        have = len;
    size_t n = truncate_to(w, w->recvs++, have, false);
    if (n == 0)
        return 0;

    uint64_t mask = w->capacity - 1;
    uint64_t at = consumed & mask;
    size_t first = n;
    if (at + first > w->capacity)
        first = (size_t)(w->capacity - at);
    memcpy(buffer, ring(w, w->creator ? 1 : 0) + at, first);
    if (first < n)
        memcpy((unsigned char *)buffer + first, ring(w, w->creator ? 1 : 0), n - first);

    /* Release the space only once the bytes are out of it. */
    atomic_store_explicit(&w->rx->consumed, consumed + n, memory_order_seq_cst);
    return n;
}

uint32_t osoe_wire_seq(const struct osoe_wire *w)
{
    return atomic_load_explicit(&w->rx->wake, memory_order_acquire);
}

bool osoe_wire_wait(struct osoe_wire *w, uint32_t seq, const struct osoe_deadline *deadline)
{
    return osoe_futex_wait(&w->rx->wake, seq, deadline, OSOE_FUTEX_SHARED) == 0;
}
