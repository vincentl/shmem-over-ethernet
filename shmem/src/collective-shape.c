/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "collective-shape.h"

#include <stdbool.h>

#include "collective-circulant.h"
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
    /* No setting reaches these - the fan-out is 2 at least, the window 1, the
     * piece and the bandwidth not 0 - but a cost is no reason to divide by 0
     * (the adversarial read of Part 1): a shape that cannot run costs the
     * most there is. */
    if (fanout < 2 || window == 0 || piece_bytes == 0 || link->bytes_per_second == 0)
        return UINT64_MAX;
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

/* a + b, or UINT64_MAX where it overflows */
static uint64_t add_saturating(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

/* a b, or UINT64_MAX where it overflows */
static uint64_t multiply_saturating(uint64_t a, uint64_t b)
{
    return b != 0 && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

uint64_t osoe_shape_circulant_cost(uint32_t nodes, uint32_t window, uint64_t bytes,
                                   uint64_t piece_bytes, const struct osoe_link *link)
{
    if (nodes <= 1 || bytes == 0)
        return 0;
    if (window == 0 || piece_bytes == 0 || link->bytes_per_second == 0)
        return UINT64_MAX;
    uint64_t h = link->hop_ns, o = link->message_ns, rate = link->bytes_per_second;
    uint32_t rows = osoe_circulant_rows(nodes);
    /* the grants: one to each reduce-scatter sender, then a hop; and between
     * the halves the last allgather grant's message and hop */
    uint64_t total = add_saturating(multiply_saturating(rows, o), 2 * h + o);
    /* the reduce-scatter's rounds, then the allgather's, the same bytes in
     * reverse: (s' - s) blocks, at most ceil((s' - s) bytes / nodes) */
    uint32_t outer = nodes, inner = nodes - nodes / 2;
    for (uint32_t k = 0; k < rows; k++) {
        uint64_t blocks = outer - inner;
        uint64_t m = bytes / nodes * blocks + ((bytes % nodes) * blocks + nodes - 1) / nodes;
        uint64_t pieces = m / piece_bytes + (m % piece_bytes != 0);
        uint64_t one = m < piece_bytes ? m : piece_bytes;
        uint64_t t = (one * 1000000000u + rate - 1) / rate;
        uint64_t wire = m > UINT64_MAX / 1000000000u
                            ? UINT64_MAX
                            : (m * 1000000000u + rate - 1) / rate;
        uint64_t credited = (t + 2 * h + o + window - 1) / window;
        uint64_t paced = add_saturating(t, multiply_saturating(pieces - 1, 2 * o));
        uint64_t flowed = add_saturating(
            t, multiply_saturating(pieces > window ? pieces - window : 0, credited));
        uint64_t round = wire;
        if (paced > round)
            round = paced;
        if (flowed > round)
            round = flowed;
        total = add_saturating(total, multiply_saturating(2, add_saturating(h + o, round)));
        outer = inner;
        inner = inner - inner / 2;
    }
    return total;
}

struct osoe_collective_shape osoe_shape_choose(enum osoe_shape_operation operation,
                                               uint32_t nodes, uint64_t bytes,
                                               const struct osoe_link *link, uint32_t ceiling,
                                               uint32_t lane_rows, uint32_t capacity,
                                               uint32_t small_bytes, uint64_t piece_bytes,
                                               bool circulant)
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
    uint32_t rows = osoe_circulant_rows(nodes);
    if (circulant && operation == OSOE_SHAPE_REDUCE && rows <= lane_rows) {
        /* Träff's round takes as many of a lane's rows as its rounds leave it,
         * the capacity's slots each, and keeps them all in flight. */
        uint32_t window = capacity * (lane_rows / rows);
        if (osoe_shape_circulant_cost(nodes, window, bytes, piece_bytes, link) < least)
            shape = (struct osoe_collective_shape){ OSOE_SHAPE_CIRCULANT, ceiling, window };
    }
    return shape;
}

/* ---- collect's places (P31; collective-shape.h) -------------------------- */

/* The scan's widest message, its down - the totals before and the total - and
 * the exchange's slot, the epoch and the total. */
enum { PLACES_DOWN_BYTES = 16, EXCHANGE_SLOT_BYTES = 16 };

/* ceil(log2 nodes), the allgather's steps. */
static uint32_t gather_steps(uint32_t nodes)
{
    uint32_t steps = 0;
    while ((UINT64_C(1) << steps) < nodes)
        steps++;
    return steps;
}

/* A reduction of `bytes` among `nodes` leads as the data path runs it
 * (osoe_places_cost's comment): shape_timing.py's reduction_cost(). */
static uint64_t reduction_cost(uint32_t nodes, uint64_t bytes, const struct osoe_link *link,
                               uint32_t ceiling, uint32_t lane_rows, uint32_t capacity,
                               uint32_t small_bytes, uint64_t piece_bytes, bool circulant)
{
    if (nodes <= 1 || bytes == 0)
        return 0;
    if (bytes <= small_bytes)
        return osoe_shape_cost(OSOE_SHAPE_REDUCE, nodes, ceiling, 1, bytes, bytes, link);
    struct osoe_collective_shape s =
        osoe_shape_choose(OSOE_SHAPE_REDUCE, nodes, bytes, link, ceiling, lane_rows, capacity,
                          small_bytes, piece_bytes, circulant);
    uint64_t c = s.algorithm == OSOE_SHAPE_CIRCULANT
                     ? osoe_shape_circulant_cost(nodes, s.window, bytes, piece_bytes, link)
                     : osoe_shape_cost(OSOE_SHAPE_REDUCE, nodes, s.fanout, s.window, bytes,
                                       piece_bytes, link);
    return add_saturating(c, link->hop_ns);
}

uint64_t osoe_places_cost(enum osoe_places_way way, uint32_t nodes, uint32_t members,
                          const struct osoe_link *link, uint32_t ceiling, uint32_t lane_rows,
                          uint32_t capacity, uint32_t small_bytes, uint64_t piece_bytes,
                          bool circulant)
{
    if (nodes <= 1)
        return 0;
    if (link->bytes_per_second == 0)
        return UINT64_MAX;
    switch (way) {
    case OSOE_PLACES_MEMBERS:
        return reduction_cost(nodes, (uint64_t)8 * members, link, ceiling, lane_rows, capacity,
                              small_bytes, piece_bytes, circulant);
    case OSOE_PLACES_TOTALS:
        return reduction_cost(nodes, (uint64_t)8 * nodes, link, ceiling, lane_rows, capacity,
                              small_bytes, piece_bytes, circulant);
    case OSOE_PLACES_SCAN: {
        uint64_t tree = osoe_shape_cost(OSOE_SHAPE_REDUCE, nodes, ceiling, 1, PLACES_DOWN_BYTES,
                                        PLACES_DOWN_BYTES, link);
        return add_saturating(tree,
                              multiply_saturating(gather_steps(nodes) + 1, link->message_ns));
    }
    case OSOE_PLACES_EXCHANGE: {
        uint64_t t = ((uint64_t)EXCHANGE_SLOT_BYTES * 1000000000u + link->bytes_per_second - 1) /
                     link->bytes_per_second;
        return add_saturating(link->hop_ns,
                              multiply_saturating(nodes - 1, add_saturating(link->message_ns, t)));
    }
    }
    return UINT64_MAX;
}

enum osoe_places_way osoe_places_choose(uint32_t nodes, uint32_t members, bool runs,
                                        bool ordered, uint32_t exchange_nodes,
                                        const struct osoe_link *link, uint32_t ceiling,
                                        uint32_t lane_rows, uint32_t capacity,
                                        uint32_t small_bytes, uint64_t piece_bytes,
                                        bool circulant)
{
    if (nodes <= 1 || !runs)
        return OSOE_PLACES_MEMBERS;
    enum osoe_places_way best = OSOE_PLACES_TOTALS;
    uint64_t least = osoe_places_cost(OSOE_PLACES_TOTALS, nodes, members, link, ceiling,
                                      lane_rows, capacity, small_bytes, piece_bytes, circulant);
    const enum osoe_places_way others[2] = { OSOE_PLACES_EXCHANGE, OSOE_PLACES_SCAN };
    const bool allowed[2] = {
        nodes <= exchange_nodes && nodes <= OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST, ordered
    };
    for (unsigned i = 0; i < 2; i++) {
        if (!allowed[i])
            continue;
        uint64_t c = osoe_places_cost(others[i], nodes, members, link, ceiling, lane_rows,
                                      capacity, small_bytes, piece_bytes, circulant);
        if (c < least) {
            best = others[i];
            least = c;
        }
    }
    return best;
}

