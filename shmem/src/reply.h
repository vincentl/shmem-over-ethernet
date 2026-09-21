/*
 * Deferred reply queues and the per-connection credit (§3.8, §4.6).
 *
 * A handler may enqueue, but never reserve (§3.7). So a get or fetching AMO
 * arriving at the target does not build its reply into an outbound buffer: it
 * takes a **descriptor**, and the progress thread drains descriptors into
 * buffers later, when there is room. Reserving reply *payload* space would
 * commit about 4 KiB per outstanding get; a descriptor commits 48 bytes, which
 * at 250,000 outstanding replies is the difference between roughly a gigabyte
 * and roughly 12 MiB. The reply's source data is already in the symmetric heap
 * and needs no copy until send time.
 *
 * **Sampling differs by type, and the two must not be unified.** A fetching AMO
 * is applied at handler time, atomically, and its descriptor carries the
 * resulting *value*: it cannot be deferred, because it modifies the target and
 * that modification is ordered on the stream against every later operation
 * touching the location - deferring it would reorder what the stream ordered. A
 * get reply carries a *reference*, sampled at drain time, which is what makes
 * the memory saving above possible and what §4.6's deferred-sampling hazard is
 * about.
 *
 * **Two queues, drained control-first.** Barrier latency gates the whole job, so
 * a barrier forward stuck behind a 1 MiB get reply stalls every PE. The control
 * queue is small and drains to empty; the bulk queue then gets the remaining
 * space, round-robin, one chunk per descriptor per pass. Strict FIFO there would
 * let a 1 MiB get block roughly 8 ms of small replies behind it, and get replies
 * to different PEs have no mutual ordering requirement, so rotation is legal.
 *
 * **Bounded by a per-connection credit, not a per-PE count.** An earlier draft
 * sized each connection's queues as P x G - every PE on the peer node times a
 * per-PE outstanding-get cap - which grows with *job* size: about 6 GB per node
 * at 500 nodes. Instead each node pair has a credit C. The origin takes one when
 * a PE issues a get or fetching AMO toward that peer and returns it **when the
 * reply lands**; with none left the issuing PE blocks, which the spec permits
 * (§4.6). The target's worst case is then `connections x C`, which scales with
 * node count as §1 requires.
 *
 * That bound is a **distributed invariant**: it is enforced at the origin, so a
 * peer's accounting bug overflows *this* node's pool and the symptom appears on
 * the wrong node. Overflow is therefore a hard assertion naming the peer, not a
 * silent drop, and the high-water mark is exported so the counter history says
 * which invariant broke (§3.8).
 *
 * The pool and the queues are private to the lead's progress thread, which is
 * the only thread that touches them (§3.12). The credit is not: the PEs of a
 * node share it, so it lives in the connection's shared state and is taken with
 * a CAS. A test-then-decrement lets two PEs of one node both take the last
 * credit and over-subscribe the peer by exactly the amount nobody accounted
 * for - the `race_take` case of docs/design/evidence/am-backend/credit_model.py,
 * which is how the rule was found missing from §3.8.
 */
#ifndef OSOE_REPLY_H
#define OSOE_REPLY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "transport.h"

/* No descriptor. */
#define OSOE_REPLY_NONE UINT32_C(0xffffffff)

/*
 * The collective forwards one connection can have outstanding per team: the
 * tree's degree as one edge sees it (§3.8, argument 2). The internode phase is
 * a star today - one arrival toward the root, one release back, per team - so
 * one. The tree of slice 3 sets it to its fan-out plus one.
 */
#define OSOE_COLLECTIVE_DEGREE 1u

/*
 * Descriptors per connection beyond the two arguments of §3.8. Zero: every
 * descriptor is accounted for by one of them, and a margin would only postpone
 * the fatal that names the argument that broke.
 */
#define OSOE_REPLY_SLACK 0u

enum osoe_reply_kind {
    /* A fetching AMO result, applied at handler time. Carries the value. */
    OSOE_REPLY_AMO = 0,
    /* A get reply, sampled at drain time. Carries a reference. */
    OSOE_REPLY_GET,
    /*
     * A collective's release, forwarded by the root node to each participant
     * (§5.3). It rides the control queue with the AMO results, for the reason
     * §3.8 gives: barrier latency gates the whole job, so a forward must not
     * queue behind a large get reply.
     */
    OSOE_REPLY_RELEASE,
    /*
     * This node's arrival, forwarded to the collective's root (§5.3). It is a
     * descriptor for the same reason the release is: the lead's agent is both
     * the thread that learns of the arrival and the thread that seals the
     * buffer, so reserving for it inline would be waiting on space only this
     * thread frees. Deferring costs a pass and cannot deadlock (§3.7).
     */
    OSOE_REPLY_ARRIVE,
};

/*
 * One deferred reply. §3.8 sizes this at 48 bytes and the whole memory argument
 * rests on that, so the static assert below is part of the design rather than
 * tidiness.
 */
struct osoe_reply {
    uint32_t next;          /* pool index: free list, then queue link */
    uint16_t kind;          /* enum osoe_reply_kind */
    uint16_t region;        /* enum osoe_region, for a get reply */
    uint32_t origin_pe;     /* the world rank waiting for this */
    uint32_t target_pe;     /* whose memory the bytes come from: a get reply is
                             * a reference, sampled at drain time (§3.8), so the
                             * descriptor must say where to read */
    uint32_t length;        /* total bytes owed */
    uint32_t sent;          /* drained so far; the descriptor pops when equal */
    uint64_t offset;        /* get: where in the target's region the bytes are */
    uint64_t value;         /* amo: the result, already applied */
    uint64_t token;         /* the origin's completion slot */
};

_Static_assert(sizeof(struct osoe_reply) == 48, "§3.8's memory argument sizes a descriptor at 48 bytes");

/* The node-wide pool. Every connection's queues link into it, so an idle
 * connection costs nothing (§3.8). */
struct osoe_reply_pool {
    struct osoe_reply *slot;
    uint32_t count;
    uint32_t free;          /* head of the free list */
    uint32_t in_use;
    uint32_t high_water;    /* exported, so the counter history names the break */
};

/* One queue: a singly linked list through the pool, with a tail so that
 * enqueue and rotate are O(1). */
struct osoe_reply_queue {
    uint32_t head, tail;
    uint32_t depth, high_water;
};

/* Both queues of one connection. */
struct osoe_reply_queues {
    struct osoe_reply_queue control;    /* AMO results, collective forwards */
    struct osoe_reply_queue bulk;       /* get reply chunks */
};

/* ---- The pool -------------------------------------------------------- */

void osoe_reply_pool_init(struct osoe_reply_pool *pool, struct osoe_reply *slots,
                          uint32_t count);

/*
 * Take a descriptor, or OSOE_REPLY_NONE when the pool is empty. It returns
 * rather than blocks, so a handler never waits on a resource only its own
 * thread frees (§3.7).
 *
 * An empty pool is not a condition to retry: by the time a handler asks, the
 * parser has consumed the request, so there is nothing left to leave in place
 * (an earlier version of this comment said otherwise, and the caller dropped
 * the request on the strength of it - second review, D1). The pool is sized so
 * that no legal program can empty it (progress-host.c, §3.8), which makes NONE
 * a broken invariant - usually the peer's credit accounting - and the progress
 * layer ends the job naming the peer.
 */
uint32_t osoe_reply_take(struct osoe_reply_pool *pool);
void osoe_reply_give(struct osoe_reply_pool *pool, uint32_t index);

static inline struct osoe_reply *osoe_reply_at(struct osoe_reply_pool *pool, uint32_t index)
{
    return &pool->slot[index];
}

/* ---- The queues ------------------------------------------------------ */

void osoe_reply_queues_init(struct osoe_reply_queues *q);

/* Enqueue at the tail. */
void osoe_reply_push(struct osoe_reply_pool *pool, struct osoe_reply_queue *q,
                     uint32_t index);

/* The head, without removing it, or OSOE_REPLY_NONE. */
uint32_t osoe_reply_peek(const struct osoe_reply_queue *q);

/* Remove and return the head. */
uint32_t osoe_reply_pop(struct osoe_reply_pool *pool, struct osoe_reply_queue *q);

/*
 * Move the head to the tail: one chunk per descriptor per pass, which is what
 * keeps a large get reply from blocking the small ones behind it (§3.8).
 */
void osoe_reply_rotate(struct osoe_reply_pool *pool, struct osoe_reply_queue *q);

/* ---- The credit ------------------------------------------------------ */

/*
 * The origin's credits for one connection, shared by the node's PEs.
 *
 * Take is a CAS loop, not a test and a decrement: the two steps are not one, so
 * two PEs of a node can both pass the test and both decrement, and the count
 * goes negative while the peer's pool is over-subscribed by the difference.
 * Same argument §3.5 makes for the reservation itself.
 */
struct osoe_credit {
    _Atomic uint32_t available;
};

/* Set the count before anyone else can see the connection. Inline, so that
 * connection.c can start a credit without depending on this file's object -
 * the tier-1 tests of the connection compile it on its own. */
static inline void osoe_credit_init(struct osoe_credit *c, uint32_t credits)
{
    atomic_store_explicit(&c->available, credits, memory_order_release);
}

/* True if a credit was taken. False means none is free, and §4.6 permits the
 * issuing PE to block until one is. */
bool osoe_credit_take(struct osoe_credit *c);

/* Return one. Called when the reply **lands**, not when it is sent: the two
 * differ only once gets are nonblocking, and the difference is the origin's
 * landing zone rather than the target's pool (§3.12, and the on_drain case of
 * the credit model). */
void osoe_credit_give(struct osoe_credit *c);

uint32_t osoe_credit_available(const struct osoe_credit *c);

#endif /* OSOE_REPLY_H */
