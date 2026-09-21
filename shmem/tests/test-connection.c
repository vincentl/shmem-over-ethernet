/*
 * The reservation protocol in C (§3.4, §3.5).
 *
 * The protocol is settled by two exhaustive models, whose `fixed` variant is
 * what shmem/src/conn.c implements. What a model cannot check is whether the C
 * says what the model said, so this runs the real code with real threads and
 * asserts the same properties the models do, by name:
 *
 *   ONCE      every message reaches the wire exactly once
 *   ORDER     one writer's messages reach it in issue order
 *   SEQ       segment sequence numbers do not go backwards
 *   STALESEQ  the sequence number a writer read after reserving is the one of
 *             the segment its message actually went out in - the property
 *             quiet's high_seg depends on, and the reason §3.5 assigns the
 *             number when a buffer becomes reservable rather than at seal
 *   ACCT      filled never exceeds the reserved count
 *
 * Writers here are threads rather than processes. That is the shape the tsan
 * preset can instrument, and §8.2 is explicit that it buys only this: TSan
 * instruments one process, so it sees these writers against this progress
 * thread and would see nothing of the cross-process case. The protocol's
 * cross-process safety rests on §3.16's orders and on the models, not here.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "connection.h"

enum { WRITERS = 4, PER_WRITER = 200, CAP = 512, TOTAL = WRITERS * PER_WRITER };

/*
 * §3.8's starvation guard, as flush() runs it: immediately before a free
 * buffer is opened, while it still carries the seal bit, the progress thread
 * drains a reply into it, commits it, and opens the buffer past it. It is
 * exercised on every other open because the rule that makes it safe - a freed
 * buffer keeps the seal bit, so a writer holding a stale buffer index cannot
 * reserve into it - is invisible without it.
 *
 * Replies are recorded like any other message, under a writer id of their own,
 * so ONCE, ORDER and STALESEQ cover them too, and the number delivered must be
 * the number drained. Before slice 2 nothing counted them: osoe_connection_open
 * reset the buffer over the drained reply and it was never sent, and this test
 * passed (second review, C7).
 */
#define REPLY_ID WRITERS
enum { ROLES = WRITERS + 1, MAX_REPLIES = 256 };

/* What a writer puts in a message: which writer, which message, and the
 * sequence number it read after reserving. */
struct payload {
    uint32_t writer;
    uint32_t index;
    uint64_t seq_read;
};

static struct osoe_connection_shared shared;
static unsigned char storage[2 * CAP];
static struct osoe_connection conn = { &shared, storage, CAP };

static _Atomic uint32_t writers_done;

/* Delivery record, written only by the progress thread. */
static struct {
    uint32_t writer, index;
    uint64_t seq_read, seg_seq;
} got[TOTAL + MAX_REPLIES + 64];
static size_t ngot;

static void *writer_main(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg;
    uint32_t len = osoe_message_padded(sizeof(struct payload));

    for (uint32_t i = 0; i < PER_WRITER; i++) {
        struct osoe_reservation r;
        for (;;) {
            enum osoe_reservation_status st = osoe_connection_reserve(&conn, len, &r);
            if (st == OSOE_RESERVATION_OK)
                break;
            /* FULL asks the progress thread for a seal and NONE waits for one
             * to open; a test can simply look again, which is also what makes
             * this a hard interleaving rather than a polite one. */
            sched_yield();
        }
        unsigned char *at = osoe_reservation_bytes(&conn, &r);
        struct osoe_message_header h = { (uint16_t)me, 0, (uint32_t)sizeof(struct payload) };
        struct payload p = { me, i, r.seq };
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, &p, sizeof p);
        osoe_connection_commit(&conn, &r);
    }
    atomic_fetch_add_explicit(&writers_done, 1, memory_order_seq_cst);
    return NULL;
}

/* Parse a sealed buffer the way a receiver would, recording every message. */
static void deliver(uint32_t k, uint32_t bytes)
{
    const unsigned char *base = storage + (size_t)k * CAP;
    struct osoe_am_segment_header sh;
    memcpy(&sh, base, sizeof sh);

    uint32_t off = (uint32_t)sizeof sh, count = 0;
    while (off < bytes) {
        struct osoe_message_header mh;
        memcpy(&mh, base + off, sizeof mh);
        CHECK(mh.len == sizeof(struct payload));
        /* A message whose header does not name a known role is a torn or
         * overlapping reservation: say so here rather than let the payload
         * checks below report something less specific. */
        CHECK(mh.kind <= REPLY_ID);
        struct payload p;
        memcpy(&p, base + off + sizeof mh, sizeof p);
        CHECK(ngot < sizeof got / sizeof *got);
        if (ngot < sizeof got / sizeof *got) {
            got[ngot].writer = p.writer;
            got[ngot].index = p.index;
            got[ngot].seq_read = p.seq_read;
            got[ngot].seg_seq = sh.seq;
            ngot++;
        }
        off += osoe_message_padded(mh.len);
        count++;
    }
    /* §3.4's redundant fields must agree with the bytes actually present. */
    CHECK(off == bytes);
    CHECK(sh.message_count == count);
    CHECK(sh.total_len == bytes);
}

int main(void)
{
    /* The progress thread owns the buffer states; writers never see them. */
    enum osoe_buffer_state state[2] = { OSOE_BUFFER_FREE, OSOE_BUFFER_FREE };
    uint64_t next_seq = 1;
    uint32_t pending = OSOE_CONNECTION_NONE;       /* sealed, not yet sent */
    (void)0;
    uint64_t last_seq = 0;
    uint32_t opens = 0, drained = 0;

    osoe_connection_init(&conn, 8);
    osoe_connection_open(&conn, 0, next_seq++);
    state[0] = OSOE_BUFFER_OPEN;

    pthread_t th[WRITERS];
    for (uint32_t i = 0; i < WRITERS; i++)
        CHECK_FATAL(pthread_create(&th[i], NULL, writer_main, (void *)(uintptr_t)i) == 0);

    /*
     * A stalled protocol must fail here saying what stalled, not run to the
     * ctest timeout: several of the defects this test is for - a reservation
     * that can never be committed, a buffer that can never be reopened - stop
     * delivery rather than corrupt it, and "Timeout" names none of them.
     */
    unsigned pass = 0;
    size_t last_ngot = 0;
    struct timespec moved;
    clock_gettime(CLOCK_MONOTONIC, &moved);
    while (ngot < TOTAL + drained) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ngot != last_ngot) {
            last_ngot = ngot;
            moved = now;
        } else if (now.tv_sec - moved.tv_sec > 5) {
            uint32_t h0 = atomic_load_explicit(&shared.buffer[0].head, memory_order_acquire);
            uint32_t h1 = atomic_load_explicit(&shared.buffer[1].head, memory_order_acquire);
            fprintf(stderr, "stalled after %zu of %d messages: "
                            "buf0 head=%#x filled=%u state=%d, "
                            "buf1 head=%#x filled=%u state=%d, pending=%d, "
                            "writers done %u of %d\n",
                    ngot, TOTAL,
                    h0, atomic_load_explicit(&shared.buffer[0].filled, memory_order_acquire),
                    (int)state[0],
                    h1, atomic_load_explicit(&shared.buffer[1].filled, memory_order_acquire),
                    (int)state[1], (int)pending,
                    atomic_load_explicit(&writers_done, memory_order_acquire), WRITERS);
            /* Fatal, not CHECK: the writers are spinning on a buffer that will
             * never reopen, so joining them below would hang until ctest's
             * timeout and report that instead of this. */
            CHECK_FATAL(false);
        }
        /* Finish a sealed buffer once every writer in it has committed. The
         * progress thread never spins here: if a writer is mid-copy it moves on
         * and asks again (§3.5). */
        if (pending != OSOE_CONNECTION_NONE) {
            uint32_t bytes;
            if (osoe_connection_sendable(&conn, pending, &bytes)) {
                /* Seal-time fields, filled by the progress thread (§3.4). */
                struct osoe_am_segment_header sh = { 0 };
                sh.seq = atomic_load_explicit(&shared.buffer[pending].seq,
                                              memory_order_acquire);
                sh.total_len = bytes;
                uint32_t off = (uint32_t)sizeof sh;
                while (off < bytes) {
                    struct osoe_message_header mh;
                    memcpy(&mh, storage + (size_t)pending * CAP + off, sizeof mh);
                    sh.message_count++;
                    off += osoe_message_padded(mh.len);
                }
                memcpy(storage + (size_t)pending * CAP, &sh, sizeof sh);

                CHECK(sh.seq >= last_seq);                  /* SEQ */
                last_seq = sh.seq;
                deliver(pending, bytes);
                osoe_connection_release(&conn, pending);
                state[pending] = OSOE_BUFFER_FREE;
                pending = OSOE_CONNECTION_NONE;
            }
        }

        /* One reservable buffer at a time: open a free one only when no other
         * is open. Nothing in conn.c enforces this - the states are the
         * progress thread's own bookkeeping (§3.5) - so the driver keeps it,
         * exactly as the lead's agent will have to. */
        bool any_open = state[0] == OSOE_BUFFER_OPEN || state[1] == OSOE_BUFFER_OPEN;
        if (!any_open) {
            for (uint32_t k = 0; k < 2; k++)
                if (state[k] == OSOE_BUFFER_FREE && k != pending) {
                    /* The pre-open drain, every other open, so the path runs
                     * constantly: one reply into k while only this thread can
                     * reach it, committed like any writer's, and recorded with
                     * the number k is about to be opened with. */
                    if ((opens++ & 1) == 0 && drained < MAX_REPLIES) {
                        struct osoe_reservation r;
                        state[k] = OSOE_BUFFER_DRAINING;
                        if (osoe_connection_drain_reserve(&conn, k,
                                osoe_message_padded(sizeof(struct payload)), &r)) {
                            unsigned char *at = osoe_reservation_bytes(&conn, &r);
                            struct osoe_message_header mh = { REPLY_ID, 0,
                                                              (uint32_t)sizeof(struct payload) };
                            struct payload pl = { REPLY_ID, drained++, next_seq };
                            memcpy(at, &mh, sizeof mh);
                            memcpy(at + sizeof mh, &pl, sizeof pl);
                            osoe_connection_commit(&conn, &r);
                        }
                    }
                    osoe_connection_open(&conn, k, next_seq++);
                    state[k] = OSOE_BUFFER_OPEN;
                    break;
                }
        }

        /* Seal periodically, so buffers fill and recycle many times over. */
        if (++pass % 32 == 0 && pending == OSOE_CONNECTION_NONE) {
            for (uint32_t k = 0; k < 2; k++)
                if (state[k] == OSOE_BUFFER_OPEN) {
                    osoe_connection_seal(&conn, k);
                    state[k] = OSOE_BUFFER_SEALED_STATE;
                    pending = k;
                    break;
                }
        }

        /*
         * ACCT: filled must never run past the reserved count.
         *
         * Read `filled` FIRST and `head` second. A writer reserves before it
         * commits, so every byte counted in a `filled` read at time t was
         * counted in `head` before t, and a `head` read after t includes it.
         * The other order is a false positive: a writer that reserves and
         * commits between the two loads makes `filled` account for bytes the
         * stale `head` does not. That is not hypothetical - it failed the
         * release preset here, and passed debug, asan and tsan, which is the
         * four-preset rule earning its place.
         *
         * Only this thread recycles a buffer, so there is no generation to
         * cross between the two loads.
         */
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t f = atomic_load_explicit(&shared.buffer[k].filled,
                                              memory_order_acquire);
            uint32_t h = OSOE_BUFFER_USED(atomic_load_explicit(&shared.buffer[k].head,
                                                            memory_order_acquire));
            CHECK(f <= h);
        }
    }

    for (uint32_t i = 0; i < WRITERS; i++)
        pthread_join(th[i], NULL);

    /* ONCE: every message exactly once. */
    CHECK(ngot >= TOTAL);
    /* Every drained reply was delivered, and at least some were drained. */
    size_t reply_count = 0;
    for (size_t i = 0; i < ngot; i++)
        reply_count += got[i].writer == REPLY_ID;
    CHECK(drained > 0);
    CHECK(reply_count == drained);
    enum { SLOTS = (int)PER_WRITER > (int)MAX_REPLIES ? (int)PER_WRITER : (int)MAX_REPLIES };
    static bool seen[ROLES][SLOTS];
    for (size_t i = 0; i < ngot; i++) {
        uint32_t bound = got[i].writer == REPLY_ID ? MAX_REPLIES : PER_WRITER;
        CHECK(got[i].writer < ROLES && got[i].index < bound);
        if (got[i].writer < ROLES && got[i].index < bound) {
            CHECK(!seen[got[i].writer][got[i].index]);
            seen[got[i].writer][got[i].index] = true;
        }
    }

    /* ORDER: one writer's messages in issue order. */
    uint32_t next[ROLES] = { 0 };
    for (size_t i = 0; i < ngot; i++)
        if (got[i].writer < ROLES) {
            CHECK(got[i].index == next[got[i].writer]);
            next[got[i].writer] = got[i].index + 1;
        }

    /*
     * STALESEQ: the number a writer read is the segment it went out in. This
     * is the one that fails if the sequence number is assigned at seal instead
     * of when the buffer becomes reservable, and quiet's high_seg would then be
     * a generation behind - returning early, which is silent.
     */
    for (size_t i = 0; i < ngot; i++)
        CHECK(got[i].seq_read == got[i].seg_seq);

    return CHECK_DONE();
}
