/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "collective-circulant.h"

uint32_t osoe_circulant_rows(uint32_t nodes)
{
    uint32_t rows = 0;
    for (uint32_t s = nodes; s > 1; s = s - s / 2)
        rows++;
    return rows;
}

uint32_t osoe_circulant_rounds(uint32_t nodes)
{
    return 2 * osoe_circulant_rows(nodes);
}

/* Where block j of `nodes` starts in `count` elements: j count / nodes, as
 * j (count / nodes) + j (count % nodes) / nodes, which cannot overflow where
 * the product could. */
static uint64_t block_start(uint32_t nodes, uint64_t count, uint32_t j)
{
    return (uint64_t)j * (count / nodes) + (uint64_t)j * (count % nodes) / nodes;
}

/* Blocks (rank + a .. rank + b - 1) mod nodes as element intervals: one, or
 * two where the run wraps past the last block. */
static void blocks(uint32_t nodes, uint64_t count, uint32_t rank, uint32_t a, uint32_t b,
                   uint64_t first[2], uint64_t end[2])
{
    first[1] = end[1] = 0;
    uint32_t start = (uint32_t)(((uint64_t)rank + a) % nodes), run = b - a;
    if (start + run <= nodes) {
        first[0] = block_start(nodes, count, start);
        end[0] = block_start(nodes, count, start + run);
    } else {
        first[0] = block_start(nodes, count, start);
        end[0] = count;
        first[1] = 0;
        end[1] = block_start(nodes, count, start + run - nodes);
    }
}

struct osoe_circulant_round osoe_circulant_round(uint32_t rank, uint32_t nodes, uint32_t round,
                                                 uint64_t count)
{
    uint32_t rows = osoe_circulant_rows(nodes);
    uint32_t k = round < rows ? round : 2 * rows - 1 - round;
    /* the pair (s', s) of skip k */
    uint32_t outer = nodes, inner = nodes - nodes / 2;
    for (uint32_t i = 0; i < k; i++) {
        outer = inner;
        inner = inner - inner / 2;
    }
    struct osoe_circulant_round r;
    r.row = k;
    r.combines = round < rows;
    if (r.combines) {
        r.to = (uint32_t)(((uint64_t)rank + inner) % nodes);
        r.from = (uint32_t)(((uint64_t)rank + nodes - inner) % nodes);
        blocks(nodes, count, rank, inner, outer, r.send_first, r.send_end);
        blocks(nodes, count, rank, 0, outer - inner, r.receive_first, r.receive_end);
    } else {
        r.to = (uint32_t)(((uint64_t)rank + nodes - inner) % nodes);
        r.from = (uint32_t)(((uint64_t)rank + inner) % nodes);
        blocks(nodes, count, rank, 0, outer - inner, r.send_first, r.send_end);
        blocks(nodes, count, rank, inner, outer, r.receive_first, r.receive_end);
    }
    return r;
}
