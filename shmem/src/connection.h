/*
 * Segment structure and the reservation protocol (§3.4, §3.5).
 *
 * A connection is one node pair's outbound half: two buffers, of which **at
 * most one is reservable at any moment**. Application threads of any local PE
 * reserve space in the open buffer and copy their message into it; the lead's
 * agent seals it, hands it to the wire, and recycles it (§3.12). Writers are
 * therefore in several processes and the reaper in another, so every word here
 * lives in shared memory and every access to one is an atomic naming an ISO
 * order (§3.16).
 *
 * **This is the protocol the review repaired, and its rules are load-bearing
 * rather than stylistic.** Two independent exhaustive models reproduced the
 * defects in the draft version and pass on this one
 * (`docs/reviews/evidence/transport/`, `docs/reviews/evidence/refutation/`,
 * the `fixed` variant). Re-running them at step 2 reproduced every recorded
 * output. Each rule below names what breaks without it:
 *
 * - **The seal is a bit in `head`**, set by the progress thread, and writers
 *   never read a separate state field. A draft checked `state == OPEN` and then
 *   CASed `head`; the progress thread could seal and submit between the two and
 *   the writer's CAS still succeeded, losing or tearing the message and leaving
 *   `filled > head` in the next generation. Setting the bit changes the value
 *   every later CAS compares against, so a sealed buffer rejects them all.
 * - **At most one reservable buffer.** A freed buffer becomes reservable only
 *   when the other is not. With two, one PE's later operation can be sealed and
 *   sent first, which breaks `fence`, signal-after-payload (invariant 5) and
 *   `quiet`.
 * - **A CAS loop, not `fetch_add`.** With `fetch_add` an overflowing writer has
 *   already moved `head` and must unwind, and the unwind must still account for
 *   itself in `filled` or the buffer never becomes sendable. The CAS makes a
 *   failed reservation a non-event.
 * - **`filled` is a count, not a watermark.** Writers complete out of order, so
 *   no prefix property holds. The buffer is sendable when `filled` equals the
 *   reserved count. Do not reach for a watermark and then discover it cannot be
 *   maintained.
 * - **Sequence numbers are assigned when a buffer becomes reservable**, not at
 *   seal. A writer reads the number after a successful reservation with no
 *   extra synchronization, and that is sound because the buffer cannot recycle
 *   until `filled == head` and this writer has not committed yet. Assigning at
 *   seal instead lets a writer record the previous generation's number, so
 *   `quiet` returns early - also model-checked.
 * - **A DRAINING buffer keeps the seal bit** while the progress thread fills it
 *   with replies (§3.8's starvation guard), so writers skip it; opening it
 *   clears the bit. This happens on every open: flush() drains pending replies
 *   into a freed buffer, up to half of it, immediately before
 *   osoe_connection_open, which then opens the buffer past them. So a reply is
 *   guaranteed room on every buffer the writers get, however hard they write
 *   (docs/design/evidence/am-backend/reservation_model.py).
 *
 * The progress thread must never spin waiting for `filled` to catch up: a
 * writer may be descheduled mid-copy, and at 256 PEs on an oversubscribed node
 * a spin here is a hang (§3.6). Every call below returns instead.
 */
#ifndef OSOE_CONNECTION_H
#define OSOE_CONNECTION_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/cacheline.h"
#include "reply.h"

/*
 * Segment header (§3.4): fixed size, at offset 0, never reserved by a writer,
 * filled by the progress thread at seal time.
 *
 * `message_count` and `total_len` are deliberately redundant with the per-message
 * lengths. Cross-checking them on receipt turns a framing bug into an immediate
 * assertion rather than a garbage dispatch 50 MB later.
 */
enum {
    /* A fence, quiet or barrier flush sealed this segment and needs the peer to
     * acknowledge it: piggybacked if the peer has traffic, and otherwise a
     * header-only segment, or one-way traffic is never acknowledged at all and
     * quiet waits forever (§3.4). */
    OSOE_AM_SEGMENT_ACK_REQUESTED = 1u << 0,
};

struct osoe_am_segment_header {
    uint64_t seq;           /* this segment's sequence number */
    uint64_t ack;           /* highest reverse-direction seq APPLIED, not merely
                             * received: completion means applied (§4.6) */
    uint32_t credits;       /* reply credits returned to the peer (§3.8) */
    uint32_t flags;
    uint32_t message_count;
    uint32_t total_len;     /* including this header */
};

_Static_assert(sizeof(struct osoe_am_segment_header) == 32, "§3.4 sizes the segment header at 16-32 bytes");

/*
 * Message header. Every message within a segment is padded to 8-byte
 * alignment: on aarch64 an unaligned 64-bit atomic faults, so an AMO payload at
 * an odd offset crashes rather than degrades (§3.4).
 */
struct osoe_message_header {
    uint16_t kind;          /* §3.13's compile-time handler enum */
    uint16_t flags;
    uint32_t len;           /* payload bytes after this header, before padding */
};

_Static_assert(sizeof(struct osoe_message_header) == 8, "a message header must be 8-byte sized");

#define OSOE_MESSAGE_ALIGN 8u

static inline uint32_t osoe_message_padded(uint32_t payload)
{
    uint32_t n = (uint32_t)sizeof(struct osoe_message_header) + payload;
    return (n + OSOE_MESSAGE_ALIGN - 1) & ~(OSOE_MESSAGE_ALIGN - 1);
}

/* The top bit of `head`. Every buffer that is not reservable carries it. */
#define OSOE_BUFFER_SEALED (UINT32_C(1) << 31)
#define OSOE_BUFFER_USED(h) ((h) & ~OSOE_BUFFER_SEALED)

/* No buffer is reservable, and "no buffer" generally. */
#define OSOE_CONNECTION_NONE UINT32_C(0xffffffff)

/* One buffer's shared words. */
struct osoe_connection_buffer {
    /* Reserved bytes in the low bits, the seal in the top bit. Writers CAS it;
     * the progress thread sets and clears the seal. */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t head;
    /* Bytes whose copy has completed: a count, not a watermark. */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t filled;
    /* Assigned when the buffer becomes reservable, published by the release
     * store on `open` below, read by writers after a successful reservation. */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t seq;
};

/*
 * A connection's shared state. No pointers: it is mapped at a different
 * address in every process that touches it.
 *
 * **There is deliberately no word naming the open buffer.** A writer tries each
 * buffer and the seal bit in `head` rejects the ones it may not have, so the
 * bit is the only thing deciding a reservation. An index would be the draft
 * §3.5 rejected in a new place - a writer reading it, then CASing a head the
 * progress thread sealed in between - and although the CAS would still fail, it
 * would leave the seal bit doing nothing that the index did not already do, and
 * §3.8's DRAINING guard rests on the bit alone. With two buffers the scan is
 * two loads on a path that then does a CAS and a memcpy.
 */
struct osoe_connection_shared {
    struct osoe_connection_buffer buffer[2];
    /*
     * The highest segment sequence any writer of this node needs the peer to
     * acknowledge (§3.4): "acknowledge through S". quiet and fence set it, and
     * the lead's agent reads it when it decides which segments carry
     * ACK_REQUESTED.
     *
     * It is shared because the *writer* is the one that needs the ack - an
     * application thread in any local PE - while the flag is written into a
     * segment header by the lead's agent, the only thread that finalizes
     * (§3.12). It is a sequence number and not a flag, because a flag cannot
     * say which segment it is for: taken at the top of a pass, it rode
     * whichever segment was finalized next, which could be one sealed before
     * the write the writer was waiting for (second review, C2). A monotone
     * maximum, raised by CAS and never lowered, so two writers asking at once
     * ask for the later of the two and the lead never has to take and clear it.
     *
     * 64-bit sequences do not wrap: at a segment a nanosecond it would take
     * five centuries. The comparisons are plain `<=` and say so.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t ack_wanted_through;
    /*
     * The highest segment sequence the peer has acknowledged **applying**
     * (§4.6), published by the lead's agent as it learns it.
     *
     * It is shared for the same reason ack_wanted_through is, from the other side:
     * the evidence quiet waits on has to be readable by the PE that is waiting,
     * and the lead's own copy of it lives in the lead's process. Single-writer
     * publication - only the lead's agent ever stores here - so a release store
     * against an acquire load, not a two-sided protocol (§3.16). A reader that
     * sees a stale value waits one more round, which is why this needs no
     * handshake of its own.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint64_t acked;
    /*
     * §3.8's reply credit, shared by the PEs of this node.
     *
     * It bounds what the *peer* must hold: the origin takes one when a local PE
     * issues a get or fetching AMO toward that peer and returns it when the
     * reply lands, so the peer's deferred reply queues never exceed
     * connections x C and scale with node count rather than job size. Shared
     * rather than per PE because the bound is per connection - which is also
     * why taking it is a CAS and not a test and a decrement (reply.h).
     */
    _Alignas(OSOE_CACHELINE) struct osoe_credit credit;
};

/* Process-private handle onto one connection. */
struct osoe_connection {
    struct osoe_connection_shared *s;
    unsigned char *data;        /* buffer k's bytes at data + k * capacity */
    uint32_t capacity;               /* bytes per buffer */
};

/* The progress thread's own bookkeeping, private to the lead (§3.5). Writers
 * see only the seal bit, so this never needs to be shared. */
enum osoe_buffer_state {
    OSOE_BUFFER_FREE = 0,
    OSOE_BUFFER_DRAINING,      /* only the progress thread may reserve (§3.8) */
    OSOE_BUFFER_OPEN,
    OSOE_BUFFER_SEALED_STATE,
    OSOE_BUFFER_SENDING,
};

/* A granted reservation. */
struct osoe_reservation {
    uint32_t buffer;           /* which buffer */
    uint32_t off;           /* byte offset within it */
    uint32_t len;           /* what was reserved, padded */
    uint64_t seq;           /* the segment this message will go out in */
};

enum osoe_reservation_status {
    OSOE_RESERVATION_OK,
    OSOE_RESERVATION_FULL,         /* the open buffer cannot fit this: ask for a seal */
    OSOE_RESERVATION_NONE,         /* nothing is reservable: wait (§3.11) */
};

/* ---- Lifecycle -------------------------------------------------------- */

/*
 * Put a connection's shared words into their starting state: **both buffers
 * sealed**, so nothing is reservable until the progress thread opens one.
 *
 * This is not optional bookkeeping. Zeroed shared memory leaves both heads at
 * 0, which is "unsealed and empty" - two reservable buffers, the very state
 * §3.5 forbids, before the progress thread has opened anything at all. Writers
 * would reserve into a buffer the progress thread believes is free, and it
 * would reset the head over their messages when it opened it. ftruncate zeroing
 * the segment is what makes this reachable, so the connection area calls this
 * for every connection before any PE can reach it.
 */
void osoe_connection_init(struct osoe_connection *c, uint32_t credits);

/* ---- Writer side: application threads, in any local PE ---------------- */

/*
 * Reserve `len` bytes, which the caller must have padded with osoe_message_padded.
 * On OSOE_RESERVATION_OK the caller copies into osoe_reservation_bytes() and then commits;
 * until it commits, the buffer cannot be sent, so the window must stay short
 * (§3.6's publication-window stall, which is accepted and instrumented rather
 * than fixed).
 */
enum osoe_reservation_status osoe_connection_reserve(struct osoe_connection *c, uint32_t len,
                                        struct osoe_reservation *r);

/* Where a granted reservation's bytes go. */
static inline void *osoe_reservation_bytes(const struct osoe_connection *c, const struct osoe_reservation *r)
{
    return c->data + (size_t)r->buffer * c->capacity + r->off;
}

/* Announce that the copy is complete. Until every reservation in a buffer has
 * been committed, the buffer is not sendable. */
void osoe_connection_commit(struct osoe_connection *c, const struct osoe_reservation *r);

/* Ask the peer to acknowledge every segment through `seq` (§3.4): raise
 * ack_wanted_through to at least `seq`. Called by an application thread in
 * fence and quiet with the highest segment it has written into; idempotent,
 * and a lower request than one already made changes nothing. */
void osoe_connection_request_ack(struct osoe_connection *c, uint64_t seq);

/* What this node's writers want acknowledged. The lead's agent, once a pass. */
static inline uint64_t osoe_connection_ack_wanted(const struct osoe_connection *c)
{
    return atomic_load_explicit(&c->s->ack_wanted_through, memory_order_acquire);
}

/* This connection's reply credit (§3.8), for the origin to take and give. */
static inline struct osoe_credit *osoe_connection_credit(struct osoe_connection *c)
{
    return &c->s->credit;
}

/* ---- Progress thread side: the lead's agent only ---------------------- */

/*
 * Make buffer k reservable with sequence number `seq`. The caller must know it
 * is free and that no other buffer is open: **the one-reservable rule is the
 * progress thread's to keep**, and nothing here enforces it, because the states
 * are its own bookkeeping (§3.5). With two reservable buffers, one PE's later
 * operation can be sealed and sent first, which breaks fence,
 * signal-after-payload (invariant 5) and quiet.
 */
void osoe_connection_open(struct osoe_connection *c, uint32_t k, uint64_t seq);
/* ... and it opens past whatever osoe_connection_drain_reserve has put there. */

/* Publish what the peer has acknowledged applying. The lead's agent only. */
void osoe_connection_publish_acked(struct osoe_connection *c, uint64_t seq);

/* Read it, from any local PE. */
static inline uint64_t osoe_connection_acked(const struct osoe_connection *c)
{
    return atomic_load_explicit(&c->s->acked, memory_order_acquire);
}

/*
 * Seal buffer k, so no further reservation can succeed in it. Sealing an empty
 * buffer is allowed and is how a flush with nothing to send retires it.
 */
void osoe_connection_seal(struct osoe_connection *c, uint32_t k);

/*
 * True once every reservation in buffer k has been committed, with the byte
 * count in *bytes. Never blocks: a writer may be mid-copy, and the caller must
 * move on to other connections and ask again (§3.5).
 */
bool osoe_connection_sendable(const struct osoe_connection *c, uint32_t k, uint32_t *bytes);

/* Return a sent buffer to free. It keeps the seal bit, so no writer can touch
 * it until it is opened again. */
void osoe_connection_release(struct osoe_connection *c, uint32_t k);

/*
 * Reserve in a buffer the writers cannot reach, for §3.8's starvation guard: a
 * released buffer, still sealed, in which only the progress thread may
 * reserve, before osoe_connection_open. Commit with osoe_connection_commit as
 * usual. Returns false if the buffer is full. The caller caps each drain at
 * half the buffer, or the guard starves the other direction (§3.8, review D8).
 * The reservation's `seq` is 0: the buffer gets its number when it is opened.
 */
bool osoe_connection_drain_reserve(struct osoe_connection *c, uint32_t k, uint32_t len,
                             struct osoe_reservation *r);

/* Bytes a buffer can hold below the segment header. */
static inline uint32_t osoe_connection_payload_capacity(const struct osoe_connection *c)
{
    return c->capacity - (uint32_t)sizeof(struct osoe_am_segment_header);
}

#endif /* OSOE_CONNECTION_H */
