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
 * decodes: the one encoding it knows, rather than finds by name; and against a
 * crafted dead job, for what --gc must refuse to remove.
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
enum damage {
    GOOD, BAD_MAGIC, BAD_CHECKSUM, STRANGE_DESC_SIZE, OLDER_DIRECTORY,
    HUGE_LOCAL_COUNT, HUGE_BLOCK_COUNT
};

static void *build(const char *name, enum damage damage, size_t *bytes)
{
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
    };
    uint32_t n = (uint32_t)(sizeof table / sizeof table[0]);

    uint64_t slots_off = HEADER_BYTES;
    uint64_t metrics_off = slots_off + (uint64_t)LOCAL_COUNT * SLOT_BYTES;
    uint64_t blocks_off = metrics_off + (uint64_t)n * sizeof table[0];
    uint64_t total = blocks_off + (uint64_t)BLOCK_COUNT * BLOCK_BYTES;

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
    memcpy(p, &d, sizeof d);
    memcpy(p + metrics_off, table, (size_t)d.metrics_bytes);

    *(uint32_t *)(p + ND_WORLD_SIZE) = 7;
    *(uint32_t *)(p + ND_ABORTING) = 0;

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

    *bytes = total;
    return p;
}

static void check_good(const char *name)
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
        check_good(file);
        check_watch_gate(file, p);
        munmap(p, bytes);
        unlink(file);
    }

    /* The three spellings of one name, on a named /dev/shm object - which is
     * what a segment kept by OSOE_KEEP_CONTROL_SEGMENT is. */
    p = build(name, GOOD, &bytes);
    CHECK(p != NULL);
    if (p) {
        check_good(name);
        check_good(name + 1);
        char path[128];
        snprintf(path, sizeof path, "/dev/shm%s", name);
        check_good(path);
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
        check_good(file);
        munmap(p, bytes);
        unlink(file);
    }

    check_refused(file, BAD_MAGIC, false);
    check_refused(file, BAD_CHECKSUM, true);
    check_refused(file, STRANGE_DESC_SIZE, true);

    check_huge_counts(file, HUGE_LOCAL_COUNT);
    check_huge_counts(file, HUGE_BLOCK_COUNT);

    /* And a segment that is not there at all. */
    struct osoe_segment s;
    char err[256] = "";
    CHECK(osoe_segment_open(&s, "/osoe.definitely.not.here.0", err, sizeof err) != 0);
    CHECK(strstr(err, "no such job") != NULL);

    return CHECK_DONE();
}
