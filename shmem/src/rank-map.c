#include <stdio.h>
#include <stdlib.h>

#include "fatal.h"
#include "rank-map.h"

uint32_t osoe_rank_progressions(const uint32_t *gtids, uint32_t count,
                                struct osoe_rank_progression *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ) {
        struct osoe_rank_progression p = { gtids[i], 1, 1 };
        if (i + 1 < count && gtids[i + 1] > gtids[i]) {
            p.stride = gtids[i + 1] - gtids[i];
            p.count = 2;
            while (i + p.count < count &&
                   (uint64_t)gtids[i + p.count] ==
                       (uint64_t)gtids[i] + (uint64_t)p.count * p.stride)
                p.count++;
        }
        out[n++] = p;
        i += p.count;
    }
    return n;
}

#define UNCLAIMED UINT32_MAX

struct osoe_rank_run *osoe_rank_runs_build(const struct osoe_rank_report *reports,
                                           uint32_t node_count, uint32_t world_size,
                                           uint32_t *count, char *why, size_t why_len)
{
    if (why_len)
        why[0] = '\0';
    /* The one O(P) step, on the root alone and once per job (rank-map.h). */
    uint32_t *node_of = malloc(sizeof *node_of * (world_size ? world_size : 1));
    uint32_t *local_of = malloc(sizeof *local_of * (world_size ? world_size : 1));
    struct osoe_rank_run *runs = NULL;
    if (!node_of || !local_of) {
        snprintf(why, why_len, "out of memory for a map of %u ranks", world_size);
        goto out;
    }
    for (uint32_t r = 0; r < world_size; r++)
        node_of[r] = UNCLAIMED;
    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t local = 0;
        for (uint32_t j = 0; j < reports[i].progression_count; j++) {
            const struct osoe_rank_progression *p = &reports[i].progressions[j];
            for (uint32_t k = 0; k < p->count; k++) {
                uint64_t r = (uint64_t)p->first + (uint64_t)k * p->stride;
                if (r >= world_size) {
                    snprintf(why, why_len, "node %u claims rank %llu in a job of %u",
                             reports[i].node, (unsigned long long)r, world_size);
                    goto out;
                }
                if (node_of[r] != UNCLAIMED) {
                    snprintf(why, why_len, "rank %llu is claimed by node %u and by node %u",
                             (unsigned long long)r, node_of[r], reports[i].node);
                    goto out;
                }
                node_of[r] = reports[i].node;
                local_of[r] = local++;
            }
        }
    }
    uint32_t n = 0;
    for (uint32_t r = 0; r < world_size; r++) {
        if (node_of[r] == UNCLAIMED) {
            snprintf(why, why_len, "rank %u was not claimed by any node; the launcher's rank "
                     "lists do not cover the job", r);
            goto out;
        }
    }
    /* Greedy maximal runs: at most world_size, and O(N) or O(L) for block and
     * cyclic. Counted first, then filled. */
    for (int pass = 0; pass < 2; pass++) {
        n = 0;
        for (uint32_t r = 0; r < world_size; ) {
            struct osoe_rank_run run = { r, 1, node_of[r], local_of[r], 0, 0 };
            if (r + 1 < world_size) {
                run.node_step = (int32_t)(node_of[r + 1] - node_of[r]);
                run.local_step = (int32_t)(local_of[r + 1] - local_of[r]);
                while (r + run.count < world_size &&
                       node_of[r + run.count] ==
                           run.first_node + (uint32_t)((int64_t)run.count * run.node_step) &&
                       local_of[r + run.count] ==
                           run.first_local + (uint32_t)((int64_t)run.count * run.local_step))
                    run.count++;
                if (run.count == 1)
                    run.node_step = run.local_step = 0;
            }
            if (pass == 1)
                runs[n] = run;
            n++;
            r += run.count;
        }
        if (pass == 0 && !(runs = malloc(sizeof *runs * (n ? n : 1)))) {
            snprintf(why, why_len, "out of memory for %u runs", n);
            goto out;
        }
    }
    *count = n;
out:
    free(node_of);
    free(local_of);
    if (why[0] != '\0') {
        free(runs);
        return NULL;
    }
    return runs;
}

const struct osoe_rank_run *osoe_rank_run_find(const struct osoe_rank_run *runs,
                                               uint32_t count, uint32_t rank)
{
    uint32_t lo = 0, hi = count;                /* the last run starting at or before rank */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (runs[mid].first_rank <= rank)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const struct osoe_rank_run *r = &runs[lo - 1];
    return rank - r->first_rank < r->count ? r : NULL;
}

/* ---- One node's ranks, as a set ------------------------------------------ */

static int compare_rank(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

void osoe_rank_set_build(struct osoe_rank_set *s, const uint32_t *gtids, uint32_t count)
{
    *s = (struct osoe_rank_set){ .count = count, .first = count ? gtids[0] : 0, .stride = 1 };
    if (count > 1)
        s->stride = gtids[1] > gtids[0] ? gtids[1] - gtids[0] : 0;
    for (uint32_t i = 1; i < count && s->stride != 0; i++)
        if ((uint64_t)gtids[i] != (uint64_t)s->first + (uint64_t)i * s->stride)
            s->stride = 0;
    if (s->stride != 0)
        return;
    /* Not one progression: the node's ranks sorted, each with its local id,
     * O(L log L) once and O(L) memory. */
    uint32_t (*pairs)[2] = malloc(sizeof *pairs * count);
    s->sorted_rank = malloc(sizeof *s->sorted_rank * count);
    s->sorted_local = malloc(sizeof *s->sorted_local * count);
    if (!pairs || !s->sorted_rank || !s->sorted_local)
        osoe_fatal("out of memory for a node of %u PEs", count);
    for (uint32_t i = 0; i < count; i++) {
        pairs[i][0] = gtids[i];
        pairs[i][1] = i;
    }
    qsort(pairs, count, sizeof *pairs, compare_rank);
    for (uint32_t i = 0; i < count; i++) {
        s->sorted_rank[i] = pairs[i][0];
        s->sorted_local[i] = pairs[i][1];
    }
    free(pairs);
}

void osoe_rank_set_free(struct osoe_rank_set *s)
{
    free(s->sorted_rank);
    free(s->sorted_local);
    *s = (struct osoe_rank_set){ 0 };
}

int32_t osoe_rank_set_search(const struct osoe_rank_set *s, uint32_t rank)
{
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->sorted_rank[mid] < rank)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < s->count && s->sorted_rank[lo] == rank ? (int32_t)s->sorted_local[lo] : -1;
}
