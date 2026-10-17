/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The segment reader against a segment this build did not write (§3.14).
 *
 * "A tool built from a different commit reads a job correctly" is the claim the
 * self-describing table exists to make, and running osoe-stat against a job
 * built from the same tree tests none of it: every offset would match by
 * accident. So this test writes a control segment by hand, with
 *
 *   - record layouts that are nothing like the library's,
 *   - a metric table in a different order from the library's,
 *   - metrics this build has never heard of, and
 *   - a layout version from the future,
 *
 * and requires the reader to find every value it does know, by name, anyway.
 * Only the frozen directory and the metric descriptor are shared, which is
 * exactly the contract segment.h states. It then runs osoe-stat itself against
 * the segment at the layouts either side of the range whose watch word the tool
 * decodes: the one encoding it knows, rather than finds by name; against a
 * crafted dead job, for what --gc must refuse to remove; and for its views of
 * the owners of a node's wires (layout 36), on a segment that has them and on
 * one whose directory ends before them.
 *
 * It also checks the refusals, because a reader that is willing to follow a
 * bad offset into a mapping is worse than one that cannot read at all: a
 * corrupt segment is a thing that happens, and it happens on the machine where
 * something has already gone wrong.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../shmem/tests/check.h"
#include "segment.h"

/* Nothing like struct osoe_pe_slot, on purpose. */
enum {
    HEADER_BYTES = 192,
    SLOT_BYTES   = 128,
    BLOCK_BYTES  = 64,
    LOCAL_COUNT  = 2,
    BLOCKS_PER_PE = 2,
    BLOCK_COUNT  = LOCAL_COUNT * BLOCKS_PER_PE,
    /* Layout 36's owner records: the lead's agent and two progress helpers. */
    OWNER_BYTES  = 80,
    OWNER_COUNT  = 3,

    ND_PASSES     = 120,        /* u64: where a library before layout 36 kept transport.am.passes */
    ND_WORLD_SIZE = 128,        /* u32, in the header */
    ND_ABORTING   = 132,        /* u32 */

    PE_RANK    = 0,             /* u32, in a slot */
    PE_PID     = 4,             /* i32 */
    PE_GEN     = 8,             /* u64 */
    PE_DETAIL  = 16,            /* u64 */
    PE_SINCE   = 24,            /* u64 */
    PE_ROUTINE = 32,            /* char[32] */
    PE_SWEEPS  = 64,            /* u64 */
    PE_WATCH   = 72,            /* u64, the §4.9 watch word */

    TH_CALLS   = 0,             /* u64, in a block */
    TH_BAD     = 8,             /* u64, an invariant */
    TH_CLAIMED = 16,            /* u64 */

    /* In an owner's record, in an order and at offsets the library's has not:
     * the counters first, and its tid and wires after them. */
    AM_PASSES                = 0,       /* u64 */
    AM_WAKES                 = 8,       /* u64 */
    AM_SEGMENTS_SENT         = 16,      /* u64 */
    AM_CONTROL_SEGMENTS_SENT = 24,      /* u64 */
    AM_SEGMENTS_APPLIED      = 32,      /* u64 */
    AM_MESSAGES_APPLIED      = 40,      /* u64 */
    AM_COMPLETIONS           = 48,      /* u64 */
    AM_TID                   = 56,      /* i32 */
    AM_DATA_WIRES            = 60,      /* u32 */
    AM_CONTROL_WIRES         = 64,      /* u32 */
    AM_LAST_PASS             = 72,      /* u64, an instant (layout 40) */
};

/*
 * What each owner's record holds: the lead's agent, a progress helper, and a
 * helper whose thread has not run yet (tid 0). The values differ from field to
 * field and from owner to owner, so that a reader which found one at another's
 * offset, or in another owner's record, would say so; the zeros are as the
 * library has them with helpers, the agent holding control wires only and the
 * helpers data wires only.
 */
static const struct {
    int32_t tid;
    uint32_t data_wires, control_wires;
    uint64_t passes, wakes, segments_sent, control_segments_sent, segments_applied,
             messages_applied, completions, last_pass_ns;
} synthetic_owners[OWNER_COUNT] = {
    /* The agent's last pass the instant 1, as long ago as the machine has been
     * up; helper 1's ahead of the reader's clock, as one written under another
     * would be; helper 2, whose thread has not run, has not passed. */
    { 4001, 0, 3, 1000, 100, 10, 11, 12, 13, 14, 1 },
    { 4002, 2, 0, 2000, 200, 20, 0, 22, 23, 24, UINT64_MAX },
    { 0, 1, 0, 3000, 300, 30, 0, 32, 33, 34, 0 },
};

/* PE 0's watch word: 8 bytes at heap offset 0x40, in the encoding segment.h
 * says osoe-stat decodes - region 0 in bits 63..62, the first 4-byte grain in
 * 61..22, the length in grains in 21..0 - and what the tool shows for it. */
#define WATCH_WORD    ((UINT64_C(0x40) / 4) << 22 | 2)
#define WATCH_DECODED "heap +0x40..+0x48"

static uint32_t fnv1a(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static struct osoe_metric_descriptor desc(const char *name, const char *unit, unsigned type,
                                    unsigned kind, unsigned scope, unsigned width,
                                    uint32_t offset)
{
    struct osoe_metric_descriptor m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", name);
    snprintf(m.unit, sizeof m.unit, "%s", unit);
    m.type = (uint16_t)type;
    m.kind = (uint16_t)kind;
    m.scope = (uint16_t)scope;
    m.width = (uint16_t)width;
    m.offset = offset;
    return m;
}

/*
 * Build the segment. `damage` picks a way to be wrong: 0 is a good segment.
 * Returns the mapping, with `*bytes` its length; the caller unmaps it.
 */
/* OLDER_DIRECTORY is not damage: it is a segment written by a library from
 * before layout 6, whose directory stops before the landing-zone fields. A
 * reader must still use its table (OSOE_SEGMENT_HAS). */
/* HUGE_LOCAL_COUNT and HUGE_BLOCK_COUNT are D-7 (diagnosability review): the
 * directory's own local_count/block_count, not the size of the mapping or of
 * the section they name - so the segment is otherwise this same GOOD one,
 * just lying about how many slots or blocks it holds. Before the fix, a count
 * like this became a loop bound directly: 500-something billion iterations of
 * "for i < local_count" is what the review's fuzz run saw as osoe-stat
 * running silently for ten seconds and more. */
/* HUGE_OWNER_COUNT is the same lie about the owners' records (layout 36).
 * BEFORE_OWNERS is not damage either: it is a segment written by a library of
 * layout 35, whose directory ends where the owners' fields begin. The records
 * are in the mapping all the same, and the table names the owner scope, so a
 * reader that did not ask the directory what the writer filled in would find
 * three owners; and the AM layer's counters are the node's, as they were, in
 * the header. */
enum damage {
    GOOD, BAD_MAGIC, BAD_CHECKSUM, STRANGE_DESC_SIZE, OLDER_DIRECTORY,
    HUGE_LOCAL_COUNT, HUGE_BLOCK_COUNT, HUGE_OWNER_COUNT, BEFORE_OWNERS
};

static void *build(const char *name, enum damage damage, size_t *bytes)
{
    bool before_owners = damage == BEFORE_OWNERS;
    struct osoe_metric_descriptor table[] = {
        /* Deliberately not the library's order: node, thread and PE metrics
         * interleaved, and two names nothing in this build knows. */
        desc("node.world_size", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,
             ND_WORLD_SIZE),
        desc("future.gadget.calls", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_THREAD, 8, TH_CALLS),
        desc("pe.blocked.routine", "", OSOE_MTYPE_STRING, OSOE_MKIND_STATE, OSOE_MSCOPE_PE,
             32, PE_ROUTINE),
        desc("pe.rank", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 4, PE_RANK),
        desc("invariant.future_thing", "", OSOE_MTYPE_U64, OSOE_MKIND_INVARIANT,
             OSOE_MSCOPE_THREAD, 8, TH_BAD),
        desc("pe.blocked.gen", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 8,
             PE_GEN),
        desc("pe.pid", "", OSOE_MTYPE_I32, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 4, PE_PID),
        desc("pe.blocked.detail", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 8,
             PE_DETAIL),
        desc("node.aborting", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,
             ND_ABORTING),
        desc("pe.blocked.since_ns", "ns", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE,
             8, PE_SINCE),
        desc("thread.claimed", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_THREAD, 8,
             TH_CLAIMED),
        desc("pe.sweeps", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, OSOE_MSCOPE_PE, 8,
             PE_SWEEPS),
        desc("pe.watch_rng", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 8,
             PE_WATCH),
        /* Two that must be refused rather than followed: one that runs off the
         * end of its record, and one whose offset is not a multiple of its
         * width. Neither can be dereferenced safely, and both are what a
         * corrupt or mismatched table looks like from here. */
        desc("bogus.past_the_end", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 8,
             SLOT_BYTES - 4),
        desc("bogus.misaligned", "", OSOE_MTYPE_U64, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 8,
             PE_SWEEPS + 4),
        /* The AM layer's counters, a record each owner (layout 36). Before it
         * they were the node's: transport.am.passes then sat in the header,
         * and the rest of these are only there so that the table is the same. */
        desc("transport.am.tid", "", OSOE_MTYPE_I32, OSOE_MKIND_STATE, OSOE_MSCOPE_OWNER, 4,
             AM_TID),
        desc("transport.am.data_wires", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE,
             OSOE_MSCOPE_OWNER, 4, AM_DATA_WIRES),
        desc("transport.am.control_wires", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE,
             OSOE_MSCOPE_OWNER, 4, AM_CONTROL_WIRES),
        desc("transport.am.passes", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             before_owners ? OSOE_MSCOPE_NODE : OSOE_MSCOPE_OWNER, 8,
             before_owners ? ND_PASSES : AM_PASSES),
        desc("transport.am.wakes", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, OSOE_MSCOPE_OWNER,
             8, AM_WAKES),
        desc("transport.am.segments_sent", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_OWNER, 8, AM_SEGMENTS_SENT),
        desc("transport.am.control_segments_sent", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_OWNER, 8, AM_CONTROL_SEGMENTS_SENT),
        desc("transport.am.segments_applied", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_OWNER, 8, AM_SEGMENTS_APPLIED),
        desc("transport.am.messages_applied", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_OWNER, 8, AM_MESSAGES_APPLIED),
        desc("transport.am.completions", "", OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
             OSOE_MSCOPE_OWNER, 8, AM_COMPLETIONS),
        desc("transport.am.last_pass_ns", "ns", OSOE_MTYPE_U64, OSOE_MKIND_STATE,
             OSOE_MSCOPE_OWNER, 8, AM_LAST_PASS),
    };
    uint32_t n = (uint32_t)(sizeof table / sizeof table[0]);

    uint64_t slots_off = HEADER_BYTES;
    uint64_t metrics_off = slots_off + (uint64_t)LOCAL_COUNT * SLOT_BYTES;
    uint64_t blocks_off = metrics_off + (uint64_t)n * sizeof table[0];
    /* The owners' records are last, as the library puts them: the reader's
     * bound for their count is the end of the segment. */
    uint64_t owners_off = blocks_off + (uint64_t)BLOCK_COUNT * BLOCK_BYTES;
    uint64_t total = owners_off + (uint64_t)OWNER_COUNT * OWNER_BYTES;

    /* A path is a plain file, which is what this test is really about: a
     * segment this build did not write, read through nothing but the frozen
     * directory and the table. A bare name is a /dev/shm object, for the
     * spellings of a name. */
    int fd = name[0] == '/' && strchr(name + 1, '/')
             ? open(name, O_CREAT | O_EXCL | O_RDWR, 0600)
             : shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        perror("shm_open");
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    unsigned char *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    struct osoe_segment_directory d;
    memset(&d, 0, sizeof d);
    d.magic = damage == BAD_MAGIC ? UINT64_C(0xdeadbeef) : OSOE_SEGMENT_MAGIC;
    d.layout = 999;                         /* a layout from the future */
    d.directory_bytes = damage == OLDER_DIRECTORY
        ? (uint32_t)offsetof(struct osoe_segment_directory, landing_offset)
        : (uint32_t)sizeof d;
    /* A library of layout 35 ended its directory where the owners' fields now
     * begin, filling in nothing of them. */
    if (before_owners)
        d.directory_bytes = (uint32_t)offsetof(struct osoe_segment_directory, owners_offset);
    d.header_bytes = HEADER_BYTES;
    d.segment_bytes = total;
    d.slots_offset = slots_off;
    d.slot_bytes = SLOT_BYTES;
    /* The mapping and the slots section are still LOCAL_COUNT-sized below;
     * only the directory's count lies (D-7). */
    d.local_count = damage == HUGE_LOCAL_COUNT ? UINT32_C(4278190096) : LOCAL_COUNT;
    d.metrics_offset = metrics_off;
    d.metrics_bytes = (uint64_t)n * sizeof table[0];
    d.metrics_count = n;
    d.metric_descriptor_bytes = damage == STRANGE_DESC_SIZE ? 96 : (uint32_t)sizeof table[0];
    d.metrics_checksum = fnv1a(table, (size_t)d.metrics_bytes) +
                         (damage == BAD_CHECKSUM ? 1u : 0u);
    d.blocks_offset = blocks_off;
    d.blocks_bytes = (uint64_t)BLOCK_COUNT * BLOCK_BYTES;
    /* Likewise: the blocks section is still BLOCK_COUNT-sized (D-7). */
    d.block_count = damage == HUGE_BLOCK_COUNT ? UINT32_C(4278190096) : BLOCK_COUNT;
    d.block_bytes = BLOCK_BYTES;
    d.blocks_per_pe = BLOCKS_PER_PE;
    /* What follows the blocks, which bounds how many the reader will believe
     * (D-7): there are no landing zones here, and the owners' records come next. */
    d.landing_offset = owners_off;
    d.owners_offset = owners_off;
    d.owners_bytes = (uint64_t)OWNER_COUNT * OWNER_BYTES;
    d.owner_bytes = OWNER_BYTES;
    /* Likewise: the owners' section is still OWNER_COUNT-sized (D-7). */
    d.owner_count = damage == HUGE_OWNER_COUNT ? UINT32_C(4278190096) : OWNER_COUNT;
    memcpy(p, &d, sizeof d);
    memcpy(p + metrics_off, table, (size_t)d.metrics_bytes);

    *(uint32_t *)(p + ND_WORLD_SIZE) = 7;
    *(uint32_t *)(p + ND_ABORTING) = 0;
    if (before_owners)
        *(uint64_t *)(p + ND_PASSES) = 777;

    for (uint32_t i = 0; i < LOCAL_COUNT; i++) {
        unsigned char *slot = p + slots_off + (uint64_t)i * SLOT_BYTES;
        *(uint32_t *)(slot + PE_RANK) = 40 + i;
        *(int32_t *)(slot + PE_PID) = -1;           /* sign extension */
        *(uint64_t *)(slot + PE_SWEEPS) = 1000 + i;
    }
    /* PE 0 is blocked; PE 1 is not. Generation even, so a reader accepts it. */
    unsigned char *s0 = p + slots_off;
    *(uint64_t *)(s0 + PE_GEN) = 8;
    *(uint64_t *)(s0 + PE_DETAIL) = 0x1234;
    *(uint64_t *)(s0 + PE_SINCE) = 42;
    memcpy(s0 + PE_ROUTINE, "shmem_future_wait", sizeof "shmem_future_wait");
    *(uint64_t *)(s0 + PE_WATCH) = WATCH_WORD;

    for (uint32_t b = 0; b < BLOCK_COUNT; b++) {
        unsigned char *blk = p + blocks_off + (uint64_t)b * BLOCK_BYTES;
        *(uint64_t *)(blk + TH_CLAIMED) = 1;
        *(uint64_t *)(blk + TH_CALLS) = 10 * (b + 1);
    }

    /* The records are in the mapping whether or not the directory admits to
     * them: BEFORE_OWNERS is a reader's test of asking it. */
    for (uint32_t o = 0; o < OWNER_COUNT; o++) {
        unsigned char *record = p + owners_off + (uint64_t)o * OWNER_BYTES;
        *(uint64_t *)(record + AM_PASSES) = synthetic_owners[o].passes;
        *(uint64_t *)(record + AM_WAKES) = synthetic_owners[o].wakes;
        *(uint64_t *)(record + AM_SEGMENTS_SENT) = synthetic_owners[o].segments_sent;
        *(uint64_t *)(record + AM_CONTROL_SEGMENTS_SENT) =
            synthetic_owners[o].control_segments_sent;
        *(uint64_t *)(record + AM_SEGMENTS_APPLIED) = synthetic_owners[o].segments_applied;
        *(uint64_t *)(record + AM_MESSAGES_APPLIED) = synthetic_owners[o].messages_applied;
        *(uint64_t *)(record + AM_COMPLETIONS) = synthetic_owners[o].completions;
        *(int32_t *)(record + AM_TID) = synthetic_owners[o].tid;
        *(uint32_t *)(record + AM_DATA_WIRES) = synthetic_owners[o].data_wires;
        *(uint32_t *)(record + AM_CONTROL_WIRES) = synthetic_owners[o].control_wires;
        *(uint64_t *)(record + AM_LAST_PASS) = synthetic_owners[o].last_pass_ns;
    }

    *bytes = total;
    return p;
}

static void check_good(const char *name, uint32_t expected_owners)
{
    struct osoe_segment s;
    char err[256] = "";
    CHECK(osoe_segment_open(&s, name, err, sizeof err) == 0);
    CHECK(s.table_usable);
    CHECK(s.dir->layout == 999);            /* unknown, and still readable */

    uint64_t v = 0;
    CHECK(osoe_segment_read_name(&s, "node.world_size", 0, &v) && v == 7);
    CHECK(osoe_segment_read_name(&s, "pe.rank", 0, &v) && v == 40);
    CHECK(osoe_segment_read_name(&s, "pe.rank", 1, &v) && v == 41);
    CHECK(osoe_segment_read_name(&s, "pe.sweeps", 1, &v) && v == 1001);
    CHECK(osoe_segment_read_name(&s, "future.gadget.calls", 2, &v) && v == 30);

    /* An i32 sign-extends; a u32 of the same bits does not. */
    CHECK(osoe_segment_read_name(&s, "pe.pid", 0, &v) && (int64_t)v == -1);

    /* Blocks map to PEs by blocks_per_pe, which is the segment's number. */
    CHECK(osoe_segment_block_pe(&s, 0) == 0);
    CHECK(osoe_segment_block_pe(&s, 3) == 1);

    /* The blocked record, through the generation protocol, in a record whose
     * fields are nowhere near where this build puts them. */
    struct osoe_segment_blocked b;
    CHECK(osoe_segment_blocked(&s, 0, &b));
    CHECK(b.valid && b.blocked);
    CHECK(strcmp(b.routine, "shmem_future_wait") == 0);
    CHECK(b.detail == 0x1234 && b.since_ns == 42 && b.gen == 8);
    CHECK(osoe_segment_blocked(&s, 1, &b) && b.valid && !b.blocked);

    /* An invariant metric is found by kind, which is how invariants.h finds
     * counters it has never been told about. */
    int invariants = 0;
    for (uint32_t i = 0; i < s.nmetrics; i++)
        if (s.metrics[i].kind == OSOE_MKIND_INVARIANT)
            invariants++;
    CHECK(invariants == 1);

    /* A string, copied out and terminated. */
    char buf[64];
    CHECK(osoe_segment_read_str(&s, "pe.blocked.routine", 0, buf, sizeof buf));
    CHECK(strcmp(buf, "shmem_future_wait") == 0);

    /*
     * The owner scope (layout 36): each owner's record, found by name in
     * records laid out as the library's are not. A directory that ends before
     * the owners' fields admits to none (BEFORE_OWNERS; OLDER_DIRECTORY ends
     * sooner still), though the table names the scope and the mapping holds
     * records: only asking the directory what the writer filled in keeps them
     * unread.
     */
    CHECK(s.owner_count == expected_owners);
    for (uint32_t o = 0; o < expected_owners; o++) {
        CHECK(osoe_segment_read_name(&s, "transport.am.passes", o, &v) &&
              v == synthetic_owners[o].passes);
        CHECK(osoe_segment_read_name(&s, "transport.am.wakes", o, &v) &&
              v == synthetic_owners[o].wakes);
        CHECK(osoe_segment_read_name(&s, "transport.am.segments_sent", o, &v) &&
              v == synthetic_owners[o].segments_sent);
        CHECK(osoe_segment_read_name(&s, "transport.am.control_segments_sent", o, &v) &&
              v == synthetic_owners[o].control_segments_sent);
        CHECK(osoe_segment_read_name(&s, "transport.am.segments_applied", o, &v) &&
              v == synthetic_owners[o].segments_applied);
        CHECK(osoe_segment_read_name(&s, "transport.am.messages_applied", o, &v) &&
              v == synthetic_owners[o].messages_applied);
        CHECK(osoe_segment_read_name(&s, "transport.am.completions", o, &v) &&
              v == synthetic_owners[o].completions);
        CHECK(osoe_segment_read_name(&s, "transport.am.tid", o, &v) &&
              (int64_t)v == synthetic_owners[o].tid);
        CHECK(osoe_segment_read_name(&s, "transport.am.data_wires", o, &v) &&
              v == synthetic_owners[o].data_wires);
        CHECK(osoe_segment_read_name(&s, "transport.am.control_wires", o, &v) &&
              v == synthetic_owners[o].control_wires);
    }
    /* The first index past the owners there are, and one that wraps. (Not
     * transport.am.passes: BEFORE_OWNERS has it in the header, a node-scope
     * metric, which any index reads.) */
    CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", expected_owners, &v));
    CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", OWNER_COUNT, &v));
    CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", UINT32_MAX, &v));

    /* Refusals, all of which would otherwise be a read outside a record. */
    CHECK(!osoe_segment_read_name(&s, "bogus.past_the_end", 0, &v));
    CHECK(!osoe_segment_read_name(&s, "bogus.misaligned", 0, &v));
    CHECK(!osoe_segment_read_name(&s, "pe.rank", LOCAL_COUNT, &v));
    CHECK(!osoe_segment_read_name(&s, "future.gadget.calls", BLOCK_COUNT, &v));
    CHECK(!osoe_segment_read_name(&s, "no.such.metric", 0, &v));
    CHECK(osoe_segment_metric(&s, "no.such.metric") == NULL);
    /* A string is not a number and a number is not a string. */
    CHECK(!osoe_segment_read_name(&s, "pe.blocked.routine", 0, &v));
    CHECK(!osoe_segment_read_str(&s, "pe.rank", 0, buf, sizeof buf));

    osoe_segment_close(&s);
}

/*
 * osoe-stat itself against this segment, its layout rewritten to either side of
 * each end of the range whose watch word the tool decodes (segment.h). The
 * library only ever writes its own layout, so no tier-2 test can show one
 * outside that range; this is the test that a layout which may have changed the
 * encoding shows the raw word rather than a confident wrong range - and that
 * the segment is still read, since everything else is found by name. The tool's
 * path comes from OSOE_TEST_STAT, as it does for shmem.stat.
 */
static void check_watch_gate(const char *file, void *map)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        CHECK(0);
        return;
    }
    const struct {
        uint32_t layout;
        bool decoded;
    } cases[] = {
        { OSOE_SEGMENT_WATCH_ENCODING_FIRST_LAYOUT - 1, false },   /* before the encoding */
        { OSOE_SEGMENT_WATCH_ENCODING_FIRST_LAYOUT, true },
        { OSOE_SEGMENT_NEWEST_LAYOUT, true },
        { OSOE_SEGMENT_NEWEST_LAYOUT + 1, false },                 /* newer than the tool */
        { 999, false },
    };
    struct osoe_segment_directory *dir = map;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dir->layout = cases[i].layout;

        char cmd[512], out[32 * 1024], want[96];
        snprintf(cmd, sizeof cmd, "'%s' -d '%s' 2>&1", tool, file);
        FILE *f = popen(cmd, "r");
        CHECK(f != NULL);
        if (!f)
            return;
        size_t n = fread(out, 1, sizeof out - 1, f);
        out[n] = '\0';
        CHECK(pclose(f) == 0);

        if (cases[i].decoded)
            snprintf(want, sizeof want, "%s", WATCH_DECODED);
        else
            snprintf(want, sizeof want, "%#" PRIx64 " (layout %u: encoding unknown)",
                     WATCH_WORD, cases[i].layout);
        if (!strstr(out, want))
            fprintf(stderr, "layout %u: osoe-stat -d does not show \"%s\"\n", cases[i].layout,
                    want);
        CHECK(strstr(out, want) != NULL);
        CHECK(strstr(out, "shmem_future_wait") != NULL);   /* read by name all the same */
    }
}

/* osoe-stat with `args` on `file`: its output, both streams, into `out`, and
 * the status it ended with. */
static int run_stat(const char *args, const char *file, char *out, size_t size)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    out[0] = '\0';
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        CHECK(0);
        return -1;
    }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' %s '%s' 2>&1", tool, args, file);
    FILE *f = popen(cmd, "r");
    CHECK(f != NULL);
    if (!f)
        return -1;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    return pclose(f);
}

/* The next line of `*text` into `line`, each run of spaces in it taken for one
 * and none left at either end, so that a table is compared by its columns in
 * their order and their values, and not by how wide the tool printed them.
 * False at the end of the text. */
static bool next_line(const char **text, char *line, size_t size)
{
    const char *start = *text;
    if (*start == '\0')
        return false;
    const char *end = strchr(start, '\n');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    size_t n = 0;
    for (size_t i = 0; i < length && n + 1 < size; i++) {
        if (start[i] == ' ' && (n == 0 || line[n - 1] == ' '))
            continue;
        line[n++] = start[i];
    }
    while (n > 0 && line[n - 1] == ' ')
        n--;
    line[n] = '\0';
    *text = end ? end + 1 : start + length;
    return true;
}

/* Whether some line of `text` is `want`, as next_line sees it. */
static bool has_line(const char *text, const char *want)
{
    char line[512];
    for (const char *rest = text; next_line(&rest, line, sizeof line);)
        if (strcmp(line, want) == 0)
            return true;
    return false;
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t length = strlen(text), tail = strlen(suffix);
    return length >= tail && strcmp(text + length - tail, suffix) == 0;
}

/* Whether some line of `text`, as next_line sees it, opens with `prefix` and
 * ends with `suffix`: a row whose last column is an age, which the tool takes
 * from its own clock. */
static bool has_line_from_to(const char *text, const char *prefix, const char *suffix)
{
    char line[512];
    for (const char *rest = text; next_line(&rest, line, sizeof line);)
        if (strncmp(line, prefix, strlen(prefix)) == 0 && ends_with(line, suffix))
            return true;
    return false;
}

/* The dump's section that opens with the line `header` (with the newlines
 * round it, so that "owner 1" is not found in "owner 10"), up to the blank line
 * that ends it. */
static bool dump_section(const char *out, const char *header, char *section, size_t size)
{
    const char *start = strstr(out, header);
    if (!start)
        return false;
    const char *end = strstr(start + strlen(header), "\n\n");
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length >= size)
        length = size - 1;
    memcpy(section, start, length);
    section[length] = '\0';
    return true;
}

/*
 * osoe-stat's views of the owners (stat.c; layout 36) against the segment
 * `build` writes, whose three owners are synthetic_owners: the default view's
 * table, the dump's directory line and one section an owner, the JSON dump's
 * array, and the threads view's row for each progress helper that has a
 * thread; and the default view again with one owner, a job with no helpers.
 * The node's line is the owners' sums: 3 data and 3 control wires, 6000
 * passes, 600 wakes, 60 segments sent, 11 control segments sent, 66 segments
 * and 69 messages applied, 72 completions; and no last pass, which is not
 * summed. The agent's last pass has an age in seconds, and the helpers' none.
 */
static void check_owner_views(const char *file, void *map)
{
    const struct osoe_segment_directory *dir = map;
    char out[64 * 1024], section[8192], want[256];

    CHECK(run_stat("-c", file, out, sizeof out) == 0);
    CHECK(has_line(out, "owner who tid dwires cwires passes wakes seg sent ctrl sent seg appl "
                        "msg appl completions last pass"));
    CHECK(has_line_from_to(out, "0 agent 4001 0 3 1000 100 10 11 12 13 14 ", " s"));
    CHECK(has_line(out, "1 helper 1 4002 2 0 2000 200 20 0 22 23 24 -"));     /* clock ahead */
    CHECK(has_line(out, "2 helper 2 - 1 0 3000 300 30 0 32 33 34 -"));     /* no thread yet */
    CHECK(has_line(out, "node 3 3 6000 600 60 11 66 69 72"));
    /* After the PE lines and before the counters. */
    const char *pes = strstr(out, "blocked in"), *owners = strstr(out, "completions"),
               *counters = strstr(out, "\ncounter ");
    CHECK(pes && owners && counters && pes < owners && owners < counters);

    CHECK(run_stat("-d", file, out, sizeof out) == 0);
    snprintf(want, sizeof want, "owners %" PRIu64 " + %d * %d", dir->owners_offset,
             (int)OWNER_COUNT, (int)OWNER_BYTES);
    CHECK(has_line(out, want));
    for (uint32_t o = 0; o < OWNER_COUNT; o++) {
        char header[64];
        if (o == 0)
            snprintf(header, sizeof header, "\n  owner 0 (the lead's agent)\n");
        else
            snprintf(header, sizeof header, "\n  owner %u (progress helper %u)\n", o, o);
        CHECK(dump_section(out, header, section, sizeof section));
        snprintf(want, sizeof want, "transport.am.passes %" PRIu64, synthetic_owners[o].passes);
        CHECK(has_line(section, want));
        snprintf(want, sizeof want, "transport.am.wakes %" PRIu64, synthetic_owners[o].wakes);
        CHECK(has_line(section, want));
        snprintf(want, sizeof want, "transport.am.completions %" PRIu64,
                 synthetic_owners[o].completions);
        CHECK(has_line(section, want));
        snprintf(want, sizeof want, "transport.am.tid %d", (int)synthetic_owners[o].tid);
        CHECK(has_line(section, want));
        snprintf(want, sizeof want, "transport.am.control_wires %u",
                 synthetic_owners[o].control_wires);
        CHECK(has_line(section, want));
        /* An instant, as the record holds it: an age only in the table. */
        snprintf(want, sizeof want, "transport.am.last_pass_ns %" PRIu64,
                 synthetic_owners[o].last_pass_ns);
        CHECK(has_line(section, want));
    }
    /* Not in the node's section, where they were before layout 36. */
    CHECK(dump_section(out, "\n  node\n", section, sizeof section));
    CHECK(strstr(section, "transport.am") == NULL);

    /* The owners, the last array of the document, in order. */
    CHECK(run_stat("-j -d", file, out, sizeof out) == 0);
    static const char passes_key[] = "\"transport.am.passes\":";
    const char *cursor = strstr(out, "\"owners\":[{");
    CHECK(cursor != NULL);
    for (uint32_t o = 0; cursor && o < OWNER_COUNT; o++) {
        cursor = strstr(cursor, passes_key);
        CHECK(cursor != NULL);
        if (cursor) {
            cursor += sizeof passes_key - 1;
            CHECK(strtoull(cursor, NULL, 10) == synthetic_owners[o].passes);
        }
    }
    CHECK(cursor == NULL || strstr(cursor, passes_key) == NULL);       /* and no fourth */
    CHECK(strstr(out, "\"transport.am.tid\":4002") != NULL);
    CHECK(ends_with(out, "}]}\n"));

    /*
     * The threads view: a row for each helper whose thread has run, after the
     * counter blocks - here helper 1's and not helper 2's. The lead PE's pid
     * and helper 1's tid are made this process's and its main thread's, which
     * are one number, so that the kernel can answer for them: its CPU time and
     * switches are two more columns than the "-" it prints when it cannot.
     */
    int me = (int)getpid();
    *(int32_t *)((unsigned char *)map + dir->slots_offset + PE_PID) = me;
    *(int32_t *)((unsigned char *)map + dir->owners_offset + OWNER_BYTES + AM_TID) = me;
    CHECK(run_stat("-T", file, out, sizeof out) == 0);
    bool helper_one = false, helper_two = false, agent = false;
    char row[512];
    for (const char *rest = out; next_line(&rest, row, sizeof row);) {
        helper_two = helper_two || ends_with(row, " helper 2");
        agent = agent || ends_with(row, " agent");              /* it has its counter block */
        if (!ends_with(row, " helper 1"))
            continue;
        helper_one = true;
        snprintf(want, sizeof want, "- 0 %d %d ", me, me);          /* no block, PE 0 */
        CHECK(strncmp(row, want, strlen(want)) == 0);
        unsigned columns = 1;
        for (const char *c = row; *c; c++)
            columns += *c == ' ';
        CHECK(columns == (access("/proc/self/stat", F_OK) == 0 ? 9u : 8u));
    }
    CHECK(helper_one);
    CHECK(!helper_two);
    CHECK(!agent);

    /*
     * No helpers, the default: the directory admits to one owner, though the
     * mapping holds three records. It has its row, and the node's line, which
     * would say the same, is not printed.
     */
    ((struct osoe_segment_directory *)map)->owner_count = 1;
    CHECK(run_stat("-c", file, out, sizeof out) == 0);
    CHECK(has_line_from_to(out, "0 agent 4001 0 3 1000 100 10 11 12 13 14 ", " s"));
    bool others = false;
    for (const char *rest = out; next_line(&rest, row, sizeof row);)
        others = others || strncmp(row, "node ", 5) == 0 || strstr(row, "helper") != NULL;
    CHECK(!others);
}

/*
 * The same tool on a segment of layout 35 (BEFORE_OWNERS): it has no owners to
 * show, in any view, and the AM layer's counters are where the table has them,
 * the node's.
 */
static void check_before_owners_views(const char *file)
{
    char out[64 * 1024], section[8192];

    CHECK(run_stat("-c", file, out, sizeof out) == 0);
    CHECK(strstr(out, "completions") == NULL);                /* no table of owners */

    CHECK(run_stat("-d", file, out, sizeof out) == 0);
    CHECK(strstr(out, "\n    owners ") == NULL);              /* no directory line */
    CHECK(strstr(out, "\n  owner ") == NULL);                  /* and no section */
    CHECK(dump_section(out, "\n  node\n", section, sizeof section));
    CHECK(has_line(section, "transport.am.passes 777"));

    CHECK(run_stat("-j -d", file, out, sizeof out) == 0);
    CHECK(strstr(out, "\"owners\":[]") != NULL);
    CHECK(strstr(out, "\"transport.am.passes\":777") != NULL);
}

static void check_refused(const char *name, enum damage damage, bool expect_open)
{
    size_t bytes = 0;
    void *p = build(name, damage, &bytes);
    CHECK(p != NULL);
    if (!p)
        return;

    struct osoe_segment s;
    char err[256] = "";
    int rc = osoe_segment_open(&s, name, err, sizeof err);
    CHECK((rc == 0) == expect_open);
    if (rc == 0) {
        /* Openable but not usable: the directory is still printable, which is
         * the whole reason it is frozen separately. */
        CHECK(!s.table_usable);
        CHECK(s.dir->local_count == LOCAL_COUNT);
        osoe_segment_close(&s);
    } else {
        CHECK(err[0] != '\0');
    }
    munmap(p, bytes);
    unlink(name);
}

/*
 * D-7: a directory whose local_count or block_count lies about how much its
 * section holds must not turn a loop over "every PE" or "every thread block"
 * into one over the lie. The segment opens fine and its table is still usable
 * - only a count is wrong, not the checksum - so this is not check_refused's
 * case; it is segment.c clamping local_count/block_count itself to what the
 * section can actually hold, which is what every loop bound in stat.c reads
 * instead of the directory's own (untrusted) field.
 */
static void check_huge_counts(const char *name, enum damage damage)
{
    size_t bytes = 0;
    void *p = build(name, damage, &bytes);
    CHECK(p != NULL);
    if (!p)
        return;

    struct osoe_segment s;
    char err[256] = "";
    CHECK(osoe_segment_open(&s, name, err, sizeof err) == 0);
    CHECK(s.table_usable);

    /* The lie is still there to see - print_identity shows it - but nothing
     * that loops may read it. */
    if (damage == HUGE_LOCAL_COUNT) {
        CHECK(s.dir->local_count == UINT32_C(4278190096));
        CHECK(s.local_count == LOCAL_COUNT);
        CHECK(s.block_count == BLOCK_COUNT);        /* the other count: untouched */
        CHECK(osoe_segment_read_name(&s, "pe.rank", LOCAL_COUNT - 1, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "pe.rank", LOCAL_COUNT, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "pe.rank", 1000000, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "pe.rank", UINT32_MAX, &(uint64_t){ 0 }));
    } else if (damage == HUGE_OWNER_COUNT) {
        CHECK(s.dir->owner_count == UINT32_C(4278190096));
        CHECK(s.owner_count == OWNER_COUNT);
        CHECK(s.local_count == LOCAL_COUNT);         /* the other counts: untouched */
        CHECK(s.block_count == BLOCK_COUNT);
        CHECK(osoe_segment_read_name(&s, "transport.am.wakes", OWNER_COUNT - 1,
                                     &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", OWNER_COUNT, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", 1000000, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "transport.am.wakes", UINT32_MAX, &(uint64_t){ 0 }));
    } else {
        CHECK(s.dir->block_count == UINT32_C(4278190096));
        CHECK(s.block_count == BLOCK_COUNT);
        CHECK(s.local_count == LOCAL_COUNT);         /* the other count: untouched */
        CHECK(osoe_segment_read_name(&s, "future.gadget.calls", BLOCK_COUNT - 1,
                                     &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "future.gadget.calls", BLOCK_COUNT, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "future.gadget.calls", 1000000, &(uint64_t){ 0 }));
        CHECK(!osoe_segment_read_name(&s, "future.gadget.calls", UINT32_MAX, &(uint64_t){ 0 }));
    }

    /* The blocked record goes through the same clamp: an index past the real
     * section is refused, not read as if the lie were true. */
    struct osoe_segment_blocked b;
    CHECK(!osoe_segment_blocked(&s, LOCAL_COUNT, &b));
    CHECK(!b.valid);

    osoe_segment_close(&s);
    munmap(p, bytes);
    unlink(name);
}

/* A blocked record's detail as its kind says (segment.h). The layout-14 kinds
 * name a connection's word and the peer node; a kind this reader does not know
 * prints raw. shmem.unswept-wait checks the same text against the library's. */
static void check_details(void)
{
    static const struct {
        uint32_t kind;
        const char *want;
    } cases[] = {
        { OSOE_SEGMENT_DETAIL_NODE, "node 3" },
        { OSOE_SEGMENT_DETAIL_EPOCH, "epoch 3" },
        { OSOE_SEGMENT_DETAIL_ACKNOWLEDGEMENT, "acknowledgement from node 3" },
        { OSOE_SEGMENT_DETAIL_TURNOVER, "buffer turnover toward node 3" },
        { OSOE_SEGMENT_DETAIL_CREDIT, "reply credit toward node 3" },
        { OSOE_SEGMENT_DETAIL_REPLY, "reply from node 3" },
        { OSOE_SEGMENT_DETAIL_GETS_IN_FLIGHT, "nonblocking gets, 3 bytes in flight" },
        { OSOE_SEGMENT_DETAIL_STATICS_OFFSET, "statics offset 0x3" },
        { 99, "+0x3" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct osoe_segment_blocked b = { .valid = true, .blocked = true, .detail = 3,
                                          .detail_kind = cases[i].kind };
        char buf[64];
        osoe_segment_blocked_detail(&b, buf, sizeof buf);
        CHECK(strcmp(buf, cases[i].want) == 0);
    }
}

/*
 * What a start that never finished leaves besides the control segment (segment.h):
 * which names are those of a node's job tables and connection area, and what
 * the head of each says of its creator. The magic numbers and the offsets of
 * the pid are the library's, written out here as the reader has them - it
 * includes nothing of the library's, and the library asserts the offsets where
 * its headers are declared. What agrees with a real job is shmem.leftovers.gc's;
 * these are the reader's own cases, and the ones a job cannot make.
 */
enum { JOB_PID_OFFSET = 24, CONNECTION_AREA_PID_OFFSET = 20 };
#define JOB_MAGIC UINT64_C(0x00626f6a656f736f)               /* "osoejob" */
#define CONNECTION_AREA_MAGIC UINT64_C(0x61657261656f736f)   /* "osoearea" */

/* An object in /dev/shm of `len` bytes: the magic, then a pid at `pid_offset`
 * when there is one, and zeros. */
static void plant(const char *name, uint64_t magic, size_t pid_offset, int32_t pid, size_t len)
{
    unsigned char bytes[8192] = { 0 };
    memcpy(bytes, &magic, sizeof magic);
    if (pid != 0 && pid_offset + sizeof pid <= sizeof bytes)
        memcpy(bytes + pid_offset, &pid, sizeof pid);
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    CHECK(len <= sizeof bytes && write(fd, bytes, len) == (ssize_t)len);
    close(fd);
}

static void check_startup_objects(void)
{
    static const struct {
        const char *name;
        enum osoe_startup_object_kind kind;
    } names[] = {
        { "osoe.1.0.0.job.0", OSOE_STARTUP_OBJECT_JOB },
        { "/osoe.123.4.5.job.67", OSOE_STARTUP_OBJECT_JOB },
        { "osoe.1.0.0.conn.12", OSOE_STARTUP_OBJECT_CONNECTION_AREA },
        { "/dev/shm/osoe.1.0.2.conn.0", OSOE_STARTUP_OBJECT_CONNECTION_AREA },
        { "osoe.1.0.0.0", OSOE_STARTUP_OBJECT_NONE },           /* a control segment */
        { "osoe.1.0.0.0.heap.1", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.0.statics.1", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.job.0", OSOE_STARTUP_OBJECT_NONE },         /* step 2's, which had no node */
        { "osoe.1.0.0.job", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.job.", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.job.x", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.job.0.x", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.jobs.0", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.0.connection.0", OSOE_STARTUP_OBJECT_NONE },
        { "osoe.1.0.x.job.0", OSOE_STARTUP_OBJECT_NONE },
        { "osoe..0.0.job.0", OSOE_STARTUP_OBJECT_NONE },
        { "other.1.0.0.job.0", OSOE_STARTUP_OBJECT_NONE },
        { "osoetest.1", OSOE_STARTUP_OBJECT_NONE },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        CHECK(osoe_startup_object_kind_of(names[i].name) == names[i].kind);

    /* The creator each header records, in every state an object is in while it
     * is made: not there, sized and not written, written to the magic and not
     * to the pid, and done; and what is not an object of its kind at all. */
    uint32_t base = UINT32_C(930000000) + (uint32_t)getpid() % 1000000;
    char absent[96], empty[96], zeros[96], short_job[96], done_job[96], done_area[96],
        no_pid[96], wrong[96], swapped[96], listed_control[96];
    snprintf(absent, sizeof absent, "/osoe.%u.0.0.job.0", base);
    snprintf(empty, sizeof empty, "/osoe.%u.0.1.job.0", base);
    snprintf(zeros, sizeof zeros, "/osoe.%u.0.2.conn.0", base);
    snprintf(short_job, sizeof short_job, "/osoe.%u.0.3.job.0", base);
    snprintf(done_job, sizeof done_job, "/osoe.%u.0.4.job.0", base);
    snprintf(done_area, sizeof done_area, "/osoe.%u.0.5.conn.0", base);
    snprintf(no_pid, sizeof no_pid, "/osoe.%u.0.6.job.0", base);
    snprintf(wrong, sizeof wrong, "/osoe.%u.0.7.job.0", base);
    snprintf(swapped, sizeof swapped, "/osoe.%u.0.8.conn.0", base);
    snprintf(listed_control, sizeof listed_control, "/osoe.%u.0.9.0", base);

    plant(empty, 0, 0, 0, 0);
    plant(zeros, 0, 0, 0, 8192);
    plant(short_job, JOB_MAGIC, 0, 0, JOB_PID_OFFSET);          /* ends where the pid begins */
    plant(done_job, JOB_MAGIC, JOB_PID_OFFSET, 4242, 8192);
    plant(done_area, CONNECTION_AREA_MAGIC, CONNECTION_AREA_PID_OFFSET, 4343, 8192);
    plant(no_pid, JOB_MAGIC, JOB_PID_OFFSET, 0, 8192);
    plant(wrong, UINT64_C(0x2145534f544f4e), JOB_PID_OFFSET, 4242, 8192);
    plant(swapped, JOB_MAGIC, CONNECTION_AREA_PID_OFFSET, 4343, 8192);   /* the other kind's magic */
    plant(listed_control, 0, 0, 0, 8192);

    /* Another user's, as far as this one can tell: no permission to open it. As
     * root there is none to lack. */
    char locked[96];
    snprintf(locked, sizeof locked, "/osoe.%u.0.10.job.0", base);
    plant(locked, JOB_MAGIC, JOB_PID_OFFSET, 4242, 8192);
    int locked_fd = shm_open(locked, O_RDWR, 0);
    CHECK(locked_fd >= 0 && fchmod(locked_fd, 0) == 0);
    if (locked_fd >= 0)
        close(locked_fd);

    char err[256] = "";
    CHECK(osoe_startup_object_creator(absent, err, sizeof err) == OSOE_CREATOR_ABSENT);
    CHECK(osoe_startup_object_creator(locked, err, sizeof err) ==
          (geteuid() == 0 ? 4242 : OSOE_CREATOR_FORBIDDEN));
    CHECK(osoe_startup_object_creator(empty, err, sizeof err) == OSOE_CREATOR_UNRECORDED);
    CHECK(osoe_startup_object_creator(zeros, err, sizeof err) == OSOE_CREATOR_UNRECORDED);
    CHECK(osoe_startup_object_creator(short_job, err, sizeof err) == OSOE_CREATOR_UNRECORDED);
    CHECK(osoe_startup_object_creator(no_pid, err, sizeof err) == OSOE_CREATOR_UNRECORDED);
    CHECK(osoe_startup_object_creator(done_job, err, sizeof err) == 4242);
    CHECK(osoe_startup_object_creator(done_area, err, sizeof err) == 4343);
    /* The three spellings of one name, as for a segment. */
    CHECK(osoe_startup_object_creator(done_job + 1, err, sizeof err) == 4242);
    char path[128];
    snprintf(path, sizeof path, "/dev/shm%s", done_job);
    CHECK(osoe_startup_object_creator(path, err, sizeof err) == 4242);
    /* Not an object of its kind: another magic, the other kind's, a name that is
     * neither - none is something to remove. */
    err[0] = '\0';
    CHECK(osoe_startup_object_creator(wrong, err, sizeof err) == OSOE_CREATOR_UNREADABLE);
    CHECK(strstr(err, "is not an osoe job object") != NULL);
    err[0] = '\0';
    CHECK(osoe_startup_object_creator(swapped, err, sizeof err) == OSOE_CREATOR_UNREADABLE);
    CHECK(strstr(err, "is not an osoe connection area") != NULL);
    err[0] = '\0';
    CHECK(osoe_startup_object_creator(listed_control, err, sizeof err) == OSOE_CREATOR_UNREADABLE);
    CHECK(err[0] != '\0');

    /* The listing finds the objects planted above and none else of the job: not
     * the control-segment-shaped one, and each once, in order. */
    char found[64][OSOE_SEGMENT_NAME_MAX];
    int total = osoe_startup_object_list_named(found, 64);
    CHECK(total >= 9 && total <= 64);       /* the nine planted, and any a real job holds */
    int seen = 0;
    for (int i = 0; i < total && i < 64; i++) {
        CHECK(i == 0 || strcmp(found[i - 1], found[i]) < 0);
        char prefix[64];
        snprintf(prefix, sizeof prefix, "/osoe.%u.0.", base);
        if (strncmp(found[i], prefix, strlen(prefix)) == 0) {
            seen++;
            CHECK(strcmp(found[i], listed_control) != 0);
        }
    }
    CHECK(seen == 9);

    const char *planted[] = { empty, zeros, short_job, done_job, done_area, no_pid, wrong,
                              swapped, locked, listed_control };
    for (size_t i = 0; i < sizeof planted / sizeof planted[0]; i++)
        shm_unlink(planted[i]);
}

/*
 * What --gc removes on a dead job's word, and what it refuses to (stat.c). A
 * segment's heap and statics names are written by whoever can write the
 * segment, and --gc may be run by someone else - root, clearing /dev/shm - so a
 * name is removed only if it is the segment's own, "<segment>.heap.<n>" or
 * "<segment>.statics.<n>", and its object is a regular one owned as the
 * segment is (the adversarial read of 2026-10-02). This segment is a crafted
 * one: both PEs joined and gone, at a pid no process can have, and its four
 * names
 *
 *   - PE 0's heap: an object of the test's that is no child of the segment;
 *   - PE 0's statics: the segment's own, which is removed;
 *   - PE 1's heap: a look-alike, "<segment>9.heap.1", another segment's;
 *   - PE 1's statics: the segment's own name, but a FIFO, on which an open that
 *     blocked would wait for a writer forever.
 *
 * Another owner cannot be made without privilege; owned_by's st_uid is one test
 * with its S_ISREG, which the FIFO exercises. A FIFO in place of the segment
 * itself, and of a job table, must be refused at once as well.
 */
enum {
    GC_PHASE   = 0,             /* u32, in a slot */
    GC_PID     = 4,             /* i32 */
    GC_HEAP    = 8,             /* char[56] */
    GC_STATICS = 64,            /* char[56] */
    GC_NAME_BYTES = 56,
    GC_CREATOR = 128,           /* i32, in the header: node.creator_pid */
};

/* A name into a slot's field, which the new object's zeros terminate. */
static void put_name(unsigned char *field, const char *name)
{
    size_t n = strlen(name);
    CHECK(n < GC_NAME_BYTES);
    memcpy(field, name, n < GC_NAME_BYTES ? n : GC_NAME_BYTES - 1);
}

/* Every slot at `phase` (0: not joined), every PE gone, and the segment's
 * maker `creator`. */
static void *build_collectable(const char *name, const char *names[LOCAL_COUNT][2],
                               uint32_t phase, int32_t creator, size_t *bytes)
{
    struct osoe_metric_descriptor table[] = {
        desc("node.creator_pid", "", OSOE_MTYPE_I32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,
             GC_CREATOR),
        desc("pe.phase", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 4, GC_PHASE),
        desc("pe.pid", "", OSOE_MTYPE_I32, OSOE_MKIND_STATE, OSOE_MSCOPE_PE, 4, GC_PID),
        desc("pe.heap_name", "", OSOE_MTYPE_STRING, OSOE_MKIND_STATE, OSOE_MSCOPE_PE,
             GC_NAME_BYTES, GC_HEAP),
        desc("pe.statics_name", "", OSOE_MTYPE_STRING, OSOE_MKIND_STATE, OSOE_MSCOPE_PE,
             GC_NAME_BYTES, GC_STATICS),
    };
    uint32_t n = (uint32_t)(sizeof table / sizeof table[0]);
    uint64_t slots_off = HEADER_BYTES;
    uint64_t metrics_off = slots_off + (uint64_t)LOCAL_COUNT * SLOT_BYTES;
    uint64_t blocks_off = metrics_off + (uint64_t)n * sizeof table[0];
    uint64_t total = blocks_off + (uint64_t)BLOCK_COUNT * BLOCK_BYTES;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)total) != 0) {
        perror("shm_open");
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    unsigned char *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    struct osoe_segment_directory d;
    memset(&d, 0, sizeof d);
    d.magic = OSOE_SEGMENT_MAGIC;
    d.layout = OSOE_SEGMENT_NEWEST_LAYOUT;
    d.directory_bytes = (uint32_t)sizeof d;
    d.header_bytes = HEADER_BYTES;
    d.segment_bytes = total;
    d.slots_offset = slots_off;
    d.slot_bytes = SLOT_BYTES;
    d.local_count = LOCAL_COUNT;
    d.metrics_offset = metrics_off;
    d.metrics_bytes = (uint64_t)n * sizeof table[0];
    d.metrics_count = n;
    d.metric_descriptor_bytes = (uint32_t)sizeof table[0];
    d.metrics_checksum = fnv1a(table, (size_t)d.metrics_bytes);
    d.blocks_offset = blocks_off;
    d.blocks_bytes = (uint64_t)BLOCK_COUNT * BLOCK_BYTES;
    d.block_count = BLOCK_COUNT;
    d.block_bytes = BLOCK_BYTES;
    d.blocks_per_pe = BLOCKS_PER_PE;
    memcpy(p, &d, sizeof d);
    memcpy(p + metrics_off, table, (size_t)d.metrics_bytes);

    *(int32_t *)(p + GC_CREATOR) = creator;
    for (uint32_t i = 0; i < LOCAL_COUNT; i++) {
        unsigned char *slot = p + slots_off + (uint64_t)i * SLOT_BYTES;
        *(uint32_t *)(slot + GC_PHASE) = phase;
        *(int32_t *)(slot + GC_PID) = INT32_MAX;            /* past any pid_max: gone */
        put_name(slot + GC_HEAP, names[i][0]);
        put_name(slot + GC_STATICS, names[i][1]);
    }
    *bytes = total;
    return p;
}

/* Whether /dev/shm holds `name`, of whatever kind. */
static bool named(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "/dev/shm%s", name);
    struct stat st;
    return lstat(path, &st) == 0;
}

/* osoe-stat --gc `name`, its output (both streams) into `out`; its status. */
static int run_gc(const char *tool, const char *name, char *out, size_t size)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' --gc '%s' 2>&1", tool, name);
    out[0] = '\0';
    FILE *f = popen(cmd, "r");
    CHECK(f != NULL);
    if (!f)
        return -1;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    int status = pclose(f);
    printf("--- osoe-stat --gc %s\n%s", name, out);
    return status;
}

static void plant_object(const char *name)
{
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0);
    if (fd >= 0)
        close(fd);
}

static void plant_fifo(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "/dev/shm%s", name);
    unlink(path);
    CHECK(mkfifo(path, 0600) == 0);
}

static void check_gc_refusals(void)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        CHECK(0);
        return;
    }
    int pid = (int)getpid();
    char segment[64], foreign[64], own[96], look_alike[96], fifo[96];
    snprintf(segment, sizeof segment, "/osoe-gc-test.%d", pid);
    snprintf(foreign, sizeof foreign, "/osoe-gc-victim.%d", pid);
    snprintf(own, sizeof own, "%s.statics.0", segment);
    snprintf(look_alike, sizeof look_alike, "%s9.heap.1", segment);
    snprintf(fifo, sizeof fifo, "%s.statics.1", segment);
    plant_object(foreign);
    plant_object(own);
    plant_object(look_alike);
    plant_fifo(fifo);

    const char *names[LOCAL_COUNT][2] = { { foreign, own }, { look_alike, fifo } };
    shm_unlink(segment);
    size_t bytes = 0;
    void *p = build_collectable(segment, names, 3, INT32_MAX, &bytes);   /* joined, gone */
    CHECK(p != NULL);
    if (p)
        munmap(p, bytes);       /* a mapping of this process's would keep the segment */

    char out[8192], want[256];
    CHECK(run_gc(tool, segment, out, sizeof out) == 0);
    snprintf(want, sizeof want, "%s names %s as a heap: not its own; not removed", segment,
             foreign);
    CHECK(strstr(out, want) != NULL);
    snprintf(want, sizeof want, "%s names %s as a heap: not its own; not removed", segment,
             look_alike);
    CHECK(strstr(out, want) != NULL);
    snprintf(want, sizeof want, "%s is not owned as %s is, or is no shared-memory object; "
             "not removed", fifo, segment);
    CHECK(strstr(out, want) != NULL);
    snprintf(want, sizeof want, "%s: removed", own);
    CHECK(strstr(out, want) != NULL);
    snprintf(want, sizeof want, "%s: removed", segment);
    CHECK(strstr(out, want) != NULL);
    CHECK(named(foreign) && named(look_alike) && named(fifo));
    CHECK(!named(own) && !named(segment));

    /* A FIFO by a segment's name, and by a job table's: refused, not waited on.
     * A wait would end at ctest's timeout instead. */
    char pipe_segment[64], pipe_job[64];
    snprintf(pipe_segment, sizeof pipe_segment, "/osoe-gc-test.%d.pipe", pid);
    snprintf(pipe_job, sizeof pipe_job, "/osoe.%u.0.0.job.0",
             UINT32_C(950000000) + (uint32_t)pid % 1000000);
    plant_fifo(pipe_segment);
    plant_fifo(pipe_job);
    CHECK(run_gc(tool, pipe_segment, out, sizeof out) != 0);
    snprintf(want, sizeof want, "/dev/shm%s is not a shared-memory object: no segment",
             pipe_segment);
    CHECK(strstr(out, want) != NULL);
    CHECK(run_gc(tool, pipe_job, out, sizeof out) != 0);
    snprintf(want, sizeof want, "/dev/shm%s is not a shared-memory object osoe made", pipe_job);
    CHECK(strstr(out, want) != NULL);
    CHECK(named(pipe_segment) && named(pipe_job));

    const char *planted[] = { foreign, own, look_alike, fifo, segment, pipe_segment, pipe_job };
    for (size_t i = 0; i < sizeof planted / sizeof planted[0]; i++) {
        char path[128];
        snprintf(path, sizeof path, "/dev/shm%s", planted[i]);
        unlink(path);
    }
}

/*
 * What a start leaves of its own (stat.c, §3.14): a segment no PE has joined,
 * which is a start under way while the process that made it lives and a failed
 * start's once it is gone, and a tombstone, a directory alone, collectable once
 * nothing maps it. These are the cases a job cannot be made to stand still in:
 * a maker not yet recorded, one still running - this test - and a tombstone
 * still mapped.
 */
static void check_gc_starts(void)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        CHECK(0);
        return;
    }
    int pid = (int)getpid();
    const char *none[LOCAL_COUNT][2] = { { "", "" }, { "", "" } };
    const struct {
        int32_t creator;
        const char *said;
    } cases[] = {
        { 0, "no PE has joined yet; left alone" },
        { pid, "no PE has joined yet, and the process that made it runs; left alone" },
        { INT32_MAX, "removed" },
    };
    char out[8192], want[256], segment[64];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(segment, sizeof segment, "/osoe-gc-start.%d.%zu", pid, i);
        shm_unlink(segment);
        size_t bytes = 0;
        void *p = build_collectable(segment, none, 0, cases[i].creator, &bytes);
        CHECK(p != NULL);
        if (p)
            munmap(p, bytes);
        CHECK(run_gc(tool, segment, out, sizeof out) == 0);
        snprintf(want, sizeof want, "%s: %s", segment, cases[i].said);
        CHECK(strstr(out, want) != NULL);
        CHECK(named(segment) == (cases[i].creator != INT32_MAX));
        shm_unlink(segment);
    }

    /* A tombstone, as node.c writes one: mapped by this process, then not. */
    snprintf(segment, sizeof segment, "/osoe-gc-tombstone.%d", pid);
    shm_unlink(segment);
    int fd = shm_open(segment, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0);
    unsigned char *t = fd >= 0 ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                               : MAP_FAILED;
    if (fd >= 0)
        close(fd);
    CHECK(t != MAP_FAILED);
    if (t == MAP_FAILED)
        return;
    struct osoe_segment_directory d;
    memset(&d, 0, sizeof d);
    d.magic = OSOE_SEGMENT_MAGIC;
    d.layout = OSOE_SEGMENT_NEWEST_LAYOUT;
    d.directory_bytes = (uint32_t)sizeof d;
    d.header_bytes = HEADER_BYTES;
    d.segment_bytes = 4096;
    memcpy(t, &d, sizeof d);
    CHECK(run_gc(tool, segment, out, sizeof out) == 0);
    snprintf(want, sizeof want, "%s: a failed start's tombstone that a live process still "
             "maps; left alone", segment);
    CHECK(strstr(out, want) != NULL);
    CHECK(named(segment));
    munmap(t, 4096);
    CHECK(run_gc(tool, segment, out, sizeof out) == 0);
    snprintf(want, sizeof want, "%s: removed", segment);
    CHECK(strstr(out, want) != NULL);
    CHECK(!named(segment));
    shm_unlink(segment);
}

int main(void)
{
    check_details();
    check_startup_objects();
    check_gc_refusals();
    check_gc_starts();

    char name[64], file[96];
    snprintf(name, sizeof name, "/osoe-segment-test.%d", (int)getpid());
    snprintf(file, sizeof file, "/tmp/osoe-segment-test.%d", (int)getpid());

    size_t bytes = 0;
    void *p = build(file, GOOD, &bytes);
    CHECK(p != NULL);
    if (p) {
        check_good(file, OWNER_COUNT);
        check_watch_gate(file, p);
        check_owner_views(file, p);
        munmap(p, bytes);
        unlink(file);
    }

    /* The three spellings of one name, on a named /dev/shm object - which is
     * what a segment kept by OSOE_KEEP_CONTROL_SEGMENT is. */
    p = build(name, GOOD, &bytes);
    CHECK(p != NULL);
    if (p) {
        check_good(name, OWNER_COUNT);
        check_good(name + 1, OWNER_COUNT);
        char path[128];
        snprintf(path, sizeof path, "/dev/shm%s", name);
        check_good(path, OWNER_COUNT);
        munmap(p, bytes);
        shm_unlink(name);
    }

    /*
     * A directory shorter than this reader's, from an older library. It holds
     * every field the table needs, so the table must be used. The reader once
     * required the whole structure and discarded the table of every older
     * segment; this is the case that saw it (second review, premises: that
     * mutation passed every test until now).
     */
    p = build(file, OLDER_DIRECTORY, &bytes);
    CHECK(p != NULL);
    if (p) {
        check_good(file, 0);
        munmap(p, bytes);
        unlink(file);
    }

    /*
     * And one that is short by the owners' fields alone, as a library of layout
     * 35 wrote it: every other field is there, and so are the records the
     * fields would name, so what keeps them unread is the directory's own
     * length. The tool must show the job as it always did - no owners in any
     * view, the AM layer's counter in the node's section - and not fail.
     */
    p = build(file, BEFORE_OWNERS, &bytes);
    CHECK(p != NULL);
    if (p) {
        check_good(file, 0);
        check_before_owners_views(file);
        munmap(p, bytes);
        unlink(file);
    }

    check_refused(file, BAD_MAGIC, false);
    check_refused(file, BAD_CHECKSUM, true);
    check_refused(file, STRANGE_DESC_SIZE, true);

    check_huge_counts(file, HUGE_LOCAL_COUNT);
    check_huge_counts(file, HUGE_BLOCK_COUNT);
    check_huge_counts(file, HUGE_OWNER_COUNT);

    /* And a segment that is not there at all. */
    struct osoe_segment s;
    char err[256] = "";
    CHECK(osoe_segment_open(&s, "/osoe.definitely.not.here.0", err, sizeof err) != 0);
    CHECK(strstr(err, "no such job") != NULL);

    return CHECK_DONE();
}
