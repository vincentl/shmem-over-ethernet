/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Lanes and grants (design §5.8; the team redesign's step E, its first part;
 * docs/design/team-resources-proposal.md §4 and §6; internode-generic.c). A
 * large data collective's pieces go through slots its leads' threads lend it -
 * each thread's lane - and a child of a large reduction sends its first piece
 * up only once its parent's grant names the parent's lane; a small one is a
 * single piece through its team number's own small slots. Each case below is
 * one of tools/modelcheck/lane_model.py's, on the code, and the test of a
 * breakage the model shows failing, run against it as a mutation:
 *
 *   shared-lead   two teams whose leads coincide on node 1, which roots both:
 *                 T over nodes 1 and 3, U over nodes 1 and 2. Node 3 comes
 *                 late to each of T's collectives, so node 1 sits in T's while
 *                 node 2, outside it, enters U's - the model's failing case for
 *                 no_grant. Large reductions of one piece and of several, and
 *                 small ones and broadcasts between them (node_small).
 *   two-threads   the same teams under SHMEM_THREAD_MULTIPLE, node 1's PE
 *                 leading T's and U's large reductions at once on two threads,
 *                 each with its own lane (one_lane), which node 1's lead
 *                 publishes for each team.
 *   turns         one team over two nodes whose PEs' threads take turns on it,
 *                 each node's large reductions led by another thread than the
 *                 one before, and each node late now and then (lane_early,
 *                 stale_grant, release_lane_early).
 *   rotation      the world over four nodes at a fan-out of 2: a reduction of
 *                 one large piece, then a broadcast rooted at node 1, over and
 *                 over, under OSOE_TEST_PERTURB. Node 1's child at position 0
 *                 is node 3 in the reduction's tree and node 2 in the
 *                 broadcast's, and with one piece node 1 sends it up before it
 *                 takes its release (release_lane_late).
 *   boundary      the class boundary, a byte either side of
 *                 OSOE_SMALL_COLLECTIVE_BYTES, by a byte broadcast and a byte
 *                 reduction; the class each lead publishes, and for a large
 *                 collective its lane and its pieces in flight, as
 *                 shmem.pipeline.* pins them: min(D, pieces) at a leaf, 0 at
 *                 the root.
 *   reuse         a team destroyed after three large reductions and its number
 *                 taken again by the next, whose first large reduction's parent
 *                 comes late: the grant counter zeroed as the number is taken
 *                 is the one guard (the review of step D's merge point), since
 *                 a grant's epoch cannot tell one team under a number from the
 *                 next.
 *   mismatch      two leads of one team in different large reductions: the
 *                 child's grant names the difference, and the job ends by name.
 *   scan-shared-parent, scan-turns, scan-reuse, scan-mismatch
 *                 the same for the scans (E6), whose every member grants its
 *                 tree children as it enters and lends its lane: two teams whose
 *                 scan trees share their root, PE 1, with large and small scans
 *                 and a child outside the scan its parent is in
 *                 (scan_pipeline_model.py's PQ); a team's large scans led by
 *                 its PEs' threads in turn; a number taken again; and an
 *                 inclusive scan against an exclusive one, which the grant
 *                 names.
 *   class-split   two leads of one team given counts on either side of
 *                 OSOE_SMALL_COLLECTIVE_BYTES, outside the specification, as
 *                 the README's Limits say: a large parent over a small child
 *                 ends the job naming the child's lane word (named); a small
 *                 parent over a large child leaves the child waiting for a
 *                 grant, until the deadlock detector ends the job (hang); and a
 *                 broadcast its parent takes as small and its child as large,
 *                 right after another team's large broadcast left a header in
 *                 the child's lane that matches this one's in everything but
 *                 its team (broadcast) - the adversarial read's case, which
 *                 returned the other team's data until a reader cleared what
 *                 it read.
 *
 * Every case runs on one PE a logical node, PE i on node i, so that a team's
 * node set is its PEs.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

/*
 * A lane's slot holds 16 KiB less a piece's 16-byte header: 2,046 longs. So
 * ONE_PIECE is one large piece, SEVERAL four - more than the default window of
 * two - and SMALL fits the default small slot of 1 KiB.
 */
enum {
    PIECE_LONGS = ((16 << 10) - 16) / sizeof(long),
    ONE_PIECE = 512,                    /* 4 KiB: large, one piece */
    SEVERAL = 3 * PIECE_LONGS + 5,      /* four pieces */
    SMALL = 16,                         /* 128 bytes: small at every setting */
    MOST = SEVERAL,
    PREDEFINED_TEAMS = 3,               /* numbers 0 to 2: world, shared, node */
};

static int me, npes;
static long *source, *dest;             /* MOST longs each, symmetric */

static long value(int pe, size_t i, int round)
{
    return (long)pe * 1000003L + (long)i * 7 + (long)round * 31 + 1;
}

static void sleep_us(long us)
{
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&t, NULL);
}

static shmem_team_t split(int start, int stride, int size)
{
    shmem_team_t team = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, start, stride, size, NULL, 0, &team) ==
                0);
    return team;
}

/* A sum of `n` longs on `team`, whose world ranks are `members`, from `src`
 * into `dst`, every element checked. */
static void reduce_on(shmem_team_t team, const int *members, int count, long *dst, long *src,
                      size_t n, int round, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        src[i] = value(me, i, round);
        dst[i] = -1;
    }
    CHECK(shmem_long_sum_reduce(team, dst, src, n) == 0);
    for (size_t i = 0; i < n; i++) {
        long want = 0;
        for (int m = 0; m < count; m++)
            want += value(members[m], i, round);
        if (dst[i] != want) {
            fprintf(stderr, "PE %d: %s, round %d: element %zu of %zu is %ld, not %ld\n", me,
                    what, round, i, n, dst[i], want);
            CHECK(dst[i] == want);
            return;
        }
    }
}

/* A broadcast of `n` longs on `team` from its rank `root`, world rank
 * `root_pe`, every element checked. */
static void broadcast_on(shmem_team_t team, int root, int root_pe, long *dst, long *src,
                         size_t n, int round, const char *what)
{
    for (size_t i = 0; i < n; i++) {
        src[i] = value(me, i, round);
        dst[i] = -1;
    }
    CHECK(shmem_long_broadcast(team, dst, src, n, root) == 0);
    for (size_t i = 0; i < n; i++)
        if (dst[i] != value(root_pe, i, round)) {
            fprintf(stderr, "PE %d: %s, round %d: element %zu of %zu is %ld, not %ld\n", me,
                    what, round, i, n, dst[i], value(root_pe, i, round));
            CHECK(dst[i] == value(root_pe, i, round));
            return;
        }
}

/* An inclusive or exclusive sum scan of `n` longs on `team`, whose world ranks
 * by team rank are `members`, every element checked. */
static void scan_on(shmem_team_t team, const int *members, long *dst, long *src, size_t n,
                    int round, bool inclusive, const char *what)
{
    int rank = shmem_team_my_pe(team);
    for (size_t i = 0; i < n; i++) {
        src[i] = value(me, i, round);
        dst[i] = -1;
    }
    CHECK((inclusive ? shmem_long_sum_inscan(team, dst, src, n)
                     : shmem_long_sum_exscan(team, dst, src, n)) == 0);
    for (size_t i = 0; i < n; i++) {
        long want = 0;
        for (int r = 0; r < rank + (inclusive ? 1 : 0); r++)
            want += value(members[r], i, round);
        if (dst[i] != want) {
            fprintf(stderr, "PE %d: %s, round %d: element %zu of %zu is %ld, not %ld\n", me,
                    what, round, i, n, dst[i], want);
            CHECK(dst[i] == want);
            return;
        }
    }
}

/* ---- shared-lead ------------------------------------------------------- */

enum { SHARED_ROUNDS = 24, LATE_US = 20000 };

static const int t_members[] = { 1, 3 }, u_members[] = { 1, 2 };

static void shared_lead(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t t = split(1, 2, 2);    /* PEs 1 and 3: node 1 its root */
    shmem_team_t u = split(1, 1, 2);    /* PEs 1 and 2: node 1 its root */
    static const size_t sizes[] = { ONE_PIECE, SEVERAL, SMALL };
    for (int round = 0; round < SHARED_ROUNDS; round++) {
        size_t n = sizes[round % 3];
        bool broadcast = round % 4 == 3;
        /* Node 3 late: node 1 waits in T's collective, its lane lent to it,
         * while node 2 enters U's, of which node 1 is the parent too. */
        if (me == 3)
            sleep_us(LATE_US);
        if (t != SHMEM_TEAM_INVALID) {
            if (broadcast)
                broadcast_on(t, 1, 3, dest, source, n, round, "T's broadcast");
            else
                reduce_on(t, t_members, 2, dest, source, n, round, "T's sum");
        }
        if (u != SHMEM_TEAM_INVALID) {
            if (broadcast)
                broadcast_on(u, 1, 2, dest, source, n, round, "U's broadcast");
            else
                reduce_on(u, u_members, 2, dest, source, n, round, "U's sum");
        }
    }
    shmem_barrier_all();
    if (t != SHMEM_TEAM_INVALID)
        shmem_team_destroy(t);
    if (u != SHMEM_TEAM_INVALID)
        shmem_team_destroy(u);
}

/* The scans' shared-lead: T's and U's scan trees both rooted at PE 1, their
 * rank 0, with node 3 late to T's, so that PE 2, in U alone, enters U's scan
 * while PE 1 is in T's, PE 2 at position 0 of U's tree as PE 3 is of T's. U
 * has a third member, PE 3 at its position 1, because only a child before the
 * last has a total any result depends on: a scan's down to child j carries the
 * totals of children 0 .. j - 1, and a corrupt total from the last child shows
 * nowhere. */
static const int u3_members[] = { 1, 2, 3 };

static void scan_shared_parent(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t t = split(1, 2, 2);    /* PEs 1 and 3 */
    shmem_team_t u = split(1, 1, 3);    /* PEs 1, 2 and 3 */
    static const size_t sizes[] = { ONE_PIECE, SEVERAL, SMALL };
    for (int round = 0; round < SHARED_ROUNDS; round++) {
        size_t n = sizes[round % 3];
        bool inclusive = round % 2 == 0;
        if (me == 3)
            sleep_us(LATE_US);
        if (t != SHMEM_TEAM_INVALID)
            scan_on(t, t_members, dest, source, n, round, inclusive, "T's scan");
        if (u != SHMEM_TEAM_INVALID)
            scan_on(u, u3_members, dest, source, n, round, inclusive, "U's scan");
    }
    shmem_barrier_all();
    if (t != SHMEM_TEAM_INVALID)
        shmem_team_destroy(t);
    if (u != SHMEM_TEAM_INVALID)
        shmem_team_destroy(u);
}

/* ---- two-threads ------------------------------------------------------- */

enum { THREAD_ROUNDS = 40 };

struct leading {
    shmem_team_t team;
    const int *members;
    long *dst, *src;
};

static void *lead_rounds(void *argument)
{
    const struct leading *l = argument;
    for (int round = 0; round < THREAD_ROUNDS; round++)
        reduce_on(l->team, l->members, 2, l->dst, l->src, round % 2 ? SEVERAL : ONE_PIECE,
                  round, "a sum on its own thread");
    return NULL;
}

/* The lane node 1's lead published for team number `number`'s last large
 * collective. */
static uint64_t published_lane(uint32_t number)
{
    uint64_t lane = UINT64_MAX, class = UINT64_MAX;
    uint32_t block = osoe_test_team_block(number);
    CHECK(block != UINT32_MAX);
    CHECK(osoe_test_scoped_metric("team.tree.collective_class", OSOE_MSCOPE_TEAM, block, &class));
    CHECK(osoe_test_scoped_metric("team.tree.lane", OSOE_MSCOPE_TEAM, block, &lane));
    CHECK(class == 1);
    return lane;
}

static void two_threads(void)
{
    CHECK_FATAL(npes == 4);
    shmem_team_t t = split(1, 2, 2);
    shmem_team_t u = split(1, 1, 2);
    /* Each thread its own buffers: the two reductions run at once. */
    long *t_dst = shmem_malloc(MOST * sizeof(long)), *t_src = shmem_malloc(MOST * sizeof(long));
    long *u_dst = shmem_malloc(MOST * sizeof(long)), *u_src = shmem_malloc(MOST * sizeof(long));
    CHECK_FATAL(t_dst && t_src && u_dst && u_src);
    struct leading on_t = { t, t_members, t_dst, t_src }, on_u = { u, u_members, u_dst, u_src };
    if (me == 1) {
        pthread_t a, b;
        CHECK_FATAL(pthread_create(&a, NULL, lead_rounds, &on_t) == 0);
        CHECK_FATAL(pthread_create(&b, NULL, lead_rounds, &on_u) == 0);
        CHECK(pthread_join(a, NULL) == 0);
        CHECK(pthread_join(b, NULL) == 0);
        /* T is number 3 here, and U number 4: the lowest free on both
         * members of each. Two threads, two lanes. */
        uint64_t lane_t = published_lane(PREDEFINED_TEAMS);
        uint64_t lane_u = published_lane(PREDEFINED_TEAMS + 1);
        if (lane_t == lane_u)
            fprintf(stderr, "PE 1: T's and U's large reductions, led at once on two threads, "
                    "both went through lane %llu\n", (unsigned long long)lane_t);
        CHECK(lane_t != lane_u);
    } else if (me == 3) {
        lead_rounds(&on_t);
    } else if (me == 2) {
        lead_rounds(&on_u);
    }
    shmem_barrier_all();
    shmem_free(t_dst);
    shmem_free(t_src);
    shmem_free(u_dst);
    shmem_free(u_src);
    if (t != SHMEM_TEAM_INVALID)
        shmem_team_destroy(t);
    if (u != SHMEM_TEAM_INVALID)
        shmem_team_destroy(u);
}

/* ---- turns ------------------------------------------------------------- */

enum { TURN_ROUNDS = 32, TURN_LATE_US = 5000 };

/* A second thread that runs one reduction when told, as the program's threads
 * take turns on a team: a team's collectives are not concurrent (spec §9.10),
 * but any thread may call the next. */
static pthread_mutex_t turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static int turn_round = -1;             /* the round the helper is to run, or -1 */
static bool turn_done, turn_stop;
static const int both[] = { 0, 1 };

static bool turn_scans;                 /* scan-turns: the turns scan, not reduce */

static void turn_reduce(int round)
{
    size_t n = round % 3 ? ONE_PIECE : SEVERAL;
    if (turn_scans)
        scan_on(SHMEM_TEAM_WORLD, both, dest, source, n, round, round % 2 == 0,
                "a scan made by its turn's thread");
    else
        reduce_on(SHMEM_TEAM_WORLD, both, 2, dest, source, n, round,
                  "a sum led by its turn's thread");
}

static void *turn_helper(void *argument)
{
    (void)argument;
    pthread_mutex_lock(&turn_lock);
    for (;;) {
        while (turn_round < 0 && !turn_stop)
            pthread_cond_wait(&turn_changed, &turn_lock);
        if (turn_stop)
            break;
        int round = turn_round;
        pthread_mutex_unlock(&turn_lock);
        turn_reduce(round);
        pthread_mutex_lock(&turn_lock);
        turn_round = -1;
        turn_done = true;
        pthread_cond_broadcast(&turn_changed);
    }
    pthread_mutex_unlock(&turn_lock);
    return NULL;
}

static void turns(void)
{
    CHECK_FATAL(npes == 2);
    pthread_t helper;
    CHECK_FATAL(pthread_create(&helper, NULL, turn_helper, NULL) == 0);
    for (int round = 0; round < TURN_ROUNDS; round++) {
        /* Node 0, the parent, changes thread every round, node 1 every
         * other; each comes late now and then, so that the other waits in the
         * collective - for a grant, or for a first arrival. */
        bool helper_leads = me == 0 ? round % 2 : (round / 2) % 2;
        if ((me == 0 && round % 3 == 1) || (me == 1 && round % 3 == 2))
            sleep_us(TURN_LATE_US);
        if (!helper_leads) {
            turn_reduce(round);
            continue;
        }
        pthread_mutex_lock(&turn_lock);
        turn_done = false;
        turn_round = round;
        pthread_cond_broadcast(&turn_changed);
        while (!turn_done)
            pthread_cond_wait(&turn_changed, &turn_lock);
        pthread_mutex_unlock(&turn_lock);
    }
    pthread_mutex_lock(&turn_lock);
    turn_stop = true;
    pthread_cond_broadcast(&turn_changed);
    pthread_mutex_unlock(&turn_lock);
    CHECK(pthread_join(helper, NULL) == 0);
    shmem_barrier_all();
}

/* ---- scan-reuse, scan-mismatch ----------------------------------------- */

static void scan_reuse(void)
{
    CHECK_FATAL(npes == 2);
    shmem_team_t x = split(0, 1, 2);
    for (int round = 0; round < 3; round++)
        scan_on(x, both, dest, source, ONE_PIECE, round, true, "the first team's scan");
    shmem_team_destroy(x);
    shmem_team_t y = split(0, 1, 2);
    CHECK(osoe_test_team_block(PREDEFINED_TEAMS) != UINT32_MAX);
    /* PE 0, rank 0, the scan tree's root, late: PE 1 must wait for its grant. */
    if (me == 0)
        sleep_us(200000);
    scan_on(y, both, dest, source, ONE_PIECE, 100, true, "the second team's scan");
    scan_on(y, both, dest, source, SEVERAL, 101, false, "the second team's scan");
    shmem_barrier_all();
    shmem_team_destroy(y);
}

static void scan_mismatch(void)
{
    CHECK_FATAL(npes == 2);
    for (size_t i = 0; i < ONE_PIECE; i++)
        source[i] = value(me, i, 0);
    if (me == 0)
        shmem_long_sum_inscan(SHMEM_TEAM_WORLD, dest, source, ONE_PIECE);
    else
        shmem_long_sum_exscan(SHMEM_TEAM_WORLD, dest, source, ONE_PIECE);
    fprintf(stderr, "PE %d: a mismatched scan returned\n", me);
    CHECK(false);
}

/* ---- rotation ---------------------------------------------------------- */

enum { ROTATION_ROUNDS = 400 };

static void rotation(void)
{
    CHECK_FATAL(npes == 4);
    static const int world[] = { 0, 1, 2, 3 };
    for (int round = 0; round < ROTATION_ROUNDS; round++) {
        reduce_on(SHMEM_TEAM_WORLD, world, 4, dest, source, ONE_PIECE, round, "the world's sum");
        broadcast_on(SHMEM_TEAM_WORLD, 1, 1, dest, source, ONE_PIECE, round,
                     "the broadcast from node 1");
    }
    shmem_barrier_all();
}

/* ---- boundary ---------------------------------------------------------- */

static uint32_t setting(const char *name, uint32_t otherwise)
{
    const char *v = getenv(name);
    return v ? (uint32_t)strtoul(v, NULL, 10) : otherwise;
}

/* What this node's lead published of the world's last data collective, and
 * that it is what a collective of `bytes` in pieces of `per_piece` is. Then a
 * sync, so that no lead starts another while a PE of its node still reads. */
static void check_published(size_t bytes, size_t per_piece, uint32_t small, uint32_t window,
                            const char *what)
{
    uint64_t class = UINT64_MAX, most = UINT64_MAX, lane = UINT64_MAX;
    uint32_t block = osoe_test_team_block(0);
    CHECK(block != UINT32_MAX);
    CHECK(osoe_test_scoped_metric("team.tree.collective_class", OSOE_MSCOPE_TEAM, block, &class));
    CHECK(osoe_test_scoped_metric("team.tree.most_pieces_in_flight", OSOE_MSCOPE_TEAM, block,
                                  &most));
    CHECK(osoe_test_scoped_metric("team.tree.lane", OSOE_MSCOPE_TEAM, block, &lane));
    uint64_t want_class = bytes > small;
    uint64_t pieces = want_class ? (bytes + per_piece - 1) / per_piece : 1;
    /* The world's reduction and a broadcast from PE 0 share a tree rooted at
     * node 0, whose other nodes are its leaves at the default fan-out. */
    uint64_t want_most = me == 0 ? 0 : pieces < window ? pieces : window;
    if (class != want_class || most != want_most)
        fprintf(stderr, "PE %d: %s of %zu bytes at OSOE_SMALL_COLLECTIVE_BYTES=%u went as class "
                "%llu with %llu pieces in flight, not %llu and %llu\n", me, what, bytes, small,
                (unsigned long long)class, (unsigned long long)most,
                (unsigned long long)want_class, (unsigned long long)want_most);
    CHECK(class == want_class);
    CHECK(most == want_most);
    if (want_class)
        CHECK(lane < 1024);
    CHECK(shmem_team_sync(SHMEM_TEAM_WORLD) == 0);
}

static void boundary(void)
{
    CHECK_FATAL(npes >= 2);
    uint32_t small = setting("OSOE_SMALL_COLLECTIVE_BYTES", 1024);
    uint32_t window = setting("OSOE_COLLECTIVE_PIPELINE", 2);
    unsigned char *bytes_dst = (unsigned char *)dest, *bytes_src = (unsigned char *)source;
    size_t sizes[] = { small, (size_t)small + 1, 3 * (((16 << 10) - 16)) + 1 };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        CHECK_FATAL(n <= MOST * sizeof(long));
        for (size_t i = 0; i < n; i++) {
            bytes_src[i] = (unsigned char)(me * 37 + i * 11 + s);
            bytes_dst[i] = 0;
        }
        CHECK(shmem_broadcastmem(SHMEM_TEAM_WORLD, bytes_dst, bytes_src, n, 0) == 0);
        for (size_t i = 0; i < n; i++)
            if (bytes_dst[i] != (unsigned char)(0 * 37 + i * 11 + s)) {
                fprintf(stderr, "PE %d: byte %zu of a %zu-byte broadcast is wrong\n", me, i, n);
                CHECK(false);
                break;
            }
        check_published(n, (16 << 10) - 16, small, window, "a byte broadcast");

        for (size_t i = 0; i < n; i++) {
            bytes_src[i] = (unsigned char)(me * 37 + i * 11 + s);
            bytes_dst[i] = 0;
        }
        CHECK(shmem_uchar_sum_reduce(SHMEM_TEAM_WORLD, bytes_dst, bytes_src, n) == 0);
        for (size_t i = 0; i < n; i++) {
            unsigned char want = 0;
            for (int p = 0; p < npes; p++)
                want = (unsigned char)(want + (unsigned char)(p * 37 + i * 11 + s));
            if (bytes_dst[i] != want) {
                fprintf(stderr, "PE %d: byte %zu of a %zu-byte sum is %u, not %u\n", me, i, n,
                        bytes_dst[i], want);
                CHECK(false);
                break;
            }
        }
        check_published(n, (16 << 10) - 16, small, window, "a byte sum");
    }
}

/* ---- reuse ------------------------------------------------------------- */

static void reuse(void)
{
    CHECK_FATAL(npes == 2);
    shmem_team_t x = split(0, 1, 2);
    for (int round = 0; round < 3; round++)
        reduce_on(x, both, 2, dest, source, ONE_PIECE, round, "the first team's sum");
    shmem_team_destroy(x);
    /* The same number again: the lowest free on both PEs. */
    shmem_team_t y = split(0, 1, 2);
    CHECK(osoe_test_team_block(PREDEFINED_TEAMS) != UINT32_MAX);
    /* Node 0, the parent, late: node 1 must wait for its grant, which nothing
     * of the first team's may stand in for. */
    if (me == 0)
        sleep_us(200000);
    reduce_on(y, both, 2, dest, source, ONE_PIECE, 100, "the second team's sum");
    reduce_on(y, both, 2, dest, source, SEVERAL, 101, "the second team's sum");
    shmem_barrier_all();
    shmem_team_destroy(y);
}

/* ---- mismatch ---------------------------------------------------------- */

static void mismatch(void)
{
    CHECK_FATAL(npes == 2);
    for (size_t i = 0; i < ONE_PIECE; i++)
        source[i] = value(me, i, 0);
    /* Node 1, the child, in another reduction than its parent's: its grant
     * says so before any piece moves. */
    if (me == 0)
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, ONE_PIECE);
    else
        shmem_long_max_reduce(SHMEM_TEAM_WORLD, dest, source, ONE_PIECE);
    fprintf(stderr, "PE %d: a mismatched reduction returned\n", me);
    CHECK(false);
}

/* ---- class-split ------------------------------------------------------- */

/* Node 0, the parent, given `parent` longs and node 1, its child, `child`: one
 * side of OSOE_SMALL_COLLECTIVE_BYTES each. Neither returns. */
static void class_split(size_t parent, size_t child)
{
    CHECK_FATAL(npes == 2);
    size_t n = me == 0 ? parent : child;
    for (size_t i = 0; i < n; i++)
        source[i] = value(me, i, 0);
    shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, n);
    fprintf(stderr, "PE %d: a reduction of %zu longs against %zu returned\n", me, n,
            me == 0 ? child : parent);
    CHECK(false);
}

/* class-split's broadcast: W's valid large broadcast, then Z's first, epoch 1
 * as W's was, 1,000 bytes on the parent and 1,500 on the child. */
static void class_split_broadcast(void)
{
    CHECK_FATAL(npes == 2);
    shmem_team_t w = split(0, 1, 2), z = split(0, 1, 2);
    unsigned char *d = (unsigned char *)dest, *s = (unsigned char *)source;
    memset(s, 0xaa, 1500);
    CHECK(shmem_broadcastmem(w, d, s, 1500, 0) == 0);
    size_t n = me == 0 ? 1000 : 1500;
    shmem_broadcastmem(z, d, s, n, 0);
    fprintf(stderr, "PE %d: a broadcast of %zu bytes against %zu returned, its first byte %#x\n",
            me, n, (size_t)(me == 0 ? 1500 : 1000), d[0]);
    CHECK(false);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "";
    bool threaded = strcmp(which, "two-threads") == 0 || strcmp(which, "turns") == 0 ||
                    strcmp(which, "scan-turns") == 0;
    int provided = -1;
    CHECK_FATAL(shmem_init_thread(threaded ? SHMEM_THREAD_MULTIPLE : SHMEM_THREAD_SINGLE,
                                  &provided) == 0);
    CHECK_FATAL(!threaded || provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    source = shmem_malloc(MOST * sizeof(long));
    dest = shmem_malloc(MOST * sizeof(long));
    CHECK_FATAL(source && dest);

    if (strcmp(which, "shared-lead") == 0)
        shared_lead();
    else if (strcmp(which, "two-threads") == 0)
        two_threads();
    else if (strcmp(which, "turns") == 0)
        turns();
    else if (strcmp(which, "scan-turns") == 0) {
        turn_scans = true;
        turns();
    } else if (strcmp(which, "scan-shared-parent") == 0)
        scan_shared_parent();
    else if (strcmp(which, "scan-reuse") == 0)
        scan_reuse();
    else if (strcmp(which, "scan-mismatch") == 0)
        scan_mismatch();
    else if (strcmp(which, "rotation") == 0)
        rotation();
    else if (strcmp(which, "boundary") == 0)
        boundary();
    else if (strcmp(which, "reuse") == 0)
        reuse();
    else if (strcmp(which, "mismatch") == 0)
        mismatch();
    else if (strcmp(which, "class-split-named") == 0)
        class_split(ONE_PIECE, SMALL);
    else if (strcmp(which, "class-split-hang") == 0)
        class_split(SMALL, ONE_PIECE);
    else if (strcmp(which, "class-split-broadcast") == 0)
        class_split_broadcast();
    else
        CHECK_FATAL(!"a case: shared-lead, two-threads, turns, rotation, boundary, reuse, "
                     "mismatch, scan-shared-parent, scan-turns, scan-reuse, scan-mismatch, "
                     "class-split-named, class-split-hang or class-split-broadcast");

    shmem_free(dest);
    shmem_free(source);
    shmem_barrier_all();
    if (me == 0 && !CHECK_DONE())
        fprintf(stderr, "PE 0: %s passed\n", which);
    shmem_finalize();
    return CHECK_DONE();
}
