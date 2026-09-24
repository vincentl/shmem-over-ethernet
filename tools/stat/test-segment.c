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
 * decodes: the one encoding it knows, rather than finds by name.
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

int main(void)
{
    check_details();

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
