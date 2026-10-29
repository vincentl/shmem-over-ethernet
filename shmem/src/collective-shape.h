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
 * algorithm in the shape. And, by the same rule from the same numbers, the way a
 * collect finds its places (P31; osoe_places_choose, at the end).
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
 * `lane_rows` takes Träff's where it costs less than that tree, ties to the
 * tree. `lane_rows` is the rows of a lane in the job, osoe_internal_lane_rows
 * of its fan-out ceiling and node count (internal-region.h; P3): the ceiling's
 * + 1, or the job's rounds where they are more, so that the fan-out ceiling no
 * longer decides how many nodes Träff's runs on. Its window is the slots a
 * round can have: each round takes `lane_rows` / ceil(log2 nodes) of the lane's
 * rows of `capacity` slots - at the defaults' lane of nine rows 32 pieces on
 * three or four nodes, 24 on five to eight, 16 on nine to sixteen, and the
 * capacity from 17 on, as on the node counts to 512 where a lane has no more.
 * tools/modelcheck/shape_timing.py's choose_shape(), line for line.
 */
struct osoe_collective_shape osoe_shape_choose(enum osoe_shape_operation operation,
                                               uint32_t nodes, uint64_t bytes,
                                               const struct osoe_link *link, uint32_t ceiling,
                                               uint32_t lane_rows, uint32_t capacity,
                                               uint32_t small_bytes, uint64_t piece_bytes,
                                               bool circulant);

/*
 * collect's places (P31 of docs/prompts/v0.2-open-items.md; collectives.c,
 * collect_varying): the four ways a collect of differing blocks on a team
 * spanning nodes finds each member's place, numbered as team.tree.collect_way
 * says them (node.h) - the members' vector, a reduction of 8 bytes a member;
 * each node's total (P29), a reduction of 8 bytes a node; the leads' scan of
 * the nodes' totals (P30), a small tree and a record more a step of the gather
 * after; the leads' exchange of the totals (P33), a hop and N - 1 messages.
 */
enum osoe_places_way {
    OSOE_PLACES_MEMBERS = 1,
    OSOE_PLACES_TOTALS = 2,
    OSOE_PLACES_SCAN = 3,
    OSOE_PLACES_EXCHANGE = 4,
};

/*
 * What finding a collect's places costs one way on `nodes` leads with
 * `members` members, nanoseconds, under the link: a reduction as the data path
 * runs it - the small class one piece through the ceiling's tree, a larger one
 * osoe_shape_choose's shape and a hop more for its grant, Träff's where
 * `circulant` allows it as for any reduction, in a lane of `lane_rows` rows;
 * the scan the small tree of its 16-byte down - a level-order tree's terms
 * standing in for its pre-order tree's, which is as deep - and a message cost a
 * step of the gather and one more, for its records and the share; the exchange
 * a hop, and for each of its N - 1 messages a message cost and a slot's bytes
 * on the sender's link. 0 on one node, UINT64_MAX where a sum overflows.
 * tools/modelcheck/shape_timing.py's places_cost(), line for line; provisional
 * until H24 and H25 measure the ways.
 */
uint64_t osoe_places_cost(enum osoe_places_way way, uint32_t nodes, uint32_t members,
                          const struct osoe_link *link, uint32_t ceiling, uint32_t lane_rows,
                          uint32_t capacity, uint32_t small_bytes, uint64_t piece_bytes,
                          bool circulant);

/*
 * The most nodes the default takes the leads' exchange on (P35 of
 * docs/prompts/v0.2-open-items.md, the author's decision of 2026-10-13), however
 * cheap the cost model finds it past them: H24 measured the exchange the
 * fastest of the four ways at 8 and 16 logical nodes and slower than the scan
 * at 32 (docs/design/evidence/v0.2-step4/2026-10-13-h24/), on one machine whose
 * CPUs carry every lead's N - 1 messages at once - which overstates its cost,
 * so a cluster can only move the crossover up. Past it the default takes each
 * node's total or the scan, as the link's cost chooses, where the model alone
 * took the exchange to 60 nodes on the Pi's link and 46 on a loopback's;
 * OSOE_COLLECTIVE_PLACES=exchange still forces it to its room
 * (internal-region.h, OSOE_INTERNAL_EXCHANGE_NODES_MOST). Provisional until H25
 * measures the ways on the cluster. shape_timing.py's
 * EXCHANGE_DEFAULT_NODES_MOST.
 */
enum { OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST = 16 };

/*
 * The way a collect's places take by default (OSOE_COLLECTIVE_PLACES=link):
 * the members' vector on one node, or where some node's members are not one
 * run of the team's ranks (`runs` false); otherwise the least cost of each
 * node's total, the exchange - on no more nodes than `exchange_nodes`, its
 * room, nor than OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST - and the scan - where
 * the runs meet the node set's positions in order or in reverse (`ordered`) -
 * ties to that order. shape_timing.py's choose_places(), line for line.
 */
enum osoe_places_way osoe_places_choose(uint32_t nodes, uint32_t members, bool runs,
                                        bool ordered, uint32_t exchange_nodes,
                                        const struct osoe_link *link, uint32_t ceiling,
                                        uint32_t lane_rows, uint32_t capacity,
                                        uint32_t small_bytes, uint64_t piece_bytes,
                                        bool circulant);

#endif /* OSOE_COLLECTIVE_SHAPE_H */
