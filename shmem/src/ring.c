/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/futex.h>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

#include "base/ring-check.h"
#include "fatal.h"
#include "owner-map.h"
#include "ring.h"
#include "wire.h"

/*
 * A provided buffer is the kernel's from the moment it is given back until its
 * next completion, and the engine parses buffers where the kernel put them
 * (wire.h): so in the sanitizer presets a buffer is poisoned for as long as it
 * is the kernel's, and a read of one after it went back - or past what the
 * kernel wrote - is a report, not a rare corruption (the third adversarial read
 * of v0.2 step 4, S2). The kernel's own writes are not instrumented.
 */
#if defined(__SANITIZE_ADDRESS__)
#define KERNEL_OWNS(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define ENGINE_OWNS(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define KERNEL_OWNS(p, n) ((void)(p), (void)(n))
#define ENGINE_OWNS(p, n) ((void)(p), (void)(n))
#endif

/* What a completion is for, in its user data's low four bits; a send's region
 * in the next four (wire.h); the wire's index on the ring the rest (0 for the
 * futex wait, which is the ring's, not a wire's). */
enum { RECEIVE = 1, SEND, FUTEX };

static uint64_t tag(uint32_t index, unsigned kind)
{
    return ((uint64_t)index << 8) | kind;
}

uint64_t osoe_ring_send_tag(uint32_t index, uint32_t region)
{
    return tag(index, SEND) | (uint64_t)region << 4;
}

static uint32_t next_power_of_two(uint32_t n)
{
    uint32_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

const char *osoe_ring_refuses(uint32_t node_count, uint32_t helpers, char *why, size_t len)
{
    uint32_t peers = node_count > 1 ? node_count - 1 : 0;
    uint32_t most_wires = (OSOE_RING_SUBMISSIONS_MOST - 1) / 2;
    for (uint32_t owner = 0; owner <= helpers; owner++) {
        uint64_t wires = osoe_ring_wires(owner, peers, helpers);
        if (wires <= most_wires)
            continue;
        snprintf(why, len,
                 "a job of %u nodes needs a ring of %llu connections on each node's %s, which "
                 "asks io_uring for %llu submissions where the kernel allows %u "
                 "(IORING_MAX_ENTRIES): at most %u nodes at OSOE_PROGRESS_HELPERS=%u%s",
                 node_count, (unsigned long long)wires,
                 owner == 0 ? "lead" : "progress helper", 2 * (unsigned long long)wires + 1,
                 OSOE_RING_SUBMISSIONS_MOST, (helpers == 0 ? most_wires / 2 : most_wires) + 1,
                 helpers,
                 helpers == 0 ? ", and 16384 with progress helpers, which take the data wires "
                                "off the lead's ring"
                              : "");
        return why;
    }
    return NULL;
}

/*
 * What a ring's -ENOMEM can mean, said where it ends the job: on Linux 6.14 and
 * later io_uring charges a ring's memory, and a provided-buffer ring's, to the
 * locked-memory limit (base/ring-check.h), and the kernel says no more - under
 * `ulimit -l 0` on the Pi's 6.18 every ring is refused so (2026-10-10).
 */
static const char *locked_memory_note(int e)
{
    static char note[192];
    if (e != -ENOMEM)
        return "";
    long long kib = osoe_locked_memory_kib();
    if (kib < 0)
        return "";
    snprintf(note, sizeof note, " (on Linux 6.14 and later io_uring counts its rings against "
             "the locked-memory limit, ulimit -l, which is %lld KiB here)", kib);
    return note;
}

void osoe_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t wire_count,
                    uint32_t lending_count, uint32_t buffer_count, uint32_t buffer_bytes,
                    uint32_t notifications_per_wire)
{
    *r = (struct osoe_ring){ .wires = wires, .wire_count = wire_count,
                             .buffer_count = buffer_count, .buffer_bytes = buffer_bytes };
    /*
     * §3.3's two bounds, each "can never fill". Submission: a receive re-arm and
     * a send per wire between two submits, and the one futex wait (§3.10).
     * Completion: every provided buffer filled once, a terminal completion per
     * armed receive, a send per wire, the futex wait - and the notifications
     * each wire that may lend may owe, with the zero-copy send on.
     */
    r->submission_bound = 2 * wire_count + 1;
    r->completion_bound = buffer_count + 2 * wire_count +
                          notifications_per_wire * lending_count + 1;
    struct io_uring_params p = {
        .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED | IORING_SETUP_CQSIZE,
        .cq_entries = r->completion_bound,
    };
    int e = io_uring_queue_init_params(next_power_of_two(r->submission_bound), &r->uring, &p);
    if (e < 0)
        osoe_fatal("cannot create an io_uring for %u connections: %s%s", wire_count,
                   strerror(-e), locked_memory_note(e));
    if (p.sq_entries < r->submission_bound || p.cq_entries < r->completion_bound)
        osoe_fatal("the kernel gave an io_uring of %u submissions and %u completions; %u "
                   "connections and %u receive buffers need %u and %u (§3.3)", p.sq_entries,
                   p.cq_entries, wire_count, buffer_count, r->submission_bound,
                   r->completion_bound);

    r->buffer_ring = io_uring_setup_buf_ring(&r->uring, buffer_count, 0, 0, &e);
    if (!r->buffer_ring)
        osoe_fatal("cannot register %u receive buffers: %s%s", buffer_count, strerror(-e),
                   locked_memory_note(e));
    /* malloc, not aligned_alloc: nothing here needs a page (and a page is not
     * 4096 bytes on a Pi 5, §8.5), and C11's aligned_alloc wants the size a
     * multiple of the alignment, which 4 x 64 bytes is not - asan's catch. */
    r->buffer_memory = malloc((size_t)buffer_count * buffer_bytes);
    r->next = calloc(buffer_count, sizeof *r->next);
    r->filled = calloc(buffer_count, sizeof *r->filled);
    r->readable = calloc((wire_count + 63) / 64 + 1, sizeof *r->readable);
    if (!r->buffer_memory || !r->next || !r->filled || !r->readable)
        osoe_fatal("out of memory for %u receive buffers of %u bytes", buffer_count,
                   buffer_bytes);
    KERNEL_OWNS(r->buffer_memory, (size_t)buffer_count * buffer_bytes);
    for (uint32_t i = 0; i < buffer_count; i++)
        io_uring_buf_ring_add(r->buffer_ring, r->buffer_memory + (size_t)i * buffer_bytes,
                              buffer_bytes, (unsigned short)i,
                              io_uring_buf_ring_mask(buffer_count), (int)i);
    io_uring_buf_ring_advance(r->buffer_ring, (int)buffer_count);
}

struct io_uring_sqe *osoe_ring_entry(struct osoe_ring *r)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&r->uring);
    if (!sqe)
        osoe_fatal("the io_uring's submission queue is full: its bound of %u, 2 per "
                   "connection and 1, is broken (§3.3)", r->submission_bound);
    return sqe;
}

void osoe_ring_arm_receive(struct osoe_ring *r, uint32_t index)
{
    struct osoe_wire *w = &r->wires[index];
    struct io_uring_sqe *sqe = osoe_ring_entry(r);
    io_uring_prep_recv_multishot(sqe, w->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    io_uring_sqe_set_data64(sqe, tag(index, RECEIVE));
    w->receiving = true;
}

void osoe_ring_enable(struct osoe_ring *r)
{
    int e = io_uring_enable_rings(&r->uring);
    if (e < 0)
        osoe_fatal("cannot enable the io_uring: %s", strerror(-e));
    r->enabled = true;
    for (uint32_t i = 0; i < r->wire_count; i++)
        osoe_ring_arm_receive(r, i);
    osoe_ring_submit(r);
}

void osoe_ring_submit(struct osoe_ring *r)
{
    int e = io_uring_submit(&r->uring);
    if (e < 0 && e != -EINTR && e != -EAGAIN && e != -EBUSY)
        osoe_fatal("io_uring submission failed: %s", strerror(-e));
}

void osoe_ring_recycle(struct osoe_ring *r, uint32_t buffer)
{
    KERNEL_OWNS(r->buffer_memory + (size_t)buffer * r->buffer_bytes, r->buffer_bytes);
    io_uring_buf_ring_add(r->buffer_ring, r->buffer_memory + (size_t)buffer * r->buffer_bytes,
                          r->buffer_bytes, (unsigned short)buffer,
                          io_uring_buf_ring_mask(r->buffer_count), 0);
    io_uring_buf_ring_advance(r->buffer_ring, 1);
    r->held--;
}

static void mark(struct osoe_ring *r, uint32_t index)
{
    r->readable[index / 64] |= UINT64_C(1) << (index % 64);
}

static void received(struct osoe_ring *r, struct osoe_wire *w, const struct io_uring_cqe *cqe)
{
    if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
        uint32_t buffer = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        ENGINE_OWNS(r->buffer_memory + (size_t)buffer * r->buffer_bytes, (size_t)cqe->res);
        r->filled[buffer] = (uint32_t)cqe->res;
        r->next[buffer] = OSOE_RING_NONE;
        if (w->first == OSOE_RING_NONE)
            w->first = buffer;
        else
            r->next[w->last] = buffer;
        w->last = buffer;
        w->queued += (uint32_t)cqe->res;
        r->held++;
        r->counts.receives++;
        mark(r, w->index);
    } else if (cqe->res == 0) {
        w->ended = true;                /* the peer's end of stream */
        mark(r, w->index);
    } else if (cqe->res == -ENOBUFS) {
        r->counts.enobufs++;            /* normal under load: re-armed below */
    } else if (cqe->res < 0) {
        w->error = cqe->res;
        mark(r, w->index);
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        w->receiving = false;
        /* Every completion without F_MORE re-arms (§3.3), unless the stream is
         * over. The entry is submitted at the end of the pass, after the pass
         * has emptied - and so recycled - what this reap queued. */
        if (!w->ended && !w->error) {
            osoe_ring_arm_receive(r, w->index);
            r->counts.rearms++;
        }
    }
}

void osoe_ring_reap(struct osoe_ring *r)
{
    /* §3.3, invariant 23: every buffer the last reap queued was emptied since. */
    if (r->held != 0)
        osoe_fatal("%u receive buffers were held across a reap of the io_uring; the "
                   "engine must empty every readable wire in the pass (§3.3)", r->held);
    osoe_ring_submit(r);
    struct io_uring_cqe *cqe;
    unsigned head, seen = 0;
    io_uring_for_each_cqe(&r->uring, head, cqe) {
        seen++;
        uint64_t data = io_uring_cqe_get_data64(cqe);
        if ((data & 0xf) == FUTEX) {
            /* Woken, or refused at arming because the word had moved (-EAGAIN),
             * or cancelled: either way nothing is armed now, and the next idle
             * wait arms again (§3.10). The first two are a wake. */
            r->futex_armed = false;
            if (cqe->res == 0 || cqe->res == -EAGAIN)
                r->counts.wakes++;
            continue;
        }
        uint32_t index = (uint32_t)(data >> 8);
        if (index >= r->wire_count)
            osoe_fatal("an io_uring completion names wire %u of the ring's %u", index,
                       r->wire_count);
        struct osoe_wire *w = &r->wires[index];
        uint32_t region = (uint32_t)(data >> 4) & 0xf;
        switch (data & 0xf) {
        case RECEIVE:
            received(r, w, cqe);
            break;
        case SEND:
            if (cqe->flags & IORING_CQE_F_NOTIF) {
                /*
                 * A send by reference: the kernel has let go of its bytes, and
                 * says whether it copied them after all (wire.h). It comes after
                 * that send's completion - io_uring posts it so, deferring it where
                 * task work's order cannot be relied on (io_uring/net.c,
                 * io_send_zc) - so one owed by nothing is a count gone wrong, and a
                 * buffer that count released early: the job ends naming it.
                 */
                if (region >= OSOE_WIRE_LENT_REGIONS || w->owed[region] == 0)
                    osoe_fatal("a zero-copy notification toward node %u for region %u, "
                               "which owed none (wire.h)", w->peer, region);
                w->owed[region]--;
                r->counts.zero_copy_notifications++;
                if ((uint32_t)cqe->res & IORING_NOTIF_USAGE_ZC_COPIED)
                    r->counts.zero_copy_copied++;
                break;
            }
            if (cqe->flags & IORING_CQE_F_MORE) {
                /* ... and its notification is to follow. */
                if (region >= OSOE_WIRE_LENT_REGIONS)
                    osoe_fatal("a send that copies toward node %u said a notification "
                               "would follow", w->peer);
                w->owed[region]++;
            }
            if ((cqe->res == -ENOMEM || cqe->res == -ENOBUFS) &&
                region < OSOE_WIRE_LENT_REGIONS) {
                /*
                 * A send by reference refused for want of locked memory. io_uring
                 * charges each one the length it offers, and two pages, until its
                 * notification, adding to one count for every process of the
                 * user on the host, which it holds to each process's own limit
                 * (ulimit -l, 8 MiB by default) - which sixteen logical nodes on
                 * one machine passed (the review of v0.2 step 4's merge point,
                 * portability P1) - and says -ENOMEM; -ENOBUFS, MSG_ZEROCOPY's
                 * own refusal, is taken so too, since a copy is always right.
                 * Not this connection's failure: the charge comes before the
                 * socket is reached, so nothing was sent, and this wire copies
                 * from here on. Nothing is in flight either, so the wire is
                 * marked free: the visit the mark brings, in this pass, offers
                 * the same bytes again by copy, where a send still marked in
                 * flight would only be collected there, and the bytes wait for
                 * a pass nothing brings (the adversarial read of this change,
                 * 3). A notification still owed (IORING_CQE_F_MORE, above) is
                 * counted as for any send, and says the bytes were copied.
                 * Linux 6.12 and 6.14 charge the send when it is issued; 6.18
                 * charges it as the submission prepares it, where a failure
                 * stops the submission at that entry: the entries behind it
                 * stay queued until the next submit, within the queue's bound
                 * (one send and one re-arm a wire, one futex wait), and an idle
                 * wait whose own submit stopped returns at once, with this
                 * completion to reap (the second adversarial read, finding 2).
                 */
                w->zero_copy = false;
                w->sending = false;
                w->sent = -1;
                r->counts.zero_copy_refused++;
                mark(r, index);
                break;
            }
            if (cqe->res < 0) {
                /* The peer's reset (wire.h) - not a send after this side's
                 * own end of stream, which would be this node's mistake. */
                if ((cqe->res == -EPIPE || cqe->res == -ECONNRESET) && !w->shut)
                    w->send_error = cqe->res;
                else
                    w->error = cqe->res;
                w->sent = 0;
                mark(r, index);
            } else {
                w->sent = cqe->res;
                if ((uint32_t)cqe->res < w->send_bytes)
                    r->counts.short_sends++;
            }
            break;
        default:
            osoe_fatal("an io_uring completion of unknown kind %u", (unsigned)(data & 0xf));
        }
    }
    io_uring_cq_advance(&r->uring, seen);
    r->counts.completions += seen;
}

bool osoe_ring_wait(struct osoe_ring *r, uint64_t timeout_ns)
{
    /* A relative timeout: io_uring's convention, unlike futex_wait's absolute
     * deadlines (§3.10's two conventions). */
    struct __kernel_timespec ts = { .tv_sec = (long long)(timeout_ns / 1000000000u),
                                    .tv_nsec = (long long)(timeout_ns % 1000000000u) };
    struct io_uring_cqe *cqe;
    int e = io_uring_submit_and_wait_timeout(&r->uring, &cqe, 1, &ts, NULL);
    if (e == -ETIME || e == -EINTR)
        return false;
    if (e < 0)
        osoe_fatal("waiting on the io_uring failed: %s", strerror(-e));
    return true;
}

/*
 * §3.10's idle wait: one submission that carries the sockets' completions and
 * the wake word together, so whichever comes first ends it.
 *
 * **At most one armed IORING_OP_FUTEX_WAIT per ring** (invariant 12). A wait
 * armed in an earlier idle period may still be armed - a socket completion
 * ended that period - and on an older value; it is the one this wait sleeps on,
 * and it stays correct because every bump of the word is followed by a
 * futex_wake (internode-am.c, agent.c, progress-host.c: the claimed wake),
 * which completes it whatever value it was armed with. An earlier draft armed
 * one per idle pass and one futex_wake completed 1,881 of them at once,
 * overflowing the completion queue (review D3). lead_wake_model.py checks the
 * rule and both breakages.
 *
 * Arming compares the word with `seen` - loaded by the caller before its pass
 * (§4.9) - and registers the wait in one step, so a bump since then completes
 * it at once with -EAGAIN rather than sleeping on a stale value.
 */
bool osoe_ring_wait_wake(struct osoe_ring *r, _Atomic uint32_t *wake, uint32_t seen,
                         uint64_t timeout_ns)
{
    if (!r->futex_armed) {
        struct io_uring_sqe *sqe = osoe_ring_entry(r);
        /* Shared, not FUTEX2_PRIVATE: the word is in the node segment, and the
         * wakers are other processes (tools/probe checks this form). */
        io_uring_prep_futex_wait(sqe, (uint32_t *)(void *)wake, seen, FUTEX_BITSET_MATCH_ANY,
                                 FUTEX2_SIZE_U32, 0);
        io_uring_sqe_set_data64(sqe, tag(0, FUTEX));
        r->futex_armed = true;
    }
    return osoe_ring_wait(r, timeout_ns);
}

uint64_t osoe_ring_take_readable(struct osoe_ring *r, uint32_t first, uint32_t count)
{
    if (count == 0)
        return 0;
    uint64_t want = count >= 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
    uint32_t word = first / 64, shift = first % 64;
    uint64_t taken = (r->readable[word] >> shift) & want;
    r->readable[word] &= ~(taken << shift);
    /* A range that does not start a word runs into the next. */
    if (shift != 0 && count > 64 - shift) {
        uint64_t high = r->readable[word + 1] & (want >> (64 - shift));
        r->readable[word + 1] &= ~high;
        taken |= high << (64 - shift);
    }
    return taken;
}

void osoe_ring_close(struct osoe_ring *r)
{
    if (!r->buffer_memory)
        return;
    io_uring_free_buf_ring(&r->uring, r->buffer_ring, r->buffer_count, 0);
    io_uring_queue_exit(&r->uring);
    ENGINE_OWNS(r->buffer_memory, (size_t)r->buffer_count * r->buffer_bytes);
    free(r->buffer_memory);
    free(r->next);
    free(r->filled);
    free(r->readable);
    *r = (struct osoe_ring){ 0 };
}
