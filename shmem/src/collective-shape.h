/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The shape a large reduction's or broadcast's internode tree takes for one
 * call (v0.2 step 2; design §5.3, §5.8): a fan-out k and a window W, chosen by
 * one rule from three numbers - the link - that every node holds alike, so
 * that every node computes the same shape for the same call and no message
 * negotiates it.
 *
 * Pure: nothing here reads `osoe` or any other global, so a test calls it with
 * any inputs; and integer arithmetic alone, in nanoseconds and bytes, so two
 * nodes never round apart. tools/modelcheck/shape_timing.py holds the same
 * arithmetic line for line (cost(), tree_terms(), choose()), the simulation of
 * the pipelined loop it stands for, and the fit of that simulation to the Pi
 * cluster's measurements; shmem.shape.grid holds the two to the same answers.
 *
 * A shape is a tree of fan-out k with a window of W pieces, or - since v0.2
 * step 3, for a reduction - Träff's circulant reduce-scatter and allgather
 * (collective-circulant.h), with a cost of its own under the same three numbers,
 * which the chooser weighs against the trees by the same rule: hence the
 * algorithm in the shape.
 */
#ifndef OSOE_COLLECTIVE_SHAPE_H
#define OSOE_COLLECTIVE_SHAPE_H

#include <stdbool.h>
#include <stdint.h>

enum osoe_shape_operation {
    OSOE_SHAPE_REDUCE,          /* partials up, the result down */
    OSOE_SHAPE_BROADCAST,       /* empty credits up, the data down */
};

enum osoe_shape_algorithm {
    OSOE_SHAPE_TREE,            /* the k-ary tree of collective-tree.c, pipelined */
    OSOE_SHAPE_CIRCULANT,       /* a reduction's reduce-scatter and allgather over the
                                 * leads, Träff's (collective-circulant.h; v0.2 step 3) */
};

/*
 * The link: what one lead's connection to another costs, measured by the root
 * at start-up against its first child, or set by the OSOE_LINK_* settings.
 */
struct osoe_link {
    uint64_t hop_ns;            /* h: a small message's one-way time, lead to lead,
                                 * through the sender's progress thread and the
                                 * receiver's */
    uint64_t message_ns;        /* o: what a lead's loop spends on each message it
                                 * sends, before the next */
    uint64_t bytes_per_second;  /* B: what a lead's link carries each way; never 0 */
};

/* Where a job's link in force came from (link-measure.h): none - a job of one
 * node, or the shape `fixed`, where nothing chooses - the OSOE_LINK_* settings,
 * or the root's measurement at the end of shmem_init. */
enum osoe_link_source {
    OSOE_LINK_NONE,
    OSOE_LINK_SET,
    OSOE_LINK_MEASURED,
};

struct osoe_collective_shape {
    enum osoe_shape_algorithm algorithm;
    uint32_t fanout;            /* k: 2 to the ceiling; the ceiling for Träff's */
    uint32_t window;            /* W: pieces in flight, 1 to the capacity - or for
                                 * Träff's, the capacity times the rows of a lane
                                 * each of its rounds takes */
};

/*
 * What a tree of fan-out `fanout` over `nodes` leads costs one collective of
 * `bytes` in pieces of `piece_bytes` with `window` pieces in flight, every
 * lead entering at once: the first piece's way through the tree - up, then
 * down, for a reduction; credits up, then the data down, for a broadcast -
 * then every later piece at the pace of the busiest link or of the window's
 * loop, whichever is slower. Nanoseconds, UINT64_MAX where the sum overflows;
 * 0 for one node or no bytes. A few dozen integer operations: the tree's
 * critical paths come from its shape in closed form, not from walking it.
 */
uint64_t osoe_shape_cost(enum osoe_shape_operation operation, uint32_t nodes, uint32_t fanout,
                         uint32_t window, uint64_t bytes, uint64_t piece_bytes,
                         const struct osoe_link *link);

/*
 * What Träff's reduction (collective-circulant.h) costs over `nodes` leads with
 * `window` pieces in flight a round: every lead grants its reduce-scatter's
 * senders as it enters and starts once it holds all of its own grants - a
 * message cost each, then a hop; the rounds one after another, each a hop and a
 * message cost, then its pieces at the pace of the link, of the lead's own
 * messages - a piece and a credit each - or of its credits, the first `window`
 * at once and each later one waiting for a piece, two hops and a message; and
 * between the halves the last allgather grant, a message and a hop. A round's
 * bytes are its blocks' at most, ceil((s' - s) bytes / nodes). Nanoseconds,
 * UINT64_MAX where the sum overflows; 0 for one node or no bytes.
 */
uint64_t osoe_shape_circulant_cost(uint32_t nodes, uint32_t window, uint64_t bytes,
                                   uint64_t piece_bytes, const struct osoe_link *link);

/*
 * The shape of one call. A team of one node, and a collective of
 * `small_bytes` or fewer - the small class, one piece in the team's small
 * slots - keep the ceiling's tree as before: fan-out `ceiling`, window 1.
 * Otherwise the window is `capacity` - more pieces in flight never cost a
 * large collective time, and the closed form's window term is its least exact
 * - and the fan-out the least cost from 2 to `ceiling`, ties to the smaller;
 * and where `circulant` allows it - the caller's word that it runs Träff's
 * reduction - a reduction whose rounds' rows, ceil(log2 nodes), fit a lane's
 * `ceiling` + 1 rows takes Träff's where it costs less than that tree, ties to
 * the tree. Its window is the slots a round can have: each round takes
 * (`ceiling` + 1) / ceil(log2 nodes) of the lane's rows of `capacity` slots -
 * 16 pieces on three or four nodes at the defaults, 12 on five to eight, 8 on
 * nine to sixteen, and the capacity from 17 on.
 * tools/modelcheck/shape_timing.py's choose_shape(), line for line.
 */
struct osoe_collective_shape osoe_shape_choose(enum osoe_shape_operation operation,
                                               uint32_t nodes, uint64_t bytes,
                                               const struct osoe_link *link, uint32_t ceiling,
                                               uint32_t capacity, uint32_t small_bytes,
                                               uint64_t piece_bytes, bool circulant);

#endif /* OSOE_COLLECTIVE_SHAPE_H */
