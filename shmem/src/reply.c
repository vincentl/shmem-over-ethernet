/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <limits.h>
#include <stdatomic.h>

#include "base/futex.h"
#include "fatal.h"
#include "reply.h"

void osoe_reply_pool_init(struct osoe_reply_pool *pool, struct osoe_reply *slots,
                          uint32_t count)
{
    pool->slot = slots;
    pool->count = count;
    pool->in_use = 0;
    pool->high_water = 0;
    /* One free list through `next`, built in order so that a fresh pool hands
     * out ascending indices - which makes a trace readable and costs nothing. */
    for (uint32_t i = 0; i + 1 < count; i++)
        slots[i].next = i + 1;
    if (count > 0)
        slots[count - 1].next = OSOE_REPLY_NONE;
    pool->free = count > 0 ? 0 : OSOE_REPLY_NONE;
}

uint32_t osoe_reply_take(struct osoe_reply_pool *pool)
{
    uint32_t i = pool->free;
    if (i == OSOE_REPLY_NONE)
        return OSOE_REPLY_NONE;     /* fallible on purpose (§3.7, §3.8) */
    pool->free = pool->slot[i].next;
    pool->slot[i].next = OSOE_REPLY_NONE;
    pool->in_use++;
    if (pool->in_use > pool->high_water)
        pool->high_water = pool->in_use;
    return i;
}

void osoe_reply_give(struct osoe_reply_pool *pool, uint32_t index)
{
    if (index >= pool->count)
        osoe_fatal("reply descriptor %u is outside a pool of %u", index, pool->count);
    if (pool->in_use == 0)
        osoe_fatal("reply descriptor %u returned to an empty pool", index);
    pool->slot[index].next = pool->free;
    pool->free = index;
    pool->in_use--;
}

void osoe_reply_queues_init(struct osoe_reply_queues *q)
{
    *q = (struct osoe_reply_queues){ 0 };
    q->control.head = q->control.tail = OSOE_REPLY_NONE;
    q->bulk.head = q->bulk.tail = OSOE_REPLY_NONE;
}

void osoe_reply_push(struct osoe_reply_pool *pool, struct osoe_reply_queue *q,
                     uint32_t index)
{
    pool->slot[index].next = OSOE_REPLY_NONE;
    if (q->tail == OSOE_REPLY_NONE)
        q->head = index;
    else
        pool->slot[q->tail].next = index;
    q->tail = index;
    q->depth++;
    if (q->depth > q->high_water)
        q->high_water = q->depth;
}

uint32_t osoe_reply_peek(const struct osoe_reply_queue *q)
{
    return q->head;
}

uint32_t osoe_reply_pop(struct osoe_reply_pool *pool, struct osoe_reply_queue *q)
{
    uint32_t i = q->head;
    if (i == OSOE_REPLY_NONE)
        return OSOE_REPLY_NONE;
    q->head = pool->slot[i].next;
    if (q->head == OSOE_REPLY_NONE)
        q->tail = OSOE_REPLY_NONE;
    pool->slot[i].next = OSOE_REPLY_NONE;
    q->depth--;
    return i;
}

void osoe_reply_rotate(struct osoe_reply_pool *pool, struct osoe_reply_queue *q)
{
    /* A queue of one is already its own rotation; doing the work anyway would
     * unlink and relink the same node, which is correct but reads as a bug. */
    if (q->head == OSOE_REPLY_NONE || q->head == q->tail)
        return;
    uint32_t i = q->head;
    q->head = pool->slot[i].next;
    pool->slot[i].next = OSOE_REPLY_NONE;
    pool->slot[q->tail].next = i;
    q->tail = i;
}

/*
 * The credit's word (reply.h): three 21-bit fields. Tickets count modulo 2^21,
 * and at most one per local PE is ever outstanding - a PE waits for one credit
 * at a time - so a ticket is served once the served count has passed it by less
 * than half the range.
 */
enum { FIELD_BITS = 21 };
#define FIELD_MASK ((UINT64_C(1) << FIELD_BITS) - 1)

static uint32_t available_of(uint64_t s)
{
    return (uint32_t)(s & FIELD_MASK);
}

static uint32_t next_of(uint64_t s)
{
    return (uint32_t)((s >> FIELD_BITS) & FIELD_MASK);
}

static uint32_t served_of(uint64_t s)
{
    return (uint32_t)((s >> (2 * FIELD_BITS)) & FIELD_MASK);
}

static uint64_t pack(uint32_t available, uint32_t next, uint32_t served)
{
    return (uint64_t)available | ((uint64_t)(next & FIELD_MASK) << FIELD_BITS) |
           ((uint64_t)(served & FIELD_MASK) << (2 * FIELD_BITS));
}

bool osoe_credit_take(struct osoe_credit *c, uint32_t *ticket)
{
    uint64_t s = atomic_load_explicit(&c->state, memory_order_acquire);
    for (;;) {
        uint32_t available = available_of(s), next = next_of(s), served = served_of(s);
        uint64_t want = available > 0 ? pack(available - 1, next, served)
                                      : pack(available, next + 1, served);
        if (atomic_compare_exchange_weak_explicit(&c->state, &s, want, memory_order_seq_cst,
                                                  memory_order_acquire)) {
            if (available > 0)
                return true;
            *ticket = next;
            return false;
        }
    }
}

bool osoe_credit_served(const struct osoe_credit *c, uint32_t ticket)
{
    uint32_t served = served_of(atomic_load_explicit(&c->state, memory_order_acquire));
    uint32_t past = (served - ticket) & (uint32_t)FIELD_MASK;
    return past != 0 && past <= (uint32_t)(FIELD_MASK / 2);
}

void osoe_credit_give(struct osoe_credit *c)
{
    uint64_t s = atomic_load_explicit(&c->state, memory_order_acquire);
    for (;;) {
        uint32_t available = available_of(s), next = next_of(s), served = served_of(s);
        bool queued = next != served;
        uint64_t want = queued ? pack(available, next, served + 1)
                               : pack(available + 1, next, served);
        if (atomic_compare_exchange_weak_explicit(&c->state, &s, want, memory_order_seq_cst,
                                                  memory_order_acquire)) {
            if (!queued)
                return;
            /* Served first, then the word, then the wake: a waiter woken by
             * the bump, or finding it before it sleeps, sees its ticket served
             * (credit_wait_model.py's bump_first loses a wake). */
            atomic_fetch_add_explicit(&c->wake, 1, memory_order_seq_cst);
            osoe_futex_wake_bitset(&c->wake, INT_MAX, osoe_credit_bit(served),
                                   OSOE_FUTEX_SHARED);
            return;
        }
    }
}

uint32_t osoe_credit_available(const struct osoe_credit *c)
{
    return available_of(atomic_load_explicit(&c->state, memory_order_acquire));
}

uint32_t osoe_credit_waiting(const struct osoe_credit *c)
{
    uint64_t s = atomic_load_explicit(&c->state, memory_order_acquire);
    return (next_of(s) - served_of(s)) & (uint32_t)FIELD_MASK;
}

uint32_t osoe_credit_served_count(const struct osoe_credit *c)
{
    return served_of(atomic_load_explicit(&c->state, memory_order_acquire));
}
