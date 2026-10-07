/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Träff's circulant reduce-scatter and allgather over a team's node leads (v0.2
 * step 3; J. L. Träff, "Optimal, Non-pipelined Reduce-scatter and Allreduce
 * Algorithms", arXiv 2410.14234, Algorithms 1 and 2): who sends what to whom
 * in each round, as functions of a lead's rank, the node count and the round
 * alone, so that no message negotiates it.
 *
 * Over N leads the vector is cut into N blocks, block j the elements
 * [j count / N, (j + 1) count / N), and lead r's partial of block (r + i) mod N
 * is its R[i]. The skips halve N, rounding up, to 1: s_0 = N > s_1 > ... >
 * s_q = 1, q = ceil(log2 N), and round k of a half uses the pair
 * (s', s) = (s_k, s_{k+1}).
 *   reduce-scatter  rounds 0 to q - 1: lead r sends R[s .. s' - 1] - blocks
 *                   (r + s .. r + s' - 1) mod N - to lead (r + s) mod N, and
 *                   receives as many from lead (r - s) mod N, combining them
 *                   into R[0 .. s' - s - 1]; after the last, R[0] is block r of
 *                   the result.
 *   allgather       rounds q to 2q - 1, the skips in reverse: lead r sends
 *                   R[0 .. s' - s - 1] to lead (r - s) mod N and receives
 *                   R[s .. s' - 1] from lead (r + s) mod N, copying them.
 * The blocks a lead sends are the blocks its receiver combines into or copies,
 * the same elements on both sides, so a round names element intervals once:
 * a run of blocks wraps past block N - 1 at most once, so two intervals at
 * most. Every lead sends and receives N - 1 blocks in each half, for every N:
 * no lead busier than another, and ceil(log2 N) rounds a half, the fewest
 * there can be. A round sends to one lead and receives from another - the same
 * one only where 2s = N - so no round is a swap, and its flow is credited
 * (the design's flow within a step, the author's 2026-10-06 item 7).
 *
 * The rows: round k of the reduce-scatter and the allgather's round with the
 * same skip use row k of the receiver's lane, so a reduction takes q rows.
 *
 * Pure: no global, integers alone. tools/modelcheck/circulant_model.py's
 * round_intervals() is the same arithmetic line for line, checked there against
 * every element of every lead's result being combined from every node exactly
 * once, for N from 2 to 70; shmem.circulant.arithmetic holds the C to it.
 */
#ifndef OSOE_COLLECTIVE_CIRCULANT_H
#define OSOE_COLLECTIVE_CIRCULANT_H

#include <stdbool.h>
#include <stdint.h>

/* One lead's part in one round. An interval with first == end is empty. */
struct osoe_circulant_round {
    uint32_t to;                /* the lead it sends to */
    uint32_t from;              /* the lead it receives from */
    uint32_t row;               /* the row of the receiver's lane its pieces take: the
                                 * index of the round's skip, 0 to q - 1 */
    bool combines;              /* combines what it receives (the reduce-scatter) rather
                                 * than copying it (the allgather) */
    uint64_t send_first[2], send_end[2];
    uint64_t receive_first[2], receive_end[2];
};

/* The rows a reduction over `nodes` leads takes, ceil(log2 nodes): one a
 * reduce-scatter round. 0 for one lead. */
uint32_t osoe_circulant_rows(uint32_t nodes);

/* Its rounds, twice its rows. */
uint32_t osoe_circulant_rounds(uint32_t nodes);

/* Lead `rank`'s part in round `round` (0 to osoe_circulant_rounds(nodes) - 1)
 * of a reduction of `count` elements over `nodes` leads (2 or more). */
struct osoe_circulant_round osoe_circulant_round(uint32_t rank, uint32_t nodes, uint32_t round,
                                                 uint64_t count);

#endif /* OSOE_COLLECTIVE_CIRCULANT_H */
