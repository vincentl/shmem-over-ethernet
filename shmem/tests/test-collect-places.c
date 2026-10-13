/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * collect's places (P29, P30 and P33 of docs/prompts/v0.2-open-items.md;
 * collectives.c, collect_varying): from each node's total where every node of a
 * team holds one run of its ranks, and from the members' vector where some node
 * does not - or the way OSOE_COLLECTIVE_PLACES forces, where the team allows it:
 * the members' vector anywhere, the leads' scan of the nodes' totals where the
 * runs meet the node set's positions in order or in reverse, and the leads'
 * exchange of the nodes' totals where every node is one run - on teams of the
 * map the launcher was given, which OSOE_TEST_MAP gives again. The first
 * argument names the case:
 *
 *   block            the world, its even ranks and the ranks past the first
 *                    node's, on a block map: each node's total;
 *   negative-stride  strided splits whose stride is negative - the world
 *                    backwards, every other rank backwards, and the ranks past
 *                    the first node's backwards - on a block map: each node's
 *                    total, the runs meeting the nodes in the reverse of the
 *                    node set's order, so that bases summed in the set's order
 *                    rather than team-rank order put the shares in the wrong
 *                    places, as a mutant shows (docs/design/evidence/v0.2-step4/
 *                    2026-10-12-collect-node-totals);
 *   cyclic           the world and the world backwards on a cyclic map, the
 *                    members' vector; and the first rank of each node, one a
 *                    node, each node's total after all - the way is the
 *                    team's, not the map's;
 *   unordered        the world on a block map whose nodes are numbered out of
 *                    the ranks' order (-m 1,1,0,0,2,2), each node one run that
 *                    meets the node set's positions in neither order: the scan
 *                    forced falls back to each node's total;
 *   zeros            the world collecting nothing, again and again - every
 *                    member giving 0, so that no gather follows the places and
 *                    a lead may enter the next collect while another still
 *                    reads this one's - with lengths that differ every eighth
 *                    time: the exchange's two sets of slots and counters
 *                    (exchange_model.py), which a run under OSOE_TEST_PERTURB
 *                    finds wanting where they are one.
 *
 * A team past the first node's that would span one node alone, on a map of two,
 * is left out: it has no internode phase to find places for.
 *
 * Each team collects lengths that differ, some of them 0; then, for each of
 * its nodes in turn, the same with every member there giving 0 - a node whose
 * total is nothing; then every member 0; then one member alone, the last. Every
 * byte is checked at its place, and after each collect the way this node's lead
 * published (team.tree.collect_way) and the count of the lengths it summed
 * between the nodes (team.tree.collect_lengths): the team's nodes for each
 * node's total, its members for the members' vector, one - its own node's - for
 * the scan. Each team is made, used and destroyed before the next, so each
 * takes the first number after the predefined three, and its block is found by
 * it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum {
    MAX_PES = 64,
    LONGEST = 40,                   /* the most bytes a member gives */
    ROUNDS_A_TEAM = MAX_PES + 3,    /* the most collects one team makes */
    FIRST_CREATED_NUMBER = 3,       /* after the world, SHMEM_TEAM_SHARED and the finalize team */
};

/* team.tree.collect_way's values (node.h, enum osoe_collect_way). */
enum way { WAY_NONE, WAY_MEMBERS, WAY_TOTALS, WAY_SCAN, WAY_EXCHANGE };
static const char *const way_names[] = { "none", "the members' vector", "each node's total",
                                         "the leads' scan", "the leads' exchange" };

static int me, npes;
static int node_of_rank[MAX_PES];
static const char *forced;              /* OSOE_COLLECTIVE_PLACES, or "link" */
static unsigned char *source, *dest;    /* LONGEST and MAX_PES * LONGEST bytes, symmetric */

/* A team as the test expects it. */
struct team_case {
    const char *name;
    shmem_team_t team;
    uint32_t number;                /* its team number, which its node blocks have */
    int members;
    int world[MAX_PES];             /* each member's world rank, by team rank */
    int rank;                       /* this PE's, or -1 */
    int nodes;                      /* how many nodes it spans */
    bool one_run;                   /* every node's members one run of team ranks */
    bool in_order;                  /* and the runs meet the nodes in their order */
    bool reversed;                  /* or in its reverse */
};

/* Byte i of world PE `world`'s block in collect `round`: never 0, which dest
 * holds before each collect, and differing from member to member and from
 * round to round. */
static unsigned char byte_value(int world, size_t i, int round)
{
    return (unsigned char)((world * 37 + (int)i * 11 + round * 101) % 255 + 1);
}

/* The map, one node per rank, from OSOE_TEST_MAP ("0,0,1,1"). */
static void read_map(void)
{
    const char *map = getenv("OSOE_TEST_MAP");
    CHECK_FATAL(map && npes <= MAX_PES);
    for (int r = 0; r < npes; r++) {
        char *end = NULL;
        node_of_rank[r] = (int)strtol(map, &end, 10);
        CHECK_FATAL(end != map && node_of_rank[r] >= 0 && node_of_rank[r] < MAX_PES);
        map = *end == ',' ? end + 1 : end;
    }
}

/* What the map makes of a team's ranks: how many nodes it spans, whether each
 * node's members are one run - walked as the library walks them, a node met
 * again after its run ended saying no - and whether the runs meet the nodes in
 * their order or its reverse, the node set being sorted by node. */
static void runs_of(struct team_case *t)
{
    bool met[MAX_PES] = { false };
    int previous = -1, run_nodes[MAX_PES], runs = 0;
    t->nodes = 0;
    t->one_run = true;
    for (int r = 0; r < t->members; r++) {
        int node = node_of_rank[t->world[r]];
        if (node == previous)
            continue;
        if (met[node])
            t->one_run = false;
        else
            t->nodes++;
        met[node] = true;
        previous = node;
        run_nodes[runs++] = node;
    }
    t->in_order = t->reversed = t->one_run;
    for (int j = 1; j < runs; j++) {
        t->in_order &= run_nodes[j - 1] < run_nodes[j];
        t->reversed &= run_nodes[j - 1] > run_nodes[j];
    }
}

/* The way the library takes on `t` (collectives.c, places_way): the forced one
 * where the team allows it, and the one it falls back to. */
static enum way way_of(const struct team_case *t)
{
    if (t->nodes < 2)
        return WAY_NONE;
    if (!t->one_run || strcmp(forced, "members") == 0)
        return WAY_MEMBERS;
    if (strcmp(forced, "scan") == 0 && (t->in_order || t->reversed))
        return WAY_SCAN;
    if (strcmp(forced, "exchange") == 0)
        return WAY_EXCHANGE;            /* the tests' teams span a few nodes, within its room */
    return WAY_TOTALS;
}

/* The team a triplet of world ranks makes (spec §9.4.6), and what the test
 * expects of it. */
static struct team_case split(const char *name, int start, int stride, int size)
{
    struct team_case t = { .name = name, .number = FIRST_CREATED_NUMBER, .members = size,
                           .rank = -1 };
    for (int r = 0; r < size; r++) {
        t.world[r] = start + stride * r;
        CHECK_FATAL(t.world[r] >= 0 && t.world[r] < npes);
        if (t.world[r] == me)
            t.rank = r;
    }
    runs_of(&t);
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, start, stride, size, NULL, 0, &t.team) ==
          0);
    CHECK((t.team != SHMEM_TEAM_INVALID) == (t.rank >= 0));
    if (t.rank >= 0) {
        CHECK(shmem_team_my_pe(t.team) == t.rank);
        CHECK(shmem_team_n_pes(t.team) == size);
    }
    return t;
}

static struct team_case world_team(void)
{
    struct team_case t = { .name = "the world", .team = SHMEM_TEAM_WORLD, .number = 0,
                           .members = npes, .rank = me };
    for (int r = 0; r < npes; r++)
        t.world[r] = r;
    runs_of(&t);
    return t;
}

/* The lengths one collect gives: each member's, by team rank. */
struct lengths {
    const char *what;
    size_t of[MAX_PES];
};

/*
 * One collect of `lengths` on `t`, every byte checked at its place, and the
 * count of lengths this node's lead summed between the nodes - its nodes for
 * each node's total, its members for the members' vector, and nothing on a
 * team of one node, which sums none.
 */
static void collect(const struct team_case *t, const struct lengths *lengths, int round)
{
    size_t total = 0, mine = lengths->of[t->rank];
    for (int r = 0; r < t->members; r++)
        total += lengths->of[r];
    for (size_t i = 0; i < mine; i++)
        source[i] = byte_value(me, i, round);
    memset(dest, 0, (size_t)MAX_PES * LONGEST);
    CHECK(shmem_collectmem(t->team, dest, source, mine) == 0);
    size_t at = 0, wrong = SIZE_MAX;
    for (int r = 0; r < t->members; r++)
        for (size_t i = 0; i < lengths->of[r]; i++, at++)
            if (wrong == SIZE_MAX && dest[at] != byte_value(t->world[r], i, round))
                wrong = at;
    for (size_t i = total; i < (size_t)MAX_PES * LONGEST && wrong == SIZE_MAX; i++)
        if (dest[i] != 0)
            wrong = i;
    if (wrong != SIZE_MAX)
        fprintf(stderr, "PE %d: collect on %s, %s, %zu bytes: byte %zu is %u\n", me, t->name,
                lengths->what, total, wrong, dest[wrong]);
    CHECK(wrong == SIZE_MAX);

    uint64_t summed = UINT64_MAX, way = UINT64_MAX;
    uint32_t block = osoe_test_team_block(t->number);
    CHECK(block != UINT32_MAX);
    CHECK(osoe_test_scoped_metric("team.tree.collect_lengths", OSOE_MSCOPE_TEAM, block,
                                  &summed));
    CHECK(osoe_test_scoped_metric("team.tree.collect_way", OSOE_MSCOPE_TEAM, block, &way));
    enum way want_way = way_of(t);
    uint64_t want = want_way == WAY_NONE      ? 0
                    : want_way == WAY_MEMBERS ? (uint64_t)t->members
                    : want_way == WAY_SCAN    ? 1
                                              : (uint64_t)t->nodes;
    if (summed != want || way != (uint64_t)want_way)
        fprintf(stderr, "PE %d: %s spans %d nodes, %s, and its lead here took way %llu and "
                "summed %llu lengths for a collect's places under OSOE_COLLECTIVE_PLACES=%s, "
                "not %s and %llu\n", me, t->name, t->nodes,
                !t->one_run ? "some not one run" : t->in_order ? "each one run, in order"
                : t->reversed ? "each one run, in reverse" : "each one run, out of order",
                (unsigned long long)way, (unsigned long long)summed, forced,
                way_names[want_way], (unsigned long long)want);
    CHECK(summed == want && way == (uint64_t)want_way);
}

/* Every collect of the file's comment, on one team, by its members: the
 * rounds from `round` on, which every PE gives the team alike, member or not. */
static void collects_on(const struct team_case *t, int round)
{
    if (t->rank < 0)
        return;
    struct lengths lengths = { .what = "lengths that differ" };
    for (int r = 0; r < t->members; r++) {
        int w = t->world[r];
        lengths.of[r] = (size_t)(w % 3) * 13 + (size_t)(w % 4 == 1);    /* 0 for some */
    }
    collect(t, &lengths, round++);

    bool done[MAX_PES] = { false };
    for (int r = 0; r < t->members; r++) {
        int zero = node_of_rank[t->world[r]];
        if (done[zero])
            continue;
        done[zero] = true;
        struct lengths none_there = { .what = "one node's members giving 0" };
        for (int q = 0; q < t->members; q++)
            none_there.of[q] = node_of_rank[t->world[q]] == zero
                                   ? 0 : (size_t)(t->world[q] % 5) + 1;
        collect(t, &none_there, round++);
    }

    struct lengths nothing = { .what = "every member giving 0" };
    collect(t, &nothing, round++);
    struct lengths last = { .what = "the last member alone giving any" };
    last.of[t->members - 1] = LONGEST;
    collect(t, &last, round++);
}

/* A team made by a split: its collects, then its end, so that the next takes
 * its number. */
static void split_and_collect(const char *name, int start, int stride, int size, bool one_run,
                              int round)
{
    struct team_case t = split(name, start, stride, size);
    if (t.nodes == 1 && strstr(name, "past the first node's")) {
        /* On a map of two nodes: one node alone, nothing to find across
         * nodes (the file's comment). */
        if (t.rank >= 0)
            shmem_team_destroy(t.team);
        shmem_barrier_all();
        return;
    }
    if (t.one_run != one_run)
        fprintf(stderr, "PE %d: the map makes %s %s one run on each of its %d nodes; the case "
                "wants it %s\n", me, name, t.one_run ? "is" : "is not", t.nodes,
                one_run ? "so" : "not so");
    CHECK_FATAL(t.one_run == one_run && t.nodes > 1);
    collects_on(&t, round);
    if (t.rank >= 0)
        shmem_team_destroy(t.team);
    shmem_barrier_all();
}

int main(int argc, char **argv)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    read_map();
    CHECK_FATAL(argc == 2);
    const char *which = argv[1];
    forced = getenv("OSOE_COLLECTIVE_PLACES") ? getenv("OSOE_COLLECTIVE_PLACES") : "link";
    source = shmem_malloc(LONGEST);
    dest = shmem_malloc((size_t)MAX_PES * LONGEST);
    CHECK_FATAL(source && dest);

    /* The ranks on the first node, the world's rank 0's: the first rank past
     * them starts a team that leaves that node out. */
    int first_run = 1;
    while (first_run < npes && node_of_rank[first_run] == node_of_rank[0])
        first_run++;
    struct team_case world = world_team();
    if (strcmp(which, "block") == 0) {
        CHECK_FATAL(world.one_run && world.in_order && world.nodes > 1 && first_run < npes);
        collects_on(&world, 0);
        split_and_collect("the even ranks", 0, 2, (npes + 1) / 2, true, ROUNDS_A_TEAM);
        split_and_collect("the ranks past the first node's", first_run, 1, npes - first_run,
                          true, 2 * ROUNDS_A_TEAM);
    } else if (strcmp(which, "negative-stride") == 0) {
        CHECK_FATAL(world.one_run && world.in_order && world.nodes > 1 && first_run < npes);
        split_and_collect("the world backwards", npes - 1, -1, npes, true, 0);
        split_and_collect("every other rank backwards", npes - 1, -2, (npes + 1) / 2, true,
                          ROUNDS_A_TEAM);
        split_and_collect("the ranks past the first node's backwards", npes - 1, -1,
                          npes - first_run, true, 2 * ROUNDS_A_TEAM);
    } else if (strcmp(which, "cyclic") == 0) {
        CHECK_FATAL(!world.one_run && world.nodes > 2);
        collects_on(&world, 0);
        split_and_collect("the world backwards", npes - 1, -1, npes, false, ROUNDS_A_TEAM);
        split_and_collect("the first rank of each node", 0, 1, world.nodes, true,
                          2 * ROUNDS_A_TEAM);
    } else if (strcmp(which, "unordered") == 0) {
        CHECK_FATAL(world.one_run && !world.in_order && !world.reversed && world.nodes > 2);
        collects_on(&world, 0);
    } else if (strcmp(which, "zeros") == 0) {
        CHECK_FATAL(world.one_run && world.nodes > 2);
        struct lengths nothing = { .what = "every member giving 0, again" };
        struct lengths differ = { .what = "lengths that differ, between the zeros" };
        for (int r = 0; r < npes; r++)
            differ.of[r] = (size_t)(r % 3) * 7 + 1;
        for (int round = 0; round < 96; round++)
            collect(&world, round % 8 == 7 ? &differ : &nothing, round);
    } else {
        fprintf(stderr, "no case '%s': block, negative-stride, cyclic, unordered or zeros\n",
                which);
        CHECK_FATAL(false);
    }

    shmem_free(dest);
    shmem_free(source);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
