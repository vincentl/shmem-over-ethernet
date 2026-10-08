/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Deferred reply queues and the per-connection credit (§3.8).
 *
 * Pure logic and one atomic, so tier 1. What it pins down is the set of
 * properties §3.8 argues for separately, each of which is easy to lose:
 *
 *   BOUND      the pool never hands out more than it holds, and says so rather
 *              than wrapping or returning a stale index
 *   ORDER      a queue is FIFO, so replies to one PE keep their order
 *   ROTATION   the bulk queue rotates, so one large reply cannot block the
 *              small ones behind it
 *   PRIORITY   the value queue drains to empty before the bulk queue gets
 *              the remaining space
 *   CHUNKS     a descriptor survives partial draining and pops only when it has
 *              sent everything it owed
 *   CREDIT     a credit taken is a credit unavailable, and the count never goes
 *              negative under contention
 *
 * The last is the one the model found missing from §3.8, so it is checked here
 * against the real CAS as well.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "check.h"
#include "reply.h"

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

enum { POOL = 8 };

static struct osoe_reply slots[POOL];
static struct osoe_reply_pool pool;

/* Queue contents as a string of origin PEs, so order is checkable at a glance. */
static void contents(const struct osoe_reply_queue *q, char *out, size_t len)
{
    size_t off = 0;
    out[0] = '\0';
    for (uint32_t i = q->head; i != OSOE_REPLY_NONE && off + 4 < len;
         i = slots[i].next)
        off += (size_t)snprintf(out + off, len - off, off ? ",%u" : "%u",
                                slots[i].origin_pe);
}

static uint32_t enqueue(struct osoe_reply_queue *q, uint32_t pe, uint32_t length)
{
    uint32_t i = osoe_reply_take(&pool);
    CHECK(i != OSOE_REPLY_NONE);
    if (i == OSOE_REPLY_NONE)
        return i;
    struct osoe_reply *r = osoe_reply_at(&pool, i);
    r->kind = OSOE_REPLY_GET;
    r->origin_pe = pe;
    r->length = length;
    r->sent = 0;
    osoe_reply_push(&pool, q, i);
    return i;
}

/*
 * One credit, not three, and that is the whole reason this case works. With
 * three the exhausted state is rare - threads release instantly, so two of them
 * almost never read the last credit at the same moment - and a test-then-
 * decrement take passed 6 runs of 6. With one, exhaustion is the common case:
 * the window between the load and the decrement opens each time the queue of
 * tickets drains, which idle is often, and the same mutation fails 6 of 6. A
 * contention test that does not contend proves nothing, and the difference
 * here is one constant.
 */
enum { THREADS = 4, ROUNDS = 50000, CREDITS = 1 };
static struct osoe_credit shared_credit;
static _Atomic uint32_t concurrently_held;
static _Atomic uint32_t over_limit;

/* Take, hold briefly, return. A credit handed out twice shows as more holders
 * than credits - which is exactly the over-subscription the peer's pool would
 * suffer, on a node with no way to detect it (§3.8's distributed invariant).
 *
 * A take that finds none free joins the queue, and the credit is this
 * thread's once its ticket is served. It watches for that with plain loads
 * for a while, then sleeps on the credit's word as the library does
 * (transport-internode.c's take_credit): the word first, then the ticket, then a
 * sleep on the ticket's bit, swept like every wait. The watch is what keeps
 * the race open - a waiter that slept at once kept the queue standing, the
 * free credit was seldom contended, and a test-then-decrement take passed 9
 * runs of 10. And it watches with loads, not sched_yield, as it once did:
 * under eight busy loops on four cores, two runs in ten of that stalled past
 * 120 s at 0.1% of a CPU, the yielding waiters starved behind the loops rather
 * than spinning (the release review's K3) - each yield pushes the yielder's
 * deadline back a slice on Linux's EEVDF scheduler, and a sleep is what resets
 * it. As it is, idle, the mutant fails 50 runs of 50 in the release build -
 * two of them by hanging until the timeout - and every one of 20 in each
 * other preset; and under the busy loops every run ends, within about 1 s
 * (within 32 s under ThreadSanitizer, of its 60). There the mutant is often
 * missed - caught in 58 runs of 90 in release and 6 of 20 in debug, though
 * every time under asan and tsan: the loops keep the queue standing. So this
 * guards the suite's idle run, as the suite runs (Part 6's adversarial read,
 * 2026-10-03). */
enum { WATCH = 10000 };

static void *hammer(void *unused)
{
    (void)unused;
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t ticket;
        if (!osoe_credit_take(&shared_credit, &ticket))
            for (int watched = 0;; watched++) {
                uint32_t seen = atomic_load_explicit(&shared_credit.wake,
                                                     memory_order_acquire);
                if (osoe_credit_served(&shared_credit, ticket))
                    break;
                if (watched < WATCH)
                    continue;
                struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
                osoe_futex_wait_bitset(&shared_credit.wake, seen, osoe_credit_bit(ticket),
                                       &sweep, OSOE_FUTEX_SHARED);
            }
        uint32_t held = atomic_fetch_add(&concurrently_held, 1) + 1;
        if (held > CREDITS)
            atomic_fetch_add(&over_limit, 1);
        atomic_fetch_sub(&concurrently_held, 1);
        osoe_credit_give(&shared_credit);
    }
    return NULL;
}

int main(void)
{
    char buf[64];
    struct osoe_reply_queues q;

    /* ---- BOUND ---- */
    osoe_reply_pool_init(&pool, slots, POOL);
    osoe_reply_queues_init(&q);
    CHECK(pool.in_use == 0 && pool.high_water == 0);

    uint32_t held[POOL];
    for (uint32_t i = 0; i < POOL; i++) {
        held[i] = osoe_reply_take(&pool);
        CHECK(held[i] != OSOE_REPLY_NONE);
        CHECK(held[i] < POOL);
    }
    /* An exhausted pool says so. §3.8 makes reservation fallible on purpose,
     * so that a handler never has to block and §3.7's rule stays trivially
     * satisfiable. */
    CHECK(osoe_reply_take(&pool) == OSOE_REPLY_NONE);
    CHECK(pool.in_use == POOL);
    CHECK(pool.high_water == POOL);

    /* Every index distinct: a pool that handed one out twice would corrupt two
     * replies at once and is exactly what the free list can get wrong. */
    for (uint32_t i = 0; i < POOL; i++)
        for (uint32_t j = 0; j < i; j++)
            CHECK(held[i] != held[j]);

    for (uint32_t i = 0; i < POOL; i++)
        osoe_reply_give(&pool, held[i]);
    CHECK(pool.in_use == 0);
    CHECK(pool.high_water == POOL);      /* the mark stays: it is the evidence */

    /* ---- ORDER ---- */
    osoe_reply_pool_init(&pool, slots, POOL);
    osoe_reply_queues_init(&q);
    for (uint32_t pe = 1; pe <= 4; pe++)
        enqueue(&q.bulk, pe, 100);
    contents(&q.bulk, buf, sizeof buf);
    CHECK(strcmp(buf, "1,2,3,4") == 0);
    CHECK(q.bulk.depth == 4 && q.bulk.high_water == 4);

    /* ---- ROTATION ---- */
    osoe_reply_rotate(&pool, &q.bulk);
    contents(&q.bulk, buf, sizeof buf);
    CHECK(strcmp(buf, "2,3,4,1") == 0);
    osoe_reply_rotate(&pool, &q.bulk);
    osoe_reply_rotate(&pool, &q.bulk);
    osoe_reply_rotate(&pool, &q.bulk);
    contents(&q.bulk, buf, sizeof buf);
    CHECK(strcmp(buf, "1,2,3,4") == 0);  /* four rotations is the identity */

    /* Rotating a queue of one, and an empty one, must not corrupt it: both are
     * their own rotation, and a list unlinked and relinked in place is where an
     * off-by-one lives. */
    while (osoe_reply_peek(&q.bulk) != OSOE_REPLY_NONE)
        osoe_reply_give(&pool, osoe_reply_pop(&pool, &q.bulk));
    osoe_reply_rotate(&pool, &q.bulk);
    CHECK(osoe_reply_peek(&q.bulk) == OSOE_REPLY_NONE);
    enqueue(&q.bulk, 9, 10);
    osoe_reply_rotate(&pool, &q.bulk);
    contents(&q.bulk, buf, sizeof buf);
    CHECK(strcmp(buf, "9") == 0);
    CHECK(q.bulk.depth == 1);
    osoe_reply_give(&pool, osoe_reply_pop(&pool, &q.bulk));

    /* ---- PRIORITY and CHUNKS ---- */
    osoe_reply_pool_init(&pool, slots, POOL);
    osoe_reply_queues_init(&q);
    /* One large get reply, and an AMO's result arriving behind it. */
    uint32_t big = enqueue(&q.bulk, 7, 1000);
    uint32_t small = enqueue(&q.bulk, 8, 40);
    uint32_t value = osoe_reply_take(&pool);
    CHECK(value != OSOE_REPLY_NONE);
    osoe_reply_at(&pool, value)->kind = OSOE_REPLY_AMO;
    osoe_reply_at(&pool, value)->origin_pe = 3;
    osoe_reply_at(&pool, value)->length = 8;
    osoe_reply_push(&pool, &q.values, value);

    /*
     * The drain: values to empty first, then bulk round-robin at one chunk
     * each. A thread is blocked on the value, so a result behind a 1 MiB get
     * reply stalls it for no reason - which is the case this ordering exists
     * for, and the case a single FIFO would get wrong. (The barrier's forwards,
     * once the reason, have a wire of their own: progress.h.)
     */
    uint32_t drained_values = 0;
    while (osoe_reply_peek(&q.values) != OSOE_REPLY_NONE) {
        uint32_t i = osoe_reply_pop(&pool, &q.values);
        drained_values++;
        osoe_reply_give(&pool, i);
    }
    CHECK(drained_values == 1);
    CHECK(q.values.depth == 0);
    CHECK(osoe_reply_peek(&q.bulk) == big);    /* bulk untouched meanwhile */

    /* Now the bulk queue, 100 bytes per pass. The small reply must finish long
     * before the large one, which is what rotation buys. */
    enum { CHUNK = 100 };
    uint32_t passes = 0, small_done = 0, big_done = 0;
    while (osoe_reply_peek(&q.bulk) != OSOE_REPLY_NONE) {
        uint32_t i = osoe_reply_peek(&q.bulk);
        struct osoe_reply *r = osoe_reply_at(&pool, i);
        uint32_t n = r->length - r->sent < CHUNK ? r->length - r->sent : CHUNK;
        r->sent += n;
        passes++;
        if (r->sent == r->length) {          /* pop only when it owes nothing */
            osoe_reply_pop(&pool, &q.bulk);
            if (i == small)
                small_done = passes;
            if (i == big)
                big_done = passes;
            osoe_reply_give(&pool, i);
        } else {
            osoe_reply_rotate(&pool, &q.bulk);
        }
    }
    CHECK(small_done > 0 && big_done > 0);
    CHECK(small_done < big_done);            /* ROTATION, as a consequence */
    CHECK(small_done == 2);                  /* second pass: 1000 then 40 */
    CHECK(pool.in_use == 0);

    /* ---- CREDIT ---- */
    struct osoe_credit credit;
    /* Zero, so that a take that wrongly succeeds - and leaves its ticket
     * unwritten - fails the checks below rather than reading garbage (Clang's
     * -Wsometimes-uninitialized, once shmem.strict-c11 compiled with it). */
    uint32_t ticket = 0, first = 0, second = 0;
    osoe_credit_init(&credit, 3);
    CHECK(osoe_credit_available(&credit) == 3);
    CHECK(osoe_credit_take(&credit, &ticket) && osoe_credit_take(&credit, &ticket) &&
          osoe_credit_take(&credit, &ticket));
    CHECK(osoe_credit_available(&credit) == 0);
    /* Exhausted queues the issuer rather than failing the call (§4.6)... */
    CHECK(!osoe_credit_take(&credit, &first) && !osoe_credit_take(&credit, &second));
    CHECK(first != second && osoe_credit_waiting(&credit) == 2);
    CHECK(!osoe_credit_served(&credit, first) && !osoe_credit_served(&credit, second));
    /* ...and a give goes to the oldest ticket, never through the pool, so
     * nothing arriving meanwhile can take it (§3.11's handoff). */
    osoe_credit_give(&credit);
    CHECK(osoe_credit_served(&credit, first) && !osoe_credit_served(&credit, second));
    CHECK(osoe_credit_available(&credit) == 0 && !osoe_credit_take(&credit, &ticket));
    osoe_credit_give(&credit);
    CHECK(osoe_credit_served(&credit, second) && !osoe_credit_served(&credit, ticket));
    osoe_credit_give(&credit);
    CHECK(osoe_credit_served(&credit, ticket) && osoe_credit_waiting(&credit) == 0);
    /* With nobody queued, a give returns the credit to the pool. */
    osoe_credit_give(&credit);
    CHECK(osoe_credit_available(&credit) == 1);
    CHECK(osoe_credit_take(&credit, &ticket));
    CHECK(osoe_credit_available(&credit) == 0);

    /*
     * Under contention, which is the only configuration where the credit binds
     * at all. The credit is per *connection*, so the PEs of a node share it;
     * the model reports every wrong rule as correct at one PE per node, because
     * one PE cannot outrun a count only it touches. Threads stand in for the
     * PEs here - the word is in shared memory in the real thing, and the
     * instruction is the same either way.
     *
     * Each thread takes and returns repeatedly. What must hold is that the
     * count never exceeds what was issued and never goes below zero, and that
     * every credit taken is one no other thread holds: the sum at the end is
     * the sum at the start, and no take ever succeeded past the limit.
     */
    osoe_credit_init(&shared_credit, CREDITS);

    /* A failed create leaves th[i] undefined, and every th[] is joined below
     * unconditionally - joining an undefined pthread_t is not survivable. */
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        CHECK_FATAL(pthread_create(&th[i], NULL, hammer, NULL) == 0);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);

    CHECK(atomic_load(&over_limit) == 0);
    CHECK(atomic_load(&concurrently_held) == 0);
    CHECK(osoe_credit_available(&shared_credit) == CREDITS);
    CHECK(osoe_credit_waiting(&shared_credit) == 0);

    return CHECK_DONE();
}
