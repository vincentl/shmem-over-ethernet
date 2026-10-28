/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Active messages: the handler table and the stream parser (§3.3, §3.7, §3.13).
 *
 * A segment (§3.4) carries a header and then messages, each padded to 8-byte
 * alignment. This is what turns a byte stream back into those messages, and
 * what says which handler each one belongs to.
 *
 * **Parse as a stream; never hold a buffer.** §3.3 is explicit that a segment
 * spans receive buffers, and that holding them until a segment is complete
 * stalls once a segment outgrows the ring - with one ring serving several
 * connections, two connections each holding buffers for partial segments
 * deadlock, which the review reproduced. So each connection keeps a parse
 * cursor: header bytes accumulate in a small per-connection area, and payload
 * bytes are copied **straight to their destination** as they arrive, rather
 * than into a reassembly buffer that would have to be as large as a segment.
 *
 * That destination is resolved by the caller, through `struct osoe_am_sink`,
 * at the moment the fixed part of a message is complete and before any payload
 * byte has arrived. The parser therefore knows nothing about heaps, landing
 * zones or PEs - which is what keeps §3.3's rule ("never hold a provided
 * buffer") a property of this file rather than a convention the backend has to
 * remember.
 *
 * **What this cannot catch is truncation.** A segment header claiming more
 * bytes than ever arrive leaves the parser waiting, and waiting for bytes that
 * have not come yet is indistinguishable from waiting for bytes that never
 * will. Only the layer that owns the connection can tell those apart, by
 * noticing that it ended with a segment still open - so that check is the
 * engine's, when the socket's stream ends (progress.c, receive), using
 * osoe_am_parser_between_segments below. Everything the bytes themselves can
 * contradict is caught here and at once.
 *
 * **The handler table is fixed at init.** Identifiers are a compile-time enum
 * and dispatch is an array index with one bounds compare: no registration lock,
 * no lifetime question, no teardown race (§3.13). The table is checksummed so
 * that a mismatched binary across nodes is caught at rendezvous with a clear
 * message, rather than as an undecodable message arriving at PE 47 an hour in.
 */
#ifndef OSOE_AM_H
#define OSOE_AM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "connection.h"

/* §3.13's compile-time enum. Appending is a table change and so a checksum
 * change, which is the point. */
enum osoe_am_handler {
    OSOE_AM_PUT = 0,        /* bytes for a target's region */
    OSOE_AM_GET_REQUEST,
    OSOE_AM_GET_REPLY,
    OSOE_AM_RMW_REQUEST,
    OSOE_AM_RMW_REPLY,
    /*
     * The internode phase of a collective (§5.1, §5.3). A team's intranode
     * phase runs on the node's epoch counters; when the last local member
     * arrives it sends one of these, and the release comes back the same way
     * and is applied by the progress thread.
     */
    OSOE_AM_BARRIER_ARRIVE,
    OSOE_AM_BARRIER_RELEASE,
    /*
     * The job is ending (§3.15; phase 1 step 4): the one message of the last
     * segment a dying lead sends on a connection, flagged as its goodbye - an
     * abortive goodbye. The peer reads it before the end of stream it precedes,
     * so it knows the stream ends because the job does, and with which status,
     * where a bare end of stream is a death (abort_model.py).
     */
    OSOE_AM_ABORT,
    /*
     * The deadlock detector's (§3.15, deadlock.h): a probe, a node's report, the
     * root's declaration and a node's word that it said where its PEs wait,
     * between two nodes' leads. Not application work: neither side counts it in
     * its activity, nor needs it applied to be quiet.
     */
    OSOE_AM_DEADLOCK,
    /*
     * A node pair's data wire, asked for (H23, on demand): the node that is not
     * the mesh's caller of the pair needs the wire and asks the caller, on
     * their control wire, to dial it, so that only the caller ever dials and no
     * two calls cross (mesh-schedule.h; data_wire_model.py). Application work,
     * as the tree's messages are: the detector counts it, and a node that asked
     * is not quiet until the wire is up.
     */
    OSOE_AM_DATA_WIRE_WANTED,
    OSOE_AM_HANDLER_COUNT
};

/* ---- The fixed part of each message ---------------------------------- */
/*
 * Every one is 8-byte aligned and a multiple of 8 long, so a payload following
 * it is aligned too. The specification requires an aligned AMO target, and an
 * unaligned 64-bit atomic on aarch64 Linux does *not* fault - it silently makes
 * a misaligned, possibly line-straddling update - so alignment is checked, not
 * left to a trap (§3.4).
 */

struct osoe_am_put {
    uint32_t target_pe;         /* world rank; the wire never carries an address */
    uint16_t region;            /* enum osoe_region */
    uint16_t flags;
    uint64_t offset;
    /* payload: the bytes to write */
};

struct osoe_am_get_request {
    uint32_t target_pe;
    uint32_t origin_pe;
    uint16_t region;
    uint16_t flags;
    uint32_t length;
    uint64_t offset;
    uint64_t token;             /* the origin's completion slot */
};

struct osoe_am_get_reply {
    uint32_t origin_pe;
    uint32_t chunk_offset;      /* where in the requested range these bytes go */
    uint32_t total;             /* bytes the whole reply owes, across every chunk */
    uint32_t reserved;
    uint64_t token;
    /* payload: the bytes read */
};

struct osoe_am_rmw_request {
    uint32_t target_pe;
    uint32_t origin_pe;
    uint16_t region;
    uint16_t op;                /* enum osoe_rmw_op */
    uint32_t width;             /* 4 or 8 */
    uint64_t offset;
    uint64_t operand;
    uint64_t compare;
    uint64_t token;
    uint32_t flags;             /* OSOE_TRANSPORT_FETCH, OSOE_TRANSPORT_FENCED */
    uint32_t reserved;
};

struct osoe_am_rmw_reply {
    uint32_t origin_pe;
    uint32_t reserved;
    uint64_t token;
    uint64_t value;             /* the previous value, for a fetching rmw */
};

/*
 * The revision of what the messages' fields mean, which the handler table's
 * checksum covers (§3.13; osoe_am_table_checksum) beside each message's shape:
 * raised when a field changes meaning and not size, so that two binaries that
 * read one message two ways refuse to form a job rather than misread each
 * other. 1 since the team redesign's step D, whose barrier message names its
 * team on the receiving node (struct osoe_am_barrier's `lead`, until then
 * reserved and 0); 0 before.
 */
#define OSOE_AM_WIRE_REVISION 1u

/*
 * A team's internode arrival and release. Addressed to a *node*, not a PE:
 * §5.1's internode phase is between node leads, and the epoch counter it moves
 * is the node's, shared by every local member of the team.
 */
struct osoe_am_barrier {
    uint32_t team;              /* the team's number (§5.4, §5.7) */
    uint32_t epoch;             /* the epoch being arrived at or released */
    uint32_t from_node;
    /* The local id of the team's local lead on the node the message goes to:
     * with `team`, the team's name there, which that node's map resolves to
     * its own block (node.h; the team redesign's step D, until which this was
     * reserved and 0). */
    uint32_t lead;
};

/* The job is ending: who ended it, how, and the status every PE leaves with. */
struct osoe_am_abort {
    uint32_t origin_pe;         /* the world rank the abort names */
    int32_t status;
    uint32_t disposition;       /* 1 a fatal, 2 a global exit (node.h's enum) */
    uint32_t reserved;
};

/* A deadlock detector message (deadlock.h's, and the node it came from). */
struct osoe_am_deadlock {
    uint32_t round;
    uint16_t kind;              /* enum osoe_deadlock_kind */
    uint16_t phase;
    uint32_t ok;
    uint32_t from_node;
};

/* A data wire asked for: the node that asks, which the receiver holds to the
 * control wire it came on. */
struct osoe_am_data_wire_wanted {
    uint32_t from_node;
    uint32_t reserved;
};

_Static_assert(sizeof(struct osoe_am_put) % 8 == 0, "a fixed part must keep the payload 8-byte aligned");
_Static_assert(sizeof(struct osoe_am_data_wire_wanted) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_deadlock) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_abort) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_barrier) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_get_request) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_get_reply) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_rmw_request) % 8 == 0, "as above");
_Static_assert(sizeof(struct osoe_am_rmw_reply) % 8 == 0, "as above");

/* The largest fixed part, which bounds the parser's accumulation area. */
#define OSOE_AM_MAX_FIXED 56

/* ---- The table ------------------------------------------------------- */

struct osoe_am_descriptor {
    const char *name;           /* for diagnostics only */
    uint32_t fixed_bytes;
    bool carries_payload;
};

const struct osoe_am_descriptor *osoe_am_table(uint32_t *count);

/*
 * FNV-1a over the table's shape. Exchanged at rendezvous (§6.2, §3.13): every
 * PE must agree, and a mismatch is a mismatched binary, caught at once with a
 * clear message rather than as a garbage dispatch later.
 */
uint32_t osoe_am_table_checksum(void);

/* ---- The sink -------------------------------------------------------- */

/*
 * Where a message's payload goes, and what to do when it is complete.
 *
 * `destination` is called once the fixed part is parsed and **before any
 * payload byte has been consumed**, which is what lets the payload be copied
 * straight where it belongs. Returning NULL discards the payload - the bytes
 * are consumed and dropped, which is what a reply for a PE that has since
 * failed must do rather than stalling the stream.
 *
 * `complete` is called once the whole message has arrived. A message with no
 * payload reaches it directly.
 */
struct osoe_am_sink {
    void *(*destination)(void *context, const struct osoe_message_header *header,
                         const void *fixed, uint32_t payload_bytes);
    void (*complete)(void *context, const struct osoe_message_header *header,
                     const void *fixed, void *payload, uint32_t payload_bytes);
    /* A segment has been fully applied: its sequence number may be
     * acknowledged (§3.4, §4.6 - completion means applied, not received). */
    void (*segment_applied)(void *context, const struct osoe_am_segment_header *segment);
    void *context;
};

/* ---- The parser ------------------------------------------------------ */

enum osoe_am_stage {
    OSOE_AM_STAGE_SEGMENT = 0,  /* accumulating the segment header */
    OSOE_AM_STAGE_MESSAGE,      /* accumulating a message header */
    OSOE_AM_STAGE_FIXED,        /* accumulating a message's fixed part */
    OSOE_AM_STAGE_PAYLOAD,      /* streaming payload to its destination */
    OSOE_AM_STAGE_PADDING,      /* consuming the 8-byte alignment padding */
};

struct osoe_am_parser {
    enum osoe_am_stage stage;
    unsigned char accumulator[OSOE_AM_MAX_FIXED];
    uint32_t have;              /* bytes in the accumulator */
    uint32_t need;              /* bytes this stage wants */

    struct osoe_am_segment_header segment;
    struct osoe_message_header message;
    uint32_t segment_seen;      /* bytes of the segment consumed, header included */
    uint32_t messages_seen;
    uint32_t payload_seen;
    uint32_t payload_bytes;     /* this message's payload length */
    unsigned char *destination; /* where the payload is going, or NULL to discard */
    uint32_t peer;              /* the node this stream comes from, for a fatal to name */
};

/* `peer` is the node the bytes come from, or UINT32_MAX when there is none to
 * name (a test feeding the parser directly). */
void osoe_am_parser_init(struct osoe_am_parser *p, uint32_t peer);

/*
 * Is the parser between segments - nothing of a segment consumed? What the
 * owner of a connection asks when the stream ends: anywhere else, the peer
 * stopped mid-segment (see the note on truncation above).
 */
static inline bool osoe_am_parser_between_segments(const struct osoe_am_parser *p)
{
    return p->stage == OSOE_AM_STAGE_SEGMENT && p->have == 0;
}

/*
 * Feed bytes from the wire. Every byte is consumed: a parser that could refuse
 * bytes would need the caller to hold them, which is the buffer-holding §3.3
 * forbids. A framing error aborts the job (§3.15) - it means the peer and this
 * PE disagree about the format, and there is no recovery that is not a guess.
 *
 * **Nothing points into `bytes` once this returns**: header bytes are copied
 * into the accumulator, payload to its destination, and the sink is handed
 * those and never the input. So the caller may feed a provided buffer where
 * the kernel put it and recycle it at once (progress.c, receive).
 */
void osoe_am_parse(struct osoe_am_parser *p, const void *bytes, size_t length,
                   const struct osoe_am_sink *sink);

#endif /* OSOE_AM_H */
