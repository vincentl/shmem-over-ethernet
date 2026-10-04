/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Where a team's state lives, and how its shape is found (phase 1 step 7
 * slice 1): the descriptor's local list, the world mapping, each team's record,
 * and osoe-stat's walk of the records.
 *
 * **A local id is not a team rank.** The collectives find a member's memory by
 * its local id and its place in a result by its team rank, and the two agree
 * only when a node's ranks are the team's first. On 0,1,2,0,1,0 node 0 holds
 * world ranks 0, 3 and 5 at local ids 0, 1 and 2, so a descriptor that took one
 * for the other would put rank 3's block at place 1. The predefined teams list
 * their members in local-id order with local id 0 - the node's lead - as their
 * local lead, which is what the generic internode phase addresses on every
 * node, and so they run on a node whose local ids are given by hand against
 * its ranks (rank 0 at local id 1, rank 1 at local id 0) as on any other, one
 * node or several. Each PE checks its own node's list against what the map and
 * the launcher's OSOE_GTIDS - the local ids' ranks, in local-id order - imply,
 * then runs a reduction, a gather and a broadcast on the team, whose results
 * depend on the list and the lead being right.
 *
 * **SHMEM_TEAM_SHARED's ranks are its local ids**, and its world mapping is a
 * start and a stride when the node's ranks are one progression and a table
 * when they interleave: shmem_team_translate_pe is checked both ways for every
 * rank. Under OSOE_FORCE_REMOTE it is the PE alone, whose local list is its
 * own local id - not 0 on a node's second PE.
 *
 * **Each team's record says where its node is in the tree** (node.h), and
 * osoe-stat names the teams by their records (layout 18). Each node's lead runs
 * it on its node's control segment while the job is live: the dump must name
 * the world and finalize trees by the names they have had since layout 12,
 * show each team whose record is published, and give the world's record as
 * the map implies it - its local members, node count, index, parent, position
 * and children, each neighbour named by its lead's rank, which on 0,0,1,1 is
 * not its node's number. The program initializes twice and finalizes once
 * before it looks, so that the finalize team has run and, on a job of several
 * nodes, --summary gives that team's line as well as the world's.
 *
 * **A created team lists its members in team-rank order** (phase 1 step 7
 * slice 2), its local lead the lowest team rank on the node - on the
 * hand-given order, rank 0 at local id 1, not the node's local id 0 - and its
 * lead on each node is what its records name and what its local lead keeps,
 * by position in its node set, for the internode phase. Three are made: the
 * world again (number 3), the world reversed (4), whose lead on each node is
 * its highest rank, and each node reversed, split from SHMEM_TEAM_SHARED - a
 * table of ranks where the node's interleave with another's. Each PE checks
 * its descriptors against the map and runs the collectives on each, and
 * osoe-stat names teams 3 and 4 by their numbers, with their records as the
 * map implies them, and the node's own. Then team 4 is destroyed, and its
 * record is gone from the dump: since the team redesign's step D the last of
 * a team's members on a node to leave its block retracts the record and gives
 * the block back (node.h). The two left alive at the end are the session's
 * end to free. And a fourth team, the world again, has node 0's record
 * published 300 ms late, by a wrapped function of the link, while every member
 * syncs on it the moment its split returns: the split's sync over its parent
 * is what makes that safe (check_parent_sync).
 *
 * OSOE_TEST_MAP gives the map, and OSOE_TEST_LEADS each node's lead's rank
 * where a hand-given order makes it other than the node's lowest.
 *
 * White-box: it reads the descriptor, so the library's sources are compiled
 * into it (CMakeLists.txt), as test-internal-region.c's are.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "collective-tree.h"
#include "node.h"
#include "state.h"
#include "team.h"

enum { MAX_PES = 64 };

static int me, npes;

/* The teams this program creates: the world again, the world reversed, and
 * this PE's node reversed, whose number its node's claim decides. */
static shmem_team_t again_team = SHMEM_TEAM_INVALID, reverse_team = SHMEM_TEAM_INVALID,
                    node_team = SHMEM_TEAM_INVALID;
static uint32_t node_team_number;

/* A comma-separated list of numbers into `values`, up to `capacity`; how many. */
static uint32_t read_list(const char *list, uint32_t *values, uint32_t capacity)
{
    uint32_t count = 0;
    while (list && *list && count < capacity) {
        values[count++] = (uint32_t)strtoul(list, NULL, 10);
        list = strchr(list, ',');
        if (list)
            list++;
    }
    return count;
}

/* The logical node map, one node per rank, from OSOE_TEST_MAP ("0,1,2,0,1,0"),
 * or every rank on node 0 when it is unset; returns the node count. */
static uint32_t read_map(uint32_t *node_of)
{
    for (int r = 0; r < npes; r++)
        node_of[r] = 0;
    read_list(getenv("OSOE_TEST_MAP"), node_of, (uint32_t)npes);
    uint32_t nodes = 0;
    for (int r = 0; r < npes; r++)
        if (node_of[r] + 1 > nodes)
            nodes = node_of[r] + 1;
    return nodes;
}

/* Each node's lead's rank: OSOE_TEST_LEADS, or the node's lowest rank, which is
 * its local id 0 as osoe-launch numbers them. */
static void read_leads(const uint32_t *node_of, uint32_t nodes, uint32_t *lead_of)
{
    for (uint32_t n = 0; n < nodes; n++)
        lead_of[n] = UINT32_MAX;
    for (int r = npes - 1; r >= 0; r--)
        lead_of[node_of[r]] = (uint32_t)r;
    read_list(getenv("OSOE_TEST_LEADS"), lead_of, nodes);
}

/*
 * The descriptor's local list for a predefined team every local PE belongs
 * to, against the map and the launcher's local ids: every local id in order,
 * each with the rank the launcher gave it, all of them this node's by the map,
 * and local id 0 the lead.
 */
static void check_every_local_pe(const struct osoe_team *t, const uint32_t *node_of,
                                 const uint32_t *gtids, uint32_t local_count)
{
    uint32_t here = node_of[me], expected = 0;
    for (int r = 0; r < npes; r++)
        if (node_of[r] == here)
            expected++;
    CHECK(t->local_members == expected);
    CHECK(t->local_members == local_count);
    for (uint32_t m = 0; m < t->local_members && m < MAX_PES; m++) {
        CHECK(t->local_ids[m] == m);
        CHECK(t->local_ranks[m] == gtids[m]);
        CHECK(t->local_ranks[m] < (uint32_t)npes && node_of[t->local_ranks[m]] == here);
    }
    CHECK(t->lead_local_id == 0 && t->local_ids[0] == 0);
}

/* A sum, a gather of every rank and a broadcast from the last rank on `team`,
 * of `members` members numbered from 0: each result depends on the lead, the
 * members' places and their memory being found by the local list. */
static void check_collectives(shmem_team_t team, int members, int my_rank_in_team)
{
    long *source = shmem_malloc(sizeof *source * (size_t)MAX_PES);
    long *dest = shmem_malloc(sizeof *dest * (size_t)MAX_PES);
    CHECK_FATAL(source && dest);
    source[0] = shmem_team_translate_pe(team, my_rank_in_team, SHMEM_TEAM_WORLD) + 1;
    CHECK(shmem_long_sum_reduce(team, dest, source, 1) == 0);
    long sum = 0;
    for (int r = 0; r < members; r++)
        sum += shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD) + 1;
    CHECK(dest[0] == sum);

    CHECK(shmem_long_fcollect(team, dest, source, 1) == 0);
    for (int r = 0; r < members; r++)
        CHECK(dest[r] == shmem_team_translate_pe(team, r, SHMEM_TEAM_WORLD) + 1);

    source[0] = 1000 + my_rank_in_team;
    dest[0] = -1;
    CHECK(shmem_long_broadcast(team, dest, source, 1, members - 1) == 0);
    CHECK(dest[0] == 1000 + members - 1);
    CHECK(shmem_team_sync(team) == 0);
    shmem_free(source);
    shmem_free(dest);
}

static void check_world(const uint32_t *node_of, uint32_t nodes, const uint32_t *gtids,
                        uint32_t local_count)
{
    const struct osoe_team *world = osoe_team_of("test-team-storage", SHMEM_TEAM_WORLD);
    CHECK_FATAL(world != NULL);
    CHECK(world->number == OSOE_TEAM_WORLD);
    CHECK(world->members == (uint32_t)npes);
    CHECK(world->me == (uint32_t)me);
    check_every_local_pe(world, node_of, gtids, local_count);
    for (int r = 0; r < npes; r++)
        CHECK(osoe_team_to_world(world, r) == r);
    CHECK(world->nodes == nodes);
    CHECK(world->internode == (nodes > 1));
    check_collectives(SHMEM_TEAM_WORLD, npes, me);
}

static void check_shared(const uint32_t *gtids, uint32_t local_count)
{
    const struct osoe_team *shared = osoe_team_of("test-team-storage", SHMEM_TEAM_SHARED);
    CHECK_FATAL(shared != NULL);
    uint32_t local_id = osoe.bootstrap.local_id;
    if (osoe.job.force_remote) {
        /* The PE alone: its own local id is its whole list. */
        CHECK(shared->singleton);
        CHECK(shared->members == 1 && shared->local_members == 1);
        CHECK(shared->local_ids[0] == local_id && shared->local_ranks[0] == 0);
        CHECK(shared->lead_local_id == local_id);
        CHECK(shmem_team_translate_pe(SHMEM_TEAM_SHARED, 0, SHMEM_TEAM_WORLD) == me);
        for (int r = 0; r < npes; r++)
            CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, r, SHMEM_TEAM_SHARED) ==
                  (r == me ? 0 : -1));
        check_collectives(SHMEM_TEAM_SHARED, 1, 0);
        return;
    }
    CHECK(!shared->singleton);
    CHECK(shared->members == local_count && shared->local_members == local_count);
    CHECK(shared->me == local_id);
    for (uint32_t m = 0; m < local_count; m++)
        CHECK(shared->local_ids[m] == m && shared->local_ranks[m] == m);
    CHECK(shared->lead_local_id == 0);
    CHECK(shared->nodes == 1 && !shared->internode);
    /* Both ways, for every rank: a local id's rank, and a rank's local id or
     * -1 for a rank on another node. */
    for (uint32_t m = 0; m < local_count; m++)
        CHECK(shmem_team_translate_pe(SHMEM_TEAM_SHARED, (int)m, SHMEM_TEAM_WORLD) ==
              (int)gtids[m]);
    for (int r = 0; r < npes; r++) {
        int want = -1;
        for (uint32_t m = 0; m < local_count; m++)
            if (gtids[m] == (uint32_t)r)
                want = (int)m;
        CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, r, SHMEM_TEAM_SHARED) == want);
    }
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_SHARED, (int)local_count, SHMEM_TEAM_WORLD) == -1);
    check_collectives(SHMEM_TEAM_SHARED, (int)local_count, (int)local_id);
}

/*
 * A created team's descriptor against the map: its members here in team-rank
 * order, each at the local id the launcher's OSOE_GTIDS gives its rank, the
 * lead the first of them, its world mapping, and its node set - counted, and
 * every node's lead by position, the member of lowest team rank there, kept,
 * on the local lead alone. `world_of` gives the team's ranks as world ranks, and `number`
 * its number, or UINT32_MAX where the test cannot say.
 */
static void check_created(shmem_team_t team, uint32_t number, const uint32_t *world_of,
                          int members, const uint32_t *node_of, uint32_t nodes,
                          const uint32_t *gtids, uint32_t local_count)
{
    const struct osoe_team *t = osoe_team_of("test-team-storage", team);
    CHECK_FATAL(t != NULL);
    if (number != UINT32_MAX)
        CHECK(t->number == number);
    CHECK(t->members == (uint32_t)members);
    uint32_t here = osoe.job.node, count = 0;
    for (int r = 0; r < members; r++) {
        CHECK(osoe_team_to_world(t, r) == (int)world_of[r]);
        if (node_of[world_of[r]] != here)
            continue;
        uint32_t id = UINT32_MAX;
        for (uint32_t m = 0; m < local_count; m++)
            if (gtids[m] == world_of[r])
                id = m;
        CHECK(count < t->local_members && t->local_ranks[count] == (uint32_t)r &&
              t->local_ids[count] == id);
        count++;
    }
    CHECK(t->local_members == count);
    CHECK(count > 0 && t->lead_local_id == t->local_ids[0]);

    uint32_t lead_by_node[MAX_PES], spanned = 0;
    for (uint32_t n = 0; n < nodes; n++)
        lead_by_node[n] = UINT32_MAX;
    for (int r = 0; r < members; r++)
        if (lead_by_node[node_of[world_of[r]]] == UINT32_MAX) {
            lead_by_node[node_of[world_of[r]]] = world_of[r];
            spanned++;
        }
    /* Only the local lead builds the node set (team.h, `nodes`); every member
     * knows whether the team spans nodes. */
    bool lead = osoe.bootstrap.local_id == t->lead_local_id;
    CHECK(t->internode == (spanned > 1));
    CHECK(t->nodes == (lead ? spanned : spanned > 1 ? 0 : 1));
    if (lead && t->internode) {
        CHECK_FATAL(t->node_leads != NULL);
        for (uint32_t n = 0, i = 0; n < nodes; n++)
            if (lead_by_node[n] != UINT32_MAX)
                CHECK(t->node_leads[i++] == lead_by_node[n]);
    } else {
        CHECK(t->node_leads == NULL);
    }
}

/* The three teams, each checked and its collectives run. */
static void check_created_teams(const uint32_t *node_of, uint32_t nodes, const uint32_t *gtids,
                                uint32_t local_count)
{
    uint32_t world_of[MAX_PES];
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &again_team) ==
                0);
    for (int r = 0; r < npes; r++)
        world_of[r] = (uint32_t)r;
    check_created(again_team, 3, world_of, npes, node_of, nodes, gtids, local_count);
    check_collectives(again_team, npes, me);

    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, npes - 1, -1, npes, NULL, 0,
                                         &reverse_team) == 0);
    for (int r = 0; r < npes; r++)
        world_of[r] = (uint32_t)(npes - 1 - r);
    check_created(reverse_team, 4, world_of, npes, node_of, nodes, gtids, local_count);
    check_collectives(reverse_team, npes, npes - 1 - me);

    int n = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_SHARED, n - 1, -1, n, NULL, 0, &node_team) ==
                0);
    for (int r = 0; r < n; r++)
        world_of[r] = (uint32_t)shmem_team_translate_pe(SHMEM_TEAM_SHARED, n - 1 - r,
                                                        SHMEM_TEAM_WORLD);
    check_created(node_team, UINT32_MAX, world_of, n, node_of, nodes, gtids, local_count);
    const struct osoe_team *t = osoe_team_of("test-team-storage", node_team);
    const struct osoe_team *shared = osoe_team_of("test-team-storage", SHMEM_TEAM_SHARED);
    node_team_number = t->number;
    CHECK(node_team_number >= 5);
    /* A split of a table is a table (team.h). */
    CHECK((t->world_of_rank != NULL) == (shared->world_of_rank != NULL));
    check_collectives(node_team, n, shmem_team_my_pe(node_team));
}

/*
 * The split's sync over its parent orders every node's record of the new team
 * before any collective on it (node.h): the protocol's linchpin, over a window
 * of microseconds that no run meets by chance. So one record is made late on
 * purpose. The program links with --wrap=osoe_collective_tree (CMakeLists.txt),
 * which write_record asks for the node's place in a team's tree just before
 * it publishes the record, and the wrapper sleeps first when this process has
 * asked it to: PE 0 does, node 0's local lead of a team of the whole world, so
 * node 0's record of it is published 300 ms late. Every member syncs on the
 * team the moment its split returns: without the parent's sync, the other
 * nodes' arrivals would reach node 0 - the root - before its record, and a
 * node 0 member's own before it too, and the job would end naming the team.
 * The library is the one the job runs; only the link differs. On one node the
 * tree is never asked for, and nothing waits.
 */
static long record_delay_milliseconds;

bool __real_osoe_collective_tree(const uint32_t *nodes, uint32_t count, uint32_t node,
                                 uint32_t fanout, struct osoe_collective_tree *tree);
bool __wrap_osoe_collective_tree(const uint32_t *nodes, uint32_t count, uint32_t node,
                                 uint32_t fanout, struct osoe_collective_tree *tree);

bool __wrap_osoe_collective_tree(const uint32_t *nodes, uint32_t count, uint32_t node,
                                 uint32_t fanout, struct osoe_collective_tree *tree)
{
    if (record_delay_milliseconds > 0) {
        struct timespec pause = { record_delay_milliseconds / 1000,
                                  (record_delay_milliseconds % 1000) * 1000 * 1000 };
        nanosleep(&pause, NULL);
        record_delay_milliseconds = 0;
    }
    return __real_osoe_collective_tree(nodes, count, node, fanout, tree);
}

static void check_parent_sync(void)
{
    if (me == 0)
        record_delay_milliseconds = 300;
    shmem_team_t late = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &late) == 0);
    record_delay_milliseconds = 0;
    /* First, before any collective of the world's could order anything. */
    CHECK(shmem_team_sync(late) == 0);
    check_collectives(late, npes, me);
    shmem_team_destroy(late);
}

/* Run osoe-stat with `args` on this node's control segment. */
static int run_stat(const char *args, char *out, size_t len)
{
    const char *tool = getenv("OSOE_TEST_STAT");
    if (!tool) {
        fprintf(stderr, "OSOE_TEST_STAT is not set\n");
        return -1;
    }
    char command[512];
    snprintf(command, sizeof command, "'%s' %s '%s' 2>&1", tool, args, osoe.node.control_name);
    FILE *f = popen(command, "r");
    if (!f)
        return -1;
    size_t n = fread(out, 1, len - 1, f);
    out[n] = '\0';
    return pclose(f);
}

static bool says(const char *out, const char *what)
{
    if (strstr(out, what))
        return true;
    fprintf(stderr, "PE %d: osoe-stat does not say \"%s\"\n", me, what);
    return false;
}

/* The dump's value for `name`: the number after it on its own line. */
static bool dump_value(const char *out, const char *name, uint64_t *value)
{
    char line[96];
    snprintf(line, sizeof line, "\n    %s ", name);
    const char *at = strstr(out, line);
    if (!at) {
        fprintf(stderr, "PE %d: osoe-stat -d shows no %s\n", me, name);
        return false;
    }
    *value = strtoull(at + strlen(line), NULL, 10);
    return true;
}

static bool dump_is(const char *out, const char *name, uint64_t want)
{
    uint64_t value = 0;
    if (!dump_value(out, name, &value))
        return false;
    if (value == want)
        return true;
    fprintf(stderr, "PE %d: osoe-stat -d shows %s %" PRIu64 ", not %" PRIu64 "\n", me, name,
            value, want);
    return false;
}

static bool record_is(const char *out, const char *label, const char *field, uint64_t want)
{
    char name[64];
    snprintf(name, sizeof name, "team.%s.record.%s", label, field);
    return dump_is(out, name, want);
}

/*
 * A team's record on this node against the map, through osoe-stat as a person
 * reading a hung job would: `local` members here, and §5.3's k-ary tree over
 * the team's `nodes` nodes - every node of the job, or this one alone, so this
 * node at its own number's position - each neighbour named by its lead's rank,
 * `lead_of` by node.
 */
static void check_record(const char *out, const char *label, uint32_t local, uint32_t nodes,
                         const uint32_t *lead_of)
{
    uint32_t here = osoe.job.node, k = osoe.bootstrap.collective_fanout;
    CHECK(record_is(out, label, "published", 1));
    CHECK(record_is(out, label, "local_members", local));
    CHECK(record_is(out, label, "nodes", nodes));
    if (nodes < 2)
        return;
    CHECK(record_is(out, label, "index", here));
    CHECK(record_is(out, label, "parent", here == 0 ? UINT32_MAX : lead_of[(here - 1) / k]));
    CHECK(record_is(out, label, "position", here == 0 ? 0 : (here - 1) % k));
    uint32_t children = 0;
    for (uint32_t c = 0; c < k && (uint64_t)k * here + 1 + c < nodes; c++) {
        char field[32];
        snprintf(field, sizeof field, "child.%u", c);
        CHECK(record_is(out, label, field, lead_of[k * here + 1 + c]));
        children++;
    }
    CHECK(record_is(out, label, "children", children));
}

/* Whether the summary has a line for node `node` and team `team`. */
static bool summary_has(const char *out, uint32_t node, const char *team)
{
    char word[32];
    snprintf(word, sizeof word, " %s ", team);
    const char *line = out;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char copy[512];
        snprintf(copy, sizeof copy, "%.*s", (int)(len < sizeof copy ? len : sizeof copy - 1),
                 line);
        if (strncmp(copy, "node ", 5) == 0 && strtoul(copy + 5, NULL, 10) == node &&
            strstr(copy, word))
            return true;
        line += len + (end != NULL);
    }
    fprintf(stderr, "PE %d: osoe-stat --summary has no %s line for node %u\n", me, team, node);
    return false;
}

static void check_stat(const uint32_t *node_of, uint32_t nodes, const uint32_t *lead_of,
                       const uint32_t *lowest_of, const uint32_t *highest_of)
{
    static char out[256 * 1024];
    uint32_t local = 0;
    for (int r = 0; r < npes; r++)
        if (node_of[r] == osoe.job.node)
            local++;
    CHECK(run_stat("-d", out, sizeof out) == 0);
    CHECK(says(out, "team.world.tree.epoch_open"));
    CHECK(says(out, "team.finalize.tree.epoch_open"));
    CHECK(says(out, "team world (number 0)"));
    CHECK(says(out, "team finalize (number 2)"));
    if (!osoe.job.force_remote)
        CHECK(says(out, "team shared (number 1)"));
    /* Every PE's epochs, since layout 18 in a row of their own. */
    CHECK(says(out, "pe.world_epoch"));
    check_record(out, "world", local, nodes, lead_of);

    /* The created teams, by number: the world again and reversed, led on each
     * node by its lowest and its highest rank, and this PE's node reversed, on
     * this node alone. */
    CHECK(says(out, "team 3 (number 3)"));
    CHECK(says(out, "team.3.tree.epoch_open"));
    CHECK(says(out, "team 4 (number 4)"));
    /* With the world ranks each record carries, which name a team alike on
     * every node (osoe-stat's team_ranks; the review of step D's merge point,
     * readiness Q5-2). */
    char ranks[96];
    if (npes > 1) {
        snprintf(ranks, sizeof ranks, "team 3 (number 3) of world ranks 0 to %d,", npes - 1);
        CHECK(says(out, ranks));
        snprintf(ranks, sizeof ranks, "team 4 (number 4) of world ranks %d to 0 by -1,",
                 npes - 1);
        CHECK(says(out, ranks));
    } else {
        CHECK(says(out, "team 3 (number 3) of world rank 0,"));
    }
    check_record(out, "3", local, nodes, lowest_of);
    check_record(out, "4", local, nodes, highest_of);
    char label[16];
    snprintf(label, sizeof label, "%u", node_team_number);
    check_record(out, label, osoe.job.force_remote ? 1 : local, 1, NULL);

    CHECK(run_stat("--summary", out, sizeof out) == 0);
    CHECK(summary_has(out, osoe.job.node, "world"));
    /* A created team that spans nodes and has run, as its number and its first
     * member's world rank. */
    if (nodes > 1) {
        CHECK(summary_has(out, osoe.job.node, "3@r0"));
        snprintf(ranks, sizeof ranks, "4@r%d", npes - 1);
        CHECK(summary_has(out, osoe.job.node, ranks));
    }
    /* The finalize team has run once, at the first finalize: on a job of
     * several nodes its tree has closed an epoch, and it has a line too. */
    if (nodes > 1)
        CHECK(summary_has(out, osoe.job.node, "finalize"));
}

/*
 * Two teams with no PE in common may share a number (step D, D1), and
 * osoe-stat must still tell them apart (the review of step D's merge point,
 * readiness Q5-2): the even ranks and the odd ranks, each split from the world,
 * agree the same lowest free number - every PE holds the same numbers here -
 * and the summary labels each by that number and its first world rank, 0 and
 * 1, on every node with a member of it. Run once every team above has run, on
 * a job of several nodes and four PEs or more, so that both span nodes.
 */
static void check_shared_number(const uint32_t *node_of, uint32_t nodes)
{
    if (npes < 4 || nodes < 2 || osoe.job.force_remote)
        return;
    shmem_team_t even = SHMEM_TEAM_INVALID, odd = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, (npes + 1) / 2, NULL, 0,
                                         &even) == 0);
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 2, npes / 2, NULL, 0, &odd) == 0);
    shmem_team_t mine = me % 2 ? odd : even;
    CHECK_FATAL(mine != SHMEM_TEAM_INVALID);
    CHECK((me % 2 ? even : odd) == SHMEM_TEAM_INVALID);
    int *number = shmem_malloc(3 * sizeof *number);
    CHECK_FATAL(number != NULL);
    number[0] = (int)osoe_team_of("test-team-storage", mine)->number;
    CHECK(shmem_int_max_reduce(SHMEM_TEAM_WORLD, &number[1], &number[0], 1) == 0);
    CHECK(shmem_int_min_reduce(SHMEM_TEAM_WORLD, &number[2], &number[0], 1) == 0);
    CHECK(number[1] == number[2]);
    shmem_team_sync(mine);
    shmem_barrier_all();
    if (osoe.bootstrap.local_id == 0) {
        static char out[256 * 1024];
        bool even_here = false, odd_here = false;
        for (int r = 0; r < npes; r++)
            if (node_of[r] == osoe.job.node)
                *(r % 2 ? &odd_here : &even_here) = true;
        CHECK(run_stat("--summary", out, sizeof out) == 0);
        char label[32];
        snprintf(label, sizeof label, "%d@r0", number[0]);
        if (even_here)
            CHECK(summary_has(out, osoe.job.node, label));
        snprintf(label, sizeof label, "%d@r1", number[0]);
        if (odd_here)
            CHECK(summary_has(out, osoe.job.node, label));
    }
    shmem_barrier_all();
    shmem_team_destroy(mine);
    shmem_free(number);
}

int main(void)
{
    /* Twice, so that the first finalize below is a barrier on the finalize
     * team and not the end of the session. */
    shmem_init();
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(npes <= MAX_PES);

    uint32_t node_of[MAX_PES], gtids[MAX_PES], lead_of[MAX_PES];
    uint32_t nodes = read_map(node_of);
    CHECK_FATAL(nodes <= MAX_PES);
    read_leads(node_of, nodes, lead_of);
    uint32_t local_count = read_list(getenv("OSOE_GTIDS"), gtids, MAX_PES);
    CHECK_FATAL(local_count == osoe.bootstrap.local_count);
    CHECK(osoe.job.node_count == nodes);

    check_world(node_of, nodes, gtids, local_count);
    check_shared(gtids, local_count);
    check_created_teams(node_of, nodes, gtids, local_count);
    check_parent_sync();

    /* Each node's lowest and highest rank: the leads there of the world again
     * and of the world reversed. */
    uint32_t lowest_of[MAX_PES], highest_of[MAX_PES];
    for (uint32_t n = 0; n < nodes; n++) {
        lowest_of[n] = UINT32_MAX;
        highest_of[n] = 0;
    }
    for (int r = 0; r < npes; r++) {
        if (lowest_of[node_of[r]] == UINT32_MAX)
            lowest_of[node_of[r]] = (uint32_t)r;
        highest_of[node_of[r]] = (uint32_t)r;
    }

    /* Everything has run a collective, and then the finalize team; the node's
     * lead reads its segment. */
    shmem_finalize();
    shmem_barrier_all();
    if (osoe.bootstrap.local_id == 0)
        check_stat(node_of, nodes, lead_of, lowest_of, highest_of);
    shmem_barrier_all();
    check_shared_number(node_of, nodes);

    /* A destroyed team's record goes with its block (step D): team 4 is gone
     * from the dump once every member here has left it, and team 3 is not. */
    shmem_team_destroy(reverse_team);
    shmem_barrier_all();
    if (osoe.bootstrap.local_id == 0) {
        static char out[256 * 1024];
        CHECK(run_stat("-d", out, sizeof out) == 0);
        CHECK(says(out, "team 3 (number 3)"));
        CHECK(!strstr(out, "team 4 (number 4)"));
        CHECK(!strstr(out, "team.4.record"));
    }
    shmem_barrier_all();

    shmem_finalize();
    return CHECK_DONE();
}
