/*
 * Contexts (spec §9.5; design §4.6, §4.11; phase 1 step 8 slice 1): creation
 * to exhaustion and back, the options, the implicit quiet of destroy, two
 * contexts' quiets independent, a team context's PE numbering, pe_quiet on a
 * team context, put-with-signal and AMOs on contexts, the landing zone shared
 * by contexts' gets, and what a team's destroy and finalize do with contexts
 * left live.
 *
 * Pinned by counters where a value alone would not tell: a quiet on one
 * context copies out only its own gets (am.gets_completed), and an issue
 * that finds the records used up by another context completes what is in
 * flight rather than hanging (am.landing_full). ctest runs the multi-node
 * maps with the records small for the second (CMakeLists.txt).
 *
 * OSOE_TEST_CONTEXT selects a misuse, each ending the job by name, judged by
 * ctest's expression on the output: destroyed, foreign, invalid, outside (a
 * PE outside the context's team), default (destroying SHMEM_CTX_DEFAULT),
 * private-team (a team destroyed under a live private context), and
 * after-team (a shareable context used after its team's destroy).
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

static int me, npes;

/* This PE's total for a thread-scope counter (test-get-nbi.c). */
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

static bool crosses(const void *symmetric, int pe)
{
    return shmem_ptr(symmetric, pe) == NULL;
}

static long value(int pe, int tag)
{
    return (long)pe * 1000 + tag;
}

/* The evens of the world, in world order: a strided team every map has. */
static shmem_team_t evens(void)
{
    shmem_team_t t = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, (npes + 1) / 2, NULL, 0, &t) == 0);
    return t;
}

static void misuse(const char *which)
{
    long *word = shmem_malloc(sizeof *word);
    long local = 0;
    CHECK_FATAL(word != NULL);
    shmem_ctx_t ctx = SHMEM_CTX_INVALID;
    if (strcmp(which, "destroyed") == 0) {
        CHECK_FATAL(shmem_ctx_create(0, &ctx) == 0);
        shmem_ctx_destroy(ctx);
        shmem_ctx_long_put(ctx, word, &local, 1, me);
    } else if (strcmp(which, "foreign") == 0) {
        shmem_ctx_long_put((shmem_ctx_t)&local, word, &local, 1, me);
    } else if (strcmp(which, "invalid") == 0) {
        shmem_ctx_long_put(SHMEM_CTX_INVALID, word, &local, 1, me);
    } else if (strcmp(which, "outside") == 0) {
        shmem_team_t t = evens();
        if (me % 2 == 0) {
            CHECK_FATAL(shmem_team_create_ctx(t, 0, &ctx) == 0);
            shmem_ctx_long_put(ctx, word, &local, 1, shmem_team_n_pes(t));
        }
        shmem_barrier_all();            /* the others wait for the abort */
    } else if (strcmp(which, "default") == 0) {
        shmem_ctx_destroy(SHMEM_CTX_DEFAULT);
    } else if (strcmp(which, "private-team") == 0) {
        shmem_team_t t = evens();
        if (me % 2 == 0)
            CHECK_FATAL(shmem_team_create_ctx(t, SHMEM_CTX_PRIVATE, &ctx) == 0);
        shmem_team_destroy(t);
        shmem_barrier_all();
    } else if (strcmp(which, "after-team") == 0) {
        shmem_team_t t = evens();
        if (me % 2 == 0)
            CHECK_FATAL(shmem_team_create_ctx(t, 0, &ctx) == 0);
        shmem_team_destroy(t);
        if (me % 2 == 0)
            shmem_ctx_long_put(ctx, word, &local, 1, 0);
        shmem_barrier_all();
    } else {
        fprintf(stderr, "OSOE_TEST_CONTEXT=%s is not a misuse this test knows\n", which);
    }
    fprintf(stderr, "PE %d: the misuse %s did not end the job\n", me, which);
    shmem_finalize();
    exit(1);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    const char *which = getenv("OSOE_TEST_CONTEXT");
    if (which)
        misuse(which);

    long *table = shmem_malloc(16 * sizeof *table);
    CHECK_FATAL(table != NULL);
    for (int i = 0; i < 16; i++)
        table[i] = value(me, i);
    /* Three words of the table for the team section: a put's target, a
     * signal and a sum, the last two starting at zero. */
    long *word = &table[11], *sum = &table[13];
    uint64_t *signal_word = (uint64_t *)&table[12];
    *signal_word = 0;
    *sum = 0;
    int right = (me + 1) % npes, left = (me + npes - 1) % npes;
    shmem_barrier_all();

    /* ---- §9.5.4 on the predefined handles ---------------------------------- */
    shmem_team_t of = SHMEM_TEAM_WORLD;
    CHECK(shmem_ctx_get_team(SHMEM_CTX_INVALID, &of) != 0 && of == SHMEM_TEAM_INVALID);
    CHECK(shmem_ctx_get_team(SHMEM_CTX_DEFAULT, &of) == 0 && of == SHMEM_TEAM_WORLD);

    /* ---- Creation to exhaustion, and a destroyed one's place reused --------- */
    const char *limit = getenv("OSOE_MAX_CONTEXTS");
    int max = limit ? atoi(limit) : 64;
    shmem_ctx_t *many = malloc((size_t)max * sizeof *many);
    CHECK_FATAL(many != NULL);
    int made = 0;
    for (int i = 0; i < max; i++) {
        many[i] = SHMEM_CTX_DEFAULT;
        if (shmem_ctx_create(0, &many[i]) != 0) {
            CHECK(many[i] == SHMEM_CTX_INVALID);
            break;
        }
        made++;
    }
    /* The default context counts: max - 1 creations, the next refused. */
    CHECK(made == max - 1);
    shmem_ctx_t one = SHMEM_CTX_DEFAULT;
    CHECK(shmem_ctx_create(0, &one) != 0 && one == SHMEM_CTX_INVALID);
    shmem_ctx_destroy(many[made / 2]);
    CHECK(shmem_ctx_create(0, &one) == 0 && one != SHMEM_CTX_INVALID);
    CHECK(shmem_ctx_get_team(one, &of) == 0 && of == SHMEM_TEAM_WORLD);
    /* Every created context works: a put on each to the right, quieted. */
    for (int i = 0; i < made; i++) {
        shmem_ctx_t c = i == made / 2 ? one : many[i];
        shmem_ctx_long_put(c, &table[1], &table[0], 1, right);
        shmem_ctx_quiet(c);
    }
    shmem_barrier_all();
    CHECK(table[1] == value(left, 0));
    for (int i = 0; i < made; i++)
        shmem_ctx_destroy(i == made / 2 ? one : many[i]);
    free(many);
    shmem_barrier_all();

    /* ---- The options: each accepted, each context carrying traffic --------- */
    long options[] = { SHMEM_CTX_SERIALIZED, SHMEM_CTX_PRIVATE, SHMEM_CTX_NOSTORE,
                       SHMEM_CTX_SERIALIZED | SHMEM_CTX_NOSTORE };
    for (int k = 0; k < 4; k++) {
        shmem_ctx_t c = SHMEM_CTX_INVALID;
        CHECK_FATAL(shmem_ctx_create(options[k], &c) == 0);
        long v = value(me, 10 + k);
        shmem_ctx_long_put(c, &table[2], &v, 1, right);
        shmem_ctx_fence(c);
        long got = shmem_ctx_long_g(c, &table[0], right);
        CHECK(got == value(right, 0));
        shmem_ctx_quiet(c);
        shmem_barrier_all();
        CHECK(table[2] == value(left, 10 + k));
        shmem_ctx_destroy(c);
        shmem_barrier_all();
    }

    /* ---- Destroy's implicit quiet ------------------------------------------ *
     * A nonblocking get and a put outstanding on the context: the get's dst
     * holds its bytes the moment destroy returns, and the put is visible at
     * its target after the sync that follows, which has no quiet of its own. */
    {
        shmem_ctx_t c = SHMEM_CTX_INVALID;
        CHECK_FATAL(shmem_ctx_create(0, &c) == 0);
        long got = -1, v = value(me, 20);
        shmem_ctx_long_get_nbi(c, &got, &table[3], 1, right);
        shmem_ctx_long_put_nbi(c, &table[4], &v, 1, right);
        shmem_ctx_destroy(c);
        CHECK(got == value(right, 3));
        shmem_sync_all();
        CHECK(table[4] == value(left, 20));
        shmem_barrier_all();
    }

    /* ---- Two contexts' quiets independent ----------------------------------- *
     * Gets in flight on two contexts: a quiet on the first copies out its own
     * and no other's - one piece completed, then two - and each dst holds its
     * value after its own context's quiet. Only where the gets cross a node
     * is anything in flight to count. */
    if (crosses(table, right)) {
        shmem_ctx_t a = SHMEM_CTX_INVALID, b = SHMEM_CTX_INVALID;
        CHECK_FATAL(shmem_ctx_create(0, &a) == 0 && shmem_ctx_create(0, &b) == 0);
        uint64_t before = counter("am.gets_completed");
        long from_a = -1, from_b = -1;
        shmem_ctx_long_get_nbi(a, &from_a, &table[5], 1, right);
        shmem_ctx_long_get_nbi(b, &from_b, &table[6], 1, right);
        shmem_ctx_quiet(a);
        CHECK(from_a == value(right, 5));
        CHECK(counter("am.gets_completed") == before + 1);
        shmem_ctx_quiet(b);
        CHECK(from_b == value(right, 6));
        CHECK(counter("am.gets_completed") == before + 2);
        shmem_ctx_destroy(a);
        shmem_ctx_destroy(b);
    }

    /* ---- The landing zone shared by two contexts' gets ----------------------- *
     * Every record taken by gets on one context, then a get on another: the
     * issue finds no record and completes what is in flight - the other's -
     * rather than hanging (am.landing_full); every value is right after. */
    if (crosses(table, right)) {
        shmem_ctx_t a = SHMEM_CTX_INVALID, b = SHMEM_CTX_INVALID;
        CHECK_FATAL(shmem_ctx_create(0, &a) == 0 && shmem_ctx_create(0, &b) == 0);
        const char *records = getenv("OSOE_GETS_IN_FLIGHT");
        int n = records ? atoi(records) : 64;
        long *got = calloc((size_t)n + 1, sizeof *got);
        CHECK_FATAL(got != NULL);
        uint64_t full = counter("am.landing_full");
        for (int i = 0; i < n; i++)
            shmem_ctx_long_get_nbi(b, &got[i], &table[i % 16], 1, right);
        shmem_ctx_long_get_nbi(a, &got[n], &table[7], 1, right);
        CHECK(counter("am.landing_full") > full);
        shmem_ctx_quiet(a);
        CHECK(got[n] == value(right, 7));
        shmem_ctx_quiet(b);
        for (int i = 0; i < n; i++) {
            if (got[i] != value(right, i % 16))
                fprintf(stderr, "PE %d: get %d on the second context read %ld, not %ld\n", me, i,
                        got[i], value(right, i % 16));
            CHECK(got[i] == value(right, i % 16));
        }
        free(got);
        shmem_ctx_destroy(a);
        shmem_ctx_destroy(b);
    }
    shmem_barrier_all();

    /* ---- A team context: PE numbering checked by value ---------------------- *
     * On the evens, each member puts its team rank to the next member, sums
     * into the team's rank 0 with an AMO, signals the next member, and quiets
     * the next member alone: every PE received from the member before it in
     * team order, so a translation that landed on a world rank shows. */
    {
        shmem_team_t t = evens();
        shmem_barrier_all();
        if (me % 2 == 0) {
            int my = shmem_team_my_pe(t), n = shmem_team_n_pes(t);
            int next = (my + 1) % n, previous = (my + n - 1) % n;
            shmem_ctx_t c = SHMEM_CTX_INVALID;
            CHECK_FATAL(shmem_team_create_ctx(t, 0, &c) == 0);
            CHECK(shmem_ctx_get_team(c, &of) == 0 && of == t);
            long v = value(my, 30);
            shmem_ctx_long_put(c, word, &v, 1, next);
            shmem_ctx_long_atomic_add(c, sum, my + 1, 0);
            long s = value(my, 31);
            shmem_ctx_long_put_signal(c, &table[8], &s, 1, signal_word,
                                      (uint64_t)my + 1, SHMEM_SIGNAL_SET, next);
            shmem_ctx_pe_quiet(c, &next, 1);
            CHECK(shmem_ctx_long_g(c, &table[0], next) == value(2 * next, 0));
            long got = -1;
            shmem_ctx_long_get_nbi(c, &got, &table[9], 1, next);
            shmem_ctx_quiet(c);
            CHECK(got == value(2 * next, 9));
            shmem_ctx_destroy(c);
            CHECK(shmem_team_sync(t) == 0);
            CHECK(*word == value(previous, 30));
            CHECK(table[8] == value(previous, 31));
            CHECK(*signal_word == (uint64_t)previous + 1);
            if (my == 0)
                CHECK(*sum == (long)n * (n + 1) / 2);
            /* A shareable context left live goes with the team (§9.4.8). */
            CHECK_FATAL(shmem_team_create_ctx(t, 0, &c) == 0);
        }
        shmem_team_destroy(t);
        shmem_barrier_all();
    }

    /* ---- Left for finalize ---------------------------------------------------- */
    shmem_ctx_t leftover = SHMEM_CTX_INVALID;
    CHECK(shmem_ctx_create(SHMEM_CTX_PRIVATE, &leftover) == 0);
    long v = value(me, 40);
    shmem_ctx_long_put_nbi(leftover, &table[10], &v, 1, right);

    shmem_barrier_all();
    shmem_free(table);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
