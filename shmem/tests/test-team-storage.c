/*
 * Where a team's state lives, and how its shape is found (phase 1 step 7
 * slice 1): the descriptor's local list, the world mapping, and osoe-stat's
 * walk of the team records.
 *
 * **The local list is in team-rank order, and a local id is not a team rank.**
 * The collectives find a member's memory by its local id and its place in a
 * result by its team rank, and the two orders agree only when a node's ranks
 * are the team's first. On 0,1,2,0,1,0 node 0 holds world ranks 0, 3 and 5 at
 * local ids 0, 1 and 2, so a descriptor that took one for the other would put
 * rank 3's block at place 1; and with the local ids given by hand against the
 * ranks - rank 0 at local id 1, rank 1 at local id 0 - the world's local lead,
 * its member with the lowest rank, is not the node's local id 0 at all. Each PE
 * checks its own node's list against what the map and the launcher's
 * OSOE_GTIDS - the local ids' ranks, in local-id order - imply, and then runs a
 * reduction, a gather and a broadcast on the team, whose results depend on the
 * list being right.
 *
 * **SHMEM_TEAM_SHARED's ranks are its local ids**, and its world mapping is a
 * start and a stride when the node's ranks are one progression and a table
 * when they interleave: shmem_team_translate_pe is checked both ways for every
 * rank. Under OSOE_FORCE_REMOTE it is the PE alone, whose local list is its
 * own local id - not 0 on a node's second PE.
 *
 * **osoe-stat names the teams by their records** (layout 18): each node's
 * lead runs it on its node's control segment while the job is live, and the
 * dump must name the world and finalize trees by the names they have had
 * since layout 12 and show each team whose record is published; on a job of
 * several nodes, --summary must give the world team's line.
 *
 * White-box: it reads the descriptor, so the library's sources are compiled
 * into it (CMakeLists.txt), as test-internal-region.c's are.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "node.h"
#include "state.h"
#include "team.h"

enum { MAX_PES = 64 };

static int me, npes;

/* The logical node map, one node per rank, from OSOE_TEST_MAP ("0,1,2,0,1,0"),
 * or every rank on node 0 when it is unset. */
static void read_map(uint32_t *node_of)
{
    const char *map = getenv("OSOE_TEST_MAP");
    for (int r = 0; r < npes; r++)
        node_of[r] = 0;
    for (int r = 0; map && *map && r < npes; r++) {
        node_of[r] = (uint32_t)strtoul(map, NULL, 10);
        map = strchr(map, ',');
        if (map)
            map++;
    }
}

/* The launcher's ranks for this node in local-id order (OSOE_GTIDS). */
static uint32_t read_gtids(uint32_t *gtids)
{
    const char *list = getenv("OSOE_GTIDS");
    uint32_t count = 0;
    while (list && *list && count < MAX_PES) {
        gtids[count++] = (uint32_t)strtoul(list, NULL, 10);
        list = strchr(list, ',');
        if (list)
            list++;
    }
    return count;
}

/* The descriptor's local list for a team every local PE belongs to, against
 * the map and the launcher's local ids: ascending ranks, exactly this node's,
 * each at the local id the launcher gave it, the lead first. */
static void check_every_local_pe(const struct osoe_team *t, const uint32_t *node_of,
                                 const uint32_t *gtids, uint32_t local_count)
{
    uint32_t here = node_of[me], expected = 0;
    for (int r = 0; r < npes; r++)
        if (node_of[r] == here)
            expected++;
    CHECK(t->local_members == expected);
    CHECK(t->local_members == local_count);
    uint32_t next = 0;
    for (uint32_t m = 0; m < t->local_members && m < MAX_PES; m++) {
        uint32_t rank = t->local_ranks[m], local = t->local_ids[m];
        /* The m-th lowest of this node's ranks, per the map. */
        while (next < (uint32_t)npes && node_of[next] != here)
            next++;
        CHECK(rank == next);
        next++;
        CHECK(local < local_count && gtids[local] == rank);
    }
    CHECK(t->lead_local_id == t->local_ids[0]);
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

static void check_world(const uint32_t *node_of, const uint32_t *gtids, uint32_t local_count)
{
    const struct osoe_team *world = osoe_team_of("test-team-storage", SHMEM_TEAM_WORLD);
    CHECK_FATAL(world != NULL);
    CHECK(world->number == OSOE_TEAM_WORLD);
    CHECK(world->members == (uint32_t)npes);
    CHECK(world->me == (uint32_t)me);
    check_every_local_pe(world, node_of, gtids, local_count);
    for (int r = 0; r < npes; r++)
        CHECK(osoe_team_to_world(world, r) == r);
    uint32_t nodes = 0;
    for (int r = 0; r < npes; r++)
        if (node_of[r] + 1 > nodes)
            nodes = node_of[r] + 1;
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

static void check_stat(bool several_nodes)
{
    static char out[256 * 1024];
    CHECK(run_stat("-d", out, sizeof out) == 0);
    CHECK(says(out, "team.world.tree.epoch_open"));
    CHECK(says(out, "team.finalize.tree.epoch_open"));
    CHECK(says(out, "team world (number 0)"));
    CHECK(says(out, "team.world.record.published"));
    CHECK(says(out, "team finalize (number 2)"));
    if (!osoe.job.force_remote)
        CHECK(says(out, "team shared (number 1)"));
    /* Every PE's epochs, since layout 18 in a row of their own. */
    CHECK(says(out, "pe.world_epoch"));

    CHECK(run_stat("--summary", out, sizeof out) == 0);
    char line[64];
    snprintf(line, sizeof line, "node %-3u", osoe.job.node);
    CHECK(says(out, line));
    if (several_nodes)
        CHECK(says(out, " world "));
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    CHECK_FATAL(npes <= MAX_PES);

    uint32_t node_of[MAX_PES], gtids[MAX_PES];
    read_map(node_of);
    uint32_t local_count = read_gtids(gtids);
    CHECK_FATAL(local_count == osoe.bootstrap.local_count);

    check_world(node_of, gtids, local_count);
    check_shared(gtids, local_count);

    /* Everything has run a collective; the node's lead reads its segment. */
    shmem_barrier_all();
    if (osoe.bootstrap.local_id == 0)
        check_stat(osoe.job.node_count > 1);
    shmem_barrier_all();

    shmem_finalize();
    return CHECK_DONE();
}
