#include <stdatomic.h>

#include "connection.h"

/*
 * §3.16's orders, applied here once so the rest reads as prose:
 *   - every load of a shared protocol word names acquire;
 *   - the reservation CAS and the `filled` accumulation name seq_cst, and
 *     compile to locked instructions on every target;
 *   - the progress thread's publication of a newly open buffer is a release
 *     store on `open`, which is what carries head, filled and seq with it.
 *
 * This protocol is single-writer publication for everything the progress
 * thread sets, and a locked read-modify-write for everything a writer sets, so
 * it is also safe under x86's store buffering without any extra fence.
 */

void osoe_connection_init(struct osoe_connection *c, uint32_t credits)
{
    for (uint32_t k = 0; k < 2; k++) {
        atomic_store_explicit(&c->s->buffer[k].filled, 0, memory_order_release);
        atomic_store_explicit(&c->s->buffer[k].seq, 0, memory_order_release);
        /* Sealed and empty: not reservable, and not mistakable for an open
         * buffer with nothing in it. */
        atomic_store_explicit(&c->s->buffer[k].head, OSOE_BUFFER_SEALED, memory_order_release);
    }
    atomic_store_explicit(&c->s->ack_wanted_through, 0, memory_order_release);
    atomic_store_explicit(&c->s->acked, 0, memory_order_release);
    c->s->acked_wait = (struct osoe_wait_word){ 0 };
    c->s->turnover_wait = (struct osoe_wait_word){ 0 };
    osoe_credit_init(&c->s->credit, credits);
}

enum osoe_reservation_status osoe_connection_reserve(struct osoe_connection *c, uint32_t len,
                                        struct osoe_reservation *r)
{
    struct osoe_connection_shared *s = c->s;
    for (;;) {
        /*
         * Find the reservable buffer by the seal bit and nothing else. At most
         * one is ever unsealed (§3.5), so this picks the only candidate; if
         * both carry the bit there is nothing to reserve and the caller waits
         * (§3.11).
         */
        uint32_t k = OSOE_CONNECTION_NONE, h = 0;
        for (uint32_t i = 0; i < 2; i++) {
            /* seq_cst: a writer that finds no room checks again here after
             * registering on turnover_wait, and this is that check - the load
             * half of the wait word's two-sided protocol (base/wait-word.h).
             * Free on x86-64, and the same ldar on aarch64's default target. */
            uint32_t v = atomic_load_explicit(&s->buffer[i].head, memory_order_seq_cst);
            if (!(v & OSOE_BUFFER_SEALED)) {
                k = i;
                h = v;
                break;
            }
        }
        if (k == OSOE_CONNECTION_NONE)
            return OSOE_RESERVATION_NONE;

        if (h + len > c->capacity)
            return OSOE_RESERVATION_FULL;      /* the caller asks for a seal, then retries */

        /*
         * A CAS, not a fetch_add: a writer that loses this race has changed
         * nothing and simply looks again. The seal bit is part of the compared
         * value, so a seal landing between the load above and this CAS makes it
         * fail rather than succeed into a buffer already on its way out.
         */
        if (atomic_compare_exchange_weak_explicit(&s->buffer[k].head, &h, h + len,
                                                  memory_order_seq_cst,
                                                  memory_order_acquire)) {
            r->buffer = k;
            r->off = h;
            r->len = len;
            /*
             * Read the sequence number after the reservation, with no extra
             * synchronization. This is sound, and the comment is here because
             * it looks like a race and someone will want to "fix" it with a
             * lock: the buffer cannot recycle until filled == head, and this
             * writer has not committed, so the number cannot change underneath.
             */
            r->seq = atomic_load_explicit(&s->buffer[k].seq, memory_order_acquire);
            return OSOE_RESERVATION_OK;
        }
    }
}

void osoe_connection_commit(struct osoe_connection *c, const struct osoe_reservation *r)
{
    atomic_fetch_add_explicit(&c->s->buffer[r->buffer].filled, r->len, memory_order_seq_cst);
}

void osoe_connection_open(struct osoe_connection *c, uint32_t k, uint64_t seq)
{
    struct osoe_connection_shared *s = c->s;
    /*
     * Nothing may reserve here yet: the buffer still carries the seal bit, so
     * every writer CAS on it fails. filled and seq are therefore published
     * first, and clearing the bit last is what hands the buffer over - a
     * release store, so a writer that acquires the head sees both.
     *
     * head starts past the segment header, which the progress thread fills at
     * seal time and no writer ever reserves (§3.4) - or past the replies the
     * pre-open drain already put here (§3.8), which are committed: the progress
     * thread wrote them itself. So filled and head open at the same extent, and
     * a writer's first reservation lands after the last reply rather than on it.
     */
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    uint32_t extent = OSOE_BUFFER_USED(atomic_load_explicit(&s->buffer[k].head,
                                                            memory_order_acquire));
    if (extent < header)
        extent = header;
    atomic_store_explicit(&s->buffer[k].filled, extent, memory_order_release);
    atomic_store_explicit(&s->buffer[k].seq, seq, memory_order_release);
    /* The hand-over, and the value writers waiting for room are waiting on:
     * seq_cst, then wake them if any registered (base/wait-word.h). A release
     * store here lets the agent read the count before its store is visible and
     * a writer check the old head - wait_word_model.py's release_level. */
    atomic_store_explicit(&s->buffer[k].head, extent, memory_order_seq_cst);
    osoe_wait_word_wake_all(&s->turnover_wait);
}

void osoe_connection_seal(struct osoe_connection *c, uint32_t k)
{
    /*
     * One locked instruction, and it is the whole seal. Every writer CAS
     * compares against a head value that this changes, so a reservation racing
     * the seal fails rather than landing in a buffer already on its way out -
     * which is the defect the draft protocol had, with its separate state field
     * read before the CAS.
     */
    atomic_fetch_or_explicit(&c->s->buffer[k].head, OSOE_BUFFER_SEALED, memory_order_seq_cst);
}

bool osoe_connection_sendable(const struct osoe_connection *c, uint32_t k, uint32_t *bytes)
{
    const struct osoe_connection_shared *s = c->s;
    uint32_t h = atomic_load_explicit(&s->buffer[k].head, memory_order_acquire);
    uint32_t f = atomic_load_explicit(&s->buffer[k].filled, memory_order_acquire);
    uint32_t used = OSOE_BUFFER_USED(h);
    /*
     * Equality, not >=. filled can never exceed the reserved count in a correct
     * run, and a strict comparison would hide the generation-crossing defect
     * the models call ACCT - a writer's commit landing in the next generation -
     * behind a test that still looked satisfied.
     */
    if (f != used)
        return false;
    *bytes = used;
    return true;
}

void osoe_connection_release(struct osoe_connection *c, uint32_t k)
{
    /*
     * Keep the seal bit. A freed buffer that cleared it would accept a writer
     * that had loaded `open` naming this buffer in the previous generation and
     * not yet run its CAS, which is the DRAINRES defect of §3.8's guard. The
     * bit comes off only in osoe_connection_open, together with `open`.
     */
    atomic_store_explicit(&c->s->buffer[k].head, OSOE_BUFFER_SEALED, memory_order_release);
    atomic_store_explicit(&c->s->buffer[k].filled, 0, memory_order_release);
}

bool osoe_connection_drain_reserve(struct osoe_connection *c, uint32_t k, uint32_t len,
                             struct osoe_reservation *r)
{
    struct osoe_connection_shared *s = c->s;
    /*
     * Only the progress thread runs here, on a buffer it has released and not
     * yet opened. What keeps writers out is the seal bit osoe_connection_release
     * left on it: every writer CAS against it fails, and a writer scanning for
     * an unsealed buffer skips it. So nothing here races, and plain release
     * stores are enough; the bit stays in the stored head, and only
     * osoe_connection_open takes it off.
     *
     * `filled` must count the same bytes `head` does, or the buffer is never
     * sendable (sendable is filled == used). A released buffer has head 0 and
     * filled 0; the first reservation starts past the segment header, so
     * filled starts there too - osoe_connection_commit then adds each message,
     * exactly as it does for a writer. The version before slice 2 counted the
     * messages and not the header, and left head=96 against filled=64 (second
     * review, atomics/drain-reserve-accounting.out); nothing called it, so
     * nothing noticed.
     */
    const uint32_t header = (uint32_t)sizeof(struct osoe_am_segment_header);
    uint32_t used = OSOE_BUFFER_USED(atomic_load_explicit(&s->buffer[k].head,
                                                          memory_order_acquire));
    if (used < header) {
        used = header;
        atomic_store_explicit(&s->buffer[k].filled, header, memory_order_release);
    }
    if (used + len > c->capacity)
        return false;
    atomic_store_explicit(&s->buffer[k].head, (used + len) | OSOE_BUFFER_SEALED,
                          memory_order_release);
    r->buffer = k;
    r->off = used;
    r->len = len;
    r->seq = 0;             /* not yet assigned: osoe_connection_open does that */
    return true;
}

void osoe_connection_request_ack(struct osoe_connection *c, uint64_t seq)
{
    /* A monotone maximum: CAS, seq_cst, and nothing to do once someone has
     * already asked for as much (§3.16; ack_model.py's design variant). */
    uint64_t have = atomic_load_explicit(&c->s->ack_wanted_through, memory_order_acquire);
    while (have < seq &&
           !atomic_compare_exchange_weak_explicit(&c->s->ack_wanted_through, &have, seq,
                                                  memory_order_seq_cst,
                                                  memory_order_acquire))
        ;
}

void osoe_connection_publish_acked(struct osoe_connection *c, uint64_t seq)
{
    /* seq_cst, then the count: a quiet that registered after the count was
     * loaded checks acked after this store (base/wait-word.h). */
    atomic_store_explicit(&c->s->acked, seq, memory_order_seq_cst);
    osoe_wait_word_wake_all(&c->s->acked_wait);
}
