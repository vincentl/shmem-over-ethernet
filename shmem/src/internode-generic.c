/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The generic internode phase of the data collectives (internode-generic.h;
 * §5.5, §5.8), moved behind internode.h from collectives.c, where phase 1 step
 * 5's first slice built it: a reduction and a broadcast through §5.3's tree of
 * node leads, and since phase 1 step 5 slice 6 a gather as an allgather among
 * the same leads (the last part of this comment).
 *
 * **Only the node leads take part**, over §5.3's k-ary tree of nodes, and they
 * meet over the transport rather than through AM messages the agents interpret:
 * a child puts its partial result into a slot of its parent's
 * OSOE_REGION_INTERNAL and adds one to the parent's arrival counter for its
 * position with a fenced rmw, so the add lands after the bytes (invariant 5);
 * the parent's application thread waits on that counter, combines each slot
 * into its buffer, and sends up in turn. The root holds the result, and it
 * comes down the same way: a put into each child's release slot and an add to
 * its release counter. The lead's thread is blocked in the collective
 * regardless and is the only writer of its buffer either way, so an AM design
 * would still have had to land each partial result where that thread could
 * read it and wake it; this does exactly that with operations every transport
 * has (§5.8). A put returns at local completion, so a result sent down is the
 * lead's to reuse at once, and §5.5's hazard of a forward that reads `dest` by
 * reference after the lead has left cannot arise.
 *
 * **A large collective goes through the tree in pieces of 16 KiB, and
 * each lead keeps D of them in flight** (§5.8, "Pipelined pieces"; phase 1
 * step 5 slice 5): D is the call's window, at most OSOE_COLLECTIVE_PIPELINE,
 * the capacity, which every node agrees on (v0.2 step 2: the chooser's, below,
 * and a scan's its own), and a lead's lane keeps capacity slots per child
 * position and capacity release slots, piece k in slot k mod the capacity
 * (internal-region.h; the lanes are below). A small
 * one - at most OSOE_SMALL_COLLECTIVE_BYTES - is one piece through its team
 * number's own small slots, and the loop is the same at one piece. The loop,
 * in_pieces, is
 * tools/modelcheck/pipeline_model.py's node loop, and it keeps the model's
 * rules - the first, third and fourth each with a breakage the model shows
 * failing, the second pinned by the tests, where an equality wait hangs:
 *   - **one arrival counter per child position**, not one per node: with
 *     pieces in flight a fast child's second arrival would stand in for a slow
 *     child's first, and the parent would read a slot not yet written;
 *   - **counters cumulative over the team's life, compared as at least**:
 *     within a collective a child runs up to D pieces ahead of its parent's
 *     wait, so §5.4's equality no longer holds, and the window is what bounds
 *     the skew;
 *   - **a node sends piece k up only once it holds piece k - D's release**,
 *     which is the parent's word that slot k mod D has been read;
 *   - **a node copies a release out and forwards it to its children before it
 *     counts it**, because the arrival that count lets through is what tells
 *     the parent the release slot is free: counted first, a node's next
 *     arrival lets its parent overwrite a release it has not copied.
 * And a fifth, which the model allows and does not require: **a lead takes
 * whichever of its inputs is ready**, its children's copies of the next piece
 * up or its parent's next release, and waits for either when neither is. A
 * lead that waited for its children alone, with a release waiting, would hold
 * back the release its children need to send what it waits for, and a tree
 * deeper than one level would run at about h / (h - 1) times one piece at a
 * time's speed, whatever D (in_pieces). D = 1 keeps one piece at a time, each a
 * whole pass up and down, as before the slice. Every lead publishes the most
 * pieces it had in flight at once in the team's tree state (node.h,
 * team.tree.most_pieces_in_flight): min(D, pieces) at a leaf, which fills its
 * window before it looks for a release, and a count a test pins, where a time
 * could only be compared; at most that at a lead with children, as the
 * schedule has it.
 *
 * **Each piece carries a header** - the team's epoch, the piece's number and
 * bytes, and which collective it is - which the lead receiving it checks
 * (§5.5's "asserted on receipt"). A reduction's pieces carry one up and every
 * piece carries one down; a broadcast's arrivals carry nothing, so a
 * reduction's lead that counted one as its child's finds out from the slot.
 * **It names a divergence where the pieces meet, and only there.** Nodes whose
 * PEs called collectives whose trees share no edge - a reduction against a
 * broadcast rooted elsewhere, or a data collective against a barrier - wait in
 * different places: a hang, whose blocked records name each PE's routine.
 * Broadcasts from two roots on one node carry the same root position and are
 * not told apart. And within a node, members' calls are not compared at all.
 * A broadcast's pieces go up empty: a child's arrival for piece k says its
 * release slot k mod D is free, since piece k - D has been copied out of it,
 * and only then does the parent write piece k there.
 *
 * **Trees differ from one collective to the next** - a broadcast is rooted at
 * its root's node, a reduction at the first - so a position's counter is fed
 * by different children over the team's life, and a lead's next arrivals may
 * come early, from nodes that were not its children this time. Each lead keeps
 * a base per child position and one for releases, and at a collective's end
 * adds to them what its own tree says each received: the pieces, at every
 * position it had a child at, and at the release base unless it was the root.
 * Early arrivals are harmless for one reason: the root releases a piece only
 * once every other node has sent it up, and a node sends a piece up only after
 * its own wait for its children's; so by the time any node holds a collective's
 * last release, and can start the next, every node is past its last wait on
 * its children in this one, and an early add only counts toward the next.
 * Nothing is written into a node's release slot for a piece before that
 * node's own arrival for it, and nothing into a child slot while its owner
 * still reads it: every node has read the last of its child slots before the
 * collective's last release exists. The model checks the rotated trees, one
 * collective after another (its cases with several roots).
 *
 * **The tree is over the team's nodes**, its sorted node set (§5.2), by their
 * positions in it: the reduction's tree is rooted at position 0, from the
 * team's record (node.h), read the first time a call names the team, naming
 * each neighbour by its lead's rank; a broadcast's is the same tree rotated to
 * its root's position, from this node's position in the record, with arithmetic
 * alone, and names each node's lead by position through the SHMEM layer
 * (internode.h, osoe_internode_team_lead): for the predefined teams, which
 * span every node, the job's table of each node's local id 0 (job.h); for a
 * team a split creates, the list of every node's lead - its member with the
 * lowest team rank - that each node's local lead of the team keeps (team.c;
 * phase 1 step 7 slice 2). Each team number has its own counters and small
 * slots in the internal region, and a large collective the lanes its leads
 * lent it (below), so two teams whose leads coincide on a node never meet
 * there, whatever their trees share.
 *
 * **Small and large, lanes and grants** (the team redesign's step E,
 * docs/design/team-resources-proposal.md §4; tools/modelcheck/lane_model.py).
 * A data collective is *small* when its bytes are at most
 * OSOE_SMALL_COLLECTIVE_BYTES and *large* otherwise, which every lead decides
 * alike from the call's bytes (osoe_collective_class): a reduction's count
 * times its width, a broadcast's bytes - the same on every PE, as the
 * specification has every PE pass the same count, and as every node agrees on
 * the setting. A small one moves as a one-piece collective always has, through
 * the team number's small slot for each position and its small release slot:
 * safe because a child reaches the team's next collective only once it holds
 * this one's release, which exists only after every lead has read its
 * children's slots. A large one's slots are a **lane**: one per thread record
 * of the PE (node.h), which the thread lends to the collective it is in from
 * its entry to its return, any team's - a thread is in one collective at a
 * time, and two threads of a PE leading two teams' collectives at once have
 * two lanes. Since where a lane is depends on which thread leads, which may
 * change from one of a team's collectives to the next, it travels:
 *   - a lead entering a large **reduction** sends each child of its tree a
 *     **grant** as it enters, before it waits for its own node's members
 *     (osoe_internode_reduce_enter): the grant's words - the epoch, the
 *     collective's identity, its bytes and the lane's index - put into the
 *     child's grant line for the team, then one more on the child's grant
 *     counter, fenced after them. A child sends its first piece up only once
 *     its grant counter shows this collective's grant, counted as every other
 *     counter is, and puts into the lane the grant names; a grant naming
 *     another collective ends the job, naming both leads, a hop before a
 *     piece's header would. A **broadcast** needs none: its pieces go up
 *     empty, writing nothing into the parent's lane;
 *   - a child puts its own lane's index and the collective's epoch into its
 *     row's lane word at its parent before its first add of a large
 *     collective, and the parent reads it **as it takes that child's first
 *     piece** - the arrival that shows it written - and keeps it for the
 *     collective, for the releases it sends that child. Not later, as the
 *     proposal had it, at the first release: with no more pieces than the
 *     window a middle lead's last piece goes up before its first release comes
 *     down, the collective can end elsewhere, and in the team's next
 *     collective - a broadcast rooted elsewhere - another node can be this
 *     lead's child at the position and write the word first (the model's
 *     release_lane_late, which the code's epoch check names).
 * **Every write into a lead's slots, its lane's or its small ones, is one that
 * lead waits for before it returns** (the proposal's E5): its children's
 * pieces, every one of which it combines, and its parent's releases, every one
 * of which it copies out. So at return nothing addressed to the lane is still
 * to come, and the next collective's children cannot write before the lead's
 * next grant or, for a release, before their own next arrival - the rule the
 * lane model checks, with the grant left out (no_grant), taken stale
 * (stale_grant) or read early (lane_early, release_lane_early) failing.
 *
 * Costs, with N nodes, k the fan-out, n bytes and p = ceil(n / slot) pieces:
 * O(k n) combining and k puts per node per piece; and since each lead keeps D
 * pieces between sending them up and holding their release, and takes each
 * input as it comes, the p pieces take about p / D round trips of the tree's
 * depth ceil(log_k N) where one at a time took p, for as long as the round trip
 * rather than the links is the limit (a timing simulation of the loop gives D
 * times at depths 1 to 4, for D of 2, 4 and 8). Every call finds its tree's
 * k + 1 neighbouring leads in O(k) - a broadcast's rotated, and since v0.2
 * step 2 a reduction's of the call's fan-out. A large reduction's grant costs a
 * lead a put and a fenced add to
 * each child as it enters, and a child a wait for it, once: when the two enter
 * together, a hop less the child's own intranode time - nothing once that is a
 * hop - and when the parent enters after its children, up to a round trip, the
 * grant down and the first piece back, less the parent's own intranode time,
 * where a team number's own slots had that piece waiting for it
 * (tools/modelcheck/grant_timing.py; the proposal's "What it costs" had only
 * the first). State: O(k) per team.
 *
 * **A large reduction takes Träff's circulant reduce-scatter and allgather
 * instead of the tree where the chooser says it costs less** (v0.2 step 3;
 * shape_of): collective-circulant.h has the rounds, tools/modelcheck/
 * circulant_model.py the protocol, docs/decisions/2026-10-07-circulant.md the
 * design. A tree carries every node's whole vector up and the result down, k + 1
 * vectors on an interior lead's link; Träff's carries under two on every lead's.
 * Over the team's N leads, in ceil(log2 N) rounds a half, each lead sends to one
 * lead and receives from another - in the reduce-scatter combining what it
 * receives into its accumulator, until it holds its N-th of the result, and in
 * the allgather copying the other N-ths in - who and what being functions of the
 * lead's position, N and the round alone (osoe_circulant_round). Its pieces go
 * through lanes, as a tree's large pieces do, and it counts on words of its own
 * (internal-region.h, the reduce-scatter's words); in_rounds is the model's
 * lead program:
 *   - **rows**: round k of either half moves its pieces into the k-th block
 *     of the receiver's lane, a block being the call's window of slots - a
 *     round takes as many of the lane's rows as the rounds leave it, so that
 *     at the defaults' lane of nine rows the window is 16 pieces on four nodes
 *     and the capacity from 17 - piece j into the block's slot j mod the
 *     window, each a put of its header
 *     and bytes and then a fenced add on the receiver's arrival counter for the
 *     row, cumulative over the team's life and compared as at least. The
 *     receiver checks each piece's header, combines or copies what follows it
 *     into its accumulator at the piece's place on its own application thread
 *     - §5.8's rules 1 and 2: pieces land in scratch, and only the lead's
 *     thread writes `dest` - and clears it. A reduction takes ceil(log2 N)
 *     rows, and a lane has as many as that or the fan-out ceiling's + 1,
 *     whichever is more (osoe_internal_lane_rows; P3), so the ceiling does not
 *     limit the nodes it runs on: the chooser is told the lane's rows and takes
 *     it only where they fit, which they do for every team the reduce-scatter's
 *     words hold, up to 65,536 nodes;
 *   - **credits**: a round sends to one lead and receives from another, so no
 *     piece says how far its sender's receiver has got. The receiver adds a
 *     credit at the sender for each piece it takes, and the sender puts piece
 *     j only once it holds j - W + 1 of the round's, W the call's window - one
 *     counter a lead, cumulative. Only for the pieces the sender's window waits
 *     on, the first P - W of a round of P: so the sender's last put of a round
 *     waits for every credit of the round, and none lands once it has returned
 *     (the model's QUIET, and credit_every_piece);
 *   - **the reduce-scatter's grants**: a lead's reduce-scatter receivers are as
 *     many leads as it has rounds, each lending its own lane, so each grant
 *     carries a lane of its own - into the field for its round of the sender's
 *     grant line, with the reduction's epoch, identity and bytes - and one
 *     counter cannot say which has come. Every lead grants each of its senders
 *     as it enters, before its node's members arrive
 *     (osoe_internode_generic_reduce_enter), and starts its reduce-scatter
 *     once its count shows all of its own;
 *   - **the allgather's grants and lanes**: a lead's allgather round k comes
 *     from the lead its reduce-scatter round k sent to, into the row that
 *     round's pieces filled, so the lead grants that sender - a count - as its
 *     own round k ends, and a lead starts its allgather once it holds all of
 *     its own. Where its allgather round k goes it learned in the
 *     reduce-scatter: the lead that sent it round k put its own lane into the
 *     field for row k of the senders' lanes line before its first piece's add,
 *     and the lead read it as it took that piece.
 * Every write into a lead's words and lane is one the lead waits for before it
 * returns, so they are quiet when the team gives its number back. They are its
 * own and no other collective's because the allgather ends with no step every
 * lead waits for: a lead whose last pieces have come leaves, and can start a
 * small collective whose child adds to its parent's counters at once while
 * another lead still waits on its last pieces. And the grant counters are safe
 * from one Träff's reduction to the next on one fact the arithmetic shows:
 * every lead's result needs every lead's reduce-scatter, so no lead enters the
 * next before every lead has read this one's grants.
 *
 * Its pieces' headers carry, in place of a tree's piece number, the round in
 * their top five bits and the piece's index in the round below, so that a
 * piece of one round or half never passes for another's. Costs, with N nodes
 * and n bytes: 2 ceil(log2 N) rounds, each a hop, a message cost and its pieces
 * at the pace of the link or of the credits, N - 1 blocks of n / N each way a
 * half on every lead's link; ceil(log2 N) + 3 lines of words a team number and
 * the grant's - four more up to 32 nodes, five to 1,024 - and a lane's
 * ceil(log2 N) rows a reduction, which a lane of the ceiling's + 1 holds up to
 * 512 nodes at the default and a longer lane, 64 KiB a row at the defaults,
 * beyond.
 *
 * **A gather is Bruck's allgather among the team's node leads** (§5.8, "An
 * allgather for fcollect"; phase 1 step 5 slice 6), tools/modelcheck/
 * allgather_model.py's lead program: the tree running an or, which it
 * replaced, carried the whole result through every level. The leads are the
 * tree's - positions 0 ... N - 1 of the team's node set, this node's from its
 * record and any other's lead through osoe_internode_team_lead. A node's
 * *share* is its members' blocks at their places in the result. At step s,
 * for s from 0 to ceil(log2 N) - 1, the lead at position i puts every share
 * it holds - positions i to i + c - 1, c = min(2^s, N - 2^s), its own and
 * those gathered so far - straight to their places in the buffer of the lead
 * at i - 2^s, and receives positions i + 2^s onward from the lead at i + 2^s:
 * after the last step it holds every share, each put into its buffer once,
 * N - 1 in all, and has sent as many. Nothing is staged and nothing rotated.
 * Two counters per step, in the team number's own words (internal-region.h),
 * each cumulative over the team's life and waited on as at least - each gather
 * adds one to each, and the lead counts its gathers on the team, so its
 * gather x waits for x + 1 - keep it the model's:
 *   - **READY[s]**, at a writer: at entry a lead adds one to READY[s] at the
 *     lead that writes to it at step s, the one at i + 2^s, for every s; a
 *     writer waits for its own before its step-s puts. This is internode.h's
 *     rule that a peer's buffer is written only once the peer has entered,
 *     since until then its members may still be copying the gather before's
 *     result out of it - and the model's no_ready breakage;
 *   - **DATA[s]**, at a receiver: a writer adds one to DATA[s] at the lead it
 *     wrote to, fenced after its puts (invariant 5); a lead waits for its own
 *     before its step s + 1, which forwards what step s brought, and after its
 *     last step before returning - the model's no_data_wait breakage.
 * And ceil(log2 N) steps, not floor: on a node count that is not a power of
 * two some shares would never arrive (short_steps). A lead reads the shares it
 * sends at a step while the leads after it write the ones it has yet to
 * receive: different bytes, since the shares a lead receives are positions
 * i + 1 ... i + N - 1, each once, and it sends only those it holds.
 *
 * **The shares' members are found once per team** (find_intervals), at its
 * first gather, since they depend on the team and the rank map alone: the
 * team's ranks walked in order, their world ranks from the progression the
 * team's record carries (node.h, decision E) and their nodes from the rank
 * map's runs,
 * each stretch of members on one node joining the list of every step whose
 * shares include its node, merged with the interval before it when the two
 * touch. A list holds intervals of consecutive team ranks, and consecutive
 * ranks are contiguous in the result whatever its layout, so under a block
 * distribution a step's shares are one interval, or two when they wrap past
 * position N - 1, and under a cyclic one at most L + 1, L the node's PEs. The
 * walk costs O(N + intervals) under a block distribution, whose rank-map runs
 * each hold a node's members, and O(P) under a cyclic one, whose runs step
 * from node to node a member at a time - and, for a team spanning fewer than
 * every node, a binary search of its node set per stretch, O(log N) lookups.
 * It is made once. **Every gather costs O(intervals)** beyond its puts: each
 * interval's ends mapped to bytes by the call's `block` or `places`, and
 * checked there - the places non-decreasing, from 0 to the result's end - so
 * that no put leaves [0, bytes) whatever the places hold (layout_overwritten).
 * Those ends, 0 and the team's size are the only places it reads; and where
 * every node holds one run of the team's ranks each end is the first rank of a
 * node's run or the team's size, an interval being one node's stretch of ranks,
 * merged with the next only where the two touch. That is what lets collect
 * take its places from each node's total rather than from every member's length
 * (collectives.c, collect_varying).
 * Every put but a step's last operation - its fenced add - says more follows
 * (OSOE_TRANSPORT_MORE). State: the lists, O(intervals) per team.
 *
 * **A lead says what gather it has entered** (§5.5's "asserted on receipt"),
 * first of all: into the READY[0] line of the lead that writes to it at step
 * 0, past the counter, it puts the gather's shape - the team's epoch, the
 * result's bytes, and how it is laid out: by a block, the block then, which is
 * every node's alike; by places, collect's second gather, whose blocks differ
 * from node to node and whose places are what every node shares; or by the
 * ranges of each node's share, under collect's scan of the nodes' totals (P30,
 * below) - and its add to that READY[0] is fenced after it. The writer compares the shape with its own
 * after its READY wait and before any put, and a difference ends the job
 * naming both leads (check_shape). Step 0 joins every lead to the one before
 * it, a ring, so its comparisons alone cover every pair, and nothing lands in
 * a buffer whose owner reckons the result another size. A gather against
 * another collective still waits in words the other never touches, a hang
 * whose blocked records name each PE's routine, as for collectives whose
 * trees share no edge (above).
 *
 * Each lead publishes, in the team's tree state (node.h), the steps its last
 * gather made and the (node, step) pairs its lists hold - ceil(log2 N) and
 * N - 1, counted as the walk fills the lists rather than computed - so that a
 * test pins the algorithm by counts rather than by time: a last step sending
 * the full 2^s shares, whose extra bytes only rewrite what the receiver holds,
 * shows there and nowhere else.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collective-circulant.h"
#include "collective-shape.h"
#include "collective-tree.h"
#include "combine.h"
#include "fatal.h"
#include "internal-region.h"
#include "internode-generic.h"
#include "job.h"
#include "metrics.h"
#include "node.h"
#include "perturb.h"
#include "region-wait.h"
#include "state.h"
#include "transport.h"

/*
 * The internal region's layout (internal-region.h): per team number, a small
 * row per child position and one for releases - a lane word, then the small
 * slot - and its words: an eight-byte arrival counter per child position (that
 * child's pieces in), the release counter (the parent's pieces down) and the
 * grant counter, each alone on a line of the region's fixed 128 bytes, the
 * grant's words past its counter; and, past every number's places, a lane per
 * thread record, a row of D slots of a header and 16 KiB per child position
 * and a release row,
 * piece k in a row's slot k mod D. Each team number has its own counters and
 * small slots, so two teams whose trees share a lead count on different words
 * (§7.3); a large collective's pieces go through the lanes its leads' threads
 * lend it (the file's comment).
 */
/*
 * The number of the team in block `team` of this node's pool - every `team`
 * here being a block index since the team redesign's step D (internode.h) -
 * which is what places the team's rows and counters in each PE's internal
 * region, the same on every node, where a block index is the node's own. Read
 * from the block's name, which the record's publication, acquired before any
 * call here, orders (node.h).
 */
static uint32_t number_of(uint32_t team)
{
    const struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
    uint32_t number = b ? atomic_load_explicit(&b->name.number, memory_order_acquire) : team;
    if (number >= osoe.bootstrap.max_teams)
        osoe_fatal("block %u of the node's teams names number %u, past OSOE_MAX_TEAMS (%u)",
                   team, number, osoe.bootstrap.max_teams);
    return number;
}

/* Team number `number`'s small row for `position`: a child position's below
 * the fan-out, the release row at it. */
static uint64_t small_row(uint32_t number, uint32_t position)
{
    uint32_t small = osoe.bootstrap.small_collective_bytes;
    return (uint64_t)number *
               osoe_internal_team_small_bytes(osoe.bootstrap.collective_fanout, small) +
           (uint64_t)position * osoe_internal_small_row_bytes(small);
}

/* The row's lane word: where the child at `position` says which lane it lends
 * to the large collective under way. */
static uint64_t lane_word(uint32_t number, uint32_t position)
{
    return small_row(number, position);
}

/* The row's small slot: a small collective's one piece, behind its header. */
static uint64_t small_slot(uint32_t number, uint32_t position)
{
    return small_row(number, position) + OSOE_INTERNAL_LANE_WORD_BYTES;
}

/* Lane `lane`'s slot for piece `piece` of `position`'s row - a child
 * position's below the fan-out, the release row at it. */
static uint64_t lane_slot(uint32_t lane, uint32_t position, uint64_t piece)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    uint32_t pipeline = osoe.bootstrap.collective_pipeline;
    return osoe_internal_lanes_offset(fanout, osoe.bootstrap.max_teams,
                                      osoe.bootstrap.small_collective_bytes,
                                      osoe.bootstrap.node_count) +
           (uint64_t)lane * osoe_internal_lane_bytes(fanout, pipeline, osoe.bootstrap.node_count) +
           ((uint64_t)position * pipeline + piece % pipeline) * OSOE_INTERNAL_SLOT_BYTES;
}

/* Where team number `number`'s words start: its arrival counter for each
 * child position, then its release counter and its grant counter, a line
 * each. */
static uint64_t words_of_number(uint32_t number)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    return osoe_internal_words_offset(fanout, osoe.bootstrap.max_teams,
                                      osoe.bootstrap.small_collective_bytes) +
           (uint64_t)number * osoe_internal_team_words_bytes(fanout);
}

static uint64_t arrival_counter(uint32_t number, uint32_t position)
{
    return words_of_number(number) + (uint64_t)position * OSOE_INTERNAL_LINE_BYTES;
}

static uint64_t release_counter(uint32_t number)
{
    return arrival_counter(number, osoe.bootstrap.collective_fanout);
}

static uint64_t grant_counter(uint32_t number)
{
    return arrival_counter(number, osoe.bootstrap.collective_fanout + 1);
}

/* The grant's words, past its counter and so past every lead's watch. */
static uint64_t grant_words(uint32_t number)
{
    return grant_counter(number) + 8;
}

/*
 * The allgather's words (internal-region.h): per team number, past every
 * number's words, READY for each step and then DATA for each, each alone on
 * its line, with room for as many steps as the job's node count takes - the
 * most any team's gather takes.
 */
static uint64_t gather_of_number(uint32_t number)
{
    uint32_t steps = osoe_internal_gather_steps(osoe.bootstrap.node_count);
    return osoe_internal_gather_offset(osoe.bootstrap.collective_fanout,
                                       osoe.bootstrap.max_teams,
                                       osoe.bootstrap.small_collective_bytes) +
           (uint64_t)number * osoe_internal_team_gather_bytes(steps);
}

static uint64_t gather_word(uint32_t team, uint32_t line)
{
    return gather_of_number(number_of(team)) + (uint64_t)line * OSOE_INTERNAL_LINE_BYTES;
}

/* READY[step], at the lead that writes at `step`: the entries of the lead it
 * writes to. */
static uint64_t ready_counter(uint32_t team, uint32_t step)
{
    return gather_word(team, step);
}

/* DATA[step], at the lead written to at `step`: its writer's steps done. */
static uint64_t data_counter(uint32_t team, uint32_t step)
{
    return gather_word(team, osoe_internal_gather_steps(osoe.bootstrap.node_count) + step);
}

/*
 * The reduce-scatter's words (internal-region.h): per team number, past every
 * number's allgather words, an arrival counter for each row - as many rows as
 * the job's node count takes, at most OSOE_INTERNAL_CIRCULANT_ROWS_MOST - then
 * the credit counter and the allgather's grant counter, each on its own line;
 * the reduce-scatter's grant lines, its counter and a field a row, one to four
 * lines; and the senders' lanes line. Each line where internal-region.h puts
 * it, as test-circulant-grants.c finds it.
 */
static uint32_t circulant_layout_rows(void)
{
    return osoe_internal_circulant_rows(osoe.bootstrap.node_count);
}

static uint64_t circulant_line(uint32_t number, uint32_t line)
{
    return osoe_internal_circulant_line(osoe.bootstrap.collective_fanout,
                                        osoe.bootstrap.max_teams,
                                        osoe.bootstrap.small_collective_bytes,
                                        osoe.bootstrap.node_count, number, line);
}

/* Row `row`'s arrival counter: the pieces of the rounds that take that row of
 * this lead's lane. */
static uint64_t circulant_arrival_counter(uint32_t number, uint32_t row)
{
    return circulant_line(number, row);
}

static uint64_t circulant_credit_counter(uint32_t number)
{
    return circulant_line(number, osoe_internal_circulant_credit_line(circulant_layout_rows()));
}

static uint64_t circulant_allgather_grant_counter(uint32_t number)
{
    return circulant_line(number,
                          osoe_internal_circulant_allgather_grant_line(circulant_layout_rows()));
}

static uint64_t circulant_reduce_scatter_grant_counter(uint32_t number)
{
    return circulant_line(
        number, osoe_internal_circulant_reduce_scatter_grant_line(circulant_layout_rows()));
}

/* The reduce-scatter grant's fields, a round's each, past its counter and so
 * past every lead's watch. */
static uint64_t circulant_reduce_scatter_grant_words(uint32_t number)
{
    return circulant_reduce_scatter_grant_counter(number) + 8;
}

/* The senders' lanes line, past the grant's lines: a lane word a row. */
static uint64_t circulant_senders(uint32_t number)
{
    return circulant_line(number, osoe_internal_circulant_senders_line(circulant_layout_rows()));
}

/* This PE's own internal region, at `offset`. */
static unsigned char *internal(uint64_t offset)
{
    return (unsigned char *)osoe.node.heap_of[osoe.bootstrap.local_id] +
           osoe.node.internal_offset + offset;
}

/* The leads around this node in a tree rooted at one node. */
struct lead_tree {
    uint32_t parent;                    /* the parent lead's rank, or UINT32_MAX */
    uint32_t position;                  /* this node among the parent's children */
    uint32_t children;
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];
};

/* Team ranks `first` up to `end`: consecutive members whose blocks go at one
 * step of a gather, and so one range of its result whatever its layout. */
struct member_interval {
    uint32_t first;
    uint32_t end;
};

/* The intervals a lead sends at one step of its team's gathers, ascending. */
struct interval_list {
    struct member_interval *intervals;
    uint32_t count;
    uint32_t capacity;
};

/*
 * A team's internode phase as this node's lead keeps it: the team's node count
 * and this node's place among them, from the team's record, which every call's
 * tree is built from (find_lead_tree, at the call's fan-out since v0.2 step 2:
 * a data collective's tree is the call's, the record's the barrier's); and the
 * bases its waits count from - where each child
 * position's arrival counter, the release counter and the grant counter stood
 * when the team's last collective ended, by this lead's own trees (the file's
 * comment), since the counters in the internal region are cumulative over the
 * team's life and reset only when a PE takes the number for another team
 * (osoe_internode_generic_number_taken); and the epoch of the large reduction
 * it last entered, whose grants it sent (osoe_internode_generic_reduce_enter). One per block of the node's pool
 * (OSOE_TEAM_BLOCKS), allocated at init and filled the first time a call names
 * the team. And what its gathers keep: the gathers so far, which READY and DATA
 * count; and, found at the first and the same for every one after it, the
 * steps, the team's size, the (node, step) pairs the lists hold, and each
 * step's intervals of members.
 *
 * Kept for the block's team at `generation` (node.h, osoe_team_name): a block
 * is given back when its team is destroyed and claimed for another, whose
 * counters start again at 0 on every member, and the state of the old team is
 * emptied when a call meets the block at another generation (team_state) -
 * the bases with the counters they count from.
 */
struct generic_team {
    uint32_t generation;
    bool registered;
    uint32_t count;                     /* how many nodes it spans */
    uint32_t index;                     /* this node's position among them */
    uint64_t arrival_base[OSOE_COLLECTIVE_FANOUT_MAX];
    uint64_t release_base;
    uint64_t grant_base;
    uint32_t entered;                   /* the epoch of the large reduction entered */
    struct osoe_collective_state *published;    /* the team's tree state (node.h) */
    uint64_t gathers;
    uint32_t steps;
    uint32_t members;
    uint32_t shares;
    struct interval_list *step_intervals;
    /* The pieces its scans have moved (osoe_internode_generic_scan): what each
     * scan slot a member uses has received, since the team's tree is one; its
     * scans, every member's count alike; and the base of the scans' grant
     * counter, as the reductions' grant base is. */
    uint64_t scan_pieces;
    uint32_t scans;
    uint64_t scan_grant_base;
    /* Its Träff's reductions (in_rounds): the bases of their own counters -
     * each row's arrivals, the credits, and the two grant counters, to each of
     * which every one of them adds the team's rows. */
    uint64_t circulant_arrival_base[OSOE_INTERNAL_CIRCULANT_ROWS_MOST];
    uint64_t circulant_credit_base;
    uint64_t circulant_grant_base;
    /* collect's exchanges of the node totals (P33): how many the team has made,
     * whose parity picks the set of slots and the counter, and the adds each
     * set's counter has had - N - 1 an exchange of its parity. */
    uint64_t exchanges;
    uint64_t exchange_base[2];
};

static struct generic_team *teams;
static uint32_t team_count;

void osoe_internode_generic_init(void)
{
    team_count = osoe.bootstrap.team_blocks;
    teams = calloc(team_count, sizeof *teams);
    if (!teams)
        osoe_fatal("out of memory for %u team blocks' internode phases", team_count);
}

void osoe_internode_generic_fini(void)
{
    for (uint32_t t = 0; teams && t < team_count; t++) {
        for (uint32_t s = 0; teams[t].step_intervals && s < teams[t].steps; s++)
            free(teams[t].step_intervals[s].intervals);
        free(teams[t].step_intervals);
    }
    free(teams);
    teams = NULL;
    team_count = 0;
}

/*
 * The team's state. The first call that names a team learns it from its
 * record in this node's block (node.h): `published` loaded with acquire, then
 * the rest (§3.16) - the node count and this node's position in the team's
 * node set, from which each call builds its tree (find_lead_tree), whose leads
 * the record names by rank. Any team whose record says it spans nodes:
 * the predefined ones, and since phase 1 step 7's second slice those a split
 * creates, whose leads are wherever their lowest team ranks are - which the
 * record names, and osoe_internode_team_lead gives by position.
 */
/* The process's state for the team in block `team`, emptied first if it was
 * kept for another team the block held (struct generic_team): the generation
 * read with acquire (§3.16), though the caller is a member holding the block,
 * whose generation moves only while nobody does (node.h). */
static struct generic_team *team_state(uint32_t team, const char *routine)
{
    if (team >= team_count)
        osoe_fatal("%s: block %u is past the node's %u team blocks", routine, team, team_count);
    struct generic_team *g = &teams[team];
    uint32_t generation =
        atomic_load_explicit(&osoe_node_team(&osoe.node, team)->name.generation,
                             memory_order_acquire);
    if (g->generation != generation) {
        for (uint32_t s = 0; g->step_intervals && s < g->steps; s++)
            free(g->step_intervals[s].intervals);
        free(g->step_intervals);
        *g = (struct generic_team){ .generation = generation };
    }
    return g;
}

static struct generic_team *registered_team(uint32_t team, const char *routine)
{
    struct generic_team *g = team_state(team, routine);
    if (!g->registered) {
        const struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
        if (!atomic_load_explicit(&b->record.published, memory_order_acquire) ||
            b->record.nodes < 2)
            osoe_fatal("%s: block %u of the node's teams holds no team with an internode "
                       "phase on this node", routine, team);
        const struct osoe_team_record *r = &b->record;
        if (r->children > osoe.bootstrap.collective_fanout || r->index >= r->nodes)
            osoe_fatal("%s: team %u's record on this node says %u children at position %u of "
                       "%u nodes, at a fan-out of %u", routine, number_of(team), r->children,
                       r->index, r->nodes, osoe.bootstrap.collective_fanout);
        g->count = r->nodes;
        g->index = r->index;
        g->published = &osoe_node_team(&osoe.node, team)->tree;
        g->registered = true;
    }
    return g;
}

/* The lead of the node at position `index` of the team's node set (the file's
 * comment). */
static uint32_t lead_of(uint32_t team, uint32_t index, const char *routine)
{
    uint32_t rank = osoe_internode_team_lead(team, index);
    if (rank == UINT32_MAX)
        osoe_fatal("%s: team %u has no lead for the node at position %u of its node set",
                   routine, number_of(team), index);
    return rank;
}

/*
 * §5.3's k-ary tree over the team's node set, positions 0 ... N - 1, rotated
 * so that `root` is position 0: position i's parent is (i - 1) / k and its
 * children k i + 1 ... k i + k. The reduction's tree when the root is position
 * 0; a broadcast roots it at its root's node, so the data travels depth levels
 * and not up to position 0 first. `k` is the call's fan-out (shape_of), at
 * most the ceiling the layout is sized for, so every child position it gives
 * has its rows and its counter.
 */
static void find_lead_tree(const struct generic_team *g, uint32_t team, uint32_t root,
                           uint32_t k, struct lead_tree *out, const char *routine)
{
    uint32_t n = g->count;
    uint32_t index = (g->index + n - root) % n;
    out->parent = UINT32_MAX;
    out->position = 0;
    if (index > 0) {
        out->parent = lead_of(team, ((index - 1) / k + root) % n, routine);
        out->position = (index - 1) % k;
    }
    out->children = 0;
    for (uint32_t c = 0; c < k; c++) {
        uint64_t child = (uint64_t)k * index + 1 + c;
        if (child >= n)
            break;
        out->child[out->children++] = lead_of(team, (uint32_t)((child + root) % n), routine);
    }
}

/*
 * The shape of one call (v0.2 step 2; collective-shape.h): its fan-out and its
 * window, chosen from the job's link - which every PE holds alike since the
 * end of shmem_init (link-measure.h) - and from the call's arguments, which
 * every member passes alike (§9.10), by integer arithmetic alone: so every
 * node of the team computes the same shape for the same call, and no message
 * carries it (the brief's rule). The ceiling and the capacity, 0.1.0's shape,
 * where the shape is `fixed` or there is no link to choose from. Both bound
 * what the chooser returns, and the layout is sized for them, so every shape
 * fits: its child positions within the ceiling's rows and counters, its
 * window within the capacity's slots. And Träff's reduction (v0.2 step 3),
 * under `link` and not `tree`, where its rows fit the reduce-scatter's words,
 * which hold OSOE_INTERNAL_CIRCULANT_ROWS_MOST - a team of up to 65,536 nodes -
 * and a lane, which the chooser checks against the lane's rows it is told,
 * osoe_internal_lane_rows of the ceiling and the job's node count: the ceiling's
 * + 1 or the job's rounds, whichever is more, so that every team the words
 * hold fits (P3).
 */
static struct osoe_collective_shape shape_of(enum osoe_shape_operation operation,
                                             const struct generic_team *g, uint64_t bytes)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    if (b->collective_shape == OSOE_COLLECTIVE_SHAPE_FIXED ||
        osoe.job.link_source == OSOE_LINK_NONE)
        return (struct osoe_collective_shape){ OSOE_SHAPE_TREE, b->collective_fanout,
                                               b->collective_pipeline };
    bool circulant = b->collective_shape == OSOE_COLLECTIVE_SHAPE_LINK &&
                     osoe_circulant_rows(g->count) <= circulant_layout_rows();
    return osoe_shape_choose(operation, g->count, bytes, &osoe.job.link, b->collective_fanout,
                             osoe_internal_lane_rows(b->collective_fanout, b->node_count),
                             b->collective_pipeline, b->small_collective_bytes,
                             OSOE_INTERNAL_PIECE_BYTES, circulant);
}

/*
 * What a piece carries ahead of its bytes (the file's comment). Sixteen bytes,
 * so that the bytes after it stay aligned for every element.
 */
struct piece_header {
    uint32_t epoch;                     /* the team's epoch of the collective */
    uint32_t piece;                     /* which of its pieces */
    uint32_t collective;                /* which: reduction_identity, or the root */
    uint32_t bytes;                     /* this piece's */
};
enum { HEADER_BYTES = OSOE_INTERNAL_PIECE_HEADER_BYTES };
_Static_assert(sizeof(struct piece_header) == HEADER_BYTES, "a piece's header is 16 bytes");

/*
 * What a grant carries past its counter (the file's comment): the epoch and the
 * identity of the large reduction its parent entered, as a piece's header names
 * them, the reduction's bytes, and the lane the parent lends to it.
 */
struct grant {
    uint32_t epoch;
    uint32_t collective;
    uint64_t bytes;
    uint32_t lane;
    uint32_t reserved;
};
_Static_assert(sizeof(struct grant) == 24 && 8 + sizeof(struct grant) <= OSOE_INTERNAL_LINE_BYTES,
               "a grant's words fit its line, past the counter");

/* What a child puts into its row's lane word at its parent (the file's
 * comment): the large collective's epoch, and the lane the child lends to it. */
struct lane_word {
    uint32_t epoch;
    uint32_t lane;
};
_Static_assert(sizeof(struct lane_word) <= OSOE_INTERNAL_LANE_WORD_BYTES,
               "a lane word fits the room its row keeps for it");

/*
 * What a Träff's reduction's reduce-scatter grant carries past its counter (the
 * file's comment): a field for each round, where that round's receiver alone
 * puts the epoch and the identity of the reduction it entered, as a piece's
 * header names them, its bytes, and the lane it lends - plus one, so that a
 * field never written, or cleared, reads as none. A field a receiver, and not
 * one head every receiver put: with progress helpers two receivers' puts land
 * from two threads at once, and the lead would check only the last to land
 * (the review of v0.2 step 4's merge point, correctness Q2-4). The senders'
 * lanes line past the grant's lines is a lane word a row.
 */
struct circulant_grant_field {
    uint32_t epoch;
    uint32_t collective;
    uint64_t bytes;
    uint32_t lane;
    uint32_t unused;
};
_Static_assert(sizeof(struct circulant_grant_field) == OSOE_INTERNAL_CIRCULANT_GRANT_FIELD_BYTES,
               "a reduce-scatter grant's field is the size the region keeps for it");
_Static_assert(OSOE_INTERNAL_CIRCULANT_ROWS_MOST * sizeof(struct lane_word) <=
                   OSOE_INTERNAL_LINE_BYTES,
               "the senders' lanes line holds a lane word for every row");

/* No lane known yet. */
#define NO_LANE UINT32_MAX

/* A reduction's `collective`, its top bit set; a broadcast's is its root's index;
 * the leads' scan of collect's node totals (P30) has bit 30 set and bit 31
 * clear, which neither of the others can have. */
enum { NODE_TOTALS_SCAN = UINT32_C(1) << 30 };

static uint32_t reduction_identity(enum osoe_reduction_operation operation,
                                   struct osoe_element element)
{
    return UINT32_C(1) << 31 | (uint32_t)operation << 16 | (uint32_t)element.kind << 8 |
           (element.width & 0xff);
}

static const char *operation_name(uint32_t operation)
{
    static const char *const names[] = {
        [OSOE_REDUCTION_AND] = "and", [OSOE_REDUCTION_OR] = "or", [OSOE_REDUCTION_XOR] = "xor",
        [OSOE_REDUCTION_MAXIMUM] = "maximum", [OSOE_REDUCTION_MINIMUM] = "minimum",
        [OSOE_REDUCTION_SUM] = "sum", [OSOE_REDUCTION_PRODUCT] = "product",
    };
    return operation < sizeof names / sizeof names[0] && names[operation] ? names[operation]
                                                                         : "operation";
}

static const char *kind_name(uint32_t kind)
{
    static const char *const names[] = {
        [OSOE_ELEMENT_SIGNED_INTEGER] = "signed integers",
        [OSOE_ELEMENT_UNSIGNED_INTEGER] = "unsigned integers",
        [OSOE_ELEMENT_FLOATING] = "floating-point numbers",
        [OSOE_ELEMENT_COMPLEX] = "complex numbers",
    };
    return kind < sizeof names / sizeof names[0] && names[kind] ? names[kind] : "elements";
}

/* A header as a reader wants it: "piece 0 (64 bytes) of epoch 3's sum of
 * 8-byte signed integers". */
static void describe(const struct piece_header *h, char *out, size_t size)
{
    if (h->epoch == 0)
        snprintf(out, size, "nothing: no piece was put there");
    else if (h->collective >> 31)
        snprintf(out, size, "piece %u (%u bytes) of epoch %u's %s of %u-byte %s", h->piece,
                 h->bytes, h->epoch, operation_name((h->collective >> 16) & 0x7fff), h->collective & 0xff,
                 kind_name((h->collective >> 8) & 0xff));
    else if (h->collective == NODE_TOTALS_SCAN)
        snprintf(out, size, "piece %u (%u bytes) of epoch %u's scan of the nodes' totals for "
                 "collect's places", h->piece, h->bytes, h->epoch);
    else
        snprintf(out, size, "piece %u (%u bytes) of epoch %u's broadcast from the node at "
                 "index %u", h->piece, h->bytes, h->epoch, h->collective);
}

/* The slot at `slot` holds the piece `expected` says, or the job ends: `from`
 * says what it should hold, and `sender` whose it is. */
static void check_piece(const unsigned char *slot, const struct piece_header *expected,
                        const char *from, uint32_t sender, const char *routine)
{
    struct piece_header got;
    memcpy(&got, slot, sizeof got);
    if (got.epoch == expected->epoch && got.piece == expected->piece &&
        got.collective == expected->collective && got.bytes == expected->bytes)
        return;
    char want[160], found[160];
    describe(expected, want, sizeof want);
    describe(&got, found, sizeof found);
    osoe_fatal("%s: %s, from node %u's lead (PE %u), should be %s, and is %s: PEs called "
               "different collectives, or the same one with different arguments", routine,
               from, osoe_node_of(&osoe.job, sender), sender, want, found);
}

/* A collective as a reader wants it: "epoch 3's sum of 8-byte signed
 * integers". */
static void describe_collective(uint32_t epoch, uint32_t collective, char *out, size_t size)
{
    if (epoch == 0)
        snprintf(out, size, "nothing: no grant was put there");
    else if (collective >> 31)
        snprintf(out, size, "epoch %u's %s of %u-byte %s", epoch,
                 operation_name((collective >> 16) & 0x7fff), collective & 0xff,
                 kind_name((collective >> 8) & 0xff));
    else if (collective == NODE_TOTALS_SCAN)
        snprintf(out, size, "epoch %u's scan of the nodes' totals for collect's places", epoch);
    else
        snprintf(out, size, "epoch %u's broadcast from the node at index %u", epoch, collective);
}

/*
 * One collective's pieces, as every lead cuts them alike: its buffer, `bytes`
 * long, in pieces of `bytes_per_piece` - the whole buffer for a small
 * collective, and for a large one a whole number of elements that fits a
 * lane's slot behind its header - the last of which may be shorter; its class,
 * every lead's alike; its team's number, which places the team's rows and
 * counters; and what each piece's header says besides its number and bytes.
 * `combine` is NULL for a broadcast, whose pieces go up empty.
 */
struct pieces {
    uint32_t team;
    uint32_t number;
    uint32_t epoch;
    uint32_t collective;                /* the header's: reduction_identity, or the root */
    enum osoe_collective_class class;
    unsigned char *buffer;
    size_t bytes;
    size_t bytes_per_piece;
    size_t width;
    osoe_combine_function combine;
    const char *routine;
};

/*
 * A large collective's lanes as this lead learns them (the file's comment):
 * its own, which its thread lends to the collective; its parent's, from the
 * grant, where a reduction's pieces go up; and each child's, read as this lead
 * takes the child's first piece, where the releases go down. A small
 * collective has none.
 */
struct lanes {
    uint32_t own;
    uint32_t parent;
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];
};

/* The lane of this thread: its thread record's index (node.h), which every
 * application thread of the PE holds while it lives. */
static uint32_t own_lane(void)
{
    return osoe_metrics_thread_index();
}

/* The most bytes of `width`-byte elements a piece carries, behind its header
 * in a lane's slot: 16 KiB for every element the specification names. */
static size_t piece_bytes(size_t width)
{
    return (OSOE_INTERNAL_PIECE_BYTES / width) * width;
}

/* How every lead cuts `bytes` of `width`-byte elements of a collective of
 * `class`: in one piece when it is small - its small slot holds
 * OSOE_SMALL_COLLECTIVE_BYTES - and in a lane's slots when it is large. */
static size_t bytes_per_piece(enum osoe_collective_class class, size_t bytes, size_t width)
{
    return class == OSOE_COLLECTIVE_SMALL && bytes > 0 ? bytes : piece_bytes(width);
}

/* Piece `number`'s header. */
static struct piece_header header_of(const struct pieces *pieces, uint64_t number)
{
    size_t left = pieces->bytes - (size_t)number * pieces->bytes_per_piece;
    return (struct piece_header){
        .epoch = pieces->epoch,
        .piece = (uint32_t)number,
        .collective = pieces->collective,
        .bytes = (uint32_t)(left < pieces->bytes_per_piece ? left : pieces->bytes_per_piece),
    };
}

/* Where piece `number` is in the buffer. */
static unsigned char *piece_at(const struct pieces *pieces, uint64_t number)
{
    return pieces->buffer + (size_t)number * pieces->bytes_per_piece;
}

/* Where this lead finds piece `number` from child position `position` - the
 * team number's small slot for the position, or its own lane's row - and, at
 * the fan-out, its release of the piece. */
static uint64_t own_slot(const struct pieces *pieces, const struct lanes *lanes,
                         uint32_t position, uint64_t number)
{
    return pieces->class == OSOE_COLLECTIVE_SMALL ? small_slot(pieces->number, position)
                                                  : lane_slot(lanes->own, position, number);
}

/*
 * What this lead waits for, said in the team's tree state (node.h,
 * team.tree.data_wait; D1), so that osoe-stat's summary walks a hung data
 * collective from node to node as it walks a barrier: `rank` the lead whose
 * word ends the wait, named by its node, or UINT32_MAX where one counter of
 * several leads' cannot say whose is missing, `step` then how many are. At
 * every wait of this file but a scan's, whose member names the PE it waits on
 * in its blocked record instead, a scan's tree being of members, not leads:
 * before a wait on one lead's word, and from the condition of a wait on
 * several, each time it finds itself still waiting (say_awaited,
 * node_totals_in, count_in). Three stores, which order nothing (§3.16).
 */
static void waits_for(const struct generic_team *team_phase, enum osoe_data_wait what,
                      uint32_t rank, uint64_t step)
{
    osoe_collective_data_wait(team_phase->published, what,
                              rank == UINT32_MAX ? OSOE_COLLECTIVE_TREE_NONE
                                                 : osoe_node_of(&osoe.job, rank),
                              step < UINT32_MAX ? (uint32_t)step : UINT32_MAX);
}

/*
 * A wait for one cumulative counter of several leads' adds to reach `want` -
 * Träff's grants, the exchange of collect's totals - saying each time it finds
 * it still short how many are missing (D1): one counter cannot say whose. The
 * load is seq_cst (transport.h), and so acquire, as osoe_region_wait's is: what
 * the adders put before their fenced adds is there to read after it.
 */
struct count_awaited {
    const struct generic_team *team_phase;
    enum osoe_data_wait what;
    uint64_t counter;
    uint64_t want;
};

static bool count_in(const void *context)
{
    const struct count_awaited *a = context;
    uint64_t have = osoe_transport_load(OSOE_REGION_INTERNAL, a->counter, 8);
    if (have >= a->want)
        return true;
    waits_for(a->team_phase, a->what, UINT32_MAX, a->want - have);
    return false;
}

static void wait_count(const char *routine, const struct generic_team *team_phase,
                       enum osoe_data_wait what, uint64_t counter, uint64_t want, uint32_t epoch)
{
    struct count_awaited awaited = { team_phase, what, counter, want };
    osoe_region_wait_condition(routine, OSOE_REGION_INTERNAL, counter, 8, count_in, &awaited,
                               OSOE_BLOCKED_DETAIL_EPOCH, epoch);
}

/*
 * The first child position that has not sent piece `number` up, or the
 * children's count when all have: each child position's arrival counter shows
 * it, at least - a child may be up to D pieces ahead, and the counter holds
 * the team's whole life, so it is compared with the position's base plus the
 * piece. The loads are seq_cst (transport.h), and so acquire: the bytes a
 * child put before its fenced add are there to read.
 */
static uint32_t child_missing(const struct generic_team *team_phase, const struct pieces *pieces,
                              const struct lead_tree *tree, uint64_t number)
{
    for (uint32_t c = 0; c < tree->children; c++)
        if (osoe_transport_load(OSOE_REGION_INTERNAL, arrival_counter(pieces->number, c), 8) <
            team_phase->arrival_base[c] + number + 1)
            return c;
    return tree->children;
}

/* Whether every child has sent piece `number` up. */
static bool children_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                          const struct lead_tree *tree, uint64_t number)
{
    return child_missing(team_phase, pieces, tree, number) == tree->children;
}

/* Whether the parent has sent piece `number`'s release down: likewise, the
 * release counter against the release base. */
static bool release_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                         uint64_t number)
{
    return osoe_transport_load(OSOE_REGION_INTERNAL, release_counter(pieces->number), 8) >=
           team_phase->release_base + number + 1;
}

/* Whether the parent's grant for this large reduction is in: the grant
 * counter against its base, as at least, like every other counter here. */
static bool grant_in(const struct generic_team *team_phase, const struct pieces *pieces)
{
    return osoe_transport_load(OSOE_REGION_INTERNAL, grant_counter(pieces->number), 8) >=
           team_phase->grant_base + 1;
}

/*
 * **A reader clears what it read**: a piece's header once its bytes are
 * combined or copied out, a lane word or a grant's words once taken. Each is
 * written again only after something its reader does next - its next arrival,
 * or its return - which the clear precedes, so it races with nothing a valid
 * program does. What it buys is the invalid one's: a slot or a word nobody
 * wrote again since reads as nothing, and the check that reads it ends the job
 * naming both leads. A lane is shared by every team its thread leads, and a
 * header names no team, so without the clear a header another team's
 * collective left - same epoch, same piece, same root and bytes, as two teams'
 * first collectives can be - would pass for this one's where the nodes took a
 * collective's class differently (the adversarial read of step E's first
 * part: a broadcast of 1,000 bytes on the parent and 1,500 on the child
 * returned another team's data with status 0, where step D's tree named it).
 */
static void cleared(unsigned char *read, size_t bytes)
{
    memset(read, 0, bytes);
}

/*
 * The parent's grant, once grant_in says it is in: its words, put before the
 * add the seq_cst load saw, name the parent's lane, and must name this
 * collective - its epoch, its identity and its bytes - or the job ends naming
 * both leads, a hop before a piece's header would.
 */
static void take_grant(const struct pieces *pieces, const struct lead_tree *tree,
                       struct lanes *lanes)
{
    struct grant grant;
    memcpy(&grant, internal(grant_words(pieces->number)), sizeof grant);
    cleared(internal(grant_words(pieces->number)), sizeof grant);
    if (grant.epoch != pieces->epoch || grant.collective != pieces->collective ||
        grant.bytes != pieces->bytes) {
        char theirs[160], mine[160];
        describe_collective(grant.epoch, grant.collective, theirs, sizeof theirs);
        describe_collective(pieces->epoch, pieces->collective, mine, sizeof mine);
        osoe_fatal("%s: the grant from node %u's lead (PE %u) is for %s, %llu bytes, and this "
                   "node's lead (PE %u) is in %s, %zu bytes: PEs called different collectives, "
                   "or the same one with different arguments", pieces->routine,
                   osoe_node_of(&osoe.job, tree->parent), tree->parent, theirs,
                   (unsigned long long)grant.bytes, osoe.job.me, mine, pieces->bytes);
    }
    if (grant.lane >= OSOE_MAX_THREADS_HIGHEST)
        osoe_fatal("%s: the grant from node %u's lead (PE %u) names lane %u, past any PE's %u",
                   pieces->routine, osoe_node_of(&osoe.job, tree->parent), tree->parent,
                   grant.lane, (unsigned)OSOE_MAX_THREADS_HIGHEST);
    lanes->parent = grant.lane;
}

/*
 * Each child's lane, read as this lead takes the child's first piece of a large
 * collective - the arrival children_sent saw, which the child made after
 * putting its lane word - and kept for the collective (the file's comment).
 * Not later: once this lead's last piece has gone up the collective can end
 * elsewhere, and a node that is this lead's child at the position in the
 * team's next collective, in a tree rotated to another root, puts its own lane
 * word there (tools/modelcheck/lane_model.py, release_lane_late). The word
 * names the collective's epoch, or the job ends.
 */
static void take_child_lanes(const struct pieces *pieces, const struct lead_tree *tree,
                             struct lanes *lanes)
{
    for (uint32_t c = 0; c < tree->children; c++) {
        struct lane_word word;
        memcpy(&word, internal(lane_word(pieces->number, c)), sizeof word);
        cleared(internal(lane_word(pieces->number, c)), sizeof word);
        if (word.epoch != pieces->epoch || word.lane >= OSOE_MAX_THREADS_HIGHEST)
            osoe_fatal("%s: the lane word of node %u's lead (PE %u), this node's child at "
                       "position %u, says epoch %u and lane %u, and this node's lead (PE %u) "
                       "is in epoch %u's collective: %s", pieces->routine,
                       osoe_node_of(&osoe.job, tree->child[c]), tree->child[c], c, word.epoch,
                       word.lane, osoe.job.me, pieces->epoch,
                       word.epoch == 0 ? "no word was put, so the child took this collective "
                                         "as small - PEs called it with counts on either side "
                                         "of OSOE_SMALL_COLLECTIVE_BYTES - or is in another"
                                       : "PEs called different collectives");
        lanes->child[c] = word.lane;
    }
}

/*
 * Piece `number` from every child, once children_sent says it is in: check
 * the header in the position's slot for the piece and combine what follows it
 * into `at`. Position by position, in order, so that every run combines alike
 * (§5.8 rule 3). A broadcast's children put nothing: their arrival says only
 * that their release slot for this piece is free.
 */
static void combine_children(const struct pieces *pieces, const struct lanes *lanes,
                             const struct lead_tree *tree, const struct piece_header *header,
                             uint64_t number, unsigned char *at)
{
    if (!pieces->combine)
        return;
    for (uint32_t c = 0; c < tree->children; c++) {
        unsigned char *slot = internal(own_slot(pieces, lanes, c, number));
        check_piece(slot, header, "a child's partial result", tree->child[c], pieces->routine);
        pieces->combine(at, slot + HEADER_BYTES, header->bytes / pieces->width);
        cleared(slot, HEADER_BYTES);
    }
}

/* Piece `number`'s release, once release_sent says it is in: check the header
 * in the release slot for the piece, and copy what follows it out to `at`. */
static void copy_release(const struct pieces *pieces, const struct lanes *lanes,
                         const struct lead_tree *tree, const struct piece_header *header,
                         uint64_t number, unsigned char *at)
{
    unsigned char *slot =
        internal(own_slot(pieces, lanes, osoe.bootstrap.collective_fanout, number));
    check_piece(slot, header, "the result from the parent", tree->parent, pieces->routine);
    memcpy(at, slot + HEADER_BYTES, header->bytes);
    cleared(slot, HEADER_BYTES);
}

/*
 * Piece `number` up: the partial result in `at` into the parent's slot for
 * this node's position and the piece - the team number's small slot, or the
 * parent's lane's row - behind its header; a broadcast's goes up empty. Before
 * a large collective's first, this thread's lane into the position's lane
 * word, for the releases. Then one more on the parent's arrival counter for
 * the position, fenced, so that the add lands after the bytes (invariant 5).
 */
static void to_parent(const struct pieces *pieces, const struct lanes *lanes,
                      const struct lead_tree *tree, const struct piece_header *header,
                      uint64_t number, const unsigned char *at)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    bool small = pieces->class == OSOE_COLLECTIVE_SMALL;
    if (!small && number == 0) {
        struct lane_word word = { pieces->epoch, lanes->own };
        osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                           lane_word(pieces->number, tree->position), &word, sizeof word,
                           OSOE_TRANSPORT_MORE);
    }
    if (pieces->combine) {
        uint64_t slot = small ? small_slot(pieces->number, tree->position)
                              : lane_slot(lanes->parent, tree->position, number);
        osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL, slot, header,
                           HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL, slot + HEADER_BYTES,
                           at, header->bytes, OSOE_TRANSPORT_MORE);
    }
    osoe_transport_rmw(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                       arrival_counter(pieces->number, tree->position), OSOE_RMW_ADD, 8, 1, 0,
                       OSOE_TRANSPORT_FENCED);
}

/* Piece `number`'s release down, from `at`: into each child's release slot for
 * the piece - the team number's small one, or the child's lane's release row -
 * behind its header, then one more on the child's release counter, fenced
 * after the bytes. */
static void to_children(const struct pieces *pieces, const struct lanes *lanes,
                        const struct lead_tree *tree, const struct piece_header *header,
                        uint64_t number, const unsigned char *at)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    for (uint32_t c = 0; c < tree->children; c++) {
        uint64_t slot = pieces->class == OSOE_COLLECTIVE_SMALL
                            ? small_slot(pieces->number, fanout)
                            : lane_slot(lanes->child[c], fanout, number);
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL, slot, header,
                           HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           slot + HEADER_BYTES, at, header->bytes, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           release_counter(pieces->number), OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (c + 1 < tree->children ? OSOE_TRANSPORT_MORE : 0));
    }
}

/*
 * What a lead waits for when neither of its inputs is ready: its children's
 * copies of piece `up`, if the window lets it send that piece - and, for a
 * large reduction's first piece, its parent's grant - or its next piece down:
 * piece `down`'s release from its parent, or at the root every child's copy
 * of piece `down`. The team's counters are contiguous, an arrival counter per
 * child position, the release counter and the grant counter, so one watch
 * covers them all, and no put writes inside it - the release counter's line
 * also holds collect's spare words, which a node-mate writes with plain stores
 * that wake nobody (internal-region.h).
 */
struct awaited {
    const struct generic_team *team_phase;
    const struct pieces *pieces;
    const struct lead_tree *tree;
    bool sending;                       /* the window lets piece `up` go up */
    bool granting;                      /* and the grant it waits for is not yet taken */
    uint64_t up;
    uint64_t down;
};

static bool next_down_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                           const struct lead_tree *tree, uint64_t down)
{
    return tree->parent == UINT32_MAX ? children_sent(team_phase, pieces, tree, down)
                                      : release_sent(team_phase, pieces, down);
}

/*
 * What a lead waiting so says it waits for, for osoe-stat (D1): the first child
 * whose copy of piece `up` is missing, or else the parent, whose grant or
 * release of piece `down` is; at the root, which sends nothing up, the first
 * child missing piece `down`. Said each time the wait finds it still waiting,
 * after every wake, so that the node it names is one missing then, and not the
 * first missing when the wait began - the root of a tree waits for every child
 * in one wait, and one that has since come would be named for ever.
 */
static void say_awaited(const struct awaited *a)
{
    const struct lead_tree *tree = a->tree;
    bool root = tree->parent == UINT32_MAX;
    uint64_t piece = a->sending ? a->up : a->down;
    uint32_t missing = a->sending || root ? child_missing(a->team_phase, a->pieces, tree, piece)
                                          : tree->children;
    if (missing < tree->children)
        waits_for(a->team_phase, OSOE_DATA_WAIT_PIECE, tree->child[missing], piece);
    else if (a->sending && a->granting)
        waits_for(a->team_phase, OSOE_DATA_WAIT_GRANT, tree->parent, 0);
    else
        waits_for(a->team_phase, OSOE_DATA_WAIT_RELEASE, tree->parent, a->down);
}

static bool awaited_holds(const void *context)
{
    const struct awaited *a = context;
    bool holds = (a->sending && children_sent(a->team_phase, a->pieces, a->tree, a->up) &&
                  (!a->granting || grant_in(a->team_phase, a->pieces))) ||
                 next_down_sent(a->team_phase, a->pieces, a->tree, a->down);
    if (!holds)
        say_awaited(a);
    return holds;
}

/*
 * The whole buffer through `tree`, D pieces in flight: pipeline_model.py's
 * node loop (the file's comment). A lead keeps `up`, the next piece to send
 * up, and `down`, the next release to consume, and takes whichever is ready:
 * piece `up`, when the window allows it - a piece left to send, and fewer
 * than D sent whose release it does not hold - and every child's copy of it
 * is in, combined and sent on to the parent; or release `down`, copied out,
 * forwarded to every child, and only then counted, since the next piece up is
 * what tells the parent the release slot is free. Its children's piece first
 * when both are; a wait for either when neither is. The root sends nothing
 * up: for each piece it waits for every child's copy, combines, and forwards
 * - its buffer is the result.
 *
 * It waits for either, and never for its children alone, because a middle
 * lead that did would hold release `down` unconsumed while its children,
 * who need that release to open their own windows, could not send it the
 * piece it waits for: release k + 1 would then leave it no sooner than a round
 * trip to the bottom of its subtree after release k, whatever D - on a tree of
 * depth h, about h / (h - 1) of one piece at a time's speed rather than D times
 * (design §5.8; the phase 1 step 5 slice 5 review).
 *
 * A large collective (the file's comment) runs through lanes: this thread's,
 * bound to it since its entry; a reduction's child sends its first piece up
 * only once it holds its parent's grant, which names the parent's lane; every
 * lead takes its children's lanes from their lane words as it takes their
 * first piece, and a child puts its own lane's word before its first add. A
 * small one is a single piece through the team number's own small slots.
 *
 * **The tree and the window are the call's** (v0.2 step 2; shape_of): a team's
 * collectives are not concurrent (§9.10), so every node computes the same
 * shape for the same call from the same inputs, and the counters' cumulative
 * bases are per child position, whatever child held the position in an
 * earlier call - a position this call's tree leaves unused neither advances
 * nor stands in for another, as with a broadcast's rotated tree before it.
 *
 * At the end, the bases advance by what this tree says each counter received,
 * and the most pieces this lead had in flight at once goes into the team's
 * tree state (node.h): min(D, pieces) at a leaf, which has no children to wait
 * for and fills its window before it looks for a release; between 1 and that
 * at a middle lead, as the schedule has it; 0 at the root, which sends nothing
 * up. With it, the collective's class and, for a large one, the lane.
 */
static void in_pieces(struct generic_team *team_phase, const struct pieces *pieces,
                      const struct lead_tree *tree, struct osoe_collective_shape shape)
{
    uint64_t piece_count = (pieces->bytes + pieces->bytes_per_piece - 1) / pieces->bytes_per_piece;
    uint64_t window = shape.window;
    bool root = tree->parent == UINT32_MAX;
    bool large = pieces->class == OSOE_COLLECTIVE_LARGE;
    /* A large reduction's child sends nothing up before its parent's grant;
     * a broadcast's pieces go up empty, and need none (E4). */
    bool needs_grant = large && pieces->combine && !root;
    struct lanes lanes = { .own = large ? own_lane() : NO_LANE, .parent = NO_LANE };
    uint64_t up = 0, down = 0, most = 0;
    while (down < piece_count) {
        bool sending = !root && up < piece_count && up < down + window;
        if (sending && children_sent(team_phase, pieces, tree, up) &&
            (!needs_grant || lanes.parent != NO_LANE || grant_in(team_phase, pieces))) {
            if (needs_grant && lanes.parent == NO_LANE)
                take_grant(pieces, tree, &lanes);
            if (large && up == 0)
                take_child_lanes(pieces, tree, &lanes);
            struct piece_header header = header_of(pieces, up);
            unsigned char *at = piece_at(pieces, up);
            combine_children(pieces, &lanes, tree, &header, up, at);
            to_parent(pieces, &lanes, tree, &header, up, at);
            osoe_perturb();
            up++;
            if (up - down > most)
                most = up - down;
            continue;
        }
        if (next_down_sent(team_phase, pieces, tree, down)) {
            struct piece_header header = header_of(pieces, down);
            unsigned char *at = piece_at(pieces, down);
            if (root) {
                if (large && down == 0)
                    take_child_lanes(pieces, tree, &lanes);
                combine_children(pieces, &lanes, tree, &header, down, at);
            } else {
                copy_release(pieces, &lanes, tree, &header, down, at);
            }
            to_children(pieces, &lanes, tree, &header, down, at);
            osoe_perturb();
            down++;
            continue;
        }
        /* Neither: wait for either, saying whose word it waits on (D1;
         * say_awaited). */
        struct awaited awaited = { team_phase, pieces, tree, sending,
                                   needs_grant && lanes.parent == NO_LANE, up, down };
        uint64_t first = arrival_counter(pieces->number, 0);
        osoe_region_wait_condition(pieces->routine, OSOE_REGION_INTERNAL, first,
                                   (size_t)(grant_counter(pieces->number) + 8 - first),
                                   awaited_holds, &awaited, OSOE_BLOCKED_DETAIL_EPOCH,
                                   pieces->epoch);
    }
    for (uint32_t c = 0; c < tree->children; c++)
        team_phase->arrival_base[c] += piece_count;
    if (!root)
        team_phase->release_base += piece_count;
    if (needs_grant)
        team_phase->grant_base++;
    /* Single-writer publication for osoe-stat and the tests (§3.16), and
     * before the lead bumps the team's `released`, which every member loads
     * with acquire before it returns: so once its call has returned, a member
     * of this node reads this collective's counts. */
    atomic_store_explicit(&team_phase->published->most_pieces_in_flight, (uint32_t)most,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->collective_class, (uint32_t)pieces->class,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_fanout, shape.fanout,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_window, shape.window,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_children, tree->children,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->algorithm, (uint32_t)OSOE_SHAPE_TREE,
                          memory_order_release);
    if (large)
        atomic_store_explicit(&team_phase->published->lane, lanes.own, memory_order_release);
}

/* ---- Träff's reduction (the file's comment) ------------------------------ */

/*
 * A Träff's piece's number in its header: its round in the top five bits and
 * its index in the round below (the file's comment). Every lead cuts a round
 * alike, so both sides of a round number its pieces alike, whatever an index
 * past 2^27 wraps to.
 */
enum { ROUND_SHIFT = 27 };
_Static_assert(2 * OSOE_INTERNAL_CIRCULANT_ROWS_MOST <= 1 << (32 - ROUND_SHIFT),
               "a header's top bits hold every round");

static uint32_t round_piece_number(uint32_t round, uint64_t index)
{
    return round << ROUND_SHIFT | (uint32_t)(index & ((UINT32_C(1) << ROUND_SHIFT) - 1));
}

/* How many pieces of `per_piece` elements the intervals take, each cut on its
 * own: a round's sent intervals, or its received ones, which are the same
 * elements on both sides of the round. */
static uint64_t interval_pieces(const uint64_t first[2], const uint64_t end[2],
                                uint64_t per_piece)
{
    return (end[0] - first[0] + per_piece - 1) / per_piece +
           (end[1] - first[1] + per_piece - 1) / per_piece;
}

/* Piece `index` of those intervals: its first element and how many. */
static void interval_piece(const uint64_t first[2], const uint64_t end[2], uint64_t per_piece,
                           uint64_t index, uint64_t *at, uint64_t *elements)
{
    uint64_t in_first = (end[0] - first[0] + per_piece - 1) / per_piece;
    int i = index < in_first ? 0 : 1;
    *at = first[i] + (index - (i ? in_first : 0)) * per_piece;
    uint64_t left = end[i] - *at;
    *elements = left < per_piece ? left : per_piece;
}

/*
 * One round of a Träff's reduction as its lead runs it (in_rounds): this
 * lead's part in it, the leads it sends to and receives from and the lane its
 * receiver lends, its pieces each way and how many have gone and come, and the
 * bases its waits count from - the credits this lead held as the round began,
 * by its own count, and its row's arrivals.
 */
struct round_flight {
    const struct pieces *pieces;
    struct osoe_circulant_round round;
    uint32_t index;                     /* the round, 0 to twice the rows - 1 */
    int to, from;                       /* the leads' ranks */
    uint32_t to_lane;
    uint64_t per_piece;                 /* elements */
    uint64_t window;
    uint64_t sends, receives;           /* pieces */
    uint64_t sent, taken;
    uint64_t credit_base;
    uint64_t arrival_base;
};

/*
 * Where piece `index` of a round of row `row` goes in a lane: the lane's slots
 * in order, row by row, cut into blocks of the call's window, round k's the
 * k-th - a round takes as many of the lane's rows as its rounds leave it
 * (osoe_shape_choose), and piece j its block's slot j mod the window, as the
 * tree's pieces take their row's slot j mod the capacity.
 */
static uint64_t round_slot(uint32_t lane, uint32_t row, uint64_t window, uint64_t index)
{
    return lane_slot(lane, 0, 0) + ((uint64_t)row * window + index % window) *
                                       OSOE_INTERNAL_SLOT_BYTES;
}

/* Piece `index`'s header, of `elements` elements. */
static struct piece_header round_header(const struct round_flight *f, uint64_t index,
                                        uint64_t elements)
{
    return (struct piece_header){
        .epoch = f->pieces->epoch,
        .piece = round_piece_number(f->index, index),
        .collective = f->pieces->collective,
        .bytes = (uint32_t)(elements * f->pieces->width),
    };
}

/* A Träff's header as a reader wants it: "round 3's piece 5 (16384 bytes) of
 * epoch 7's sum of 4-byte signed integers". */
static void describe_round_piece(const struct piece_header *h, char *out, size_t size)
{
    if (h->epoch == 0) {
        snprintf(out, size, "nothing: no piece was put there");
        return;
    }
    char collective[160];
    describe_collective(h->epoch, h->collective, collective, sizeof collective);
    snprintf(out, size, "round %u's piece %u (%u bytes) of %s", h->piece >> ROUND_SHIFT,
             h->piece & ((UINT32_C(1) << ROUND_SHIFT) - 1), h->bytes, collective);
}

/* The slot at `slot` holds the piece `expected` says, or the job ends naming
 * the round's sender. */
static void check_round_piece(const unsigned char *slot, const struct piece_header *expected,
                              const struct round_flight *f)
{
    struct piece_header got;
    memcpy(&got, slot, sizeof got);
    if (got.epoch == expected->epoch && got.piece == expected->piece &&
        got.collective == expected->collective && got.bytes == expected->bytes)
        return;
    char want[256], found[256];
    describe_round_piece(expected, want, sizeof want);
    describe_round_piece(&got, found, sizeof found);
    osoe_fatal("%s: a piece of the %s, from node %u's lead (PE %d), should be %s, and is %s: "
               "PEs called different collectives, or the same one with different arguments",
               f->pieces->routine, f->round.combines ? "reduce-scatter" : "allgather",
               osoe_node_of(&osoe.job, (uint32_t)f->from), f->from, want, found);
}

/*
 * Whether the window lets piece `sent` go: one of the round's first W, or the
 * credit for the piece W before it is in - the credit counter against the
 * round's base, as at least (the file's comment).
 */
static bool round_may_send(const struct round_flight *f)
{
    if (f->sent >= f->sends)
        return false;
    if (f->sent < f->window)
        return true;
    return osoe_transport_load(OSOE_REGION_INTERNAL, circulant_credit_counter(f->pieces->number),
                               8) >= f->credit_base + f->sent - f->window + 1;
}

/* Whether piece `taken` is in: the row's arrival counter against its base, as
 * at least. The load is seq_cst (transport.h), and so acquire: the bytes its
 * sender put before its fenced add are there to read. */
static bool round_may_take(const struct round_flight *f)
{
    return f->taken < f->receives &&
           osoe_transport_load(OSOE_REGION_INTERNAL,
                               circulant_arrival_counter(f->pieces->number, f->round.row), 8) >=
               f->arrival_base + f->taken + 1;
}

/* What a lead waits for when neither is ready: either. The reduce-scatter's
 * counters are contiguous - an arrival counter a row, the credit counter and
 * both grant counters - so one watch covers them, and no put writes inside
 * it (internal-region.h). */
static bool round_awaited_holds(const void *context)
{
    const struct round_flight *f = context;
    return round_may_send(f) || round_may_take(f);
}

/*
 * Piece `sent` of the round to its receiver: before a reduce-scatter round's
 * first, this thread's lane into the field for the row of the receiver's
 * senders' lanes line, for the allgather back; then the piece behind its
 * header into the receiver's lane, in the row's slot for it, and one more on
 * the receiver's arrival counter for the row, fenced, so that the add lands
 * after the bytes (invariant 5).
 */
static void round_send(struct round_flight *f, uint32_t own)
{
    const struct pieces *pieces = f->pieces;
    struct osoe_transport_context *context = osoe_transport_context_default();
    uint64_t at, elements;
    interval_piece(f->round.send_first, f->round.send_end, f->per_piece, f->sent, &at, &elements);
    struct piece_header header = round_header(f, f->sent, elements);
    if (f->round.combines && f->sent == 0) {
        struct lane_word word = { pieces->epoch, own };
        osoe_transport_put(context, f->to, OSOE_REGION_INTERNAL,
                           circulant_senders(pieces->number) + (uint64_t)f->round.row * sizeof word,
                           &word, sizeof word, OSOE_TRANSPORT_MORE);
    }
    uint64_t slot = round_slot(f->to_lane, f->round.row, f->window, f->sent);
    osoe_transport_put(context, f->to, OSOE_REGION_INTERNAL, slot, &header, HEADER_BYTES,
                       OSOE_TRANSPORT_MORE);
    osoe_transport_put(context, f->to, OSOE_REGION_INTERNAL, slot + HEADER_BYTES,
                       pieces->buffer + at * pieces->width, header.bytes, OSOE_TRANSPORT_MORE);
    osoe_transport_rmw(context, f->to, OSOE_REGION_INTERNAL,
                       circulant_arrival_counter(pieces->number, f->round.row), OSOE_RMW_ADD, 8,
                       1, 0, OSOE_TRANSPORT_FENCED);
    f->sent++;
}

/*
 * The lane of the lead that sends this one a reduce-scatter round, from the
 * field for its row of the senders' lanes line, read as this lead takes the
 * round's first piece - the arrival that shows it written - and cleared; kept
 * for the allgather's round back to that lead (the file's comment). It names
 * the reduction's epoch, or the job ends.
 */
static void take_sender_lane(const struct round_flight *f, uint32_t *sender_lanes)
{
    const struct pieces *pieces = f->pieces;
    struct lane_word word;
    unsigned char *field =
        internal(circulant_senders(pieces->number) + (uint64_t)f->round.row * sizeof word);
    memcpy(&word, field, sizeof word);
    cleared(field, sizeof word);
    if (word.epoch != pieces->epoch || word.lane >= OSOE_MAX_THREADS_HIGHEST)
        osoe_fatal("%s: the senders' lane field of row %u, which node %u's lead (PE %d) puts "
                   "as it sends this node's lead (PE %u) a round of epoch %u's reduction, says "
                   "epoch %u and lane %u: %s", pieces->routine, f->round.row,
                   osoe_node_of(&osoe.job, (uint32_t)f->from), f->from, osoe.job.me,
                   pieces->epoch, word.epoch, word.lane,
                   word.epoch == 0 ? "no field was put, so the sender is in another collective"
                                   : "PEs called different collectives");
    sender_lanes[f->round.row] = word.lane;
}

/*
 * Piece `taken` of the round from its sender, once round_may_take says it is
 * in: with a reduce-scatter round's first, the sender's lane, for the
 * allgather back; then its header checked, what follows it combined into the
 * accumulator at the piece's place - or, in the allgather, copied there - on
 * this lead's own thread, and the header cleared; and, for one of the round's
 * first P - W pieces, its credit at the sender, which waits for it (the file's
 * comment). A plain add: the credit announces no bytes, and this lead's reads
 * of the slot come before it.
 */
static void round_take(struct round_flight *f, uint32_t own, uint32_t *sender_lanes)
{
    const struct pieces *pieces = f->pieces;
    if (f->round.combines && f->taken == 0)
        take_sender_lane(f, sender_lanes);
    uint64_t at, elements;
    interval_piece(f->round.receive_first, f->round.receive_end, f->per_piece, f->taken, &at,
                   &elements);
    struct piece_header header = round_header(f, f->taken, elements);
    unsigned char *slot = internal(round_slot(own, f->round.row, f->window, f->taken));
    check_round_piece(slot, &header, f);
    unsigned char *place = pieces->buffer + at * pieces->width;
    if (f->round.combines)
        pieces->combine(place, slot + HEADER_BYTES, (size_t)elements);
    else
        memcpy(place, slot + HEADER_BYTES, header.bytes);
    cleared(slot, HEADER_BYTES);
    f->taken++;
    if (f->receives > f->window && f->taken <= f->receives - f->window)
        osoe_transport_rmw(osoe_transport_context_default(), f->from, OSOE_REGION_INTERNAL,
                           circulant_credit_counter(pieces->number), OSOE_RMW_ADD, 8, 1, 0, 0);
}

/*
 * This lead's reduce-scatter grants, once its grant counter shows every one
 * of them (the file's comment): each round's field, put by that round's
 * receiver before the add the seq_cst load saw, must name this reduction - its
 * epoch, its identity and its bytes - and the lane the receiver lends, or the
 * job ends naming that receiver, a hop before a piece's header would. Read,
 * then cleared.
 */
static void take_round_grants(const struct generic_team *team_phase, const struct pieces *pieces,
                              uint32_t rows, uint32_t *granted)
{
    struct circulant_grant_field fields[OSOE_INTERNAL_CIRCULANT_ROWS_MOST];
    unsigned char *words = internal(circulant_reduce_scatter_grant_words(pieces->number));
    memcpy(fields, words, rows * sizeof fields[0]);
    cleared(words, rows * sizeof fields[0]);
    for (uint32_t k = 0; k < rows; k++) {
        const struct circulant_grant_field *field = &fields[k];
        bool same = field->epoch == pieces->epoch && field->collective == pieces->collective &&
                    field->bytes == pieces->bytes;
        if (same && field->lane != 0 && field->lane <= OSOE_MAX_THREADS_HIGHEST) {
            granted[k] = field->lane - 1;
            continue;
        }
        struct osoe_circulant_round round = osoe_circulant_round(
            team_phase->index, team_phase->count, k, pieces->bytes / pieces->width);
        uint32_t receiver = lead_of(pieces->team, round.to, pieces->routine);
        if (!same) {
            char theirs[160], mine[160];
            describe_collective(field->epoch, field->collective, theirs, sizeof theirs);
            describe_collective(pieces->epoch, pieces->collective, mine, sizeof mine);
            osoe_fatal("%s: the reduce-scatter grant of node %u's lead (PE %u), which this "
                       "node's lead (PE %u) sends round %u, is for %s, %llu bytes, and this "
                       "lead is in %s, %zu bytes: PEs called different collectives, or the "
                       "same one with different arguments", pieces->routine,
                       osoe_node_of(&osoe.job, receiver), receiver, osoe.job.me, k, theirs,
                       (unsigned long long)field->bytes, mine, pieces->bytes);
        }
        osoe_fatal("%s: the reduce-scatter grant of node %u's lead (PE %u), which this "
                   "node's lead (PE %u) sends round %u of epoch %u's reduction, names %s: "
                   "PEs called different collectives", pieces->routine,
                   osoe_node_of(&osoe.job, receiver), receiver, osoe.job.me, k,
                   pieces->epoch, field->lane == 0 ? "no lane" : "a lane past any PE's");
    }
}

/*
 * Träff's reduction among the team's leads, the call's window of pieces in
 * flight a round: circulant_model.py's lead program (the file's comment). Once
 * the grant counter shows every reduce-scatter grant of this lead's, its rounds
 * in order, and the allgather's once the allgather's grant counter shows every
 * one of its own. In each round the lead takes whichever is ready - its next
 * piece out, when the window lets it go, or its next piece in, once it has
 * arrived - the piece out first when both are, and waits for either when
 * neither is; the round ends when every piece has gone and come, and a
 * reduce-scatter round's end grants its receiver the row it emptied, for the
 * allgather back. The lead's accumulator, `pieces->buffer`, is read for what it
 * sends and combined or copied into for what it takes, on this thread alone:
 * what a round sends and what it takes are different blocks, and a round's
 * blocks are finished before it starts.
 *
 * At the end the bases advance by what this lead's counters received, and the
 * tree state says what the call took (node.h): the algorithm, the rounds it
 * made, counted, and the most pieces it had put in a round and not yet held
 * the credit for - at most the window, a count a test pins - with the class,
 * the lane and the shape, whose fan-out is the chooser's and whose children
 * are none.
 */
static void in_rounds(struct generic_team *team_phase, const struct pieces *pieces,
                      struct osoe_collective_shape shape)
{
    uint32_t nodes = team_phase->count, rows = osoe_circulant_rows(nodes);
    uint64_t count = pieces->bytes / pieces->width;
    uint64_t grants = team_phase->circulant_grant_base + rows;
    /* Every node computes the same shape for the call, and the chooser's
     * window leaves every round's block within a lane: a shape past it is a
     * bug, named before any put. Told by the functions that place the slots -
     * the end of the last round's last slot against where the next lane
     * begins - and not by the sizes they are meant to agree with: a lane's
     * rows the layout did not make room for would run into the next lane's. */
    uint64_t lane_start = lane_slot(0, 0, 0), lane_size = lane_slot(1, 0, 0) - lane_start;
    uint64_t blocks_size = shape.window == 0
                               ? UINT64_MAX
                               : round_slot(0, rows - 1, shape.window, shape.window - 1) +
                                     OSOE_INTERNAL_SLOT_BYTES - lane_start;
    if (blocks_size > lane_size)
        osoe_fatal("%s: Träff's %u rounds a half at a window of %u pieces need %llu bytes of "
                   "slots in a lane, which holds %llu", pieces->routine, rows, shape.window,
                   (unsigned long long)blocks_size, (unsigned long long)lane_size);
    uint32_t own = own_lane(), number = pieces->number;
    uint32_t granted[OSOE_INTERNAL_CIRCULANT_ROWS_MOST];
    uint32_t sender_lanes[OSOE_INTERNAL_CIRCULANT_ROWS_MOST];
    for (uint32_t k = 0; k < OSOE_INTERNAL_CIRCULANT_ROWS_MOST; k++)
        sender_lanes[k] = NO_LANE;
    /* Every grant on one counter, a round's receiver each: how many are
     * missing is all its word can say (D1). */
    wait_count(pieces->routine, team_phase, OSOE_DATA_WAIT_GRANT,
               circulant_reduce_scatter_grant_counter(number), grants, pieces->epoch);
    take_round_grants(team_phase, pieces, rows, granted);
    uint64_t credit_base = team_phase->circulant_credit_base, most = 0;
    uint32_t made = 0;
    for (uint32_t index = 0; index < 2 * rows; index++) {
        if (index == rows)
            wait_count(pieces->routine, team_phase, OSOE_DATA_WAIT_GRANT,
                       circulant_allgather_grant_counter(number), grants, pieces->epoch);
        struct round_flight f = {
            .pieces = pieces,
            .round = osoe_circulant_round(team_phase->index, nodes, index, count),
            .index = index,
            .per_piece = pieces->bytes_per_piece / pieces->width,
            .window = shape.window,
            .credit_base = credit_base,
        };
        f.to = (int)lead_of(pieces->team, f.round.to, pieces->routine);
        f.from = (int)lead_of(pieces->team, f.round.from, pieces->routine);
        f.to_lane = f.round.combines ? granted[f.round.row] : sender_lanes[f.round.row];
        f.sends = interval_pieces(f.round.send_first, f.round.send_end, f.per_piece);
        f.receives = interval_pieces(f.round.receive_first, f.round.receive_end, f.per_piece);
        f.arrival_base = team_phase->circulant_arrival_base[f.round.row];
        while (f.sent < f.sends || f.taken < f.receives) {
            if (round_may_send(&f)) {
                round_send(&f, own);
                uint64_t credits = osoe_transport_load(OSOE_REGION_INTERNAL,
                                                       circulant_credit_counter(number), 8) -
                                   credit_base;
                if (f.sent > credits && f.sent - credits > most)
                    most = f.sent - credits;
                osoe_perturb();
                continue;
            }
            if (round_may_take(&f)) {
                round_take(&f, own, sender_lanes);
                osoe_perturb();
                continue;
            }
            /* Neither: the sender's next piece, if it has one to come, or else
             * the receiver's credit for the next piece out (D1). */
            if (f.taken < f.receives)
                waits_for(team_phase, OSOE_DATA_WAIT_PIECE, (uint32_t)f.from, index);
            else
                waits_for(team_phase, OSOE_DATA_WAIT_CREDIT, (uint32_t)f.to, index);
            uint64_t first = circulant_arrival_counter(number, 0);
            osoe_region_wait_condition(
                pieces->routine, OSOE_REGION_INTERNAL, first,
                (size_t)(circulant_reduce_scatter_grant_counter(number) + 8 - first),
                round_awaited_holds, &f, OSOE_BLOCKED_DETAIL_EPOCH, pieces->epoch);
        }
        credit_base += f.sends > f.window ? f.sends - f.window : 0;
        team_phase->circulant_arrival_base[f.round.row] += f.receives;
        if (f.round.combines)
            osoe_transport_rmw(osoe_transport_context_default(), f.to, OSOE_REGION_INTERNAL,
                               circulant_allgather_grant_counter(number), OSOE_RMW_ADD, 8, 1, 0,
                               0);
        made++;
    }
    team_phase->circulant_credit_base = credit_base;
    team_phase->circulant_grant_base = grants;
    /* Single-writer publication for osoe-stat and the tests (§3.16), before the
     * lead bumps the team's `released`, as in_pieces publishes its counts. */
    atomic_store_explicit(&team_phase->published->most_pieces_in_flight, (uint32_t)most,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->collective_class,
                          (uint32_t)OSOE_COLLECTIVE_LARGE, memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_fanout, shape.fanout,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_window, shape.window,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_children, 0, memory_order_release);
    atomic_store_explicit(&team_phase->published->circulant_rounds, made, memory_order_release);
    atomic_store_explicit(&team_phase->published->algorithm, (uint32_t)OSOE_SHAPE_CIRCULANT,
                          memory_order_release);
    atomic_store_explicit(&team_phase->published->lane, own, memory_order_release);
}

/*
 * A Träff's reduction's grants (the file's comment), sent as its local lead
 * enters it, before the intranode phase: this thread's lane is the reduction's
 * from here to its return, and the sender of each of its reduce-scatter rounds
 * - round k's, the lead s_k positions before it - is told so: the reduction's
 * epoch, identity and bytes and the lane, put into the sender's field for the
 * round, which this lead alone writes, then one more on its grant counter,
 * fenced after it.
 */
static void circulant_grants(const struct generic_team *team_phase, uint32_t team,
                             uint32_t epoch, uint32_t collective, uint64_t bytes, uint64_t count,
                             const char *routine)
{
    uint32_t number = number_of(team), rows = osoe_circulant_rows(team_phase->count);
    struct circulant_grant_field field = { epoch, collective, bytes, own_lane() + 1, 0 };
    uint64_t words = circulant_reduce_scatter_grant_words(number);
    struct osoe_transport_context *context = osoe_transport_context_default();
    for (uint32_t k = 0; k < rows; k++) {
        struct osoe_circulant_round round =
            osoe_circulant_round(team_phase->index, team_phase->count, k, count);
        int sender = (int)lead_of(team, round.from, routine);
        osoe_transport_put(context, sender, OSOE_REGION_INTERNAL, words + k * sizeof field,
                           &field, sizeof field, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, sender, OSOE_REGION_INTERNAL,
                           circulant_reduce_scatter_grant_counter(number), OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED | (k + 1 < rows ? OSOE_TRANSPORT_MORE : 0));
    }
}

/*
 * The grants of a large reduction (the file's comment), sent as its local lead
 * enters it, before the intranode phase: this thread's lane is the
 * reduction's from here to its return, and each child of the reduction's tree
 * is told so - the grant's words put into its grant line for the team, then one
 * more on its grant counter, fenced after them - or, where the call takes
 * Träff's, each sender of its reduce-scatter (circulant_grants). A small
 * reduction needs none. It waits for nothing of the collective's.
 */
void osoe_internode_generic_reduce_enter(uint32_t team, uint32_t epoch, size_t count,
                                         enum osoe_reduction_operation operation,
                                         struct osoe_element element, const char *routine)
{
    uint64_t bytes = (uint64_t)count * element.width;
    if (osoe_collective_class(bytes, osoe.bootstrap.small_collective_bytes) ==
        OSOE_COLLECTIVE_SMALL)
        return;
    struct generic_team *team_phase = registered_team(team, routine);
    /* The call's shape and tree, which osoe_internode_generic_reduce finds
     * again from the same arguments: the leads granted here are the ones it
     * waits for. */
    struct osoe_collective_shape shape = shape_of(OSOE_SHAPE_REDUCE, team_phase, bytes);
    team_phase->entered = epoch;
    if (shape.algorithm == OSOE_SHAPE_CIRCULANT) {
        circulant_grants(team_phase, team, epoch, reduction_identity(operation, element), bytes,
                         count, routine);
        return;
    }
    struct lead_tree call_tree;
    find_lead_tree(team_phase, team, 0, shape.fanout, &call_tree, routine);
    const struct lead_tree *tree = &call_tree;
    uint32_t number = number_of(team);
    struct grant grant = { epoch, reduction_identity(operation, element), bytes, own_lane(), 0 };
    struct osoe_transport_context *context = osoe_transport_context_default();
    for (uint32_t c = 0; c < tree->children; c++) {
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           grant_words(number), &grant, sizeof grant, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           grant_counter(number), OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (c + 1 < tree->children ? OSOE_TRANSPORT_MORE : 0));
    }
}

void osoe_internode_generic_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                                   enum osoe_reduction_operation operation,
                                   struct osoe_element element, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    osoe_combine_function combine = osoe_combine_lookup(operation, element);
    if (!combine || element.width > OSOE_INTERNAL_PIECE_BYTES)
        osoe_fatal("%s: no reduction %d over elements of kind %d and width %u", routine,
                   (int)operation, (int)element.kind, element.width);
    size_t bytes = count * element.width;
    enum osoe_collective_class class =
        osoe_collective_class(bytes, osoe.bootstrap.small_collective_bytes);
    if (class == OSOE_COLLECTIVE_LARGE && team_phase->entered != epoch)
        osoe_fatal("%s: team %u's large reduction at epoch %u was not entered first "
                   "(osoe_internode_reduce_enter), so no grant left for its children",
                   routine, number_of(team), epoch);
    struct pieces pieces = { team, number_of(team), epoch, reduction_identity(operation, element),
                             class, buffer, bytes, bytes_per_piece(class, bytes, element.width),
                             element.width, combine, routine };
    struct osoe_collective_shape shape = shape_of(OSOE_SHAPE_REDUCE, team_phase, bytes);
    if (shape.algorithm == OSOE_SHAPE_CIRCULANT) {
        in_rounds(team_phase, &pieces, shape);
        return;
    }
    struct lead_tree tree;
    find_lead_tree(team_phase, team, 0, shape.fanout, &tree, routine);
    in_pieces(team_phase, &pieces, &tree, shape);
}

/* Member r's place in a gather's result (internode.h). */
static uint64_t place_of(const uint64_t *places, size_t block, uint64_t r)
{
    return places ? places[r] : r * (uint64_t)block;
}

/* How many shares a lead sends at step `step` of a gather over `nodes` nodes:
 * min(2^step, nodes - 2^step) - 2^step at every step but the last. */
static uint32_t shares_at(uint32_t nodes, uint32_t step)
{
    uint64_t power = UINT64_C(1) << step;
    return (uint32_t)(power < nodes - power ? power : nodes - power);
}

/*
 * The position of node `node` in the team's node set: its number for a team
 * spanning every node; else a binary search of the set by its leads' nodes,
 * O(log N) lookups of a lead (osoe_internode_team_lead) and of its node
 * (job.h), O(log R) each. Only a team's first gather asks, whose intervals
 * every later gather reuses.
 */
static uint32_t position_of(const struct generic_team *team_phase, uint32_t team, uint32_t node,
                            const char *routine)
{
    uint32_t count = team_phase->count, low = node;
    if (count < osoe.job.node_count) {
        uint32_t high = count;
        low = 0;
        while (low < high) {
            uint32_t middle = low + (high - low) / 2;
            if (osoe_node_of(&osoe.job, lead_of(team, middle, routine)) < node)
                low = middle + 1;
            else
                high = middle;
        }
        if (low < count && osoe_node_of(&osoe.job, lead_of(team, low, routine)) != node)
            low = count;
    }
    if (low >= count)
        osoe_fatal("%s: team %u has a member on node %u, which is not in its node set", routine,
                   number_of(team), node);
    return low;
}

/* Team ranks `first` up to `end` at the end of `list`, merged with its last
 * interval when the two touch. */
static void add_interval(struct interval_list *list, uint32_t first, uint32_t end, const char *routine)
{
    if (list->count > 0 && list->intervals[list->count - 1].end == first) {
        list->intervals[list->count - 1].end = end;
        return;
    }
    if (list->count == list->capacity) {
        uint32_t capacity = list->capacity ? 2 * list->capacity : 4;
        struct member_interval *grown = realloc(list->intervals, sizeof *grown * capacity);
        if (!grown)
            osoe_fatal("%s: out of memory for a gather's %u intervals of members", routine, capacity);
        list->intervals = grown;
        list->capacity = capacity;
    }
    list->intervals[list->count++] = (struct member_interval){ first, end };
}

/* A team's intervals as its first gather's walk finds them: the team, its lead's
 * state, the members found on this node, and each position's steps so far,
 * a bit a step, so that its (node, step) pairs are counted as they join. */
struct member_walk {
    struct generic_team *team_phase;
    uint32_t team;
    uint32_t here;
    uint64_t *steps_joined;
    const char *routine;
};

/* The members `first` to `first` + `count` - 1, at position `position`, join
 * step `step`'s list. */
static void join(struct member_walk *walk, uint32_t position, uint32_t step, uint32_t first,
                 uint32_t count)
{
    add_interval(&walk->team_phase->step_intervals[step], first, first + count, walk->routine);
    uint64_t bit = UINT64_C(1) << step;
    if (!(walk->steps_joined[position] & bit)) {
        walk->steps_joined[position] |= bit;
        walk->team_phase->shares++;
    }
}

/*
 * Team ranks `first` to `first` + `count` - 1, all on node `node`: they join
 * the list of every step whose shares include the node's - the steps s at
 * which the node is fewer than shares_at(s) positions after this one: from
 * the first s with 2^s past that distance to the last but one, and the last if
 * its fewer shares reach. So each stretch costs O(1) and the lists it joins,
 * and a binary search of the node set where positions are not node numbers.
 */
static void add_members(struct member_walk *walk, uint32_t first, uint32_t count, uint32_t node)
{
    struct generic_team *team_phase = walk->team_phase;
    uint32_t n = team_phase->count;
    uint32_t position = position_of(team_phase, walk->team, node, walk->routine);
    uint32_t after = (uint32_t)(((uint64_t)position + n - team_phase->index) % n);
    if (after == 0)
        walk->here += count;
    uint32_t last = team_phase->steps - 1;
    if (after < shares_at(n, last))
        join(walk, position, last, first, count);
    for (uint32_t s = last; s-- > 0 && (UINT64_C(1) << s) > after;)
        join(walk, position, s, first, count);
}

/*
 * A team's intervals, found at its first gather (the file's comment): the team's
 * ranks walked in order, in the progressions of world ranks the SHMEM layer
 * gives (internode.h), each cut where the rank map's runs cut it (rank-map.h).
 * A rank-map run whose ranks share a node - a block distribution's - gives one
 * stretch of members at a time, and one whose ranks step from node to node -
 * a cyclic one's - one member at a time, its node by arithmetic; a search of
 * the map only where a progression enters a run. And the checks that the walk
 * and the SHMEM layer agree, before any put: this node is where the team's
 * record puts it, and as many members are on it as the layer lists here - the
 * blocks the lead has put in the buffer, and the only ones it can send before
 * it receives any.
 */
static void find_intervals(struct generic_team *team_phase, uint32_t team, const char *routine)
{
    const struct osoe_job *job = &osoe.job;
    /* No list is a team this process holds no descriptor for, which on the
     * thread the data half runs on - its local lead's - is a bug: named, as
     * lead_of names a position with no lead, before the walk would find the
     * team empty. */
    uint32_t listed = 0;
    if (!osoe_internode_team_local_ranks(team, &listed))
        osoe_fatal("%s: team %u has no members on this node that this process knows of",
                   routine, number_of(team));
    if (team_phase->count > job->node_count)
        osoe_fatal("%s: team %u spans %u nodes of a job of %u", routine, number_of(team), team_phase->count,
                   job->node_count);
    uint32_t here = position_of(team_phase, team, job->node, routine);
    if (here != team_phase->index)
        osoe_fatal("%s: team %u's record puts this node, node %u, at position %u of its node "
                   "set, and its leads at %u", routine, number_of(team), job->node, team_phase->index, here);
    team_phase->steps = osoe_internal_gather_steps(team_phase->count);
    team_phase->step_intervals = calloc(team_phase->steps, sizeof *team_phase->step_intervals);
    struct member_walk walk = { team_phase, team, 0,
                                calloc(team_phase->count, sizeof *walk.steps_joined), routine };
    if (!team_phase->step_intervals || !walk.steps_joined)
        osoe_fatal("%s: out of memory for team %u's gather", routine, number_of(team));

    /* The team's members as world ranks, from its record on this node, which
     * this thread - the team's local lead here - wrote (node.h, decision E):
     * one progression, walked once. */
    const struct osoe_team_record *record = &osoe_node_team(&osoe.node, team)->record;
    if (!atomic_load_explicit(&record->published, memory_order_acquire))
        osoe_fatal("%s: team %u's record here is not published", routine, number_of(team));
    if (!record->strided)
        osoe_fatal("%s: team %u's record here is not a progression of world ranks", routine,
                   number_of(team));
    uint32_t world = record->world_start, count = record->members;
    int32_t stride = record->world_stride;
    if (stride == 0 && count > 1)
        osoe_fatal("%s: team %u's ranks are all world rank %u", routine, number_of(team), world);
    for (uint32_t k = 0; k < count;) {
        int64_t w = (int64_t)world + (int64_t)k * stride;
        const struct osoe_rank_run *run =
            w >= 0 && w < job->world_size
                ? osoe_rank_run_find(job->runs, job->run_count, (uint32_t)w) : NULL;
        if (!run)
            osoe_fatal("%s: team %u's rank %u is PE %lld, which the rank map places on no "
                       "node", routine, number_of(team), k, (long long)w);
        /* The progression's ranks from k on that this run holds. */
        uint64_t along = (uint64_t)w - run->first_rank;
        uint64_t in_run = stride > 0 ? (run->count - 1 - along) / (uint64_t)stride + 1
                          : stride < 0 ? along / (uint64_t)(-(int64_t)stride) + 1 : 1;
        if (in_run > count - k)
            in_run = count - k;
        if (run->node_step == 0) {
            add_members(&walk, k, (uint32_t)in_run, run->first_node);
        } else {
            for (uint64_t j = 0; j < in_run; j++) {
                int64_t at = (int64_t)along + (int64_t)j * stride;
                add_members(&walk, k + (uint32_t)j, 1,
                            (uint32_t)((int64_t)run->first_node + at * run->node_step));
            }
        }
        k += (uint32_t)in_run;
    }
    free(walk.steps_joined);
    team_phase->members = count;
    if (walk.here != listed)
        osoe_fatal("%s: the rank map puts %u of team %u's members on this node, and the team "
                   "lists %u here", routine, walk.here, number_of(team), listed);
}

/*
 * What a lead says of the gather it has entered, in the READY[0] line of the
 * lead that writes to it at step 0, past the counter (the file's comment): the
 * team's epoch, how the result is laid out - by a block, which every node's is
 * then; by places, collect's gather whose blocks differ from node to node; or
 * by ranges, collect's under the leads' scan (P30), where each lead knows its
 * own node's share alone - and its bytes, and the block, 0 but for a block.
 * Twenty-four bytes, in a line of 128 whose first eight hold the counter.
 */
enum gather_layout {
    GATHER_BLOCK,
    GATHER_PLACES,
    GATHER_RANGES,
};

struct gather_shape {
    uint32_t epoch;
    uint32_t layout;                    /* enum gather_layout */
    uint64_t bytes;
    uint64_t block;
};
enum { SHAPE_OFFSET = 8 };
_Static_assert(SHAPE_OFFSET + sizeof(struct gather_shape) <= OSOE_INTERNAL_LINE_BYTES,
               "a gather's shape fits its READY line past the counter");

/* A shape as a reader wants it: "epoch 3's gather of 64 bytes, in blocks of 8". */
static void describe_shape(const struct gather_shape *shape, char *out, size_t size)
{
    if (shape->epoch == 0)
        snprintf(out, size, "nothing: no gather's shape was put there");
    else if (shape->layout == GATHER_PLACES)
        snprintf(out, size, "epoch %u's gather of %llu bytes, at places of their own",
                 shape->epoch, (unsigned long long)shape->bytes);
    else if (shape->layout == GATHER_RANGES)
        snprintf(out, size, "epoch %u's gather of %llu bytes, by the ranges of each node's "
                 "share", shape->epoch, (unsigned long long)shape->bytes);
    else
        snprintf(out, size, "epoch %u's gather of %llu bytes, in blocks of %llu", shape->epoch,
                 (unsigned long long)shape->bytes, (unsigned long long)shape->block);
}

/* The shape the lead at step 0's receiver - `receiver` - put in this lead's
 * READY[0] line is this lead's own, or the job ends naming both, before any
 * put. */
static void check_shape(uint32_t team, const struct gather_shape *mine, uint32_t receiver,
                        const char *routine)
{
    struct gather_shape theirs;
    memcpy(&theirs, internal(ready_counter(team, 0) + SHAPE_OFFSET), sizeof theirs);
    if (theirs.epoch == mine->epoch && theirs.layout == mine->layout &&
        theirs.bytes == mine->bytes && theirs.block == mine->block)
        return;
    char here[160], there[160];
    describe_shape(mine, here, sizeof here);
    describe_shape(&theirs, there, sizeof there);
    osoe_fatal("%s: this node's lead entered %s, and node %u's lead (PE %u) %s: PEs called "
               "different collectives, or the same one with different arguments", routine,
               here, osoe_node_of(&osoe.job, receiver), receiver, there);
}

/* One gather as a call gives it: its team, its buffer - where it is, at a
 * peer as here (internode.h) - and its layout. */
struct gather {
    uint32_t team;
    const unsigned char *buffer;
    enum osoe_region region;
    uint64_t offset;
    uint64_t bytes;
    size_t block;
    const uint64_t *places;
    const char *routine;
};

/*
 * The layout does not fit the result: team rank `rank`'s place is `place`,
 * before the place of the interval before it or past the result's end. Places are
 * made consistent (internode.h), so these were overwritten after they were
 * made - in a program's collect whose dest is shorter than the result, by the
 * lead's own copy of its members' blocks, running on past dest into the room
 * that holds them (collectives.c). Named, before the put it would have cut:
 * one cut from them could land anywhere in a peer's heap.
 */
static void layout_overwritten(const struct gather *gather, uint64_t rank, uint64_t place)
{
    osoe_fatal("%s: team %u's member %llu has its place at byte %llu of a result of %llu "
               "bytes, out of the places' order: the layout was overwritten after it was made "
               "- is dest shorter than the result, so that copying into it ran on past its "
               "end?", gather->routine, number_of(gather->team), (unsigned long long)rank,
               (unsigned long long)place, (unsigned long long)gather->bytes);
}

/* Bytes `from` up to `to` of the buffer into the receiver's, at the same place. */
static void put_bytes(const struct gather *gather, struct osoe_transport_context *context,
                      int receiver, uint64_t from, uint64_t to)
{
    osoe_transport_put(context, receiver, gather->region, gather->offset + from,
                       gather->buffer + from, (size_t)(to - from), OSOE_TRANSPORT_MORE);
}

/*
 * One step's puts: each interval of the step's list mapped to its bytes by the
 * call's layout, O(1) an interval, and checked there - its end not before its
 * start, its start not before the end of the interval before it, its end within
 * the result - so that no put leaves [0, bytes) whatever the places hold;
 * intervals whose bytes touch, members with no bytes between them, go as one
 * put.
 */
static void put_step(const struct gather *gather, const struct interval_list *list,
                     struct osoe_transport_context *context, int receiver)
{
    uint64_t from = 0, to = 0, reached = 0;
    bool pending = false;
    for (uint32_t r = 0; r < list->count; r++) {
        const struct member_interval *interval = &list->intervals[r];
        uint64_t start = place_of(gather->places, gather->block, interval->first);
        uint64_t end = place_of(gather->places, gather->block, interval->end);
        if (start < reached)
            layout_overwritten(gather, interval->first, start);
        if (end < start || end > gather->bytes)
            layout_overwritten(gather, interval->end, end);
        reached = end;
        if (start == end)
            continue;
        if (pending && to == start) {
            to = end;
            continue;
        }
        if (pending)
            put_bytes(gather, context, receiver, from, to);
        from = start;
        to = end;
        pending = true;
    }
    if (pending)
        put_bytes(gather, context, receiver, from, to);
}

/*
 * The buffer as a peer's is addressed (internode.h): its region - this PE's
 * heap, or the program's statics, whose two ranges run on in one offset space
 * (node.h) - and its offset there, which is every node's.
 */
static void locate(struct gather *gather)
{
    const struct osoe_node *n = &osoe.node;
    uintptr_t at = (uintptr_t)gather->buffer, heap = (uintptr_t)n->heap_of[osoe.bootstrap.local_id];
    uintptr_t data = (uintptr_t)n->statics_data, bss = (uintptr_t)n->statics_bss;
    if (at >= heap && at - heap < n->heap_size && gather->bytes <= n->heap_size - (at - heap)) {
        gather->region = OSOE_REGION_HEAP;
        gather->offset = at - heap;
    } else if (n->statics_data_bytes && at >= data && at - data < n->statics_data_bytes) {
        gather->region = OSOE_REGION_STATICS;
        gather->offset = at - data;
    } else if (n->statics_bss_bytes && at >= bss && at - bss < n->statics_bss_bytes) {
        gather->region = OSOE_REGION_STATICS;
        gather->offset = n->statics_data_bytes + (at - bss);
    } else {
        osoe_fatal("%s: a gather's buffer, at %p, is in neither this PE's symmetric heap nor "
                   "its statics", gather->routine, (const void *)gather->buffer);
    }
}

/*
 * internode.h's gather: Bruck's allgather, allgather_model.py's lead program
 * (the file's comment). Enter before anything else - this gather's shape into
 * the READY[0] line of the lead that writes here at step 0, one more on its
 * READY[0], fenced after the shape, and one more on READY[s] at the lead that
 * writes here at every other step s - and only then find the team's intervals, at
 * its first gather, and check the call's layout at its ends. Then each step:
 * its READY, and at step 0 the shape its receiver entered; the step's puts,
 * straight into the buffer of the lead 2^s positions before this one; the
 * fenced add of that lead's DATA[s]; and this lead's own DATA[s], which says
 * the step's shares are in. Then the counts it published.
 */
void osoe_internode_generic_collect(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                                    size_t block, const uint64_t *places, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    uint64_t n = team_phase->count, i = team_phase->index, want = team_phase->gathers + 1;
    uint32_t steps = osoe_internal_gather_steps(team_phase->count);
    struct osoe_transport_context *context = osoe_transport_context_default();
    struct gather_shape mine = { epoch, places ? GATHER_PLACES : GATHER_BLOCK, bytes,
                                 places ? 0 : block };
    int first_writer = (int)lead_of(team, (uint32_t)((i + 1) % n), routine);
    osoe_transport_put(context, first_writer, OSOE_REGION_INTERNAL,
                       ready_counter(team, 0) + SHAPE_OFFSET, &mine, sizeof mine,
                       OSOE_TRANSPORT_MORE);
    osoe_transport_rmw(context, first_writer, OSOE_REGION_INTERNAL, ready_counter(team, 0),
                       OSOE_RMW_ADD, 8, 1, 0,
                       OSOE_TRANSPORT_FENCED | (steps > 1 ? OSOE_TRANSPORT_MORE : 0));
    for (uint32_t s = 1; s < steps; s++) {
        uint32_t writer = lead_of(team, (uint32_t)((i + (UINT64_C(1) << s)) % n), routine);
        osoe_transport_rmw(context, (int)writer, OSOE_REGION_INTERNAL, ready_counter(team, s),
                           OSOE_RMW_ADD, 8, 1, 0, s + 1 < steps ? OSOE_TRANSPORT_MORE : 0);
    }

    if (!team_phase->step_intervals)
        find_intervals(team_phase, team, routine);
    struct gather gather = { team, buffer, OSOE_REGION_HEAP, 0, bytes, block, places, routine };
    locate(&gather);
    uint64_t first_place = place_of(places, block, 0);
    uint64_t end_place = place_of(places, block, team_phase->members);
    if (first_place != 0)
        layout_overwritten(&gather, 0, first_place);
    if (end_place != bytes)
        layout_overwritten(&gather, team_phase->members, end_place);

    for (uint32_t s = 0; s < steps; s++) {
        uint32_t receiver = lead_of(team, (uint32_t)((i + n - (UINT64_C(1) << s)) % n), routine);
        /* READY[s] is the receiver's, added as it entered; DATA[s] the
         * writer's, 2^s positions on (D1). */
        waits_for(team_phase, OSOE_DATA_WAIT_READY, receiver, s);
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, ready_counter(team, s), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        if (s == 0)
            check_shape(team, &mine, receiver, routine);
        put_step(&gather, &team_phase->step_intervals[s], context, (int)receiver);
        osoe_transport_rmw(context, (int)receiver, OSOE_REGION_INTERNAL, data_counter(team, s),
                           OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
        waits_for(team_phase, OSOE_DATA_WAIT_PIECE,
                  lead_of(team, (uint32_t)((i + (UINT64_C(1) << s)) % n), routine), s);
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, data_counter(team, s), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
    }
    team_phase->gathers = want;
    /* Single-writer publication for osoe-stat and the tests (§3.16), before the
     * lead bumps the team's `released`, as in_pieces publishes its count. */
    atomic_store_explicit(&team_phase->published->gather_steps, steps, memory_order_release);
    atomic_store_explicit(&team_phase->published->gather_shares, team_phase->shares,
                          memory_order_release);
}

void osoe_internode_generic_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index,
                                      void *buffer, size_t bytes, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    if (root_index >= team_phase->count)
        osoe_fatal("%s: the root's node is index %u of a team spanning %u nodes", routine,
                   root_index, team_phase->count);
    struct osoe_collective_shape shape = shape_of(OSOE_SHAPE_BROADCAST, team_phase, bytes);
    struct lead_tree tree;
    find_lead_tree(team_phase, team, root_index, shape.fanout, &tree, routine);
    enum osoe_collective_class class =
        osoe_collective_class(bytes, osoe.bootstrap.small_collective_bytes);
    struct pieces pieces = { team, number_of(team), epoch, root_index, class, buffer, bytes,
                             bytes_per_piece(class, bytes, 1), 1, NULL, routine };
    in_pieces(team_phase, &pieces, &tree, shape);
}

/* ---- The scans (spec §9.10.10; phase 1 step 10 slice 1) ------------------ */

/*
 * A prefix scan over a team spanning nodes, every member at its team rank's
 * place in a pre-order k-ary tree over the ranks 0 .. M - 1
 * (docs/prompts/done/phase1-step10-slice1-prompt.md, the author's design A; the
 * protocol is tools/modelcheck/scan_model.py's `per_team`, and with pieces in
 * flight scan_pipeline_model.py's). A place covering the ranks [a, b) is rank
 * a; its children split [a + 1, b) into at most k consecutive blocks, the
 * first ones a rank longer when the split is uneven, so every subtree is a
 * contiguous range of ranks. Up: a member waits for each child's subtree
 * total, adds its own source, and puts the sum into its parent's up-slot for
 * its position, then adds one, fenced, to the parent's counter for that
 * position. Down: the root has nothing before it; every other member waits for
 * the sum of every rank before its subtree in its down-slot. That sum is its
 * exclusive result, plus its source its inclusive one, and child j is sent
 * that plus its source plus the totals of children 0 .. j - 1.
 *
 * **A small scan is one piece and a large one goes in pieces, D of them in
 * flight** (the author's decision of 2026-09-27; D is the scan's window,
 * scan_window below: OSOE_COLLECTIVE_PIPELINE under OSOE_COLLECTIVE_SHAPE=fixed,
 * 0.1.0's two under link and tree; the team redesign's step E, E6): a small scan - its
 * bytes at most OSOE_SMALL_COLLECTIVE_BYTES - goes through the team number's
 * own small slots, one per child position and one for the down; a large one
 * through the members' lanes, as a large reduction does, in pieces sized to
 * the scan - its count's share of D, but at least 8 KiB and at most a lane's
 * 16 KiB slot (the author's decision of 2026-10-04, the rule the Pi measured
 * best of three) - each member's lane a row of D up-slots per child position
 * and a row of D down-slots, piece g of the team's scans in slot g mod D
 * (internal-region.h) - and every member grants its tree children as it
 * enters, as a reduction's lead does: its lane's index, the scan's number
 * among the team's and its identity into the child's scans' grant line, then a
 * fenced add to the child's scans' grant counter. A child sends its first
 * piece up only once that counter shows this scan's grant, and puts its own
 * lane's word into its parent's row before its first add, which the parent
 * reads as it takes the child's first total (the reductions' rule, and the
 * same reason: lane_model.py). Each row has a counter, cumulative over the
 * team's life and compared as at least. Every member of a team moves the same
 * pieces, so each expects, in every row it uses, one more piece than the
 * team's scans have moved so far (`scan_pieces`, counted per team as the
 * generic implementation's other bases are). The loop is the reductions'
 * (in_pieces) with the scan's rules, each with a breakage the model shows
 * failing:
 *   - **a member sends piece q up only once it holds piece q - D's down**,
 *     the parent's word that it read piece q - D's total out of the slot
 *     (`window`);
 *   - **a member copies a down out before it counts it**, since its next
 *     piece up is what lets its parent write that slot again
 *     (`count_before_copy`);
 *   - **one counter per child position**, not one per member (`summed`);
 *   - **a member takes a piece down only once it has taken it up**: the downs
 *     it sends its children carry the totals of the children before each,
 *     read when the piece went up - at the root, whose down is its own, this
 *     is the only rule that orders the two (`root_early`);
 *   - **its own source and its children's totals are kept for the D pieces in
 *     flight**, until each piece has gone down (`ring_short`).
 * And the reductions' fifth: a member takes whichever input is ready - its
 * children's totals for piece `up` while the window allows, or piece `down`'s
 * down - the children's first when both are, and waits for either when
 * neither is.
 *
 * No slot is written while its reader may still need it: a child puts piece q
 * up - of this scan or the team's next - only after its down for piece q - D
 * arrived, which its parent sent after reading piece q - D's total; and a
 * parent puts piece q down only after the child's total for q arrived, which
 * the child sent after copying out its down for q - D. Two threads may scan
 * two teams at once (spec §9.4): each team's counters and small rows are its
 * number's own, and a large scan's pieces go through the calling thread's
 * lane, which no other thread of the PE lends while the scan runs - slots
 * shared by every team, as the first build had, would not allow it (the
 * review of the slice; round six's H6).
 *
 * Costs, with M members, k the fan-out, D pieces in flight and p pieces:
 * 2 ceil(log_k M) steps a piece, each a put and a fenced add, taking about
 * p / D round trips of the tree where one piece at a time took p, for as long
 * as the round trip rather than the links is the limit, and a large scan's
 * child at most a hop more, once, for its parent's grant; O(k) small slots and
 * counters a team number a PE, the large slots the lanes'; O(k D) pieces of
 * private memory a scan;
 * O(k log_k M) arithmetic to find a place. Each member publishes in its PE
 * slot the most pieces it had in flight at once (node.h): min(D, p) at a
 * leaf, which fills its window before it looks for a down.
 */
/* Team number `number`'s scans' area (internal-region.h). */
static uint64_t scan_of_number(uint32_t number)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    uint32_t small = osoe.bootstrap.small_collective_bytes;
    return osoe_internal_scan_offset(fanout, osoe.bootstrap.max_teams, small,
                                     osoe.bootstrap.node_count) +
           (uint64_t)number * osoe_internal_team_scan_bytes(fanout, small);
}

/* Its small row for `position`: a child position's below the fan-out, the
 * down row at it - a lane word, then the small slot. */
static uint64_t scan_small_row(uint32_t number, uint32_t position)
{
    return scan_of_number(number) +
           (uint64_t)position * osoe_internal_scan_row_bytes(osoe.bootstrap.small_collective_bytes);
}

static uint64_t scan_lane_word(uint32_t number, uint32_t position)
{
    return scan_small_row(number, position);
}

static uint64_t scan_small_slot(uint32_t number, uint32_t position)
{
    return scan_small_row(number, position) + OSOE_INTERNAL_LANE_WORD_BYTES;
}

/* Row `position`'s counter, past every row - and at the fan-out + 1 the
 * scans' grant counter, whose words lie past it. */
static uint64_t scan_counter_of_number(uint32_t number, uint32_t position)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    return scan_of_number(number) +
           (uint64_t)(fanout + 1) *
               osoe_internal_scan_row_bytes(osoe.bootstrap.small_collective_bytes) +
           (uint64_t)position * OSOE_INTERNAL_LINE_BYTES;
}

static uint64_t scan_grant_counter(uint32_t number)
{
    return scan_counter_of_number(number, osoe.bootstrap.collective_fanout + 1);
}

static uint64_t scan_grant_words(uint32_t number)
{
    return scan_grant_counter(number) + 8;
}

/*
 * A scan's fan-out and window. Its tree is over the team's members, not its
 * nodes, and its rows share one base - the pieces the team's scans have moved -
 * so it cannot take a shape per call as the reductions and broadcasts do: a row
 * one call left unused would fall behind the base, and the next call to use it
 * would wait forever. Under `link` and `tree` it keeps 0.1.0's shape, a fan-out
 * of 4 and two pieces in flight, or the ceiling and the capacity where those are
 * less;
 * under `fixed` the ceiling and the capacity, as every tree. On the Pi (v0.2
 * step 2, Part 2's record), a scan over 16 PEs on four nodes at the defaults'
 * fan-out of 8 took 1.5 to 2.0 times as long from 8 KiB up - its root's node
 * sending six copies over its link where 4 sends three - and at 4 with the
 * capacity's four pieces in flight it was 1.30 times faster at 1 MiB but 6.6%
 * slower at 64 KiB: 0.1.0's shape is the one no size of it is slower in.
 */
enum { SCAN_FANOUT_UNDER_LINK = 4, SCAN_WINDOW_UNDER_LINK = 2 };

static uint32_t scan_fanout(void)
{
    uint32_t ceiling = osoe.bootstrap.collective_fanout;
    if (osoe.bootstrap.collective_shape == OSOE_COLLECTIVE_SHAPE_FIXED ||
        ceiling < SCAN_FANOUT_UNDER_LINK)
        return ceiling;
    return SCAN_FANOUT_UNDER_LINK;
}

static uint32_t scan_window(void)
{
    uint32_t capacity = osoe.bootstrap.collective_pipeline;
    if (osoe.bootstrap.collective_shape == OSOE_COLLECTIVE_SHAPE_FIXED ||
        capacity < SCAN_WINDOW_UNDER_LINK)
        return capacity;
    return SCAN_WINDOW_UNDER_LINK;
}

/* The children of the place covering [a, b): how many, and child j's first
 * rank and end. */
static uint32_t scan_children(uint32_t a, uint32_t b, uint32_t k)
{
    uint32_t rest = b - a - 1;
    return rest < k ? rest : k;
}

static void scan_child(uint32_t a, uint32_t b, uint32_t k, uint32_t j, uint32_t *first,
                       uint32_t *end)
{
    uint32_t rest = b - a - 1, n = scan_children(a, b, k);
    uint32_t size = rest / n, longer = rest % n;
    *first = a + 1 + j * size + (j < longer ? j : longer);
    *end = *first + size + (j < longer ? 1 : 0);
}

/* The place of `rank` in the tree over `members` ranks: its subtree [*a, *b),
 * its parent's rank (UINT32_MAX at the root) and its position there. */
static void scan_place(uint32_t members, uint32_t k, uint32_t rank, uint32_t *a, uint32_t *b,
                       uint32_t *parent, uint32_t *position)
{
    *a = 0;
    *b = members;
    *parent = UINT32_MAX;
    *position = 0;
    while (*a != rank) {
        uint32_t n = scan_children(*a, *b, k), first = 0, end = 0, j;
        for (j = 0; j < n; j++) {
            scan_child(*a, *b, k, j, &first, &end);
            if (rank < end)
                break;
        }
        *parent = *a;
        *position = j;
        *a = first;
        *b = end;
    }
}

/*
 * One scan as a member moves it: its team's number, the scan's number among
 * the team's - every member's alike, since every member makes every scan of
 * its team - and its identity, which a grant and a lane word name; its class;
 * and its lanes, as a large reduction's (struct lanes): its own, its parent's
 * from the grant, its children's from their lane words.
 */
struct scan_flight {
    uint32_t number;
    uint32_t sequence;
    uint32_t identity;
    enum osoe_collective_class class;
    uint64_t bytes;
    uint64_t base;                      /* the team's scan pieces before this scan's */
    struct lanes lanes;
};

/* A large scan's pieces (osoe_internode_generic_scan): its count's share of D,
 * but at least 8 KiB and at most a lane's 16 KiB slot. */
enum { SCAN_LEAST_PIECE_BYTES = 8 << 10 };

/* A scan's identity: inclusive or not, and its element. */
static uint32_t scan_identity(bool inclusive, struct osoe_element element)
{
    return (inclusive ? UINT32_C(1) << 24 : 0) | (uint32_t)element.kind << 8 |
           (element.width & 0xff);
}

/* A scan as a reader wants it: "scan 3 of the team, an inclusive one of
 * 8-byte signed integers". */
static void describe_scan(uint32_t sequence, uint32_t identity, char *out, size_t size)
{
    if (sequence == 0)
        snprintf(out, size, "nothing: no grant was put there");
    else
        snprintf(out, size, "scan %u of the team, an %s one of %u-byte %s", sequence,
                 identity >> 24 ? "inclusive" : "exclusive", identity & 0xff,
                 kind_name((identity >> 8) & 0xff));
}

/* Where this member finds row `position`'s piece `piece` of the team's scans:
 * the team number's small slot for the row, or its lane's. */
static uint64_t scan_own_slot(const struct scan_flight *f, uint32_t position, uint64_t piece)
{
    return f->class == OSOE_COLLECTIVE_SMALL ? scan_small_slot(f->number, position)
                                             : lane_slot(f->lanes.own, position, piece);
}

/* Whether row `position`'s counter shows piece `piece` of the team's scans,
 * numbered from 0 over the team's life. The load is seq_cst (transport.h), and
 * so acquire: the bytes put before the fenced add are there to read. */
static bool scan_arrived(uint32_t number, uint32_t position, uint64_t piece)
{
    return osoe_transport_load(OSOE_REGION_INTERNAL, scan_counter_of_number(number, position), 8) >=
           piece + 1;
}

static bool scan_children_arrived(uint32_t number, uint32_t children, uint64_t piece)
{
    for (uint32_t j = 0; j < children; j++)
        if (!scan_arrived(number, j, piece))
            return false;
    return true;
}

/* Whether this scan's grant is in: the scans' grant counter against its base,
 * as at least. */
static bool scan_grant_in(const struct generic_team *g, uint32_t number)
{
    return osoe_transport_load(OSOE_REGION_INTERNAL, scan_grant_counter(number), 8) >=
           g->scan_grant_base + 1;
}

/* The parent's grant, once scan_grant_in says it is in: it names this scan, or
 * the job ends naming both PEs. */
static void scan_take_grant(struct scan_flight *f, int parent_pe, const char *routine)
{
    struct grant grant;
    memcpy(&grant, internal(scan_grant_words(f->number)), sizeof grant);
    cleared(internal(scan_grant_words(f->number)), sizeof grant);
    if (grant.epoch != f->sequence || grant.collective != f->identity || grant.bytes != f->bytes) {
        char theirs[160], mine[160];
        describe_scan(grant.epoch, grant.collective, theirs, sizeof theirs);
        describe_scan(f->sequence, f->identity, mine, sizeof mine);
        osoe_fatal("%s: the scan grant from PE %d is for %s, %llu bytes, and PE %u is in %s, "
                   "%llu bytes: PEs called different scans, or the same one with different "
                   "arguments", routine, parent_pe, theirs, (unsigned long long)grant.bytes,
                   osoe.job.me, mine, (unsigned long long)f->bytes);
    }
    if (grant.lane >= OSOE_MAX_THREADS_HIGHEST)
        osoe_fatal("%s: the scan grant from PE %d names lane %u, past any PE's %u", routine,
                   parent_pe, grant.lane, (unsigned)OSOE_MAX_THREADS_HIGHEST);
    f->lanes.parent = grant.lane;
}

/* Each child's lane, read as this member takes the child's first total - the
 * arrival that shows the word written - and kept for the scan (the reductions'
 * rule, take_child_lanes). */
static void scan_take_child_lanes(struct scan_flight *f, uint32_t children, const int *child_pe,
                                  const char *routine)
{
    for (uint32_t j = 0; j < children; j++) {
        struct lane_word word;
        memcpy(&word, internal(scan_lane_word(f->number, j)), sizeof word);
        cleared(internal(scan_lane_word(f->number, j)), sizeof word);
        if (word.epoch != f->sequence || word.lane >= OSOE_MAX_THREADS_HIGHEST)
            osoe_fatal("%s: the lane word of PE %d, this PE's child at position %u in the "
                       "team's scan tree, says scan %u and lane %u, and PE %u is in scan %u",
                       routine, child_pe[j], j, word.epoch, word.lane, osoe.job.me,
                       f->sequence);
        f->lanes.child[j] = word.lane;
    }
}

/* What a member waits for when neither input is ready: every child's total
 * for piece `up`, if the window lets it take that piece up - and for a large
 * scan's first, its parent's grant - or piece `down`'s down, if it has taken
 * that piece up and is not the root. The rows' counters and the scans' grant
 * counter are contiguous, a line each, so one watch covers them all. */
struct scan_awaited {
    const struct generic_team *team_phase;
    uint32_t number;
    uint32_t children;
    uint32_t down_row;
    bool sending;
    bool granting;
    bool descending;
    uint64_t up;                        /* the team's piece numbers */
    uint64_t down;
};

static bool scan_awaited_holds(const void *context)
{
    const struct scan_awaited *a = context;
    return (a->sending && scan_children_arrived(a->number, a->children, a->up) &&
            (!a->granting || scan_grant_in(a->team_phase, a->number))) ||
           (a->descending && scan_arrived(a->number, a->down_row, a->down));
}

void osoe_internode_generic_scan(uint32_t team, uint32_t rank, void *dest, const void *source,
                                 size_t count, struct osoe_element element, bool inclusive,
                                 const char *routine)
{
    const struct osoe_team_record *record = &osoe_node_team(&osoe.node, team)->record;
    if (!atomic_load_explicit(&record->published, memory_order_acquire))
        osoe_fatal("%s: team %u's record here is not published", routine, number_of(team));
    if (!record->strided || rank >= record->members)
        osoe_fatal("%s: team %u's record here is %s", routine, number_of(team),
                   !record->strided ? "not a progression of world ranks"
                                    : "shorter than this PE's rank");
    osoe_combine_function add = osoe_combine_lookup(OSOE_REDUCTION_SUM, element);
    if (!add)
        osoe_fatal("%s: no sum over elements of kind %d and width %u", routine,
                   (int)element.kind, element.width);
    uint32_t k = scan_fanout(), members = record->members;
    uint32_t a, b, parent, position;
    scan_place(members, k, rank, &a, &b, &parent, &position);
    uint32_t children = scan_children(a, b, k);
    int child_pe[OSOE_COLLECTIVE_FANOUT_MAX];
    for (uint32_t j = 0; j < children; j++) {
        uint32_t first, end;
        scan_child(a, b, k, j, &first, &end);
        child_pe[j] = (int)((int64_t)record->world_start + (int64_t)first * record->world_stride);
    }
    bool root = parent == UINT32_MAX;
    int parent_pe = root ? -1
        : (int)((int64_t)record->world_start + (int64_t)parent * record->world_stride);

    struct generic_team *g = team_state(team, routine);
    size_t width = element.width;
    struct scan_flight flight = {
        .number = number_of(team),
        .sequence = g->scans + 1,
        .identity = scan_identity(inclusive, element),
        .class = osoe_collective_class((uint64_t)count * width,
                                       osoe.bootstrap.small_collective_bytes),
        .bytes = (uint64_t)count * width,
        .base = g->scan_pieces,
        .lanes = { .own = NO_LANE, .parent = NO_LANE },
    };
    bool large = flight.class == OSOE_COLLECTIVE_LARGE;
    bool needs_grant = large && !root;
    struct osoe_transport_context *context = osoe_transport_context_default();
    if (large) {
        /* Entering: this thread's lane is the scan's from here to its return,
         * and each tree child is told so before anything else (E6). */
        flight.lanes.own = own_lane();
        struct grant grant = { flight.sequence, flight.identity, flight.bytes, flight.lanes.own, 0 };
        for (uint32_t j = 0; j < children; j++) {
            osoe_transport_put(context, child_pe[j], OSOE_REGION_INTERNAL,
                               scan_grant_words(flight.number), &grant, sizeof grant,
                               OSOE_TRANSPORT_MORE);
            osoe_transport_rmw(context, child_pe[j], OSOE_REGION_INTERNAL,
                               scan_grant_counter(flight.number), OSOE_RMW_ADD, 8, 1, 0,
                               OSOE_TRANSPORT_FENCED |
                               (j + 1 < children ? OSOE_TRANSPORT_MORE : 0));
        }
    }

    uint64_t window = scan_window();
    /* A small scan's one piece is the whole of it. A large one's pieces are
     * sized to the scan (the author's decision of 2026-10-04), every member
     * alike from its count, its element and D: the count's share of D, rounded
     * up, but at least 8 KiB of elements, since each piece costs a message the
     * progress thread handles, and at most a lane's slot of them - one piece to
     * 8 KiB, 8 KiB ones to D x 8 KiB, D to D x 16 KiB and slots past that.
     * Counted in elements, so that D pieces are D and not D + 1 with a
     * remainder: the second part's adversarial read found sizing in bytes cut
     * 2,049 longs 1,024 + 1,024 + 1, a third piece waiting a round trip of the
     * tree. On the Pi, of three rules - a lane's 16 KiB at every size (1.17
     * times slower than step D at 16 KiB, one piece where D's 4 KiB pieces had
     * kept two in flight), 4 KiB below 64 KiB (slower at 8 and 32 KiB, where
     * fewer messages won) and this one - this was the fastest at every scan
     * size, and step D's equal or better from 8 KiB up
     * (docs/decisions/2026-10-04-team-scans.md). */
    size_t least_per_piece = (SCAN_LEAST_PIECE_BYTES + width - 1) / width;
    size_t most_per_piece = OSOE_INTERNAL_PIECE_BYTES / width;
    size_t share = (count + window - 1) / window;
    size_t per_piece = !large ? (count ? count : 1)
                     : share < least_per_piece ? least_per_piece
                     : share > most_per_piece ? most_per_piece : share;
    size_t piece_bytes = (count < per_piece ? count : per_piece) * width;
    uint64_t pieces = (count + per_piece - 1) / per_piece;
    /* Kept for the pieces in flight, D of each: this PE's source, then each
     * child's total; then a piece's running sum, up or down. */
    size_t ring = (size_t)window * piece_bytes;
    unsigned char *buffers = malloc((size_t)(children + 1) * ring + piece_bytes);
    if (!buffers)
        osoe_fatal("%s: out of memory for a scan's pieces", routine);
    unsigned char *own = buffers, *totals = buffers + ring;
    unsigned char *sum = buffers + (size_t)(children + 1) * ring;
    uint64_t base = flight.base, up = 0, down = 0, most = 0;

    while (down < pieces) {
        bool sending = up < pieces && up < down + window;
        if (sending && scan_children_arrived(flight.number, children, base + up) &&
            (!needs_grant || flight.lanes.parent != NO_LANE ||
             scan_grant_in(g, flight.number))) {
            if (needs_grant && flight.lanes.parent == NO_LANE)
                scan_take_grant(&flight, parent_pe, routine);
            if (large && up == 0)
                scan_take_child_lanes(&flight, children, child_pe, routine);
            size_t first = (size_t)up * per_piece;
            size_t n = count - first < per_piece ? count - first : per_piece;
            size_t bytes = n * width, at = (size_t)(up % window) * piece_bytes;
            /* This PE's source first: dest may be the same array, and this
             * piece's part of it is written only when the piece comes down. */
            memcpy(own + at, (const unsigned char *)source + first * width, bytes);
            memcpy(sum, own + at, bytes);
            for (uint32_t j = 0; j < children; j++) {
                unsigned char *total = totals + (size_t)j * ring + at;
                unsigned char *slot = internal(scan_own_slot(&flight, j, base + up));
                memcpy(total, slot, bytes);
                /* A scan's piece has no header, but a lane's slot is read by
                 * the other collectives' header checks too: cleared where a
                 * header would be, so that a slot a scan left reads as nothing
                 * to them (a reader clears what it read). */
                if (large)
                    cleared(slot, HEADER_BYTES);
                add(sum, total, n);
            }
            if (!root) {
                if (large && up == 0) {
                    struct lane_word word = { flight.sequence, flight.lanes.own };
                    osoe_transport_put(context, parent_pe, OSOE_REGION_INTERNAL,
                                       scan_lane_word(flight.number, position), &word,
                                       sizeof word, OSOE_TRANSPORT_MORE);
                }
                uint64_t slot = large ? lane_slot(flight.lanes.parent, position, base + up)
                                      : scan_small_slot(flight.number, position);
                osoe_transport_put(context, parent_pe, OSOE_REGION_INTERNAL, slot, sum, bytes,
                                   OSOE_TRANSPORT_MORE);
                osoe_transport_rmw(context, parent_pe, OSOE_REGION_INTERNAL,
                                   scan_counter_of_number(flight.number, position), OSOE_RMW_ADD,
                                   8, 1, 0, OSOE_TRANSPORT_FENCED);
            }
            osoe_perturb();
            up++;
            if (up - down > most)
                most = up - down;
            continue;
        }
        if (down < up && (root || scan_arrived(flight.number, k, base + down))) {
            size_t first = (size_t)down * per_piece;
            size_t n = count - first < per_piece ? count - first : per_piece;
            size_t bytes = n * width, at = (size_t)(down % window) * piece_bytes;
            /* The sum of every rank before this subtree - nothing at the root -
             * copied out before anything lets the parent write the slot again. */
            if (root)
                memset(sum, 0, bytes);
            else {
                unsigned char *slot = internal(scan_own_slot(&flight, k, base + down));
                memcpy(sum, slot, bytes);
                if (large)
                    cleared(slot, HEADER_BYTES);
            }
            unsigned char *out = (unsigned char *)dest + first * width;
            memcpy(out, sum, bytes);
            if (inclusive)
                add(out, own + at, n);
            add(sum, own + at, n);
            for (uint32_t j = 0; j < children; j++) {
                uint64_t slot = large ? lane_slot(flight.lanes.child[j], k, base + down)
                                      : scan_small_slot(flight.number, k);
                osoe_transport_put(context, child_pe[j], OSOE_REGION_INTERNAL, slot, sum, bytes,
                                   OSOE_TRANSPORT_MORE);
                osoe_transport_rmw(context, child_pe[j], OSOE_REGION_INTERNAL,
                                   scan_counter_of_number(flight.number, k), OSOE_RMW_ADD, 8, 1,
                                   0, OSOE_TRANSPORT_FENCED);
                add(sum, totals + (size_t)j * ring + at, n);
            }
            osoe_perturb();
            down++;
            continue;
        }
        /* Neither: wait for either, naming the PE it waits on - a child whose
         * total is missing, else the parent, whose grant or down is. */
        int from = parent_pe;
        if (sending)
            for (uint32_t j = 0; j < children; j++)
                if (!scan_arrived(flight.number, j, base + up)) {
                    from = child_pe[j];
                    break;
                }
        struct scan_awaited awaited = { g, flight.number, children, k, sending,
                                        needs_grant && flight.lanes.parent == NO_LANE,
                                        !root && down < up, base + up, base + down };
        uint64_t watched = scan_counter_of_number(flight.number, 0);
        osoe_region_wait_condition(routine, OSOE_REGION_INTERNAL, watched,
                                   (size_t)(scan_grant_counter(flight.number) + 8 - watched),
                                   scan_awaited_holds, &awaited, OSOE_BLOCKED_DETAIL_PE,
                                   (uint64_t)(from < 0 ? 0 : from));
    }
    g->scan_pieces = base + pieces;
    g->scans = flight.sequence;
    if (needs_grant)
        g->scan_grant_base++;
    /* Single-writer publication for osoe-stat and the tests (§3.16), before
     * the scan returns. */
    atomic_store_explicit(&osoe.node.slots[osoe.bootstrap.local_id].scan_most_in_flight,
                          (uint32_t)most, memory_order_release);
    free(buffers);
}

/* ---- collect's places by a scan of the node totals (P30) ------------------ */

/*
 * docs/design/collect-scan-proposal.md, and tools/modelcheck/allgather_model.py's
 * ranged and ranged_scan cases, whose leads these are. Where every node of a
 * team holds one run of its ranks and the runs meet the node set's positions in
 * order or in reverse, a lead needs of the places only its own node's base and
 * the total, which a scan of the nodes' totals among the leads gives; and in the
 * gather each step says what it carries, so that no lead needs another node's
 * places.
 *
 * **The leads' scan** (osoe_internode_generic_scan_node_totals): one eight-byte
 * word a lead, its node's total - not the member scans' tree above, which is
 * every member's, deeper, gives no total and counts its rows on every member. A
 * pre-order tree over the node set's positions at the fan-out's ceiling, rooted
 * at position 0, by the member scans' arithmetic over positions instead of ranks
 * (scan_place), so that every subtree is a contiguous range of positions and the
 * totals before a subtree are what comes before it in position order: the
 * reductions' tree, whose subtrees are not contiguous, could not give them (the
 * model's heap_tree). Up, a lead waits for each child's subtree total, adds its
 * own node's and puts the sum into its parent's small slot for its position,
 * behind a piece's header, then a fenced add of the parent's arrival counter for
 * the position. Down, the root has nothing before it and holds the total once
 * its children's are in; every other lead waits for its release counter and
 * takes the totals before its subtree and the total from its small release slot;
 * child j is sent its parent's before, plus the parent's own total, plus the
 * subtrees of children 0 ... j - 1, and the total - the total comes with the
 * scan, no step more. Its words are a small reduction's - the team number's
 * small rows, its arrival and release counters - and so are its bases, which it
 * advances as in_pieces does: each child position's it had by one, the release
 * base unless it is the root. A tree that differs from the reductions' and the
 * broadcasts' on the same words is safe for the reason theirs differ from each
 * other's (the file's comment): the root sends its downs only once every lead
 * has sent up, a lead sends up only once its children have, so no lead starts
 * the team's next collective before every lead has read its child slots, and
 * nothing is written into a release slot before its lead's own arrival
 * (tools/modelcheck/lane_model.py's scan cases). Its header names it, a
 * `collective` of its own (NODE_TOTALS_SCAN), so that a lead in it whose peer is
 * in another collective is named where their pieces meet.
 *
 * **The gather by ranges** (osoe_internode_generic_collect_ranges): Bruck's
 * allgather above, the same pairs and the same READY and DATA. A lead's shares
 * are consecutive positions, and with the runs in position order or its reverse
 * their bytes are one run of the result - up from the lead's base, or down from
 * its share's end - wrapping past position N - 1 at most once. So a lead keeps
 * what it holds as two ranges, `first` the bytes of its positions up to N - 1
 * and `second` those past it, each kept where its bytes will begin while it is
 * empty: `first` at its own share, `second` at 0 in order and at the total in
 * reverse. At step s it puts what it holds into its receiver's buffer at the same
 * places - a put a range, none for an empty one, one where they touch - then its
 * **record**, the epoch and the two ranges, past the counter in the receiver's
 * DATA[s] line, then the fenced add of that counter: the record is fenced as the
 * bytes are. The last step sends N - 2^(S - 1) shares, fewer than the lead holds
 * unless N is a power of two, and the ones it does not send begin at its
 * receiver's own share, so the lead **cuts** what it holds where that share
 * begins - its start in order, its end in reverse - in `first` where its
 * positions stop short of the wrap, else in `second`, `first` then whole: the
 * positions, not the bytes, say which, so an empty share at the cut is no
 * ambiguity. Each lead puts its own share's range with the epoch past the
 * counter of the READY[S - 1] line of the lead that writes to it at the last
 * step, before that READY add, fenced, and the writer reads it after its own
 * READY[S - 1] wait. Once DATA[s] says the step landed, the receiver takes its
 * record and checks it - this epoch's, within the result, and continuing what it
 * holds: while its position plus 2^s is below N, the writer's `first` continues
 * its `first` and the writer's `second` its `second`; past that every position
 * the writer sends lies past its wrap, the writer's `first` continuing its
 * `second`, the writer's `second` empty - and then holds the union. After the
 * last step it holds the whole result, which it checks.
 *
 * **No record is overwritten before it is taken**: a record of step s is put only
 * after its writer's READY[s] wait - the wait that already guards the bytes - and
 * the writer's next one, in the gather after, needs READY[s] of that gather,
 * which the receiver adds only once it has returned from this one, after taking
 * every record of it. READY has a second duty, and the model's
 * range_before_ready, the record put ahead of the wait, fails without it. The
 * share in a READY[S - 1] line is safe the other way about: its lead writes it
 * again only as it enters the next gather, after its own last DATA wait, which
 * the writer's add satisfies after the writer read the share.
 *
 * Costs, with N nodes and k the ceiling: the scan 2 levels of a tree as deep as
 * the reductions' at k, eight bytes up and sixteen down a message and O(k) a
 * lead; the gather a record of 40 bytes more a step and one put of 24 bytes at
 * entry - S + 1 small puts a lead - and O(1) a step to cut, check and merge, with
 * no lists of members to find.
 */
struct node_totals_down {
    uint64_t before;                    /* the totals before the subtree */
    uint64_t total;
};

/* What the scan's lead waits for before its up: every child's subtree total,
 * each child position's arrival counter against its base, as at least. */
struct node_totals_awaited {
    const struct generic_team *team_phase;
    uint32_t number;
    uint32_t children;
    const uint32_t *child;              /* each child's lead's rank */
};

/* Saying, while one is missing, the first child whose total has not come (D1),
 * at every look, as say_awaited does. */
static bool node_totals_in(const void *context)
{
    const struct node_totals_awaited *a = context;
    for (uint32_t j = 0; j < a->children; j++)
        if (osoe_transport_load(OSOE_REGION_INTERNAL, arrival_counter(a->number, j), 8) <
            a->team_phase->arrival_base[j] + 1) {
            waits_for(a->team_phase, OSOE_DATA_WAIT_PIECE, a->child[j], 0);
            return false;
        }
    return true;
}

/* a + b, or the job ends: collect's blocks past what eight bytes count, as
 * collectives.c refuses them. */
static uint64_t total_plus(uint64_t a, uint64_t b, const char *routine)
{
    if (b > UINT64_MAX - a)
        osoe_fatal("%s: the members' blocks add up to more bytes than there are", routine);
    return a + b;
}

void osoe_internode_generic_scan_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                             uint64_t *before, uint64_t *total,
                                             const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    uint32_t k = osoe.bootstrap.collective_fanout, number = number_of(team);
    uint32_t a, b, parent, position;
    scan_place(team_phase->count, k, team_phase->index, &a, &b, &parent, &position);
    uint32_t children = scan_children(a, b, k);
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];
    for (uint32_t j = 0; j < children; j++) {
        uint32_t first, end;
        scan_child(a, b, k, j, &first, &end);
        child[j] = lead_of(team, first, routine);
    }
    struct osoe_transport_context *context = osoe_transport_context_default();
    struct piece_header up = { epoch, 0, NODE_TOTALS_SCAN, sizeof(uint64_t) };
    struct piece_header down = { epoch, 0, NODE_TOTALS_SCAN, sizeof(struct node_totals_down) };

    /* Up: each child's subtree total, position by position, then this
     * subtree's to the parent. */
    uint64_t below[OSOE_COLLECTIVE_FANOUT_MAX], subtree = own;
    if (children > 0) {
        struct node_totals_awaited awaited = { team_phase, number, children, child };
        uint64_t first = arrival_counter(number, 0);
        osoe_region_wait_condition(routine, OSOE_REGION_INTERNAL, first,
                                   (size_t)(grant_counter(number) + 8 - first), node_totals_in,
                                   &awaited, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
    }
    for (uint32_t j = 0; j < children; j++) {
        unsigned char *slot = internal(small_slot(number, j));
        check_piece(slot, &up, "a child's subtree total of the nodes' totals", child[j], routine);
        memcpy(&below[j], slot + HEADER_BYTES, sizeof below[j]);
        cleared(slot, HEADER_BYTES);
        subtree = total_plus(subtree, below[j], routine);
    }
    struct node_totals_down got = { 0, subtree };
    if (parent != UINT32_MAX) {
        int parent_pe = (int)lead_of(team, parent, routine);
        uint64_t slot = small_slot(number, position);
        osoe_transport_put(context, parent_pe, OSOE_REGION_INTERNAL, slot, &up, HEADER_BYTES,
                           OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, parent_pe, OSOE_REGION_INTERNAL, slot + HEADER_BYTES, &subtree,
                           sizeof subtree, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, parent_pe, OSOE_REGION_INTERNAL,
                           arrival_counter(number, position), OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED);
        /* Down: the totals before this subtree, and the total. */
        waits_for(team_phase, OSOE_DATA_WAIT_RELEASE, (uint32_t)parent_pe, 0);
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, release_counter(number), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, team_phase->release_base + 1,
                         OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        unsigned char *release = internal(small_slot(number, k));
        check_piece(release, &down, "the totals before this node's subtree", (uint32_t)parent_pe,
                    routine);
        memcpy(&got, release + HEADER_BYTES, sizeof got);
        cleared(release, HEADER_BYTES);
    }
    /* What every lead's subtree is within: a lead whose numbers do not hold so
     * was given them by a lead in another collective, or by one that reckons
     * the team's nodes otherwise. */
    if (got.before > got.total || subtree > got.total - got.before)
        osoe_fatal("%s: team %u's scan of its nodes' totals gives this node, at position %u, "
                   "%llu bytes before it and %llu in all, and its subtree holds %llu: the leads "
                   "disagree about the team's nodes", routine, number, team_phase->index,
                   (unsigned long long)got.before, (unsigned long long)got.total,
                   (unsigned long long)subtree);
    uint64_t at = got.before + own;
    for (uint32_t j = 0; j < children; j++) {
        struct node_totals_down theirs = { at, got.total };
        uint64_t slot = small_slot(number, k);
        osoe_transport_put(context, (int)child[j], OSOE_REGION_INTERNAL, slot, &down,
                           HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)child[j], OSOE_REGION_INTERNAL, slot + HEADER_BYTES,
                           &theirs, sizeof theirs, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)child[j], OSOE_REGION_INTERNAL, release_counter(number),
                           OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (j + 1 < children ? OSOE_TRANSPORT_MORE : 0));
        at += below[j];
    }
    /* The bases, as in_pieces advances them: one piece at every child position
     * this tree had, and down unless it is the root. */
    for (uint32_t j = 0; j < children; j++)
        team_phase->arrival_base[j]++;
    if (parent != UINT32_MAX)
        team_phase->release_base++;
    *before = got.before;
    *total = got.total;
    /* Single-writer publication for osoe-stat and the tests (§3.16), before the
     * lead releases its members, as in_pieces publishes a small collective's. */
    atomic_store_explicit(&team_phase->published->most_pieces_in_flight,
                          parent != UINT32_MAX ? 1u : 0u, memory_order_release);
    atomic_store_explicit(&team_phase->published->collective_class,
                          (uint32_t)OSOE_COLLECTIVE_SMALL, memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_fanout, k, memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_window, 1, memory_order_release);
    atomic_store_explicit(&team_phase->published->shape_children, children, memory_order_release);
    atomic_store_explicit(&team_phase->published->algorithm, (uint32_t)OSOE_SHAPE_TREE,
                          memory_order_release);
}

/* A range of a gather's result, bytes `start` up to `end`. */
struct gather_range {
    uint64_t start;
    uint64_t end;
};

/* What a lead holds of the result, and what a step sends (the section's
 * comment): `first` its positions' bytes up to N - 1, `second` past it. */
struct gather_ranges {
    struct gather_range first;
    struct gather_range second;
};

/* A step's record, past the counter of the receiver's DATA[s] line. */
struct gather_record {
    uint32_t epoch;
    uint32_t reserved;
    struct gather_ranges ranges;
};

/* A lead's own share, past the counter of the READY[S - 1] line of the lead
 * that writes to it at the last step - past the gather's shape too, which the
 * line holds where S is 1 and it is READY[0]'s. */
struct gather_share {
    uint32_t epoch;
    uint32_t reserved;
    struct gather_range range;
};

enum {
    RECORD_OFFSET = 8,
    SHARE_OFFSET = SHAPE_OFFSET + sizeof(struct gather_shape),
};
_Static_assert(RECORD_OFFSET + sizeof(struct gather_record) <= OSOE_INTERNAL_LINE_BYTES &&
                   SHARE_OFFSET + sizeof(struct gather_share) <= OSOE_INTERNAL_LINE_BYTES,
               "a step's record fits its DATA line, and a share its READY line past the shape");

/* Whether `range` lies within a result of `bytes`. */
static bool within(struct gather_range range, uint64_t bytes)
{
    return range.start <= range.end && range.end <= bytes;
}

/* `held` with `more` after it - up from its end in order, down from its start
 * in reverse - or false where `more` does not continue it. */
static bool continued(struct gather_range *held, struct gather_range more, bool reverse)
{
    if (!reverse && more.start == held->end) {
        held->end = more.end;
        return true;
    }
    if (reverse && more.end == held->start) {
        held->start = more.start;
        return true;
    }
    return false;
}

/* "bytes 3 up to 7 and none past the wrap", for a fatal. */
static void describe_ranges(const struct gather_ranges *r, char *out, size_t size)
{
    snprintf(out, size, "bytes %llu up to %llu and %llu up to %llu past the last position",
             (unsigned long long)r->first.start, (unsigned long long)r->first.end,
             (unsigned long long)r->second.start, (unsigned long long)r->second.end);
}

/* The bytes of `ranges` into the receiver's buffer at the same places: a put a
 * range, none for an empty one, one for both where they touch. */
static void put_ranges(const struct gather *gather, struct osoe_transport_context *context,
                       int receiver, const struct gather_ranges *ranges)
{
    struct gather_range a = ranges->first, b = ranges->second;
    if (a.start == a.end || b.start == b.end) {
        struct gather_range one = a.start == a.end ? b : a;
        if (one.start != one.end)
            put_bytes(gather, context, receiver, one.start, one.end);
    } else if (a.end == b.start || b.end == a.start) {
        put_bytes(gather, context, receiver, a.start < b.start ? a.start : b.start,
                  a.end > b.end ? a.end : b.end);
    } else {
        put_bytes(gather, context, receiver, a.start, a.end);
        put_bytes(gather, context, receiver, b.start, b.end);
    }
}

void osoe_internode_generic_collect_ranges(uint32_t team, uint32_t epoch, void *buffer,
                                           size_t bytes, uint64_t share_start,
                                           uint64_t share_end, bool reverse,
                                           const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    uint64_t n = team_phase->count, i = team_phase->index, want = team_phase->gathers + 1;
    uint32_t steps = osoe_internal_gather_steps(team_phase->count);
    uint32_t number = number_of(team);
    struct gather_range own = { share_start, share_end };
    if (!within(own, bytes))
        osoe_fatal("%s: team %u's gather by ranges was given this node's share as bytes %llu up "
                   "to %llu of a result of %llu", routine, number, (unsigned long long)share_start,
                   (unsigned long long)share_end, (unsigned long long)bytes);
    struct osoe_transport_context *context = osoe_transport_context_default();

    /* Enter: the shape into the READY[0] line of the lead that writes here at
     * step 0, this node's share into the READY[S - 1] line of the one that
     * writes here at the last, each before its READY add, fenced; one more on
     * READY[s] at every other writer. */
    struct gather_shape mine = { epoch, GATHER_RANGES, bytes, 0 };
    struct gather_share share = { epoch, 0, own };
    for (uint32_t s = 0; s < steps; s++) {
        int writer = (int)lead_of(team, (uint32_t)((i + (UINT64_C(1) << s)) % n), routine);
        bool fenced = s == 0 || s + 1 == steps;
        if (s == 0)
            osoe_transport_put(context, writer, OSOE_REGION_INTERNAL,
                               ready_counter(team, 0) + SHAPE_OFFSET, &mine, sizeof mine,
                               OSOE_TRANSPORT_MORE);
        if (s + 1 == steps)
            osoe_transport_put(context, writer, OSOE_REGION_INTERNAL,
                               ready_counter(team, s) + SHARE_OFFSET, &share, sizeof share,
                               OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, writer, OSOE_REGION_INTERNAL, ready_counter(team, s),
                           OSOE_RMW_ADD, 8, 1, 0,
                           (fenced ? OSOE_TRANSPORT_FENCED : 0) |
                           (s + 1 < steps ? OSOE_TRANSPORT_MORE : 0));
    }
    struct gather gather = { team, buffer, OSOE_REGION_HEAP, 0, bytes, 0, NULL, routine };
    locate(&gather);

    struct gather_ranges held = { own, { reverse ? bytes : 0, reverse ? bytes : 0 } };
    uint32_t shares = 0;
    for (uint32_t s = 0; s < steps; s++) {
        uint64_t power = UINT64_C(1) << s;
        uint32_t receiver = lead_of(team, (uint32_t)((i + n - power) % n), routine);
        waits_for(team_phase, OSOE_DATA_WAIT_READY, receiver, s);     /* as Bruck's (D1) */
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, ready_counter(team, s), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        if (s == 0)
            check_shape(team, &mine, receiver, routine);
        uint32_t count = shares_at(team_phase->count, s);
        struct gather_ranges sent = held;
        if (s + 1 == steps) {
            /* The cut, at the receiver's own share, which it put here as it
             * entered: in `first` where these positions stop short of the
             * wrap, else in `second`. */
            struct gather_share theirs;
            unsigned char *line = internal(ready_counter(team, s) + SHARE_OFFSET);
            memcpy(&theirs, line, sizeof theirs);
            cleared(line, sizeof theirs);
            bool in_first = i + count < n;
            struct gather_range *cut = in_first ? &sent.first : &sent.second;
            uint64_t at = reverse ? theirs.range.end : theirs.range.start;
            if (theirs.epoch != epoch || !within(theirs.range, bytes) || at < cut->start ||
                at > cut->end) {
                char what[160];
                describe_ranges(&held, what, sizeof what);
                osoe_fatal("%s: node %u's lead (PE %u) says its share of epoch %u's gather is "
                           "bytes %llu up to %llu, and this node's lead (PE %u), in epoch %u's "
                           "gather of %llu bytes, holds %s, which do not run on into it: PEs "
                           "called different collectives, or the leads disagree about the "
                           "team's nodes", routine, osoe_node_of(&osoe.job, receiver), receiver,
                           theirs.epoch, (unsigned long long)theirs.range.start,
                           (unsigned long long)theirs.range.end, osoe.job.me, epoch,
                           (unsigned long long)bytes, what);
            }
            if (reverse)
                cut->start = at;
            else
                cut->end = at;
            if (in_first)
                sent.second = (struct gather_range){ reverse ? bytes : 0, reverse ? bytes : 0 };
        }
        put_ranges(&gather, context, (int)receiver, &sent);
        struct gather_record record = { epoch, 0, sent };
        osoe_transport_put(context, (int)receiver, OSOE_REGION_INTERNAL,
                           data_counter(team, s) + RECORD_OFFSET, &record, sizeof record,
                           OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)receiver, OSOE_REGION_INTERNAL, data_counter(team, s),
                           OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
        shares += count;

        /* This step's own record, from the lead 2^s on: once DATA says the
         * step landed, taken, checked and merged into what this lead holds. */
        waits_for(team_phase, OSOE_DATA_WAIT_PIECE,
                  lead_of(team, (uint32_t)((i + power) % n), routine), s);
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, data_counter(team, s), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        struct gather_record got;
        unsigned char *line = internal(data_counter(team, s) + RECORD_OFFSET);
        memcpy(&got, line, sizeof got);
        cleared(line, sizeof got);
        struct gather_ranges merged = held;
        bool fits = got.epoch == epoch && within(got.ranges.first, bytes) &&
                    within(got.ranges.second, bytes);
        if (fits && i + power < n)
            fits = continued(&merged.first, got.ranges.first, reverse) &&
                   continued(&merged.second, got.ranges.second, reverse);
        else if (fits)
            fits = got.ranges.second.start == got.ranges.second.end &&
                   continued(&merged.second, got.ranges.first, reverse);
        /* And the two never meet past this node's share: what wraps stays below
         * its base in order, above its share's end in reverse - a step that ran
         * on into the share would rewrite it (the model's no_cut). */
        if (fits)
            fits = reverse ? merged.first.end <= merged.second.start
                           : merged.second.end <= merged.first.start;
        if (!fits) {
            uint32_t writer = lead_of(team, (uint32_t)((i + power) % n), routine);
            char theirs[160], ours[160];
            describe_ranges(&got.ranges, theirs, sizeof theirs);
            describe_ranges(&held, ours, sizeof ours);
            osoe_fatal("%s: step %u of the gather from node %u's lead (PE %u) says epoch %u "
                       "and %s, and this node's lead (PE %u), in epoch %u's gather of %llu "
                       "bytes, holds %s, which those do not continue: PEs called different "
                       "collectives, or the leads disagree about the team's nodes", routine, s,
                       osoe_node_of(&osoe.job, writer), writer, got.epoch, theirs, osoe.job.me,
                       epoch, (unsigned long long)bytes, ours);
        }
        held = merged;
    }
    /* The whole result, as two ranges meeting at this node's share. */
    struct gather_ranges whole = reverse
        ? (struct gather_ranges){ { 0, share_end }, { share_end, bytes } }
        : (struct gather_ranges){ { share_start, bytes }, { 0, share_start } };
    if (memcmp(&held, &whole, sizeof held) != 0) {
        char what[160];
        describe_ranges(&held, what, sizeof what);
        osoe_fatal("%s: team %u's gather by ranges ended with this node's lead holding %s of a "
                   "result of %llu bytes: a broken invariant of the allgather", routine, number,
                   what, (unsigned long long)bytes);
    }
    team_phase->gathers = want;
    /* Single-writer publication for osoe-stat and the tests (§3.16), before the
     * lead bumps the team's `released`, as the gather by places publishes. */
    atomic_store_explicit(&team_phase->published->gather_steps, steps, memory_order_release);
    atomic_store_explicit(&team_phase->published->gather_shares, shares, memory_order_release);
}

/* ---- collect's node totals by a direct exchange (P33) -------------------- */

/*
 * docs/design/collect-scan-proposal.md, "A direct exchange of the totals", and
 * tools/modelcheck/exchange_model.py, whose leads these are. P29's way gives
 * every lead every node's total through a reduction of a vector of the nodes,
 * up the tree and down; the exchange gives the same in one hop: each lead puts
 * its node's total, with the collect's epoch, into its own position's slot at
 * every other lead and adds one to that lead's counter, fenced after the put,
 * and a lead whose counter shows the other N - 1 reads its slots. Nothing waits
 * before writing them - the reason it is one hop - so each team number keeps
 * two sets of slots and a counter for each (internal-region.h), the exchange's
 * parity choosing, and both rules hold without leaning on what follows:
 *   - **the slots**: a lead writing exchange x's total has finished x - 1, which
 *     it finished only with every lead's x - 1 total, which each lead put as it
 *     entered x - 1, after finishing x - 2 - after reading x - 2's slots, the
 *     ones x reuses. One set fails (the model's one_slot): a lead that finished
 *     a collect of nothing - which has no gather - writes the next collect's
 *     total over a slot its reader has yet to read;
 *   - **the counters**: an add for x + 1 into a counter that x also counts can
 *     arrive before a slower lead's add for x, and the wait for x would read a
 *     slot not yet written (one_counter); within a parity an add for x + 2 comes
 *     only after its sender finished x + 1, so after every lead entered x + 1 and
 *     finished its wait for x.
 * The counters are cumulative over the team's life and compared as at least: a
 * parity's gains N - 1 an exchange of that parity, kept in exchange_base. Each
 * slot names its collect's epoch, and a total of another is named with its
 * sender, as a piece's header is (§5.5's "asserted on receipt"); a reader
 * clears what it read. A lead's puts begin at the next position on, so that no
 * lead is everyone's first. Costs, with N nodes: a hop and N - 1 messages out of
 * every lead and as many in, against the tree's 2 ceil(log_k N) levels; O(N) a
 * lead to read.
 */
struct exchange_slot {
    uint32_t epoch;
    uint32_t reserved;
    uint64_t total;
};
_Static_assert(sizeof(struct exchange_slot) == OSOE_INTERNAL_EXCHANGE_SLOT_BYTES,
               "an exchange's slot is the size the region keeps for it");

static uint32_t exchange_positions(void)
{
    return osoe_internal_exchange_positions(osoe.bootstrap.node_count);
}

/* Team number `number`'s exchange words. */
static uint64_t exchange_of_number(uint32_t number)
{
    return osoe_internal_exchange_offset(osoe.bootstrap.collective_fanout,
                                         osoe.bootstrap.max_teams,
                                         osoe.bootstrap.small_collective_bytes,
                                         osoe.bootstrap.node_count) +
           (uint64_t)number * osoe_internal_team_exchange_bytes(exchange_positions());
}

/* Set `parity`'s counter, a line each. */
static uint64_t exchange_counter(uint32_t number, uint32_t parity)
{
    return exchange_of_number(number) + (uint64_t)parity * OSOE_INTERNAL_LINE_BYTES;
}

/* Set `parity`'s slot for the node at `position`, past both counters' lines. */
static uint64_t exchange_slot(uint32_t number, uint32_t parity, uint32_t position)
{
    return exchange_of_number(number) + 2 * (uint64_t)OSOE_INTERNAL_LINE_BYTES +
           ((uint64_t)parity * exchange_positions() + position) *
               OSOE_INTERNAL_EXCHANGE_SLOT_BYTES;
}

uint32_t osoe_internode_generic_exchange_most_nodes(void)
{
    return exchange_positions();
}

void osoe_internode_generic_exchange_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                                 uint64_t *totals, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    uint32_t n = team_phase->count, i = team_phase->index, number = number_of(team);
    if (n > exchange_positions())
        osoe_fatal("%s: team %u spans %u nodes, and the exchange of collect's totals has room "
                   "for %u", routine, number, n, exchange_positions());
    uint32_t parity = (uint32_t)(team_phase->exchanges % 2);
    struct exchange_slot mine = { epoch, 0, own };
    struct osoe_transport_context *context = osoe_transport_context_default();
    for (uint32_t d = 1; d < n; d++) {
        int peer = (int)lead_of(team, (i + d) % n, routine);
        osoe_transport_put(context, peer, OSOE_REGION_INTERNAL, exchange_slot(number, parity, i),
                           &mine, sizeof mine, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, peer, OSOE_REGION_INTERNAL, exchange_counter(number, parity),
                           OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED | (d + 1 < n ? OSOE_TRANSPORT_MORE : 0));
        osoe_perturb();
    }
    team_phase->exchange_base[parity] += n - 1;
    /* Every lead's total on one counter: how many are missing is all it says
     * (D1). */
    wait_count(routine, team_phase, OSOE_DATA_WAIT_TOTALS, exchange_counter(number, parity),
               team_phase->exchange_base[parity], epoch);
    osoe_perturb();
    for (uint32_t j = 0; j < n; j++) {
        if (j == i) {
            totals[j] = own;
            continue;
        }
        struct exchange_slot got;
        unsigned char *slot = internal(exchange_slot(number, parity, j));
        memcpy(&got, slot, sizeof got);
        cleared(slot, sizeof got);
        if (got.epoch != epoch) {
            uint32_t sender = lead_of(team, j, routine);
            char theirs[64];
            if (got.epoch == 0)
                snprintf(theirs, sizeof theirs, "holds nothing: no total was put there");
            else
                snprintf(theirs, sizeof theirs, "is epoch %u's", got.epoch);
            osoe_fatal("%s: this node's slot for the total of node %u's lead (PE %u) %s, and "
                       "this node's lead (PE %u) is in epoch %u's collect: PEs called different "
                       "collectives", routine, osoe_node_of(&osoe.job, sender), sender, theirs,
                       osoe.job.me, epoch);
        }
        totals[j] = got.total;
    }
    team_phase->exchanges++;
}

/* Every counter of team number `number` in this PE's internal region
 * (internal-region.h): its arrivals, its release and its grant, the grant's
 * words, its gathers' READY and DATA lines whole - the counters, and past them
 * the shape, the records and the shares of collect's gather by ranges (P30) -
 * its scans' rows and their grant, its reduce-scatter's words - the counters,
 * the grant's words and the senders' lanes - and its exchange's counters and
 * slots (P33). Atomic stores, as
 * each counter is written by the transport's adds; the split's sync that
 * follows orders them before any. Zeroing the grant counter keeps the count
 * exact against the team that held the number before - a grant's epoch and
 * identity cannot tell that team's last grant from the next team's first,
 * since epochs start again at 1 under the same number - and the grant's words,
 * which take_grant and scan_take_grant also clear as they read them, are the
 * second guard: a stale count meets words that say "nothing", and the job ends
 * by name. */
void osoe_internode_generic_number_taken(uint32_t number)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    uint32_t steps = osoe_internal_gather_steps(osoe.bootstrap.node_count);
    for (uint32_t line = 0; line < fanout + 2; line++)
        atomic_store_explicit((_Atomic uint64_t *)internal(arrival_counter(number, line)), 0,
                              memory_order_release);
    memset(internal(grant_words(number)), 0, sizeof(struct grant));
    for (uint32_t position = 0; position < fanout + 2; position++)
        atomic_store_explicit(
            (_Atomic uint64_t *)internal(scan_counter_of_number(number, position)), 0,
            memory_order_release);
    memset(internal(scan_grant_words(number)), 0, sizeof(struct grant));
    for (uint32_t line = 0; line < 2 * steps; line++) {
        uint64_t at = gather_of_number(number) + (uint64_t)line * OSOE_INTERNAL_LINE_BYTES;
        atomic_store_explicit((_Atomic uint64_t *)internal(at), 0, memory_order_release);
        memset(internal(at + 8), 0, OSOE_INTERNAL_LINE_BYTES - 8);
    }
    uint32_t rows = circulant_layout_rows();
    for (uint32_t line = 0;
         rows > 0 && line <= osoe_internal_circulant_reduce_scatter_grant_line(rows); line++)
        atomic_store_explicit((_Atomic uint64_t *)internal(circulant_line(number, line)), 0,
                              memory_order_release);
    if (rows > 0) {
        memset(internal(circulant_reduce_scatter_grant_words(number)), 0,
               (size_t)rows * sizeof(struct circulant_grant_field));
        memset(internal(circulant_senders(number)), 0, OSOE_INTERNAL_LINE_BYTES);
    }
    if (exchange_positions() > 0) {
        for (uint32_t parity = 0; parity < 2; parity++)
            atomic_store_explicit((_Atomic uint64_t *)internal(exchange_counter(number, parity)),
                                  0, memory_order_release);
        memset(internal(exchange_slot(number, 0, 0)), 0,
               (size_t)2 * exchange_positions() * OSOE_INTERNAL_EXCHANGE_SLOT_BYTES);
    }
}
