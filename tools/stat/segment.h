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
};

/*
 * Map `name` read-only. `name` may be given as "/osoe.1.0.0.0", "osoe.1.0.0.0"
 * or "/dev/shm/osoe.1.0.0.0"; all three name the same object.
 *
 * Returns 0, or -1 with a reason in `err`. A segment whose magic is wrong is
 * refused. A segment whose *layout* is unknown is **not**: the directory is
 * frozen precisely so that such a segment can still be printed, so it opens
 * with `table_usable` false and the caller says what it found.
 */
int osoe_segment_open(struct osoe_segment *s, const char *name, char *err, size_t errlen);
void osoe_segment_close(struct osoe_segment *s);

/* The layout this reader was built against. Not a requirement - see above. */
#define OSOE_SEGMENT_KNOWN_LAYOUT 7

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
    uint64_t since_ns;
    char routine[64];
};

bool osoe_segment_blocked(const struct osoe_segment *s, uint32_t pe, struct osoe_segment_blocked *out);

/* CLOCK_MONOTONIC now, so `since_ns` can be turned into an age. The clock is
 * system-wide on Linux, so the tool's reading and the job's are comparable. */
uint64_t osoe_segment_now_ns(void);

/*
 * Every control segment in /dev/shm, sorted, written into `names`. Returns how
 * many were found, which may exceed `max`; at most `max` are written.
 */
int osoe_segment_list(char (*names)[OSOE_SEGMENT_NAME_MAX], int max);

#endif /* OSOE_STAT_SEGMENT_H */
