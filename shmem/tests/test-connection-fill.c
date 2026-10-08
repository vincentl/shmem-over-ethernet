/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Filling the room, in C (v0.2 step 4's first rung; §3.5).
 *
 * osoe_connection_reserve_up_to lets a writer take as much of the open buffer
 * as there is room for, down to a floor, and a put cuts its message to what it
 * took and sends the rest as the next part. reservation_model.py's `fill`
 * cases settle the protocol; this runs the real code with real threads - each
 * writer sending messages of every length in parts, as remote_put does -
 * beside a writer of small messages reserved whole and a progress thread that
 * seals, sends and opens the two buffers and drains replies into them, and
 * asserts what the model asserts, by name:
 *
 *   WHOLE     a writer's messages arrive whole: each part continues its message
 *             where the last part left off, with no gap and nothing twice
 *   ORDER     one writer's parts arrive in the order it reserved them
 *   STALESEQ  the sequence number a writer read after reserving is the one of
 *             the segment its part went out in
 *   FLOOR     a part shorter than the floor is the last of its message
 *   FILL      a part cut short ends its buffer: nothing was room for more
 *   BYTES     every payload byte is the one its writer wrote there
 *   ACCT      filled never exceeds the reserved count
 *
 * Threads, not processes, for the same reason test-connection.c gives: the
 * shape the tsan preset can instrument.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "connection.h"

enum {
    WRITERS = 4,                    /* the last sends small messages, whole */
    MESSAGES = 300,                 /* a writer's */
    CAPACITY = 1024,                /* a buffer's bytes: small, so messages split often */
    FLOOR = 64,                     /* the least payload a cut part takes */
    SMALL = 128,                    /* the small writer's longest message, whole */
    LONGEST = 2 * CAPACITY,         /* a message's longest payload */
    MAX_REPLIES = 256,
};
#define SMALL_WRITER (WRITERS - 1)
#define REPLY_ID WRITERS

/* A part's fixed field, after the message header: whose, which message, where
 * in it this part starts, how long the message is, whether the room cut it
 * short, and the sequence number read after reserving. The part's payload
 * bytes follow. */
struct part {
    uint32_t writer;
    uint32_t message;
    uint32_t start;
    uint32_t length;
    uint32_t cut;
    uint32_t reserved;
    uint64_t seq_read;
};

static struct osoe_connection_shared shared;
static unsigned char storage[2 * CAPACITY];
static struct osoe_connection connection = { &shared, storage, CAPACITY };

static _Atomic uint32_t writers_done;

/* The byte a writer puts at position j of its message m. */
static unsigned char pattern(uint32_t writer, uint32_t message, uint32_t j)
{
    return (unsigned char)(writer * 131u + message * 7u + j * 13u + (j >> 8));
}

/* A message's length, from the writer and its index alone, so the receiver
 * can check it: lengths spread over 1 to LONGEST across the writers. */
static uint32_t length_of(uint32_t writer, uint32_t message)
{
    uint32_t x = writer * 2654435761u ^ message * 40503u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    /* The small writer's are a fetch-add's size or so, header and fixed part in. */
    uint32_t longest = writer == SMALL_WRITER ?
        SMALL - (uint32_t)(sizeof(struct osoe_message_header) + sizeof(struct part)) : LONGEST;
    return 1 + x % longest;
}

static void *writer_main(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg;
    uint32_t overhead = (uint32_t)(sizeof(struct osoe_message_header) + sizeof(struct part));
    for (uint32_t m = 0; m < MESSAGES; m++) {
        uint32_t length = length_of(me, m);
        for (uint32_t start = 0; start < length; ) {
            uint32_t n = length - start;
            /* A part fits a buffer whole at most, as payload_limit makes a
             * put's chunk fit one. */
            uint32_t most_payload = CAPACITY - (uint32_t)sizeof(struct osoe_am_segment_header) -
                                    overhead;
            most_payload &= ~(OSOE_MESSAGE_ALIGN - 1);
            if (n > most_payload)
                n = most_payload;
            uint32_t most = osoe_message_padded((uint32_t)sizeof(struct part) + n);
            uint32_t least = osoe_message_padded((uint32_t)sizeof(struct part) +
                                                 (n < FLOOR ? n : FLOOR));
            if (me == SMALL_WRITER)
                least = most;           /* a small message is reserved whole */
            struct osoe_reservation r;
            for (;;) {
                enum osoe_reservation_status st =
                    osoe_connection_reserve_up_to(&connection, most, least, &r);
                if (st == OSOE_RESERVATION_OK)
                    break;
                sched_yield();
            }
            CHECK(r.len <= most && r.len >= least && r.len % OSOE_MESSAGE_ALIGN == 0);
            bool cut = r.len < most;
            if (cut)
                n = r.len - overhead;
            unsigned char *at = osoe_reservation_bytes(&connection, &r);
            struct osoe_message_header h = { (uint16_t)me, 0, (uint32_t)sizeof(struct part) + n };
            struct part p = { .writer = me, .message = m, .start = start, .length = length,
                              .cut = cut, .seq_read = r.seq };
            memcpy(at, &h, sizeof h);
            memcpy(at + sizeof h, &p, sizeof p);
            for (uint32_t j = 0; j < n; j++)
                at[overhead + j] = pattern(me, m, start + j);
            osoe_connection_commit(&connection, &r);
            start += n;
        }
    }
    atomic_fetch_add_explicit(&writers_done, 1, memory_order_seq_cst);
    return NULL;
}

/* What the receiver knows of each writer: the message and byte it expects
 * next. Written only by the progress thread. */
static struct {
    uint32_t message, next;
} expect[WRITERS];
static uint32_t replies_delivered;
static uint64_t parts_delivered, parts_cut;

/* Parse a sealed buffer as a receiver would. */
static void deliver(uint32_t k, uint32_t bytes)
{
    const unsigned char *base = storage + (size_t)k * CAPACITY;
    struct osoe_am_segment_header sh;
    memcpy(&sh, base, sizeof sh);
    uint32_t overhead = (uint32_t)(sizeof(struct osoe_message_header) + sizeof(struct part));

    uint32_t off = (uint32_t)sizeof sh, count = 0;
    while (off < bytes) {
        struct osoe_message_header mh;
        memcpy(&mh, base + off, sizeof mh);
        CHECK(mh.kind <= REPLY_ID && mh.len >= sizeof(struct part));
        if (!(mh.kind <= REPLY_ID && mh.len >= sizeof(struct part)))
            return;
        struct part p;
        memcpy(&p, base + off + sizeof mh, sizeof p);
        uint32_t n = mh.len - (uint32_t)sizeof p;
        CHECK(p.seq_read == sh.seq);                                    /* STALESEQ */
        if (mh.kind == REPLY_ID) {
            CHECK(p.message == replies_delivered);                      /* ORDER */
            replies_delivered++;
        } else {
            uint32_t w = mh.kind;
            CHECK(p.writer == w);
            CHECK(p.message == expect[w].message);                      /* ORDER */
            CHECK(p.start == expect[w].next);                           /* WHOLE */
            CHECK(p.length == length_of(w, p.message));
            CHECK(p.start + n <= p.length);
            bool last = p.start + n == p.length;
            CHECK(last || n >= FLOOR);                                  /* FLOOR */
            const unsigned char *payload = base + off + overhead;
            for (uint32_t j = 0; j < n; j++)
                if (payload[j] != pattern(w, p.message, p.start + j)) {
                    CHECK(false);                                       /* BYTES */
                    break;
                }
            if (p.cut) {
                /* FILL: cut short, so it took the room to the last whole
                 * 8 bytes, and nothing came after it in this buffer. */
                uint32_t end = off + osoe_message_padded(mh.len);
                CHECK(!last && end == bytes && CAPACITY - end < OSOE_MESSAGE_ALIGN);
                parts_cut++;
            }
            expect[w].next = last ? 0 : p.start + n;
            expect[w].message += last;
        }
        parts_delivered++;
        off += osoe_message_padded(mh.len);
        count++;
    }
    CHECK(off == bytes);
    CHECK(sh.message_count == count);
    CHECK(sh.total_len == bytes);
}

static bool all_delivered(void)
{
    for (uint32_t w = 0; w < WRITERS; w++)
        if (expect[w].message < MESSAGES)
            return false;
    return true;
}

int main(void)
{
    enum osoe_buffer_state state[2] = { OSOE_BUFFER_FREE, OSOE_BUFFER_FREE };
    uint64_t next_seq = 1, last_seq = 0;
    uint32_t pending = OSOE_CONNECTION_NONE;
    uint32_t opens = 0, drained = 0;

    osoe_connection_init(&connection, 8);
    osoe_connection_open(&connection, 0, next_seq++);
    state[0] = OSOE_BUFFER_OPEN;

    pthread_t th[WRITERS];
    for (uint32_t i = 0; i < WRITERS; i++)
        CHECK_FATAL(pthread_create(&th[i], NULL, writer_main, (void *)(uintptr_t)i) == 0);

    unsigned pass = 0;
    uint64_t last_parts = 0;
    struct timespec moved;
    clock_gettime(CLOCK_MONOTONIC, &moved);
    while (!all_delivered() || replies_delivered < drained) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (parts_delivered != last_parts) {
            last_parts = parts_delivered;
            moved = now;
        } else if (now.tv_sec - moved.tv_sec > 5) {
            fprintf(stderr, "stalled after %llu parts: buffer 0 head=%#x filled=%u state=%d, "
                            "buffer 1 head=%#x filled=%u state=%d, pending=%d, writers done %u\n",
                    (unsigned long long)parts_delivered,
                    atomic_load_explicit(&shared.buffer[0].head, memory_order_acquire),
                    atomic_load_explicit(&shared.buffer[0].filled, memory_order_acquire),
                    (int)state[0],
                    atomic_load_explicit(&shared.buffer[1].head, memory_order_acquire),
                    atomic_load_explicit(&shared.buffer[1].filled, memory_order_acquire),
                    (int)state[1], (int)pending,
                    atomic_load_explicit(&writers_done, memory_order_acquire));
            CHECK_FATAL(false);
        }
        if (pending != OSOE_CONNECTION_NONE) {
            uint32_t bytes;
            if (osoe_connection_sendable(&connection, pending, &bytes)) {
                struct osoe_am_segment_header sh = { 0 };
                sh.seq = atomic_load_explicit(&shared.buffer[pending].seq, memory_order_acquire);
                sh.total_len = bytes;
                uint32_t off = (uint32_t)sizeof sh;
                while (off < bytes) {
                    struct osoe_message_header mh;
                    memcpy(&mh, storage + (size_t)pending * CAPACITY + off, sizeof mh);
                    sh.message_count++;
                    off += osoe_message_padded(mh.len);
                }
                memcpy(storage + (size_t)pending * CAPACITY, &sh, sizeof sh);
                CHECK(sh.seq >= last_seq);
                last_seq = sh.seq;
                deliver(pending, bytes);
                osoe_connection_release(&connection, pending);
                state[pending] = OSOE_BUFFER_FREE;
                pending = OSOE_CONNECTION_NONE;
            }
        }
        bool any_open = state[0] == OSOE_BUFFER_OPEN || state[1] == OSOE_BUFFER_OPEN;
        if (!any_open) {
            for (uint32_t k = 0; k < 2; k++)
                if (state[k] == OSOE_BUFFER_FREE && k != pending) {
                    /* The pre-open drain on every other open, a whole reply,
                     * as §3.8 guarantees it a place however the writers fill. */
                    if ((opens++ & 1) == 0 && drained < MAX_REPLIES) {
                        struct osoe_reservation r;
                        state[k] = OSOE_BUFFER_DRAINING;
                        if (osoe_connection_drain_reserve(&connection, k,
                                osoe_message_padded(sizeof(struct part)), &r)) {
                            unsigned char *at = osoe_reservation_bytes(&connection, &r);
                            struct osoe_message_header mh = { REPLY_ID, 0,
                                                              (uint32_t)sizeof(struct part) };
                            struct part pl = { .writer = REPLY_ID, .message = drained++,
                                               .seq_read = next_seq };
                            memcpy(at, &mh, sizeof mh);
                            memcpy(at + sizeof mh, &pl, sizeof pl);
                            osoe_connection_commit(&connection, &r);
                        }
                    }
                    osoe_connection_open(&connection, k, next_seq++);
                    state[k] = OSOE_BUFFER_OPEN;
                    break;
                }
        }
        /* Seal now and then, so buffers go out part full as well as full. */
        if (++pass % 16 == 0 && pending == OSOE_CONNECTION_NONE) {
            for (uint32_t k = 0; k < 2; k++)
                if (state[k] == OSOE_BUFFER_OPEN) {
                    osoe_connection_seal(&connection, k);
                    state[k] = OSOE_BUFFER_SEALED_STATE;
                    pending = k;
                    break;
                }
        }
        for (uint32_t k = 0; k < 2; k++) {                              /* ACCT */
            uint32_t f = atomic_load_explicit(&shared.buffer[k].filled, memory_order_acquire);
            uint32_t h = OSOE_BUFFER_USED(atomic_load_explicit(&shared.buffer[k].head,
                                                               memory_order_acquire));
            CHECK(f <= h);
        }
    }
    for (uint32_t i = 0; i < WRITERS; i++)
        pthread_join(th[i], NULL);

    CHECK(drained > 0 && replies_delivered == drained);
    /* The test is only a test of filling if parts were cut. */
    CHECK(parts_cut > 0);
    printf("parts %llu, of them cut to fill the room %llu, replies %u\n",
           (unsigned long long)parts_delivered, (unsigned long long)parts_cut, drained);
    return CHECK_DONE();
}
