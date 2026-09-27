/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The compressed rank map (§6.2, rank-map.h): a node's ranks as progressions,
 * the job's runs built from every node's, lookups against the uncompressed map
 * they replace, the refusals the rendezvous relies on, and the sizes at
 * 4,096 x 256 that are the point of it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "rank-map.h"

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* Build the runs of a map given as node_of[rank] (local ids in rank order, as
 * Slurm's are), check every rank's lookup against it, and return the run count. */
static uint32_t check_map(const uint32_t *node_of, uint32_t world, uint32_t nodes)
{
    /* Every node's gtids, in one pass over the ranks: bucketed by node, each
     * bucket in rank order, which is local-id order (Slurm's). */
    uint32_t *gtids = malloc(sizeof *gtids * world);
    uint32_t *start = calloc(nodes + 1, sizeof *start), *fill = calloc(nodes, sizeof *fill);
    for (uint32_t r = 0; r < world; r++)
        start[node_of[r] + 1]++;
    for (uint32_t n = 0; n < nodes; n++)
        start[n + 1] += start[n];
    for (uint32_t r = 0; r < world; r++)
        gtids[start[node_of[r]] + fill[node_of[r]]++] = r;
    struct osoe_rank_progression *prog = malloc(sizeof *prog * world);
    struct osoe_rank_report *reports = calloc(nodes, sizeof *reports);
    uint32_t used = 0;
    for (uint32_t n = 0; n < nodes; n++) {
        reports[n].node = n;
        reports[n].progressions = prog + used;
        reports[n].progression_count =
            osoe_rank_progressions(gtids + start[n], start[n + 1] - start[n], prog + used);
        used += reports[n].progression_count;
    }
    free(start);
    free(fill);
    uint32_t runs_count = 0;
    char why[128];
    struct osoe_rank_run *runs = osoe_rank_runs_build(reports, nodes, world, &runs_count, why,
                                                      sizeof why);
    CHECK(runs != NULL);
    if (!runs) {
        fprintf(stderr, "build refused: %s\n", why);
        return 0;
    }
    uint32_t *next_local = calloc(nodes, sizeof *next_local);
    for (uint32_t r = 0; r < world; r++) {
        const struct osoe_rank_run *run = osoe_rank_run_find(runs, runs_count, r);
        CHECK(run != NULL);
        if (!run)
            break;
        uint32_t k = r - run->first_rank;
        uint32_t node = run->first_node + (uint32_t)((int64_t)k * run->node_step);
        uint32_t local = run->first_local + (uint32_t)((int64_t)k * run->local_step);
        CHECK(node == node_of[r]);
        CHECK(local == next_local[node_of[r]]++);
    }
    CHECK(osoe_rank_run_find(runs, runs_count, world) == NULL);
    free(next_local);
    free(runs);
    free(reports);
    free(prog);
    free(gtids);
    return runs_count;
}

static void check_set(const uint32_t *gtids, uint32_t count, uint32_t world, uint32_t stride)
{
    struct osoe_rank_set s;
    osoe_rank_set_build(&s, gtids, count);
    CHECK(s.stride == stride);
    for (uint32_t r = 0; r < world; r++) {
        int32_t want = -1;
        for (uint32_t i = 0; i < count; i++)
            if (gtids[i] == r)
                want = (int32_t)i;
        CHECK(osoe_rank_set_index(&s, r) == want);
    }
    osoe_rank_set_free(&s);
}

int main(void)
{
    /* Progressions: block, cyclic, a mix, a single rank. */
    struct osoe_rank_progression p[8];
    const uint32_t block[] = { 4, 5, 6, 7 }, cyclic[] = { 1, 4, 7 }, mixed[] = { 0, 3, 5 };
    CHECK(osoe_rank_progressions(block, 4, p) == 1 && p[0].first == 4 && p[0].stride == 1 &&
          p[0].count == 4);
    CHECK(osoe_rank_progressions(cyclic, 3, p) == 1 && p[0].stride == 3 && p[0].count == 3);
    CHECK(osoe_rank_progressions(mixed, 3, p) == 2 && p[0].count == 2 && p[1].first == 5);
    const uint32_t single[] = { 9 };
    CHECK(osoe_rank_progressions(single, 1, p) == 1 && p[0].first == 9 && p[0].count == 1);

    /* §8.1's four poles, and each lookup against the map itself. */
    const uint32_t one_node[] = { 0, 0, 0, 0 }, one_each[] = { 0, 1, 2, 3 },
                   uneven[] = { 0, 0, 0, 1, 2, 2 }, interleaved[] = { 0, 1, 2, 0, 1, 0 };
    CHECK(check_map(one_node, 4, 1) == 1);
    CHECK(check_map(one_each, 4, 4) == 1);
    CHECK(check_map(uneven, 6, 3) <= 3);
    CHECK(check_map(interleaved, 6, 3) >= 1);

    /* The sizes that are the point (§6.2): 4,096 nodes x 256, block and cyclic. */
    enum { NODES = 4096, PER = 256 };
    uint32_t *big = malloc(sizeof *big * NODES * PER);
    for (uint32_t r = 0; r < NODES * PER; r++)
        big[r] = r / PER;
    CHECK(check_map(big, NODES * PER, NODES) == NODES);       /* one run per node */
    for (uint32_t r = 0; r < NODES * PER; r++)
        big[r] = r % NODES;
    CHECK(check_map(big, NODES * PER, NODES) == PER);         /* one run per round */
    free(big);

    /* The refusals the rendezvous names a node by. */
    struct osoe_rank_progression a = { 0, 1, 2 }, b = { 1, 1, 2 }, c = { 0, 1, 1 };
    struct osoe_rank_report twice[] = { { 0, &a, 1 }, { 1, &b, 1 } };
    uint32_t n;
    char why[128];
    CHECK(osoe_rank_runs_build(twice, 2, 3, &n, why, sizeof why) == NULL &&
          strstr(why, "claimed by node 0 and by node 1"));
    struct osoe_rank_report gap[] = { { 0, &c, 1 } };
    CHECK(osoe_rank_runs_build(gap, 1, 2, &n, why, sizeof why) == NULL &&
          strstr(why, "not claimed"));
    struct osoe_rank_report beyond[] = { { 0, &b, 1 } };
    CHECK(osoe_rank_runs_build(beyond, 1, 2, &n, why, sizeof why) == NULL &&
          strstr(why, "claims rank 2"));

    /* This node's ranks as a set: one progression (O(1)) or sorted (search). */
    check_set(block, 4, 10, 1);
    check_set(cyclic, 3, 10, 3);
    check_set(mixed, 3, 10, 0);
    check_set(single, 1, 12, 1);
    return CHECK_DONE();
}
