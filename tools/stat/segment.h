/*
 * Reading an osoe control segment from outside the job (design §3.14).
 *
 * This is the reader half of the self-describing segment, and it is written
 * **against the segment, not the library**: it includes nothing from
 * `shmem/src` and nothing from `base/`, only libc. That is the point. If the
 * reader could reach into the library's headers it would find fields by
 * `offsetof` like the library does, the metric table would describe nothing
 * anyone depends on, and the first structural change would break every tool
 * while the build still passed. Having no other way to find anything is what
 * keeps "self-describing" a mechanism rather than a claim.
 *
 * So exactly two things are known here by layout, and both are frozen:
 *
 *   - `struct osoe_segment_directory`, the segment's opening prefix. §3.14
 *     freezes it: fields are appended, never reordered, resized or removed,
 *     and `directory_bytes` says how much of it the writer filled in.
 *   - `struct osoe_metric_descriptor`, one entry of the metric table. The table is
 *     what makes everything else readable, so its own element must be readable
 *     without it; `directory_bytes`'s companion `metric_descriptor_bytes` exists so a
 *     reader can check that assumption instead of assuming it. A reader that
 *     finds a size it does not know prints the directory and says so.
 *
 * Everything else - every field of the header, of a PE slot, and of a counter
 * block - is found by **name** through the table. A job built from a different
 * commit, with different counters, is read correctly.
 *
 * Nothing here writes to the segment; it is mapped PROT_READ. Loads are
 * `memory_order_acquire` and values are a moment stale (§3.14).
 */
#ifndef OSOE_STAT_SEGMENT_H
#define OSOE_STAT_SEGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "osoenode", little-endian. */
#define OSOE_SEGMENT_MAGIC UINT64_C(0x65646f6e656f736f)

/* Long enough for /osoe.<u64>.<u32>.<u32>.<u32> and then some. */
enum { OSOE_SEGMENT_NAME_MAX = 128 };

/* ---- The frozen layout, and only it -------------------------------- */

struct osoe_segment_directory {
    uint64_t magic;
    uint32_t layout;
    uint32_t directory_bytes;
    uint32_t header_bytes;
    uint32_t reserved;
    uint64_t segment_bytes;

    uint64_t slots_offset;
    uint32_t slot_bytes;
    uint32_t local_count;

    uint64_t metrics_offset;
    uint64_t metrics_bytes;
    uint32_t metrics_count;
    uint32_t metrics_checksum;
    uint32_t metric_descriptor_bytes;

    uint64_t blocks_offset;
    uint64_t blocks_bytes;
    uint32_t block_count;
    uint32_t block_bytes;
    uint32_t blocks_per_pe;

    /* Appended at layout 6: the §3.12 landing zones. A reader built before
     * them sees a shorter `directory_bytes` and simply does not ask. */
    uint64_t landing_offset;
    uint64_t landing_bytes;
    uint32_t landing_slot_bytes;
    uint32_t landing_count;

    /* Appended at layout 12: one record per connection toward a peer node,
     * the fourth metric scope. A reader built before them asks and finds the
     * fields absent, exactly as with the landing zones. */
    uint64_t connections_offset;
    uint64_t connections_bytes;
    uint32_t connection_bytes;
    uint32_t connection_count;
};

/*
 * Is this field present in the directory the writer actually filled in?
 *
 * The append-only rule only works if a reader asks per field rather than for
 * the whole structure. An earlier version of this reader required
 * `directory_bytes >= sizeof(struct osoe_segment_directory)` before it would
 * use the metric table at all, which made the contract one-directional: the
 * moment the library appended a field, a tool built against the older
 * directory was fine but a tool built against the newer one discarded the
 * table of every older segment - losing everything, over a field it was not
 * going to read. Ask for what you are about to use.
 */
#define OSOE_SEGMENT_HAS(dir, field) \
    ((dir)->directory_bytes >= offsetof(struct osoe_segment_directory, field) + \
                               sizeof (dir)->field)

struct osoe_metric_descriptor {
    char     name[40];
    char     unit[8];
    uint16_t type;
    uint16_t kind;
    uint16_t scope;
    uint16_t width;
    uint32_t offset;
    uint32_t reserved;
};

enum osoe_metric_type {
    OSOE_MTYPE_U64 = 1,
    OSOE_MTYPE_U32,
    OSOE_MTYPE_I32,
    OSOE_MTYPE_STRING,
};

enum osoe_metric_kind {
    OSOE_MKIND_COUNTER = 0,
    OSOE_MKIND_GAUGE,
    OSOE_MKIND_HIGHWATER,
    OSOE_MKIND_STATE,
    OSOE_MKIND_INVARIANT,
};

enum osoe_metric_scope {
    OSOE_MSCOPE_NODE = 0,
    OSOE_MSCOPE_PE,
    OSOE_MSCOPE_THREAD,
    OSOE_MSCOPE_CONNECTION,
};

/* ---- An opened segment --------------------------------------------- */

struct osoe_segment {
    char name[OSOE_SEGMENT_NAME_MAX];            /* "/osoe.<job>.<step>.<node>.<session>" */
    const unsigned char *map;
    size_t bytes;
    const struct osoe_segment_directory *dir;
    const struct osoe_metric_descriptor *metrics;  /* NULL when metrics_count == 0 */
    uint32_t nmetrics;
    bool table_usable;                       /* checksum and element size agree */

    /*
     * `dir->local_count` and `dir->block_count`, clamped to what the slots and
     * blocks sections can actually hold (segment_bytes and the next section's
     * offset), so that a directory naming more records than exist cannot turn
     * a loop over "every PE" or "every thread block" into one over four
     * billion (diagnosability review, D-7). A single indexed read was already
     * safe - record() bounds every offset against the mapping - but a *count*
     * used as a loop bound never was: every "for i < local_count" and
     * "for b < block_count" must use these, not the directory's own fields.
     */
    uint32_t local_count;
    uint32_t block_count;
    /* Clamped the same way: connection records between their offset and the
     * end of the segment. Zero on a job of one node, and on any segment
     * written before layout 12. */
    uint32_t connection_count;
};

/*
 * Map `name` read-only. `name` may be given as "/osoe.1.0.0.0", "osoe.1.0.0.0"
 * or "/dev/shm/osoe.1.0.0.0"; all three name the same object.
 *
 * Returns 0, or -1 with a reason in `err`. A segment whose magic is wrong is
 * refused. A segment whose *layout* is unknown is **not**, and is read like any
 * other: everything past the frozen directory is found by name, so the layout
 * decides only whether the watch word is decoded (below). What leaves
 * `table_usable` false is a table this reader cannot check - a descriptor size
 * it does not know, a table outside the mapping, a checksum that fails - and
 * then the caller prints the directory and says what it found.
 */
int osoe_segment_open(struct osoe_segment *s, const char *name, char *err, size_t errlen);
void osoe_segment_close(struct osoe_segment *s);

/*
 * The layouts whose watch word this reader decodes: from the first layout with
 * the encoding it knows to the newest layout it was built against. Neither is a
 * requirement - see above. Outside them the word prints raw, with the layout,
 * and everything found by name is read as usual.
 *
 * `layout` covers the encoding of every word in the segment, not only where
 * fields sit (§10), and the watch word is the one word this reader decodes
 * rather than finds by name. So the range has a top: a layout newer than this
 * reader may have changed the encoding, and must show the raw word rather than
 * a confident wrong range. Raising the top means checking that the new layout
 * did not; shmem.stat, which checks the decode against a waiting PE, fails for
 * a job whose layout is above it.
 *
 *    4  the watch word became a range: region, first 4-byte grain, length
 *    5  the metric table, and this reader's first layout - the first in which
 *       it can find pe.watch_rng at all
 *    6  the landing zones, appended to the directory
 *    7  the arrival handoff, appended
 *    8  the AM layer's node counters, appended
 *    9  pe.blocked.detail_kind; an older segment lacks it, and its blocked
 *       details print raw
 *   10  the finalize team's counters
 *   11  transport.am.passes and send_visits, appended
 *   12  per-team tree state and one record per connection, appended; the
 *       directory grew 136 -> 160 bytes to say where the records are
 *   13  the ring's counters, appended
 *   14  blocked-record detail kinds 5 to 8, the word an internode wait
 *       sleeps on (enum osoe_segment_detail, below)
 *   15  the abort word says how the job ends, with the status and the rank
 *       (enum osoe_segment_abort, below), where it had been 0 or 1 beside a
 *       field for the rank
 *   16  each PE's landing zone also holds its nonblocking gets in flight -
 *       records and an arena after its blocking slot - so the directory's
 *       landing_slot_bytes is now a whole zone rather than a slot and a chunk;
 *       and blocked-record detail kind 9, a wait for them, the detail being
 *       the bytes still in flight
 *
 * None of 5 to 16 changed the watch word's encoding.
 */
#define OSOE_SEGMENT_WATCH_ENCODING_FIRST_LAYOUT 4
#define OSOE_SEGMENT_NEWEST_LAYOUT               16

/* What node.aborting holds since layout 15 (node.h, the segment's encoding): the
 * rank in its low 32 bits, the exit status's low eight bits, then how the job is
 * ending. Before it, any nonzero value was an abort of no stated kind, and
 * node.abort_pe a field of its own; since, node.abort_pe names the word's low
 * half, so a reader of either finds the rank there. */
enum osoe_segment_abort {
    OSOE_SEGMENT_ABORT_NONE = 0,
    OSOE_SEGMENT_ABORT_FATAL,
    OSOE_SEGMENT_ABORT_GLOBAL_EXIT,
};
#define OSOE_SEGMENT_ABORT_STATUS_SHIFT      32
#define OSOE_SEGMENT_ABORT_DISPOSITION_SHIFT 40

/* The metric of that name, or NULL. */
const struct osoe_metric_descriptor *osoe_segment_metric(const struct osoe_segment *s, const char *name);

/*
 * Load a numeric metric with acquire. `index` selects the record within the
 * metric's scope: ignored for NODE, the local PE id for PE, the block number
 * for THREAD. I32 metrics are sign-extended into the uint64_t.
 *
 * False when the metric is absent, is a string, is misaligned for its width, or
 * would read outside its record or the mapping - so a malformed segment is
 * reported rather than dereferenced.
 */
bool osoe_segment_read(const struct osoe_segment *s, const struct osoe_metric_descriptor *m,
                   uint32_t index, uint64_t *out);
bool osoe_segment_read_name(const struct osoe_segment *s, const char *name, uint32_t index,
                        uint64_t *out);

/* A string metric, NUL-terminated into `buf`. Strings in the segment are fixed
 * width and may be unterminated, so this always terminates. */
bool osoe_segment_read_str(const struct osoe_segment *s, const char *name, uint32_t index,
                       char *buf, size_t len);

/* Which PE owns counter block `block`. */
uint32_t osoe_segment_block_pe(const struct osoe_segment *s, uint32_t block);

/*
 * The §3.14 "blocked in" record for local PE `pe`, read under the generation
 * protocol: load the generation, read the fields, load it again, accept only an
 * unchanged even value (model-checked in
 * docs/design/evidence/blocked-record/). `valid` is false if the PE kept
 * republishing through every attempt, which is not a hung PE and is reported as
 * what it is.
 *
 * `blocked` is false when the record is the empty one a wait leaves behind: the
 * PE is not asleep in a library wait.
 */
struct osoe_segment_blocked {
    bool valid;
    bool blocked;
    uint64_t gen;
    uint64_t detail;
    uint32_t detail_kind;       /* 0 if the segment predates layout 9 */
    uint64_t since_ns;
    char routine[64];
};

/*
 * The kinds of `detail`: the segment's encoding (node.h, enum
 * osoe_blocked_detail), which this reader knows by number, as it knows the
 * watch word. Layout 14 added the four that name which of a connection's words
 * an internode wait sleeps on (§3.11), the detail being the peer node; an older
 * segment's internode waits are all DETAIL_NODE.
 */
enum osoe_segment_detail {
    OSOE_SEGMENT_DETAIL_NODE = 1,
    OSOE_SEGMENT_DETAIL_EPOCH,
    OSOE_SEGMENT_DETAIL_OFFSET,
    OSOE_SEGMENT_DETAIL_PE,
    OSOE_SEGMENT_DETAIL_ACKNOWLEDGEMENT,
    OSOE_SEGMENT_DETAIL_TURNOVER,
    OSOE_SEGMENT_DETAIL_CREDIT,
    OSOE_SEGMENT_DETAIL_REPLY,
    OSOE_SEGMENT_DETAIL_GETS_IN_FLIGHT,   /* layout 16: bytes of nonblocking gets */
};

/* `detail` as its kind says - "node 3", "epoch 17", "offset 0x40",
 * "acknowledgement from node 3" - or raw. */
void osoe_segment_blocked_detail(const struct osoe_segment_blocked *b, char *buf, size_t len);

bool osoe_segment_blocked(const struct osoe_segment *s, uint32_t pe, struct osoe_segment_blocked *out);

/* CLOCK_MONOTONIC now, so `since_ns` can be turned into an age. The clock is
 * system-wide on Linux, so the tool's reading and the job's are comparable. */
uint64_t osoe_segment_now_ns(void);

/*
 * Every control segment in /dev/shm, sorted, written into `names`. Returns how
 * many were found, which may exceed `max`; at most `max` are written.
 */
int osoe_segment_list(char (*names)[OSOE_SEGMENT_NAME_MAX], int max);

/* Only the segments that still have a name in /dev/shm - those kept by
 * OSOE_KEEP_CONTROL_SEGMENT, or left by a job that died during init. These,
 * and only these, are what --gc may remove. */
int osoe_segment_list_named(char (*names)[OSOE_SEGMENT_NAME_MAX], int max);

/* Does any process this user can inspect map /dev/shm<name>? --gc refuses to
 * remove a segment that is still mapped, whatever its slots say. */
bool osoe_segment_mapped(const char *name);

#endif /* OSOE_STAT_SEGMENT_H */
