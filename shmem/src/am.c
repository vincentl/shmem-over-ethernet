/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "am.h"
#include "fatal.h"

/*
 * §3.13: the whole subsystem is a static const array. Dispatch is an index and
 * one bounds compare, so there is no registration lock, no lifetime question
 * and no teardown race.
 */
static const struct osoe_am_descriptor table[] = {
    [OSOE_AM_PUT]         = { "put",         sizeof(struct osoe_am_put),         true  },
    [OSOE_AM_GET_REQUEST] = { "get.request", sizeof(struct osoe_am_get_request), false },
    [OSOE_AM_GET_REPLY]   = { "get.reply",   sizeof(struct osoe_am_get_reply),   true  },
    [OSOE_AM_RMW_REQUEST] = { "rmw.request", sizeof(struct osoe_am_rmw_request), false },
    [OSOE_AM_RMW_REPLY]   = { "rmw.reply",   sizeof(struct osoe_am_rmw_reply),   false },
    [OSOE_AM_BARRIER_ARRIVE]  = { "barrier.arrive",  sizeof(struct osoe_am_barrier), false },
    [OSOE_AM_BARRIER_RELEASE] = { "barrier.release", sizeof(struct osoe_am_barrier), false },
    [OSOE_AM_ABORT]           = { "abort",           sizeof(struct osoe_am_abort),   false },
    [OSOE_AM_DEADLOCK]        = { "deadlock",        sizeof(struct osoe_am_deadlock), false },
};

_Static_assert(sizeof table / sizeof *table == OSOE_AM_HANDLER_COUNT,
               "every handler in the enum needs a table entry");

const struct osoe_am_descriptor *osoe_am_table(uint32_t *count)
{
    *count = OSOE_AM_HANDLER_COUNT;
    return table;
}

uint32_t osoe_am_table_checksum(void)
{
    /*
     * FNV-1a over what a peer must agree about: the identifier, the size of the
     * fixed part, and whether a payload follows - and, first, the revision of
     * what the fields mean (OSOE_AM_WIRE_REVISION), which a message's shape
     * cannot show. Not the name, which is a diagnostic and may be reworded
     * without making two binaries incompatible.
     */
    uint32_t h = UINT32_C(2166136261);
    for (uint32_t revision = OSOE_AM_WIRE_REVISION, b = 0; b < 4; b++, revision >>= 8) {
        h ^= revision & 0xff;
        h *= UINT32_C(16777619);
    }
    for (uint32_t i = 0; i < OSOE_AM_HANDLER_COUNT; i++) {
        unsigned char shape[9];
        shape[0] = (unsigned char)i;
        uint32_t fixed = table[i].fixed_bytes;
        memcpy(shape + 1, &fixed, sizeof fixed);
        uint32_t payload = table[i].carries_payload;
        memcpy(shape + 5, &payload, sizeof payload);
        for (size_t b = 0; b < sizeof shape; b++) {
            h ^= shape[b];
            h *= UINT32_C(16777619);
        }
    }
    return h;
}

void osoe_am_parser_init(struct osoe_am_parser *p, uint32_t peer)
{
    *p = (struct osoe_am_parser){ 0 };
    p->peer = peer;
    p->stage = OSOE_AM_STAGE_SEGMENT;
    p->need = (uint32_t)sizeof(struct osoe_am_segment_header);
}

/* Take up to `want` bytes into the accumulator; returns how many were taken. */
static size_t accumulate(struct osoe_am_parser *p, const unsigned char *in, size_t left)
{
    size_t want = p->need - p->have;
    if (want > left)
        want = left;
    memcpy(p->accumulator + p->have, in, want);
    p->have += (uint32_t)want;
    return want;
}

static void begin_message(struct osoe_am_parser *p)
{
    p->stage = OSOE_AM_STAGE_MESSAGE;
    p->have = 0;
    p->need = (uint32_t)sizeof(struct osoe_message_header);
}

/*
 * The segment ends when its **bytes** are exhausted, not when its message count
 * is reached.
 *
 * Which of the two terminates the segment decides what a corrupt header does. A
 * count that terminated it would make a count that is too large wait forever
 * for a message that is not coming - no complaint, just a stalled connection -
 * and a count that is too small would end the segment mid-message and mis-frame
 * everything after it. Length terminates; the count is then cross-checked, and
 * disagreeing either way is caught at once. That is what §3.4 means by making
 * the two deliberately redundant.
 */
static bool segment_complete(const struct osoe_am_parser *p)
{
    return p->segment_seen >= p->segment.total_len;
}

/* "node 3", for a fatal: the receiving node is where a peer's bug surfaces, so
 * the message must say which peer (§3.15; second review, D4). */
static const char *from(const struct osoe_am_parser *p, char *buf, size_t len)
{
    if (p->peer == UINT32_MAX)
        snprintf(buf, len, "an unnamed peer");
    else
        snprintf(buf, len, "node %u", p->peer);
    return buf;
}

static void end_segment(struct osoe_am_parser *p, const struct osoe_am_sink *sink)
{
    /*
     * §3.4's redundant fields, cross-checked. They are deliberately redundant
     * with the per-message lengths so that a framing bug becomes an immediate
     * assertion rather than a garbage dispatch 50 MB later - so checking them
     * is the whole reason they are on the wire.
     */
    if (p->messages_seen != p->segment.message_count)
        osoe_fatal("segment %llu carried %u messages, its header says %u",
                   (unsigned long long)p->segment.seq, p->messages_seen,
                   p->segment.message_count);
    if (p->segment_seen != p->segment.total_len)
        osoe_fatal("segment %llu was %u bytes, its header says %u",
                   (unsigned long long)p->segment.seq, p->segment_seen,
                   p->segment.total_len);
    if (sink->segment_applied)
        sink->segment_applied(sink->context, &p->segment);
    p->stage = OSOE_AM_STAGE_SEGMENT;
    p->have = 0;
    p->need = (uint32_t)sizeof(struct osoe_am_segment_header);
    p->segment_seen = 0;
    p->messages_seen = 0;
}

void osoe_am_parse(struct osoe_am_parser *p, const void *bytes, size_t length,
                   const struct osoe_am_sink *sink)
{
    const unsigned char *in = bytes;
    size_t left = length;

    while (left > 0) {
        switch (p->stage) {
        case OSOE_AM_STAGE_SEGMENT: {
            size_t took = accumulate(p, in, left);
            in += took; left -= took; p->segment_seen += (uint32_t)took;
            if (p->have < p->need)
                break;                          /* more of the header to come */
            memcpy(&p->segment, p->accumulator, sizeof p->segment);
            if (p->segment.total_len < sizeof p->segment)
                osoe_fatal("segment %llu claims %u bytes, less than its own header",
                           (unsigned long long)p->segment.seq, p->segment.total_len);
            /*
             * Every message needs at least its header, so a count that cannot
             * fit in the bytes the segment says it has is refused here, before
             * a byte of any of them is read. The check at the end of the
             * segment would catch it too, but only after the first message had
             * been parsed out of bytes that belong to the next segment.
             */
            uint32_t room = p->segment.total_len - (uint32_t)sizeof p->segment;
            if ((uint64_t)p->segment.message_count * sizeof(struct osoe_message_header) >
                room) {
                char who[32];
                osoe_fatal("segment %llu from %s claims %u messages in no room: it is %u "
                           "bytes, %u past its header",
                           (unsigned long long)p->segment.seq, from(p, who, sizeof who),
                           p->segment.message_count, p->segment.total_len, room);
            }
            if (p->segment.message_count == 0) {
                /* A header-only segment: an acknowledgement with nothing to
                 * apply, which is how one-way traffic is acknowledged at all
                 * (§3.4's ack-only segments). */
                end_segment(p, sink);
                break;
            }
            begin_message(p);
            break;
        }

        case OSOE_AM_STAGE_MESSAGE: {
            size_t took = accumulate(p, in, left);
            in += took; left -= took; p->segment_seen += (uint32_t)took;
            if (p->have < p->need)
                break;
            memcpy(&p->message, p->accumulator, sizeof p->message);
            if (p->message.kind >= OSOE_AM_HANDLER_COUNT)
                osoe_fatal("segment %llu carries message kind %u; this build knows %u",
                           (unsigned long long)p->segment.seq, p->message.kind,
                           OSOE_AM_HANDLER_COUNT);
            /* One bounds compare, then an array index: §3.13's whole dispatch. */
            const struct osoe_am_descriptor *d = &table[p->message.kind];
            if (p->message.len < d->fixed_bytes)
                osoe_fatal("a %s message is %u bytes, less than its fixed part of %u",
                           d->name, p->message.len, d->fixed_bytes);
            /*
             * The message, padding included, must fit in what is left of its
             * segment - checked now, before any of it is applied. A length
             * that runs past the segment would otherwise have its payload
             * written from the next segment's bytes, and the cross-check at
             * the segment's end would fire only after the damage (D4).
             */
            {
                uint32_t header = (uint32_t)sizeof(struct osoe_message_header);
                uint64_t padded = ((uint64_t)header + p->message.len + 7u) & ~UINT64_C(7);
                uint64_t remaining = p->segment.total_len > p->segment_seen
                                     ? p->segment.total_len - p->segment_seen : 0;
                if (padded - header > remaining) {
                    char who[32];
                    osoe_fatal("a %s message of %u bytes runs past the end of segment %llu "
                               "from %s: %llu bytes of the segment remain", d->name,
                               p->message.len, (unsigned long long)p->segment.seq,
                               from(p, who, sizeof who), (unsigned long long)remaining);
                }
            }
            p->payload_bytes = p->message.len - d->fixed_bytes;
            if (!d->carries_payload && p->payload_bytes != 0)
                osoe_fatal("a %s message carries %u payload bytes and should carry none",
                           d->name, p->payload_bytes);
            p->stage = OSOE_AM_STAGE_FIXED;
            p->have = 0;
            p->need = d->fixed_bytes;
            break;
        }

        case OSOE_AM_STAGE_FIXED: {
            size_t took = accumulate(p, in, left);
            in += took; left -= took; p->segment_seen += (uint32_t)took;
            if (p->have < p->need)
                break;
            /*
             * The fixed part is complete and no payload byte has been consumed:
             * this is the moment the destination is resolved, so that payload
             * can be copied straight there (§3.3) instead of into a reassembly
             * buffer the size of a segment.
             */
            p->payload_seen = 0;
            p->destination = NULL;
            if (p->payload_bytes > 0 && sink->destination)
                p->destination = sink->destination(sink->context, &p->message,
                                                   p->accumulator, p->payload_bytes);
            if (p->payload_bytes == 0) {
                if (sink->complete)
                    sink->complete(sink->context, &p->message, p->accumulator, NULL, 0);
                p->messages_seen++;
                goto after_message;
            }
            p->stage = OSOE_AM_STAGE_PAYLOAD;
            break;
        }

        case OSOE_AM_STAGE_PAYLOAD: {
            size_t want = p->payload_bytes - p->payload_seen;
            if (want > left)
                want = left;
            /* A NULL destination discards: the bytes are consumed either way,
             * because refusing them would mean holding the rest of the stream. */
            if (p->destination)
                memcpy(p->destination + p->payload_seen, in, want);
            p->payload_seen += (uint32_t)want;
            in += want; left -= want; p->segment_seen += (uint32_t)want;
            if (p->payload_seen < p->payload_bytes)
                break;
            if (sink->complete)
                sink->complete(sink->context, &p->message, p->accumulator,
                               p->destination, p->payload_bytes);
            p->messages_seen++;
            goto after_message;
        }

        case OSOE_AM_STAGE_PADDING: {
            size_t want = p->need - p->have;
            if (want > left)
                want = left;
            p->have += (uint32_t)want;
            in += want; left -= want; p->segment_seen += (uint32_t)want;
            if (p->have < p->need)
                break;
            if (segment_complete(p))
                end_segment(p, sink);
            else
                begin_message(p);
            break;
        }
        }
        continue;

after_message:
        /*
         * Each message is padded to 8 bytes (§3.4). The padding is part of the
         * segment's length, so it is consumed here rather than skipped.
         *
         * osoe_message_padded() takes the bytes *after* the message header and
         * accounts for the header itself, so the argument is the fixed part
         * plus the payload - adding or subtracting the header here would count
         * it twice or not at all, and either leaves the cursor off by eight on
         * every message.
         */
        {
            uint32_t after_header = table[p->message.kind].fixed_bytes + p->payload_bytes;
            uint32_t padded = osoe_message_padded(after_header);
            uint32_t body = (uint32_t)sizeof(struct osoe_message_header) + after_header;
            uint32_t pad = padded > body ? padded - body : 0;
            if (pad > 0) {
                p->stage = OSOE_AM_STAGE_PADDING;
                p->have = 0;
                p->need = pad;
            } else if (segment_complete(p)) {
                end_segment(p, sink);
            } else {
                begin_message(p);
            }
        }
    }
}
