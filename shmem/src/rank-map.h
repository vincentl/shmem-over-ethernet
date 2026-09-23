/*
 * Which node a rank is on, and its local id there, in space that follows the
 * shape of the job rather than its size (§6.2; phase 1 step 3).
 *
 * Until step 3 the map was one entry per rank, 6 bytes each, in a job-wide
 * segment every PE mapped - 6 MiB per node at 10^6 PEs, and a broadcast of
 * O(N·P) bytes out of node 0 had it been sent over a network (second review,
 * algorithmic-scaling 9). §6.2 named the next step: under block and cyclic
 * distributions the map is O(N) runs, not O(P) entries.
 *
 * - **A node's own ranks are a few arithmetic progressions** in local-id order:
 *   one under a block distribution (stride 1) and one under cyclic (stride N),
 *   more only under plane or arbitrary ones. That is what a lead reports at the
 *   rendezvous, O(progressions) rather than O(L).
 * - **The job's map is runs over rank space**, each a stretch of consecutive
 *   ranks whose node and local id advance by constant steps: one run per node
 *   under block (node step 0, local step 1), one per round under cyclic (node
 *   step 1, local step 0). The rendezvous's root builds them once from every
 *   node's progressions - expanding to O(P) scratch memory on that one node, to
 *   check that every rank is claimed exactly once - and every node keeps only
 *   the runs. A lookup is a binary search, O(log R), and only the network path
 *   makes one: whether a rank is on this node is the next item's question.
 * - **This node's own ranks as a set**, for the question every put asks first
 *   (osoe_routes_in_process): O(1) when they form one progression - a
 *   subtraction for block, a division for cyclic - and a binary search over the
 *   node's L ranks otherwise.
 */
#ifndef OSOE_RANK_MAP_H
#define OSOE_RANK_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* first, first + stride, ... : `count` ranks with consecutive local ids. */
struct osoe_rank_progression {
    uint32_t first;
    uint32_t stride;            /* >= 1 */
    uint32_t count;
};

/*
 * A node's ranks (its gtids, in local-id order) as progressions, written to
 * `out`, which has room for `count` of them. Returns how many; greedy, so a
 * block or cyclic node is one.
 */
uint32_t osoe_rank_progressions(const uint32_t *gtids, uint32_t count,
                                struct osoe_rank_progression *out);

/* Ranks first_rank .. first_rank + count - 1: the k-th is on node
 * first_node + k * node_step, with local id first_local + k * local_step. */
struct osoe_rank_run {
    uint32_t first_rank;
    uint32_t count;
    uint32_t first_node;
    uint32_t first_local;
    int32_t node_step;
    int32_t local_step;
};

/* One node's report, as the rendezvous's root receives it. */
struct osoe_rank_report {
    uint32_t node;
    const struct osoe_rank_progression *progressions;
    uint32_t progression_count;
};

/*
 * The job's runs, from every node's report. Expands to world_size-sized scratch
 * to check the reports: every rank of 0 .. world_size - 1 claimed by exactly one
 * node, and a node's local ids 0 .. L - 1. Returns a malloc'd array of runs in
 * rank order and its length in *count, or NULL with the problem in `why`.
 */
struct osoe_rank_run *osoe_rank_runs_build(const struct osoe_rank_report *reports,
                                           uint32_t node_count, uint32_t world_size,
                                           uint32_t *count, char *why, size_t why_len);

/* The run holding `rank`, by binary search; NULL if none does. */
const struct osoe_rank_run *osoe_rank_run_find(const struct osoe_rank_run *runs,
                                               uint32_t count, uint32_t rank);

/* ---- One node's ranks, as a set ------------------------------------------ */

struct osoe_rank_set {
    uint32_t count;
    uint32_t first;
    uint32_t stride;            /* 0: not one progression, so search `sorted` */
    uint32_t *sorted_rank;      /* ascending, with each rank's local id beside it */
    uint32_t *sorted_local;
};

/* Build from a node's gtids (local-id order). Fatal on allocation failure. */
void osoe_rank_set_build(struct osoe_rank_set *s, const uint32_t *gtids, uint32_t count);
void osoe_rank_set_free(struct osoe_rank_set *s);

/* The non-progression case of osoe_rank_set_index. */
int32_t osoe_rank_set_search(const struct osoe_rank_set *s, uint32_t rank);

/*
 * The local id of `rank` if it is in the set, else -1. On the path of every put
 * and get (§8.1's one locality question), so the progression case is inline.
 */
static inline int32_t osoe_rank_set_index(const struct osoe_rank_set *s, uint32_t rank)
{
    uint32_t d = rank - s->first;           /* wraps for rank < first: never a hit */
    if (s->stride == 1)
        return d < s->count ? (int32_t)d : -1;
    if (s->stride != 0)
        return d % s->stride == 0 && d / s->stride < s->count ? (int32_t)(d / s->stride) : -1;
    return osoe_rank_set_search(s, rank);
}

#endif /* OSOE_RANK_MAP_H */
