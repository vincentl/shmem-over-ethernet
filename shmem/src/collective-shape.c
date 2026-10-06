/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "collective-shape.h"

#include <stdbool.h>

#include "collective-tree.h"

/*
 * A critical path through a tree, as three counts the link turns into time:
 * hops, piece times (a piece's bytes over B, on one link) and messages sent.
 * tools/modelcheck/shape_timing.py's triples.
 */
struct path {
    uint64_t hops, pieces, messages;
};

/*
 * What osoe_shape_cost needs of a tree (shape_timing.py's tree_terms()): the
 * first piece's slowest ways through it, the window's slowest loops, and beta,
 * the most messages with bytes any lead's link carries for one piece of the
 * vector. At most two ways each: see terms_of.
 */
struct terms {
    struct path first[2], loop[2];
    unsigned ways;
    uint64_t beta;
};

static uint64_t power(uint64_t base, uint32_t exponent)
{
    uint64_t p = 1;
    while (exponent--)
        p *= base;
    return p;
}

/* The largest base-k digit sum of a number from 0 to `limit`, of `digits`
 * digits: limit's own, or one of limit's digits less by one with every lower
 * digit k - 1. */
static uint64_t max_digit_sum(uint64_t limit, uint64_t k, uint32_t digits)
{
    uint64_t digit[64], total = 0;
    for (uint32_t i = 0; i < digits; i++) {
        digit[i] = limit % k;
        limit /= k;
        total += digit[i];
    }
    uint64_t best = total, above = total;
    for (uint32_t i = 0; i < digits; i++) {
        above -= digit[i];              /* the digits above i */
        if (digit[i] > 0) {
            uint64_t candidate = above + digit[i] - 1 + (k - 1) * i;
            if (candidate > best)
                best = candidate;
        }
    }
    return best;
}

/*
 * A level-order k-ary tree over N leads is full to its last level but one,
 * L - 1, and partial at L; a lead's positions among its siblings on the way
 * from the root are the base-k digits of its offset in its level. So:
 * - the slowest way down ends at the leaf of level L whose digits sum
 *   largest, or at the last lead of level L - 1 if it has no children, whose
 *   positions are all k - 1: each edge costs a hop and, behind the copies to
 *   the siblings before it, (position + 1) piece times and messages;
 * - the slowest way up, a reduction's, starts under the first parent of
 *   level L - 1, which has the most children, every lead above it having k:
 *   each level a hop, a message, and its children's pieces over one link in;
 * - a broadcast's credits climb a hop a level, empty, before its data comes
 *   down;
 * - the window's loop at a leaf is, each level, two hops, its piece up (a
 *   reduction's) and its copy down, behind the copies to the siblings before;
 * - beta is the root's children, a parent's below L - 1 (k and its parent),
 *   or the first parent's of level L - 1, whichever is most.
 * shape_timing.py checks every one against a walk of every node, for every
 * fan-out to 32.
 */
static void terms_of(enum osoe_shape_operation operation, uint32_t nodes, uint32_t fanout,
                     struct terms *t)
{
    uint64_t k = fanout;
    uint32_t L = osoe_collective_tree_depth(nodes, fanout);
    uint64_t full = (power(k, L) - 1) / (k - 1);        /* the leads of levels 0 .. L-1 */
    uint64_t last = nodes - full;                       /* the leads of level L */
    uint64_t parents = (last + k - 1) / k;              /* level L-1's leads with children */

    struct path down[2];
    unsigned ways = 0;
    uint64_t sum = L + max_digit_sum(last - 1, k, L);
    down[ways++] = (struct path){ L, sum, sum };
    if (parents < power(k, L - 1)) {
        uint64_t edge = (uint64_t)(L - 1) * k;
        down[ways++] = (struct path){ L - 1, edge, edge };
    }

    uint64_t widest_last = last < k ? last : k;         /* the first parent of level L-1 */
    uint64_t per_level = operation == OSOE_SHAPE_REDUCE ? 2 : 1;
    t->ways = ways;
    t->beta = nodes - 1 < k ? nodes - 1 : k;
    for (unsigned i = 0; i < ways; i++) {
        uint64_t a = down[i].hops, s = down[i].pieces;
        t->loop[i] = (struct path){ 2 * a, a * per_level + (s - a), a * per_level };
        if (operation == OSOE_SHAPE_REDUCE)
            t->first[i] = (struct path){ L + a, widest_last + (uint64_t)(L - 1) * k + s, L + s };
        else
            t->first[i] = (struct path){ L + a, s, s };
    }
    if (operation == OSOE_SHAPE_REDUCE) {
        if (L >= 3 && k + 1 > t->beta)
            t->beta = k + 1;
        if (L >= 2 && widest_last + 1 > t->beta)
            t->beta = widest_last + 1;
    }
}

static uint64_t path_time(const struct path *p, uint64_t hop, uint64_t piece, uint64_t message)
{
    return p->hops * hop + p->pieces * piece + p->messages * message;
}

uint64_t osoe_shape_cost(enum osoe_shape_operation operation, uint32_t nodes, uint32_t fanout,
                         uint32_t window, uint64_t bytes, uint64_t piece_bytes,
                         const struct osoe_link *link)
{
    if (nodes <= 1 || bytes == 0)
        return 0;
    struct terms t;
    terms_of(operation, nodes, fanout, &t);
    uint64_t pieces = bytes / piece_bytes + (bytes % piece_bytes != 0);
    uint64_t one = bytes < piece_bytes ? bytes : piece_bytes;
    uint64_t piece_ns = (one * 1000000000u + link->bytes_per_second - 1) / link->bytes_per_second;
    uint64_t fill = 0, loop = 0;
    for (unsigned i = 0; i < t.ways; i++) {
        uint64_t f = path_time(&t.first[i], link->hop_ns, piece_ns, link->message_ns);
        uint64_t l = path_time(&t.loop[i], link->hop_ns, piece_ns, link->message_ns);
        if (f > fill)
            fill = f;
        if (l > loop)
            loop = l;
    }
    uint64_t period = t.beta * piece_ns;
    uint64_t paced = (loop + window - 1) / window;
    if (paced > period)
        period = paced;
    if (pieces > 1 && period != 0 && pieces - 1 > (UINT64_MAX - fill) / period)
        return UINT64_MAX;
    return fill + (pieces - 1) * period;
}

struct osoe_collective_shape osoe_shape_choose(enum osoe_shape_operation operation,
                                               uint32_t nodes, uint64_t bytes,
                                               const struct osoe_link *link, uint32_t ceiling,
                                               uint32_t capacity, uint32_t small_bytes,
                                               uint64_t piece_bytes)
{
    struct osoe_collective_shape shape = { OSOE_SHAPE_TREE, ceiling, 1 };
    if (nodes <= 1 || bytes <= small_bytes)
        return shape;
    bool chosen = false;
    uint64_t least = 0;
    for (uint32_t k = 2; k <= ceiling; k++) {
        uint64_t c = osoe_shape_cost(operation, nodes, k, capacity, bytes, piece_bytes, link);
        if (!chosen || c < least) {
            chosen = true;
            least = c;
            shape.fanout = k;
        }
    }
    shape.window = capacity;
    return shape;
}
