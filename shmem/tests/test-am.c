/*
 * AM framing and the stream parser (§3.3, §3.4, §3.13).
 *
 * §8.2's warning is that loopback "will never produce a short write or partial
 * read - and partial-write handling is the most common defect in hand-rolled
 * TCP framing". A test that feeds a segment in one piece would miss exactly
 * that class, so this one feeds the same bytes in **every** split pattern it
 * can afford and requires the delivered messages to be identical every time.
 *
 * At one split point that is exhaustive: every byte boundary of the stream. At
 * two and three it is exhaustive over a smaller stream. A parser that mishandles
 * a boundary mishandles some boundary, and a boundary is a byte offset - so
 * sweeping offsets is the whole space, not a sample of it.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am.h"
#include "check.h"

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

/* ---- What the sink recorded ------------------------------------------ */

enum { MAX_MESSAGES = 16, MAX_PAYLOAD = 256 };

struct record {
    uint32_t count;
    struct {
        uint16_t kind;
        uint32_t payload_bytes;
        unsigned char payload[MAX_PAYLOAD];
        unsigned char fixed[OSOE_AM_MAX_FIXED];
    } message[MAX_MESSAGES];
    uint32_t segments;
    uint64_t last_segment_seq;
};

static struct record got;
static unsigned char landing[MAX_MESSAGES][MAX_PAYLOAD];

static void *destination(void *context, const struct osoe_message_header *header,
                         const void *fixed, uint32_t payload_bytes)
{
    (void)context; (void)header; (void)fixed;
    CHECK(payload_bytes <= MAX_PAYLOAD);
    CHECK(got.count < MAX_MESSAGES);
    /* Straight to its destination: the parser never holds the payload, so the
     * test supplies somewhere for it to land, exactly as the backend will. */
    return landing[got.count];
}

static void complete(void *context, const struct osoe_message_header *header,
                     const void *fixed, void *payload, uint32_t payload_bytes)
{
    (void)context;
    if (got.count >= MAX_MESSAGES)
        return;
    uint32_t i = got.count++;
    got.message[i].kind = header->kind;
    got.message[i].payload_bytes = payload_bytes;
    memcpy(got.message[i].fixed, fixed, OSOE_AM_MAX_FIXED);
    if (payload && payload_bytes)
        memcpy(got.message[i].payload, payload, payload_bytes);
}

static void segment_applied(void *context, const struct osoe_am_segment_header *segment)
{
    (void)context;
    got.segments++;
    got.last_segment_seq = segment->seq;
}

static const struct osoe_am_sink sink = { destination, complete, segment_applied, NULL };

/* ---- Building a segment ---------------------------------------------- */

static unsigned char stream[4096];
static uint32_t stream_len;

static uint32_t append_message(uint32_t at, uint16_t kind, const void *fixed,
                               uint32_t fixed_bytes, const void *payload,
                               uint32_t payload_bytes)
{
    struct osoe_message_header h = { kind, 0, fixed_bytes + payload_bytes };
    memcpy(stream + at, &h, sizeof h);
    memcpy(stream + at + sizeof h, fixed, fixed_bytes);
    if (payload_bytes)
        memcpy(stream + at + sizeof h + fixed_bytes, payload, payload_bytes);
    uint32_t body = (uint32_t)sizeof h + fixed_bytes + payload_bytes;
    uint32_t padded = osoe_message_padded(fixed_bytes + payload_bytes);
    memset(stream + at + body, 0, padded - body);      /* §3.4's alignment padding */
    return at + padded;
}

/* A segment of: a put with payload, a get request, a get reply with payload,
 * and an rmw reply - so both payload and payload-free messages, and a message
 * whose payload length is not a multiple of eight. */
static void build(uint64_t seq)
{
    uint32_t at = (uint32_t)sizeof(struct osoe_am_segment_header);
    uint32_t count = 0;

    unsigned char bytes[37];
    for (size_t i = 0; i < sizeof bytes; i++)
        bytes[i] = (unsigned char)(i * 7 + 1);

    struct osoe_am_put put = { .target_pe = 3, .region = 0, .offset = 512 };
    at = append_message(at, OSOE_AM_PUT, &put, sizeof put, bytes, sizeof bytes);
    count++;

    struct osoe_am_get_request req = { .target_pe = 1, .origin_pe = 2, .region = 0,
                                       .length = 64, .offset = 8, .token = 99 };
    at = append_message(at, OSOE_AM_GET_REQUEST, &req, sizeof req, NULL, 0);
    count++;

    struct osoe_am_get_reply reply = { .origin_pe = 2, .chunk_offset = 0, .token = 99 };
    at = append_message(at, OSOE_AM_GET_REPLY, &reply, sizeof reply, bytes, 8);
    count++;

    struct osoe_am_rmw_reply rmw = { .origin_pe = 4, .token = 7, .value = 0xfeedface };
    at = append_message(at, OSOE_AM_RMW_REPLY, &rmw, sizeof rmw, NULL, 0);
    count++;

    struct osoe_am_segment_header sh = { .seq = seq, .message_count = count,
                                         .total_len = at };
    memcpy(stream, &sh, sizeof sh);
    stream_len = at;
}

/* Feed the stream, cut at the given offsets. */
static void feed(const uint32_t *cuts, uint32_t ncuts)
{
    struct osoe_am_parser p;
    osoe_am_parser_init(&p);
    memset(&got, 0, sizeof got);
    uint32_t from = 0;
    for (uint32_t i = 0; i < ncuts; i++) {
        osoe_am_parse(&p, stream + from, cuts[i] - from, &sink);
        from = cuts[i];
    }
    osoe_am_parse(&p, stream + from, stream_len - from, &sink);
}

static struct record expected;

static bool same_as_expected(void)
{
    if (got.count != expected.count || got.segments != expected.segments)
        return false;
    for (uint32_t i = 0; i < got.count; i++) {
        if (got.message[i].kind != expected.message[i].kind) return false;
        if (got.message[i].payload_bytes != expected.message[i].payload_bytes) return false;
        if (memcmp(got.message[i].fixed, expected.message[i].fixed, OSOE_AM_MAX_FIXED)) return false;
        if (memcmp(got.message[i].payload, expected.message[i].payload,
                   got.message[i].payload_bytes)) return false;
    }
    return true;
}

/*
 * Malformed streams. §3.4 puts message_count and total_len on the wire
 * "deliberately redundant with the per-message lengths" so that "cross-checking
 * on receipt turns a framing bug into an immediate assertion rather than a
 * garbage dispatch 50 MB later" - which is only true if the cross-check fires.
 * Each case below corrupts one field and must die naming it. They are separate
 * ctest cases because the parser's answer to a framing error is to abort the
 * job (§3.15): there is no recovery from disagreeing about the format that is
 * not a guess.
 */
static int malformed(const char *which)
{
    build(1);
    struct osoe_am_segment_header sh;
    memcpy(&sh, stream, sizeof sh);

    if (strcmp(which, "count") == 0)
        sh.message_count += 1;                  /* claims a message that is not there */
    else if (strcmp(which, "length") == 0)
        /*
         * Too *small*: the messages then run past the segment's stated end and
         * the cross-check fires. A total_len that is too large is deliberately
         * not tested here, because a stream parser cannot detect it - waiting
         * for bytes that have not arrived yet is indistinguishable from waiting
         * for bytes that never will. That is the wire's to notice, when a
         * connection ends with a segment still open, and it belongs to step 3
         * with the socket.
         */
        sh.total_len -= 8;
    else if (strcmp(which, "short-segment") == 0)
        sh.total_len = 4;                       /* shorter than its own header */
    else if (strcmp(which, "kind") == 0) {
        struct osoe_message_header mh;
        memcpy(&mh, stream + sizeof sh, sizeof mh);
        mh.kind = OSOE_AM_HANDLER_COUNT + 3;    /* a handler this build has never had */
        memcpy(stream + sizeof sh, &mh, sizeof mh);
    } else if (strcmp(which, "short-message") == 0) {
        struct osoe_message_header mh;
        memcpy(&mh, stream + sizeof sh, sizeof mh);
        mh.len = 4;                             /* shorter than the fixed part */
        memcpy(stream + sizeof sh, &mh, sizeof mh);
    } else if (strcmp(which, "long-message") == 0) {
        /* A message whose own length runs past the end of its segment: the
         * parser must refuse it before applying a byte of it (review round
         * two, D4), not discover the overrun afterwards. */
        struct osoe_message_header mh;
        memcpy(&mh, stream + sizeof sh, sizeof mh);
        mh.len = sh.total_len;
        memcpy(stream + sizeof sh, &mh, sizeof mh);
    } else if (strcmp(which, "empty-count") == 0) {
        /* Messages claimed, and no room for any: a header-only segment that
         * says it carries something. */
        sh.total_len = (uint32_t)sizeof sh;
        stream_len = (uint32_t)sizeof sh;
    } else {
        fprintf(stderr, "unknown malformed case '%s'\n", which);
        return 2;
    }
    memcpy(stream, &sh, sizeof sh);

    struct osoe_am_parser p;
    osoe_am_parser_init(&p);
    memset(&got, 0, sizeof got);
    /* Feed everything that was built, never the corrupted length: truncating
     * the feed to a bad total_len would stop the header completing and the
     * check under test would never run. */
    osoe_am_parse(&p, stream, stream_len, &sink);
    /* Reaching here means the corruption went unnoticed, which is the defect. */
    fprintf(stderr, "malformed '%s' parsed without complaint\n", which);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return malformed(argv[1]);

    /* The table's checksum must be stable within a build and cover the shape a
     * peer has to agree about (§3.13). */
    CHECK(osoe_am_table_checksum() == osoe_am_table_checksum());
    uint32_t count = 0;
    const struct osoe_am_descriptor *t = osoe_am_table(&count);
    CHECK(count == OSOE_AM_HANDLER_COUNT);
    for (uint32_t i = 0; i < count; i++) {
        CHECK(t[i].name != NULL);
        CHECK(t[i].fixed_bytes > 0 && t[i].fixed_bytes <= OSOE_AM_MAX_FIXED);
        CHECK(t[i].fixed_bytes % 8 == 0);        /* keeps a payload aligned (§3.4) */
    }

    build(4242);

    /* The whole stream in one piece: the reference. */
    feed(NULL, 0);
    CHECK(got.count == 4);
    CHECK(got.segments == 1);
    CHECK(got.last_segment_seq == 4242);
    CHECK(got.message[0].kind == OSOE_AM_PUT && got.message[0].payload_bytes == 37);
    CHECK(got.message[1].kind == OSOE_AM_GET_REQUEST && got.message[1].payload_bytes == 0);
    CHECK(got.message[2].kind == OSOE_AM_GET_REPLY && got.message[2].payload_bytes == 8);
    CHECK(got.message[3].kind == OSOE_AM_RMW_REPLY && got.message[3].payload_bytes == 0);
    expected = got;

    /* The fixed parts must survive, not only the payloads: a parser that got
     * the offset wrong would write a put to the wrong place silently. */
    struct osoe_am_put back;
    memcpy(&back, expected.message[0].fixed, sizeof back);
    CHECK(back.target_pe == 3 && back.offset == 512);

    /* ---- One cut, at every byte boundary. Exhaustive. ---- */
    uint32_t failures = 0;
    for (uint32_t c = 1; c < stream_len; c++) {
        feed(&c, 1);
        if (!same_as_expected()) {
            if (failures == 0)
                fprintf(stderr, "split at %u of %u changed the parse\n", c, stream_len);
            failures++;
        }
    }
    CHECK(failures == 0);

    /* ---- Two cuts, every pair. Still exhaustive, quadratic but small. ---- */
    failures = 0;
    for (uint32_t a = 1; a < stream_len; a++)
        for (uint32_t b = a; b < stream_len; b++) {
            uint32_t cuts[2] = { a, b };
            feed(cuts, 2);
            if (!same_as_expected()) {
                if (failures == 0)
                    fprintf(stderr, "splits at %u,%u of %u changed the parse\n", a, b,
                            stream_len);
                failures++;
            }
        }
    CHECK(failures == 0);

    /* ---- Byte at a time: the worst case a short read can produce. ---- */
    {
        struct osoe_am_parser p;
        osoe_am_parser_init(&p);
        memset(&got, 0, sizeof got);
        for (uint32_t i = 0; i < stream_len; i++)
            osoe_am_parse(&p, stream + i, 1, &sink);
        CHECK(same_as_expected());
    }

    /* ---- Several segments back to back in one feed ---- */
    {
        build(1);
        uint32_t one = stream_len;
        unsigned char two[8192];
        memcpy(two, stream, one);
        build(2);
        memcpy(two + one, stream, stream_len);
        uint32_t total = one + stream_len;

        struct osoe_am_parser p;
        osoe_am_parser_init(&p);
        memset(&got, 0, sizeof got);
        osoe_am_parse(&p, two, total, &sink);
        CHECK(got.segments == 2);
        CHECK(got.count == 8);
        CHECK(got.last_segment_seq == 2);
    }

    /* ---- A header-only segment: an acknowledgement with nothing to apply,
     * which is how one-way traffic gets acknowledged at all (§3.4). ---- */
    {
        struct osoe_am_segment_header sh = { .seq = 77, .message_count = 0,
                                             .total_len = sizeof sh,
                                             .flags = OSOE_AM_SEGMENT_ACK_REQUESTED };
        struct osoe_am_parser p;
        osoe_am_parser_init(&p);
        memset(&got, 0, sizeof got);
        osoe_am_parse(&p, &sh, sizeof sh, &sink);
        CHECK(got.segments == 1);
        CHECK(got.count == 0);
        CHECK(got.last_segment_seq == 77);
    }

    return CHECK_DONE();
}
