#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <linux/futex.h>

#include "fatal.h"
#include "ring.h"
#include "wire.h"

/* What a completion is for, in its user data's low byte; the wire's slot is
 * the rest (0 for the futex wait, which is the ring's, not a wire's). */
enum { RECEIVE = 1, SEND, FUTEX };

static uint64_t tag(uint32_t slot, unsigned kind)
{
    return ((uint64_t)slot << 8) | kind;
}

static uint32_t next_power_of_two(uint32_t n)
{
    uint32_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

void osoe_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t wire_count,
                    uint32_t buffer_count, uint32_t buffer_bytes)
{
    *r = (struct osoe_ring){ .wires = wires, .wire_count = wire_count,
                             .buffer_count = buffer_count, .buffer_bytes = buffer_bytes };
    /*
     * §3.3's two bounds, each "can never fill". Submission: a receive re-arm and
     * a send per wire between two submits, and the one futex wait (§3.10).
     * Completion: every provided buffer filled once, a terminal completion per
     * armed receive, a send per wire, the futex wait.
     */
    r->submission_bound = 2 * wire_count + 1;
    r->completion_bound = buffer_count + 2 * wire_count + 1;
    struct io_uring_params p = {
        .flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED | IORING_SETUP_CQSIZE,
        .cq_entries = r->completion_bound,
    };
    int e = io_uring_queue_init_params(next_power_of_two(r->submission_bound), &r->uring, &p);
    if (e < 0)
        osoe_fatal("cannot create an io_uring for %u connections: %s", wire_count, strerror(-e));
    if (p.sq_entries < r->submission_bound || p.cq_entries < r->completion_bound)
        osoe_fatal("the kernel gave an io_uring of %u submissions and %u completions; %u "
                   "connections and %u receive buffers need %u and %u (§3.3)", p.sq_entries,
                   p.cq_entries, wire_count, buffer_count, r->submission_bound,
                   r->completion_bound);

    r->buffer_ring = io_uring_setup_buf_ring(&r->uring, buffer_count, 0, 0, &e);
    if (!r->buffer_ring)
        osoe_fatal("cannot register %u receive buffers: %s", buffer_count, strerror(-e));
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

void osoe_ring_arm_receive(struct osoe_ring *r, uint32_t slot)
{
    struct osoe_wire *w = &r->wires[slot];
    struct io_uring_sqe *sqe = osoe_ring_entry(r);
    io_uring_prep_recv_multishot(sqe, w->fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = 0;
    io_uring_sqe_set_data64(sqe, tag(slot, RECEIVE));
    w->receiving = true;
}

void osoe_ring_enable(struct osoe_ring *r)
{
    int e = io_uring_enable_rings(&r->uring);
    if (e < 0)
        osoe_fatal("cannot enable the io_uring: %s", strerror(-e));
    r->enabled = true;
    for (uint32_t slot = 0; slot < r->wire_count; slot++)
        osoe_ring_arm_receive(r, slot);
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
    io_uring_buf_ring_add(r->buffer_ring, r->buffer_memory + (size_t)buffer * r->buffer_bytes,
                          r->buffer_bytes, (unsigned short)buffer,
                          io_uring_buf_ring_mask(r->buffer_count), 0);
    io_uring_buf_ring_advance(r->buffer_ring, 1);
    r->held--;
}

static void mark(struct osoe_ring *r, uint32_t slot)
{
    r->readable[slot / 64] |= UINT64_C(1) << (slot % 64);
}

static void received(struct osoe_ring *r, struct osoe_wire *w, const struct io_uring_cqe *cqe)
{
    if (cqe->res > 0 && (cqe->flags & IORING_CQE_F_BUFFER)) {
        uint32_t buffer = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
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
        mark(r, w->slot);
    } else if (cqe->res == 0) {
        w->ended = true;                /* the peer's end of stream */
        mark(r, w->slot);
    } else if (cqe->res == -ENOBUFS) {
        r->counts.enobufs++;            /* normal under load: re-armed below */
    } else if (cqe->res < 0) {
        w->error = cqe->res;
        mark(r, w->slot);
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        w->receiving = false;
        /* Every completion without F_MORE re-arms (§3.3), unless the stream is
         * over. The entry is submitted at the end of the pass, after the pass
         * has emptied - and so recycled - what this reap queued. */
        if (!w->ended && !w->error) {
            osoe_ring_arm_receive(r, w->slot);
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
        if ((data & 0xff) == FUTEX) {
            /* Woken, or refused at arming because the word had moved (-EAGAIN),
             * or cancelled: either way nothing is armed now, and the next idle
             * wait arms again (§3.10). */
            r->futex_armed = false;
            continue;
        }
        uint32_t slot = (uint32_t)(data >> 8);
        if (slot >= r->wire_count)
            osoe_fatal("an io_uring completion names connection %u of %u", slot, r->wire_count);
        struct osoe_wire *w = &r->wires[slot];
        switch (data & 0xff) {
        case RECEIVE:
            received(r, w, cqe);
            break;
        case SEND:
            if (cqe->res < 0) {
                w->error = cqe->res;
                w->sent = 0;
                mark(r, slot);
            } else {
                w->sent = cqe->res;
                if ((uint32_t)cqe->res < w->send_bytes)
                    r->counts.short_sends++;
            }
            break;
        default:
            osoe_fatal("an io_uring completion of unknown kind %u", (unsigned)(data & 0xff));
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

uint64_t osoe_ring_take_readable(struct osoe_ring *r, uint32_t word)
{
    uint64_t bits = r->readable[word];
    r->readable[word] = 0;
    return bits;
}

void osoe_ring_close(struct osoe_ring *r)
{
    if (!r->buffer_memory)
        return;
    io_uring_free_buf_ring(&r->uring, r->buffer_ring, r->buffer_count, 0);
    io_uring_queue_exit(&r->uring);
    free(r->buffer_memory);
    free(r->next);
    free(r->filled);
    free(r->readable);
    *r = (struct osoe_ring){ 0 };
}
