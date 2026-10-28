/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * An owner's io_uring (§3.3, §3.10, §3.12; phase 1 step 3; owner-map.h).
 *
 * One ring per owner - a thread with the wires attached to it, the lead's
 * agent being owner 0 - and only that thread submits: the ring is created with
 * IORING_SETUP_SINGLE_ISSUER and IORING_SETUP_R_DISABLED, set up on the thread
 * that opens the job's connections, and enabled by the thread that will drive
 * it, which is then the only one the kernel lets submit (invariant 19,
 * enforced by the kernel rather than by convention). With no helper threads
 * there is one owner, and its ring carries every wire.
 *
 * It carries its wires' receives and sends, and its owner's futex wait:
 *
 * - **Receive: one multishot recv per wire over one provided-buffer ring** that
 *   the ring's wires share, so the buffers an owner holds do not grow with its
 *   peers - a node's, with the owners it runs.
 *   A reap queues each filled buffer on the wire it arrived for, oldest first,
 *   and marks that wire readable; the engine's pass then parses every byte of
 *   it where the kernel put it (osoe_wire_peek and osoe_wire_consume),
 *   recycling each buffer the moment it is emptied. **No
 *   buffer is held across reaps** (§3.3, invariant 23): a reap that finds one
 *   still queued ends the job, since that is the path to the deadlock §3.3
 *   describes. -ENOBUFS, and any other completion without IORING_CQE_F_MORE,
 *   re-arms: both are normal.
 * - **Send: one in flight per wire** (§3.3, invariant 2), started by
 *   osoe_wire_send and finished by the reap.
 * - **A watch: one poll at a time on one descriptor** (H23, on demand): the
 *   lead's epoll set of the data wires it is making - its listener, its
 *   callers part-way through the hello and its calls (data-wires.c) - so that
 *   the owner's idle wait ends when any of them moves.
 *
 * A wire with no socket - a data wire not yet made, on demand - has nothing
 * armed and nothing in flight, and its receive is armed once it is attached
 * (osoe_ring_arm_receive), so the bounds below hold for any number of them.
 *
 * **Both bounds are asserted at open** (§3.3). The submission queue holds what
 * one pass can queue between two submits - a re-arm and a send per wire, the
 * futex wait and the watch - and the completion queue what can complete between
 * two reaps: one completion per provided buffer (a buffer is refilled only after
 * the pass has emptied it), one terminal completion per armed recv, one per
 * send, the futex wait, the watch, and with the zero-copy send on the
 * notifications a wire may owe (OSOE_WIRE_ZERO_COPY_OWED_MOST,
 * wire-settings.h). Sized that way neither can overflow, so nothing depends on
 * the kernel's overflow list.
 *
 * Private to the thread that drives it, like the rest of the engine (§3.12):
 * nothing here is shared memory, so nothing here is atomic.
 */
#ifndef OSOE_RING_H
#define OSOE_RING_H

#include <liburing.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct osoe_wire;

/* Provided buffers: OSOE_WIRE_BUFFERS of OSOE_WIRE_BYTES each (wire.h). */
struct osoe_ring {
    struct io_uring uring;
    struct io_uring_buf_ring *buffer_ring;
    unsigned char *buffer_memory;
    uint32_t buffer_count;          /* a power of two */
    uint32_t buffer_bytes;
    uint32_t *next;                 /* per buffer: the next one queued on its wire */
    uint32_t *filled;               /* per buffer: the bytes the kernel put in it */
    struct osoe_wire *wires;        /* by index (wire.h) */
    uint32_t wire_count;
    uint64_t *readable;             /* one bit per wire: something arrived */
    uint32_t held;                  /* buffers queued on wires, not yet emptied */
    uint32_t submission_bound, completion_bound;
    bool enabled;
    bool futex_armed;               /* §3.10: at most one armed wait per ring */
    bool watch_armed;               /* the watch's poll is armed (osoe_ring_watch) */
    bool watch_fired;               /* and has completed since osoe_ring_take_watch */
    /* §3.14, the node's `am` record when inside a job; the engine binds it. */
    struct osoe_ring_counts {
        uint64_t completions, receives, enobufs, rearms, sends, short_sends;
        /* The zero-copy send (wire-settings.h): sends by reference, their
         * notifications, and the ones that said the kernel copied after all;
         * and those the user's locked-memory limit refused (ring.c), which the
         * owner's pass reports once (progress.c). */
        uint64_t zero_copy_sends, zero_copy_notifications, zero_copy_copied;
        uint64_t zero_copy_refused;
        /* The armed futex wait's completions that saw the word move - woken,
         * or refused at arming because it already had: the owner's wakes,
         * where a timeout or a socket's completion is not one. */
        uint64_t wakes;
    } counts;
};

/* A provided buffer's number, or none (a wire's empty queue). */
#define OSOE_RING_NONE UINT32_MAX

/* The most completions io_uring gives a ring (the kernel's IORING_MAX_CQ_ENTRIES,
 * twice its 32,768 submissions): the bound the zero-copy send's settings are
 * checked against (wire.c). */
#define OSOE_RING_COMPLETIONS_MOST UINT32_C(65536)

/* The most submissions io_uring gives a ring (the kernel's IORING_MAX_ENTRIES).
 * osoe_ring_open asks for two a wire with a socket and two, rounded up to a
 * power of two, so a ring holds 16,383 such wires at most: owner 0's, which
 * carries every control wire and, at no helpers, every data wire too, holds
 * 2(n - 1) - past 8,192 nodes, more - and with helpers n - 1, past 16,384
 * (P17). On demand its data wires are at most the count a node may make
 * (connection-area.h's osoe_am_data_wires_most; H23): 12,288 nodes at no
 * helpers at the defaults. */
#define OSOE_RING_SUBMISSIONS_MOST UINT32_C(32768)

/*
 * Whether each node of a job of `node_count` nodes at `helpers` progress helpers,
 * of which a node may make `data_wires_most` data wires - every peer, but on
 * demand past the buffer budget's reach (connection-area.h) - can make its
 * rings: NULL if it can, or why not, written into `why`, naming the node counts
 * it can and OSOE_PROGRESS_HELPERS. The backend's check asks it before anything
 * is created (osoe_internode_backend_check), so a job past the ceiling is
 * refused by name rather than by io_uring's EINVAL at the end of the mesh,
 * "cannot create an io_uring for 16384 connections".
 */
const char *osoe_ring_refuses(uint32_t node_count, uint32_t helpers, uint32_t data_wires_most,
                              char *why, size_t len);

/*
 * Create the ring for `wire_count` wires with `buffer_count` provided buffers
 * of `buffer_bytes` each, disabled until osoe_ring_enable. At most `active` of
 * the wires have a socket at once - every one eagerly, and on demand the
 * control wires and as many data wires as the node may make (H23) - and the
 * bounds above are counted over those; at most `lending_active` of them may
 * send by reference and owe up to `notifications_per_wire` notifications each:
 * an owner's data wires, which come before owner 0's control wires on its ring
 * (owner-map.h), and a control wire never lends. Fatal if the kernel gives
 * smaller queues than the bounds above.
 */
void osoe_ring_open(struct osoe_ring *r, struct osoe_wire *wires, uint32_t wire_count,
                    uint32_t active, uint32_t lending_active, uint32_t buffer_count,
                    uint32_t buffer_bytes, uint32_t notifications_per_wire);

/* A send's user data: its wire's index on the ring, and the region it lends
 * by reference - or OSOE_WIRE_LENT_REGIONS for a send that copies (wire.h). */
uint64_t osoe_ring_send_tag(uint32_t index, uint32_t region);

/* On the thread that will drive it, once: enable the ring, which makes this
 * thread its only submitter, and arm every wire's receive. */
void osoe_ring_enable(struct osoe_ring *r);

/* Submit what is queued, and handle every completion there is: receives are
 * queued on their wires and marked readable, sends are finished. Never waits. */
void osoe_ring_reap(struct osoe_ring *r);

/* Submit what is queued, without looking at completions (end of a pass). */
void osoe_ring_submit(struct osoe_ring *r);

/*
 * Submit what is queued and sleep until a completion arrives or `timeout_ns`
 * passes. Returns true if one arrived. The reap that follows handles it.
 */
bool osoe_ring_wait(struct osoe_ring *r, uint64_t timeout_ns);

/*
 * The same, and also until `*wake` no longer holds `seen`: the progress
 * thread's idle wait (§3.10), with at most one IORING_OP_FUTEX_WAIT armed on
 * the word at a time (ring.c says why an older one still serves).
 */
bool osoe_ring_wait_wake(struct osoe_ring *r, _Atomic uint32_t *wake, uint32_t seen,
                         uint64_t timeout_ns);

/* Take, and clear, the readable bits of the ring's wires `first` to
 * `first` + `count` - 1, `count` 64 at most, as bits 0 on: a range of wires
 * need not start a word, since owner 0's control wires follow its data wires on
 * one ring (owner-map.h). */
uint64_t osoe_ring_take_readable(struct osoe_ring *r, uint32_t first, uint32_t count);

/* Give a provided buffer back to the kernel (wire.c, when a buffer is empty). */
void osoe_ring_recycle(struct osoe_ring *r, uint32_t buffer);

/* A submission queue entry; fatal if there is none, since the bound above
 * says there always is. */
struct io_uring_sqe *osoe_ring_entry(struct osoe_ring *r);

/* Arm wire `index`'s multishot receive. */
void osoe_ring_arm_receive(struct osoe_ring *r, uint32_t index);

/* Arm the watch on `fd` - one poll for input, at most one at a time - unless
 * it is armed; the reap that finds its completion marks it fired. On the
 * ring's submitter. */
void osoe_ring_watch(struct osoe_ring *r, int fd);

/* Whether the watch has fired since the last time this was asked. */
bool osoe_ring_take_watch(struct osoe_ring *r);

void osoe_ring_close(struct osoe_ring *r);

#endif /* OSOE_RING_H */
