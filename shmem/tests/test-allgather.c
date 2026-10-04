/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * fcollect's and collect's internode phase, the allgather among node leads
 * (design §5.8, "An allgather for fcollect"; phase 1 step 5 slice 6;
 * internode-generic.c), on the world and on teams a split creates - the
 * world's even ranks, the world reversed, and on five nodes a team spanning
 * three of them (OSOE_TEST_THREE_OF_FIVE's triplet) - each over its own node
 * set, whose positions are not its nodes' numbers once it leaves one out:
 *
 *   - fcollects of blocks of 1 byte, 8 bytes and 64 KiB, and a collect whose
 *     blocks differ, some empty, every byte checked against the member whose
 *     place it is;
 *   - fcollects into the program's statics, zero-initialized and not, and a
 *     collect into them, where the gather puts into a peer's statics rather
 *     than its heap;
 *   - fcollects back to back into one dest, no barrier between, the team's
 *     rank 0 - a lead - reading each result only after a pause while every
 *     other PE goes on to the next: a lead that wrote rank 0's dest before
 *     rank 0 had entered the next gather would change the result it is still
 *     reading, and READY is what keeps it out (the case the counter exists
 *     for);
 *   - the counts each lead publishes in the team's tree state
 *     (team.tree.gather_steps and team.tree.gather_shares), read as osoe-stat
 *     reads them: ceil(log2 N) steps, and N - 1 (node, step) pairs in the
 *     lists of what it sends, for a team over N nodes - the algorithm pinned
 *     by counts, not by time; a last step sending the full 2^s shares, which
 *     only rewrites bytes the receiver holds, shows only here.
 *
 * OSOE_TEST_MAP gives the node of every rank, so that the test knows how many
 * nodes each team spans. Numbers are claimed in the order the splits run, one
 * each, from the first after the predefined three.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

enum {
    MAX_PES = 64,
    LARGE_BLOCK = 64 << 10,
    STATIC_BLOCK = 24,
    ROUNDS = 6,
    PAUSE_MILLISECONDS = 20,
    PREDEFINED_TEAMS = 3,           /* the world, SHMEM_TEAM_SHARED and the finalize team */
};

static int me, npes;
static int node_of_rank[MAX_PES];

/* The program's statics (design §4.3): one gather's dest zero-initialized,
 * which the linker puts among the bss, and one initialized, among the data. */
static unsigned char static_source[STATIC_BLOCK];
static unsigned char static_dest_zeroed[MAX_PES * STATIC_BLOCK];
static unsigned char static_dest_initialized[MAX_PES * STATIC_BLOCK] = { 1 };
static unsigned char static_collect_dest[MAX_PES * 2 * 997 + MAX_PES];

/* A team as the test expects it. */
struct team_case {
    const char *name;
    shmem_team_t team;
    uint32_t number;                    /* its team number, which its node blocks have */
    int members;
    int world[MAX_PES];                 /* each member's world rank, by team rank */
    int rank;                           /* this PE's, or -1 */
    int nodes;                          /* how many nodes it spans */
};

/* Byte i of the block of world PE `world` in gather `round`: every member's
 * differs, and every round's, so a block at another's place or from another
 * round is seen. */
static unsigned char byte_value(int world, size_t i, int round)
{
    return (unsigned char)(world * 31 + (int)(i % 251) * 7 + (int)(i / 251) + round * 101 + 1);
}

static void pause_milliseconds(long milliseconds)
{
    struct timespec pause = { milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000 };
    nanosleep(&pause, NULL);
}

/* The map, one node per rank, from OSOE_TEST_MAP ("0,0,1,1"). */
static void read_map(void)
{
    const char *map = getenv("OSOE_TEST_MAP");
    CHECK_FATAL(map && npes <= MAX_PES);
    for (int r = 0; r < npes; r++) {
        char *end = NULL;
        node_of_rank[r] = (int)strtol(map, &end, 10);
        CHECK_FATAL(end != map);
        map = *end == ',' ? end + 1 : end;
    }
}

/* ceil(log2 n): the steps of a gather over n nodes. */
static int steps_over(int nodes)
{
    int steps = 0;
    while ((1 << steps) < nodes)
        steps++;
    return steps;
}

/* The team a triplet of world ranks makes (spec §9.4.6), made by a split that
 * claims number `number`, and what the test expects of it. */
static struct team_case split(const char *name, int start, int stride, int size,
                              uint32_t number)
{
    struct team_case t = { .name = name, .number = number, .members = size, .rank = -1 };
    bool on_node[MAX_PES] = { false };
    for (int r = 0; r < size; r++) {
        t.world[r] = start + stride * r;
        if (t.world[r] == me)
            t.rank = r;
        if (!on_node[node_of_rank[t.world[r]]])
            t.nodes++;
        on_node[node_of_rank[t.world[r]]] = true;
    }
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, start, stride, size, NULL, 0, &t.team) ==
          0);
    CHECK((t.team != SHMEM_TEAM_INVALID) == (t.rank >= 0));
    if (t.rank >= 0) {
        CHECK(shmem_team_my_pe(t.team) == t.rank);
        CHECK(shmem_team_n_pes(t.team) == size);
    }
    return t;
}

/* Report the first byte that differs, and count it once. */
static void first_wrong(const struct team_case *t, const char *what, const unsigned char *dest,
                        size_t total, size_t wrong, unsigned char want)
{
    if (wrong < total)
        fprintf(stderr, "PE %d: %s on %s, %zu bytes: byte %zu is %u, not %u\n", me, what,
                t->name, total, wrong, dest[wrong], want);
    CHECK(wrong == total);
}

/*
 * An fcollect of `block` bytes from every member into `dest`, checked byte by
 * byte - after a pause, when `pause` says so - and with `dest` first set to
 * what no member's block holds, when `fill` does.
 */
static void fcollect_into(const struct team_case *t, unsigned char *dest, unsigned char *source,
                          size_t block, int round, bool fill, bool pause)
{
    size_t total = (size_t)t->members * block;
    for (size_t i = 0; i < block; i++)
        source[i] = byte_value(me, i, round);
    if (fill)
        memset(dest, 0, total);
    CHECK(shmem_fcollectmem(t->team, dest, source, block) == 0);
    if (pause)
        pause_milliseconds(PAUSE_MILLISECONDS);
    size_t wrong = total;
    unsigned char want = 0;
    for (size_t at = 0; at < total && wrong == total; at++) {
        want = byte_value(t->world[at / block], at % block, round);
        if (dest[at] != want)
            wrong = at;
    }
    first_wrong(t, "fcollect", dest, total, wrong, want);
}

/* The same through the typed form, eight bytes a member. */
static void fcollect_longs(const struct team_case *t, long *dest, long *source, int round)
{
    source[0] = (long)me * 1000003L + round;
    for (int r = 0; r < t->members; r++)
        dest[r] = -1;
    CHECK(shmem_long_fcollect(t->team, dest, source, 1) == 0);
    for (int r = 0; r < t->members; r++)
        if (dest[r] != (long)t->world[r] * 1000003L + round) {
            fprintf(stderr, "PE %d: long fcollect on %s: member %d's is %ld\n", me, t->name, r,
                    dest[r]);
            CHECK(dest[r] == (long)t->world[r] * 1000003L + round);
            break;
        }
}

/* A member's block in the collect: lengths that differ, some of them 0. */
static size_t length_of(int world)
{
    return (size_t)(world % 3) * 997 + (size_t)(world % 4 == 1);
}

static void collect(const struct team_case *t, unsigned char *dest, unsigned char *source,
                    int round)
{
    size_t total = 0;
    for (int r = 0; r < t->members; r++)
        total += length_of(t->world[r]);
    size_t mine = length_of(me);
    for (size_t i = 0; i < mine; i++)
        source[i] = byte_value(me, i, round);
    memset(dest, 0, total);
    CHECK(shmem_collectmem(t->team, dest, source, mine) == 0);
    size_t wrong = total, at = 0;
    unsigned char want = 0;
    for (int r = 0; r < t->members && wrong == total; r++)
        for (size_t i = 0; i < length_of(t->world[r]) && wrong == total; i++, at++) {
            want = byte_value(t->world[r], i, round);
            if (dest[at] != want)
                wrong = at;
        }
    first_wrong(t, "collect", dest, total, wrong, want);
}

/*
 * The counts this node's lead published for its last gather on the team:
 * ceil(log2 N) steps, and N - 1 (node, step) pairs in its lists - in the team's
 * block, found by the number its name holds, a block's index being the pool's
 * since the team redesign's step D. Then a sync, so that no lead starts
 * another gather while a PE of its node has still to read this one's.
 */
static void check_counts(const struct team_case *t)
{
    uint64_t steps = UINT64_MAX, shares = UINT64_MAX;
    uint32_t block = osoe_test_team_block(t->number);
    CHECK(block != UINT32_MAX);
    CHECK(osoe_test_scoped_metric("team.tree.gather_steps", OSOE_MSCOPE_TEAM, block, &steps));
    CHECK(osoe_test_scoped_metric("team.tree.gather_shares", OSOE_MSCOPE_TEAM, block,
                                  &shares));
    uint64_t want_steps = (uint64_t)steps_over(t->nodes), want_shares = (uint64_t)t->nodes - 1;
    if (steps != want_steps || shares != want_shares)
        fprintf(stderr, "PE %d: %s spans %d nodes, and this node's lead's last gather made %llu "
                "steps sending %llu shares, not %llu and %llu\n", me, t->name, t->nodes,
                (unsigned long long)steps, (unsigned long long)shares,
                (unsigned long long)want_steps, (unsigned long long)want_shares);
    CHECK(steps == want_steps);
    CHECK(shares == want_shares);
    CHECK(shmem_team_sync(t->team) == 0);
}

/* Everything above on one team, by its members. */
static void gathers_on(const struct team_case *t, unsigned char *dest, unsigned char *source,
                       long *long_dest, long *long_source)
{
    if (t->rank < 0)
        return;
    fcollect_into(t, dest, source, 1, 0, true, false);
    fcollect_longs(t, long_dest, long_source, 1);
    fcollect_into(t, dest, source, LARGE_BLOCK, 2, true, false);
    collect(t, dest, source, 3);
    collect(t, static_collect_dest, source, 6);
    fcollect_into(t, static_dest_zeroed, static_source, STATIC_BLOCK, 4, true, false);
    fcollect_into(t, static_dest_initialized, static_source, STATIC_BLOCK, 5, true, false);

    /* Back to back into one dest: rank 0 reads each result late. */
    for (int round = 0; round < ROUNDS; round++)
        fcollect_into(t, dest, source, round % 2 ? LARGE_BLOCK : 8, 10 + round, false,
                      t->rank == 0);

    if (t->nodes > 1)
        check_counts(t);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    read_map();
    int nodes = 0;
    for (int r = 0; r < npes; r++)
        if (node_of_rank[r] + 1 > nodes)
            nodes = node_of_rank[r] + 1;

    unsigned char *source = shmem_malloc(LARGE_BLOCK);
    unsigned char *dest = shmem_malloc((size_t)npes * LARGE_BLOCK);
    long *long_source = shmem_malloc(sizeof *long_source);
    long *long_dest = shmem_malloc((size_t)npes * sizeof *long_dest);
    CHECK_FATAL(source && dest && long_source && long_dest);

    struct team_case world = { .name = "the world", .team = SHMEM_TEAM_WORLD, .number = 0,
                               .members = npes, .rank = me, .nodes = nodes };
    for (int r = 0; r < npes; r++)
        world.world[r] = r;
    gathers_on(&world, dest, source, long_dest, long_source);

    uint32_t number = PREDEFINED_TEAMS;
    struct team_case evens = split("the even ranks", 0, 2, (npes + 1) / 2, number++);
    gathers_on(&evens, dest, source, long_dest, long_source);
    struct team_case reversed = split("the world reversed", npes - 1, -1, npes, number++);
    gathers_on(&reversed, dest, source, long_dest, long_source);

    const char *triplet = getenv("OSOE_TEST_THREE_OF_FIVE");
    if (triplet) {
        int start = 0, stride = 0, size = 0;
        CHECK_FATAL(sscanf(triplet, "%d,%d,%d", &start, &stride, &size) == 3);
        struct team_case three = split("three of five nodes", start, stride, size, number++);
        CHECK(nodes == 5 && three.nodes == 3);
        gathers_on(&three, dest, source, long_dest, long_source);
        if (three.rank >= 0)
            shmem_team_destroy(three.team);
    }
    if (evens.rank >= 0)
        shmem_team_destroy(evens.team);
    if (reversed.rank >= 0)
        shmem_team_destroy(reversed.team);

    shmem_free(long_dest);
    shmem_free(long_source);
    shmem_free(dest);
    shmem_free(source);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
