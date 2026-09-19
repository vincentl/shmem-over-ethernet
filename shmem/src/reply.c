#include <stdatomic.h>

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

void osoe_credit_init(struct osoe_credit *c, uint32_t credits)
{
    atomic_store_explicit(&c->available, credits, memory_order_release);
}

bool osoe_credit_take(struct osoe_credit *c)
{
    /*
     * A CAS loop. With a test and a decrement, two PEs of this node can both
     * see the last credit and both take it: the count goes negative and the
     * peer's descriptor pool is over-subscribed by the difference, on a node
     * that has no way to detect it. §3.5 makes the same argument for the
     * reservation, and the `race_take` case of the credit model is this.
     */
    for (;;) {
        uint32_t have = atomic_load_explicit(&c->available, memory_order_acquire);
        if (have == 0)
            return false;
        if (atomic_compare_exchange_weak_explicit(&c->available, &have, have - 1,
                                                  memory_order_seq_cst,
                                                  memory_order_acquire))
            return true;
    }
}

void osoe_credit_give(struct osoe_credit *c)
{
    atomic_fetch_add_explicit(&c->available, 1, memory_order_seq_cst);
}

uint32_t osoe_credit_available(const struct osoe_credit *c)
{
    return atomic_load_explicit(&c->available, memory_order_acquire);
}
