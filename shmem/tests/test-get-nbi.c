/*
 * Nonblocking gets (spec §9.6.2; design §4.6, as built): shmem_getmem_nbi and
 * the typed forms, completed by quiet and by a barrier, to node-mates and
 * across nodes, a word at a time and larger than a chunk - more of them than a
 * PE's landing zone holds at once - with blocking gets and puts beside them.
 *
 * Across nodes a nonblocking get asks and returns: its reply lands in the PE's
 * landing arena, the lead counts it, and the wait copies it out. The failures
 * this is written against are a reply copied to the wrong place, a quiet that
 * returns before every reply has landed, and a zone that fills and hangs or
 * overwrites. ctest runs the multi-node maps with the records and the arena
 * small (CMakeLists.txt), so that an issue finds them used up and completes
 * what is in flight first - which am.landing_full counts, and this checks
 * happened, on a PE whose gets crossed a node often enough to fill them.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

enum { TABLE = 97, GATHER = 300, BIG = 5000 };

static int me, npes;

/* What PE `pe` keeps at index `i` of its table, and at byte `i` of its big
 * buffer: distinct per PE and place, so a reply from the wrong PE, or landed
 * at the wrong offset, is one check. */
static long value(int pe, int i)
{
    return (long)pe * 100000 + i * 7 + 3;
}

static unsigned char pattern(int pe, size_t i)
{
    return (unsigned char)((pe * 31 + (int)(i % 251)) & 0xff);
}

/* This PE's total for a thread-scope counter, read as osoe-stat reads it: the
 * counter blocks are per local PE, so indexed by local id (test-internode.c). */
static uint64_t counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    const char *local = osoe_test_launcher("LOCALID");
    uint32_t local_id = (uint32_t)atoi(local ? local : "0");
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, local_id * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

/* Is `pe` one this PE reaches only over the network? shmem_ptr says NULL for
 * exactly those (§4.8). */
static bool crosses(const void *symmetric, int pe)
{
    return shmem_ptr(symmetric, pe) == NULL;
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    long *table = shmem_malloc(TABLE * sizeof *table);
    unsigned char *big = shmem_malloc(BIG);
    CHECK_FATAL(table != NULL && big != NULL);  /* NULL would be dereferenced just below */
    for (int i = 0; i < TABLE; i++)
        table[i] = value(me, i);
    for (size_t i = 0; i < BIG; i++)
        big[i] = pattern(me, i);
    shmem_barrier_all();

    /* ---- A gather: many one-word gets, every PE a target in turn ----------- */
    long got[GATHER];
    int target[GATHER], index[GATHER];
    int remote = 0;
    uint64_t before = counter("am.gets_nonblocking");
    for (int k = 0; k < GATHER; k++) {
        target[k] = (me + 1 + k) % npes;
        index[k] = (k * 7 + me) % TABLE;
        got[k] = -1;
        shmem_long_get_nbi(&got[k], &table[index[k]], 1, target[k]);
        remote += crosses(table, target[k]);
    }
    shmem_quiet();
    int wrong = -1;
    for (int k = 0; k < GATHER && wrong < 0; k++)
        if (got[k] != value(target[k], index[k]))
            wrong = k;
    if (wrong >= 0)
        fprintf(stderr, "PE %d: get %d from PE %d index %d read %ld, not %ld\n", me, wrong,
                target[wrong], index[wrong], got[wrong], value(target[wrong], index[wrong]));
    CHECK(wrong < 0);
    /* Only a get that crosses a node is a request in flight - one each, being a
     * word; to a node-mate it is applied before it returns, and none is
     * counted. */
    uint64_t issued = counter("am.gets_nonblocking") - before;
    if (issued != (uint64_t)remote)
        fprintf(stderr, "PE %d: %d gets crossed a node and %llu requests went out\n", me,
                remote, (unsigned long long)issued);
    CHECK(issued == (uint64_t)remote);

    /* ---- One get larger than a chunk, in pieces ---------------------------- */
    int right = (me + 1) % npes;
    unsigned char *mine = malloc(BIG);
    CHECK_FATAL(mine != NULL);
    memset(mine, 0, BIG);
    shmem_getmem_nbi(mine, big, BIG, right);
    shmem_quiet();
    size_t bad = BIG;
    for (size_t i = 0; i < BIG && bad == BIG; i++)
        if (mine[i] != pattern(right, i))
            bad = i;
    if (bad < BIG)
        fprintf(stderr, "PE %d: byte %zu of a %d-byte get from PE %d is %u, not %u\n", me,
                bad, BIG, right, mine[bad], pattern(right, bad));
    CHECK(bad == BIG);

    /* ---- Beside a blocking get and a put, and completed by a barrier -------- *
     * A blocking get lands in the PE's blocking slot and the nonblocking ones in
     * its arena, so neither may disturb the other; a barrier completes
     * nonblocking gets as quiet does (§4.6: it begins with one). The put goes
     * to the table's last word, which the gather above read - hence the barrier
     * first - and nothing below reads. */
    shmem_barrier_all();
    int left = (me + npes - 1) % npes;
    long near = -1, blocking = -1, far[8];
    shmem_long_get_nbi(&near, &table[5], 1, right);
    blocking = shmem_long_g(&table[6], right);
    shmem_long_p(&table[TABLE - 1], value(me, 1000), right);
    for (int k = 0; k < 8; k++) {
        far[k] = -1;
        shmem_long_get_nbi(&far[k], &table[10 + k], 1, (me + k) % npes);
    }
    CHECK(blocking == value(right, 6));
    shmem_barrier_all();
    CHECK(near == value(right, 5));
    for (int k = 0; k < 8; k++)
        CHECK(far[k] == value((me + k) % npes, 10 + k));
    CHECK(table[TABLE - 1] == value(left, 1000));

    /* The table is read by everyone above; nobody rewrites it until they have. */
    shmem_barrier_all();
    table[0] = value(me, 0) + 1;
    shmem_barrier_all();
    long changed = -1;
    shmem_long_get_nbi(&changed, &table[0], 1, right);
    shmem_quiet();
    CHECK(changed == value(right, 0) + 1);

    /* ---- The zone's bound, working ----------------------------------------- *
     * With OSOE_GETS_IN_FLIGHT below the gather's crossing gets, an issue found
     * the records used up and completed what was in flight first. */
    const char *limit = getenv("OSOE_GETS_IN_FLIGHT");
    if (limit && remote > atoi(limit))
        CHECK(counter("am.landing_full") > 0);

    free(mine);
    shmem_barrier_all();
    shmem_free(big);
    shmem_free(table);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
