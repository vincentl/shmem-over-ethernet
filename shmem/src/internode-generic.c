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
 * **A vector larger than a slot goes through the tree in slot-sized pieces,
 * and each lead keeps D of them in flight** (§5.8, "Pipelined pieces"; phase 1
 * step 5 slice 5): D is OSOE_COLLECTIVE_PIPELINE, which every node agrees on,
 * and a parent keeps D slots per child position and every node D release
 * slots, piece k in slot k mod D (internal-region.h). The loop, in_pieces, is
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
 * positions in it: the reduction's tree is the team's record's (node.h), read
 * the first time a call names the team, rooted at position 0 and naming each
 * neighbour by its lead's rank; a broadcast's is the same tree rotated to its
 * root's position, from this node's position in the record, with arithmetic
 * alone, and names each node's lead by position through the SHMEM layer
 * (internode.h, osoe_internode_team_lead): for the predefined teams, which
 * span every node, the job's table of each node's local id 0 (job.h); for a
 * team a split creates, the list of every node's lead - its member with the
 * lowest team rank - that each node's local lead of the team keeps (team.c;
 * phase 1 step 7 slice 2). Each team number has its own slots and words in
 * the internal region, so two teams whose leads coincide on a node never meet
 * there, whatever their trees share.
 *
 * Costs, with N nodes, k the fan-out, n bytes and p = ceil(n / slot) pieces:
 * O(k n) combining and k puts per node per piece; and since each lead keeps D
 * pieces between sending them up and holding their release, and takes each
 * input as it comes, the p pieces take about p / D round trips of the tree's
 * depth ceil(log_k N) where one at a time took p, for as long as the round trip
 * rather than the links is the limit (a timing simulation of the loop gives D
 * times at depths 1 to 4, for D of 2, 4 and 8). A broadcast finds its rotated
 * tree's k + 1 neighbouring leads in O(k), and a reduction's tree is found once
 * per team. State: O(k) per team.
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
 * Every put but a step's last operation - its fenced add - says more follows
 * (OSOE_TRANSPORT_MORE). State: the lists, O(intervals) per team.
 *
 * **A lead says what gather it has entered** (§5.5's "asserted on receipt"),
 * first of all: into the READY[0] line of the lead that writes to it at step
 * 0, past the counter, it puts the gather's shape - the team's epoch, the
 * result's bytes, and whether places lay it out or a block, the block then,
 * which is every node's alike; collect's second gather's blocks differ from
 * node to node and its places are what every node shares - and its add to
 * that READY[0] is fenced after it. The writer compares the shape with its own
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

#include "collective-tree.h"
#include "combine.h"
#include "fatal.h"
#include "internal-region.h"
#include "internode-generic.h"
#include "job.h"
#include "node.h"
#include "region-wait.h"
#include "state.h"
#include "transport.h"

/*
 * The internal region's layout (internal-region.h): a block of slots per team
 * number, then the words. A team's block of slots is a row of D per child
 * position - that child's pieces, piece k in the row's slot k mod D - and a
 * last row the parent's releases land in, likewise. Its words are an
 * eight-byte arrival counter per child position (that child's pieces in) and
 * then the release counter (the parent's pieces down), each alone on a line of
 * the region's fixed 128 bytes. Each team number has its own, so two teams
 * whose trees share a lead put into different places (§7.3).
 */
static uint64_t child_slot(uint32_t team, uint32_t position, uint64_t piece)
{
    uint32_t pipeline = osoe.bootstrap.collective_pipeline;
    return (uint64_t)team * osoe_internal_team_slots_bytes(osoe.bootstrap.collective_fanout,
                                                           pipeline) +
           ((uint64_t)position * pipeline + piece % pipeline) * OSOE_INTERNAL_SLOT_BYTES;
}

static uint64_t release_slot(uint32_t team, uint64_t piece)
{
    return child_slot(team, osoe.bootstrap.collective_fanout, piece);
}

static uint64_t arrival_counter(uint32_t team, uint32_t position)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    return osoe_internal_words_offset(fanout, osoe.bootstrap.collective_pipeline,
                                      osoe.bootstrap.max_teams) +
           (uint64_t)team * osoe_internal_team_words_bytes(fanout) +
           (uint64_t)position * OSOE_INTERNAL_LINE_BYTES;
}

static uint64_t release_counter(uint32_t team)
{
    return arrival_counter(team, osoe.bootstrap.collective_fanout);
}

/*
 * The allgather's words (internal-region.h): per team number, past the line
 * that counts the team numbers claimed, READY for each step and then DATA for
 * each, each alone on its line, with room for as many steps as the job's
 * node count takes - the most any team's gather takes.
 */
static uint64_t gather_word(uint32_t team, uint32_t line)
{
    uint32_t steps = osoe_internal_gather_steps(osoe.bootstrap.node_count);
    return osoe_internal_gather_offset(osoe.bootstrap.collective_fanout,
                                       osoe.bootstrap.collective_pipeline,
                                       osoe.bootstrap.max_teams) +
           (uint64_t)team * osoe_internal_team_gather_bytes(steps) +
           (uint64_t)line * OSOE_INTERNAL_LINE_BYTES;
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
 * A team's internode phase as this node's lead keeps it: the reduction's tree,
 * from the team's record, and the bases its waits count from - where each child
 * position's arrival counter and the release counter stood when the team's
 * last collective ended, by this lead's own trees (the file's comment), since
 * the counters in the internal region are cumulative over the team's life and
 * never reset. One per team number (OSOE_MAX_TEAMS), allocated at init and
 * filled the first time a call names the team. And what its gathers keep:
 * the gathers so far, which READY and DATA count; and, found at the first and
 * the same for every one after it, the steps, the team's size, the (node,
 * step) pairs the lists hold, and each step's intervals of members.
 */
struct generic_team {
    bool registered;
    uint32_t count;                     /* how many nodes it spans */
    uint32_t index;                     /* this node's position among them */
    struct lead_tree reduction_tree;
    uint64_t arrival_base[OSOE_COLLECTIVE_FANOUT_MAX];
    uint64_t release_base;
    struct osoe_collective_state *published;    /* the team's tree state (node.h) */
    uint64_t gathers;
    uint32_t steps;
    uint32_t members;
    uint32_t shares;
    struct interval_list *step_intervals;
    /* The pieces its scans have moved (osoe_internode_generic_scan): what each
     * scan slot a member uses has received, since the team's tree is one. */
    uint64_t scan_pieces;
};

static struct generic_team *teams;
static uint32_t team_count;

void osoe_internode_generic_init(void)
{
    team_count = osoe.bootstrap.max_teams;
    teams = calloc(team_count, sizeof *teams);
    if (!teams)
        osoe_fatal("out of memory for %u teams' internode phases", team_count);
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
 * the rest (§3.16) - the node count, this node's position in the team's node
 * set, and its place in the reduction's tree, rooted at position 0, whose
 * leads the record names by rank. Any team whose record says it spans nodes:
 * the predefined ones, and since phase 1 step 7's second slice those a split
 * creates, whose leads are wherever their lowest team ranks are - which the
 * record names, and osoe_internode_team_lead gives by position.
 */
static struct generic_team *registered_team(uint32_t team, const char *routine)
{
    if (team >= team_count)
        osoe_fatal("%s: team %u is past the job's %u team numbers", routine, team, team_count);
    struct generic_team *g = &teams[team];
    if (!g->registered) {
        const struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
        if (!atomic_load_explicit(&b->record.published, memory_order_acquire) ||
            b->record.nodes < 2)
            osoe_fatal("%s: team %u has no internode phase on this node", routine, team);
        const struct osoe_team_record *r = &b->record;
        if (r->children > osoe.bootstrap.collective_fanout || r->index >= r->nodes)
            osoe_fatal("%s: team %u's record on this node says %u children at position %u of "
                       "%u nodes, at a fan-out of %u", routine, team, r->children, r->index,
                       r->nodes, osoe.bootstrap.collective_fanout);
        g->count = r->nodes;
        g->index = r->index;
        g->reduction_tree.parent = r->parent;
        g->reduction_tree.position = r->position;
        g->reduction_tree.children = r->children;
        for (uint32_t c = 0; c < r->children; c++)
            g->reduction_tree.child[c] = r->child[c];
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
                   routine, team, index);
    return rank;
}

/*
 * §5.3's k-ary tree over the team's node set, positions 0 ... N - 1, rotated
 * so that `root` is position 0: position i's parent is (i - 1) / k and its
 * children k i + 1 ... k i + k. The reduction's tree when the root is position
 * 0; a broadcast roots it at its root's node, so the data travels depth levels
 * and not up to position 0 first.
 */
static void find_lead_tree(const struct generic_team *g, uint32_t team, uint32_t root,
                           struct lead_tree *out, const char *routine)
{
    uint32_t n = g->count, k = osoe.bootstrap.collective_fanout;
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
 * What a piece carries ahead of its bytes (the file's comment). Sixteen bytes,
 * so that the bytes after it stay aligned for every element.
 */
struct piece_header {
    uint32_t epoch;                     /* the team's epoch of the collective */
    uint32_t piece;                     /* which of its pieces */
    uint32_t collective;                /* which: reduction_identity, or the root */
    uint32_t bytes;                     /* this piece's */
};
enum { HEADER_BYTES = 16 };
_Static_assert(sizeof(struct piece_header) == HEADER_BYTES, "a piece's header is 16 bytes");

/* A reduction's `collective`, its top bit set; a broadcast's is its root's index. */
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

/*
 * One collective's pieces, as every lead cuts them alike: its buffer, `bytes`
 * long, in pieces of `bytes_per_piece` - a whole number of elements that fits
 * a slot behind its header - the last of which may be shorter; and what each
 * piece's header says besides its number and bytes. `combine` is NULL for a
 * broadcast, whose pieces go up empty.
 */
struct pieces {
    uint32_t team;
    uint32_t epoch;
    uint32_t collective;                /* the header's: reduction_identity, or the root */
    unsigned char *buffer;
    size_t bytes;
    size_t bytes_per_piece;
    size_t width;
    osoe_combine_function combine;
    const char *routine;
};

/* The most bytes of `width`-byte elements a slot holds behind its header. */
static size_t piece_bytes(size_t width)
{
    return ((OSOE_INTERNAL_SLOT_BYTES - HEADER_BYTES) / width) * width;
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

/*
 * Whether every child has sent piece `number` up: each child position's
 * arrival counter shows it, at least - a child may be up to D pieces ahead,
 * and the counter holds the team's whole life, so it is compared with the
 * position's base plus the piece. The loads are acquire (transport.h), so the
 * bytes a child put before its fenced add are there to read.
 */
static bool children_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                          const struct lead_tree *tree, uint64_t number)
{
    for (uint32_t c = 0; c < tree->children; c++)
        if (osoe_transport_load(OSOE_REGION_INTERNAL, arrival_counter(pieces->team, c), 8) <
            team_phase->arrival_base[c] + number + 1)
            return false;
    return true;
}

/* Whether the parent has sent piece `number`'s release down: likewise, the
 * release counter against the release base. */
static bool release_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                         uint64_t number)
{
    return osoe_transport_load(OSOE_REGION_INTERNAL, release_counter(pieces->team), 8) >=
           team_phase->release_base + number + 1;
}

/*
 * Piece `number` from every child, once children_sent says it is in: check
 * the header in the position's slot for the piece and combine what follows it
 * into `at`. Position by position, in order, so that every run combines alike
 * (§5.8 rule 3). A broadcast's children put nothing: their arrival says only
 * that their release slot for this piece is free.
 */
static void combine_children(const struct pieces *pieces, const struct lead_tree *tree,
                             const struct piece_header *header, uint64_t number,
                             unsigned char *at)
{
    if (!pieces->combine)
        return;
    for (uint32_t c = 0; c < tree->children; c++) {
        const unsigned char *slot = internal(child_slot(pieces->team, c, number));
        check_piece(slot, header, "a child's partial result", tree->child[c], pieces->routine);
        pieces->combine(at, slot + HEADER_BYTES, header->bytes / pieces->width);
    }
}

/* Piece `number`'s release, once release_sent says it is in: check the header
 * in the release slot for the piece, and copy what follows it out to `at`. */
static void copy_release(const struct pieces *pieces, const struct lead_tree *tree,
                         const struct piece_header *header, uint64_t number, unsigned char *at)
{
    const unsigned char *slot = internal(release_slot(pieces->team, number));
    check_piece(slot, header, "the result from the parent", tree->parent, pieces->routine);
    memcpy(at, slot + HEADER_BYTES, header->bytes);
}

/* Piece `number` up: the partial result in `at` into the parent's slot for
 * this node's position and the piece, behind its header - a broadcast's goes
 * up empty - then one more on the parent's arrival counter for the position,
 * fenced, so that the add lands after the bytes (invariant 5). */
static void to_parent(const struct pieces *pieces, const struct lead_tree *tree,
                      const struct piece_header *header, uint64_t number,
                      const unsigned char *at)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    if (pieces->combine) {
        uint64_t slot = child_slot(pieces->team, tree->position, number);
        osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL, slot, header,
                           HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL, slot + HEADER_BYTES,
                           at, header->bytes, OSOE_TRANSPORT_MORE);
    }
    osoe_transport_rmw(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                       arrival_counter(pieces->team, tree->position), OSOE_RMW_ADD, 8, 1, 0,
                       OSOE_TRANSPORT_FENCED);
}

/* Piece `number`'s release down, from `at`: into each child's release slot for
 * the piece, behind its header, then one more on the child's release counter,
 * fenced after the bytes. */
static void to_children(const struct pieces *pieces, const struct lead_tree *tree,
                        const struct piece_header *header, uint64_t number,
                        const unsigned char *at)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    uint64_t slot = release_slot(pieces->team, number);
    for (uint32_t c = 0; c < tree->children; c++) {
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL, slot, header,
                           HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           slot + HEADER_BYTES, at, header->bytes, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           release_counter(pieces->team), OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (c + 1 < tree->children ? OSOE_TRANSPORT_MORE : 0));
    }
}

/*
 * What a lead waits for when neither of its inputs is ready: its children's
 * copies of piece `up`, if the window lets it send that piece, or its next
 * piece down - piece `down`'s release from its parent, or at the root every
 * child's copy of piece `down`. The team's words are contiguous, an arrival
 * counter per child position and then the release counter, so one watch
 * covers them all.
 */
struct awaited {
    const struct generic_team *team_phase;
    const struct pieces *pieces;
    const struct lead_tree *tree;
    bool sending;                       /* the window lets piece `up` go up */
    uint64_t up;
    uint64_t down;
};

static bool next_down_sent(const struct generic_team *team_phase, const struct pieces *pieces,
                           const struct lead_tree *tree, uint64_t down)
{
    return tree->parent == UINT32_MAX ? children_sent(team_phase, pieces, tree, down)
                                      : release_sent(team_phase, pieces, down);
}

static bool awaited_holds(const void *context)
{
    const struct awaited *a = context;
    return (a->sending && children_sent(a->team_phase, a->pieces, a->tree, a->up)) ||
           next_down_sent(a->team_phase, a->pieces, a->tree, a->down);
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
 * At the end, the bases advance by what this tree says each counter received,
 * and the most pieces this lead had in flight at once goes into the team's
 * tree state (node.h): min(D, pieces) at a leaf, which has no children to wait
 * for and fills its window before it looks for a release; between 1 and that
 * at a middle lead, as the schedule has it; 0 at the root, which sends nothing
 * up.
 */
static void in_pieces(struct generic_team *team_phase, const struct pieces *pieces,
                      const struct lead_tree *tree)
{
    uint64_t piece_count = (pieces->bytes + pieces->bytes_per_piece - 1) / pieces->bytes_per_piece;
    uint64_t window = osoe.bootstrap.collective_pipeline;
    bool root = tree->parent == UINT32_MAX;
    uint64_t up = 0, down = 0, most = 0;
    while (down < piece_count) {
        bool sending = !root && up < piece_count && up < down + window;
        if (sending && children_sent(team_phase, pieces, tree, up)) {
            struct piece_header header = header_of(pieces, up);
            unsigned char *at = piece_at(pieces, up);
            combine_children(pieces, tree, &header, up, at);
            to_parent(pieces, tree, &header, up, at);
            up++;
            if (up - down > most)
                most = up - down;
            continue;
        }
        if (next_down_sent(team_phase, pieces, tree, down)) {
            struct piece_header header = header_of(pieces, down);
            unsigned char *at = piece_at(pieces, down);
            if (root)
                combine_children(pieces, tree, &header, down, at);
            else
                copy_release(pieces, tree, &header, down, at);
            to_children(pieces, tree, &header, down, at);
            down++;
            continue;
        }
        struct awaited awaited = { team_phase, pieces, tree, sending, up, down };
        uint64_t first = arrival_counter(pieces->team, 0);
        osoe_region_wait_condition(pieces->routine, OSOE_REGION_INTERNAL, first,
                                   (size_t)(release_counter(pieces->team) + 8 - first),
                                   awaited_holds, &awaited, OSOE_BLOCKED_DETAIL_EPOCH,
                                   pieces->epoch);
    }
    for (uint32_t c = 0; c < tree->children; c++)
        team_phase->arrival_base[c] += piece_count;
    if (!root)
        team_phase->release_base += piece_count;
    /* Single-writer publication for osoe-stat and the tests (§3.16), and
     * before the lead bumps the team's `released`, which every member loads
     * with acquire before it returns: so once its call has returned, a member
     * of this node reads this collective's count. */
    atomic_store_explicit(&team_phase->published->most_pieces_in_flight, (uint32_t)most,
                          memory_order_release);
}

void osoe_internode_generic_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                                   enum osoe_reduction_operation operation,
                                   struct osoe_element element, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    osoe_combine_function combine = osoe_combine_lookup(operation, element);
    if (!combine || element.width > OSOE_INTERNAL_SLOT_BYTES - HEADER_BYTES)
        osoe_fatal("%s: no reduction %d over elements of kind %d and width %u", routine,
                   (int)operation, (int)element.kind, element.width);
    struct pieces pieces = { team, epoch, reduction_identity(operation, element), buffer,
                             count * element.width, piece_bytes(element.width), element.width,
                             combine, routine };
    in_pieces(team_phase, &pieces, &team_phase->reduction_tree);
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
                   team, node);
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
                   routine, team);
    if (team_phase->count > job->node_count)
        osoe_fatal("%s: team %u spans %u nodes of a job of %u", routine, team, team_phase->count,
                   job->node_count);
    uint32_t here = position_of(team_phase, team, job->node, routine);
    if (here != team_phase->index)
        osoe_fatal("%s: team %u's record puts this node, node %u, at position %u of its node "
                   "set, and its leads at %u", routine, team, job->node, team_phase->index, here);
    team_phase->steps = osoe_internal_gather_steps(team_phase->count);
    team_phase->step_intervals = calloc(team_phase->steps, sizeof *team_phase->step_intervals);
    struct member_walk walk = { team_phase, team, 0,
                                calloc(team_phase->count, sizeof *walk.steps_joined), routine };
    if (!team_phase->step_intervals || !walk.steps_joined)
        osoe_fatal("%s: out of memory for team %u's gather", routine, team);

    /* The team's members as world ranks, from its record on this node, which
     * this thread - the team's local lead here - wrote (node.h, decision E):
     * one progression, walked once. */
    const struct osoe_team_record *record = &osoe_node_team(&osoe.node, team)->record;
    if (!atomic_load_explicit(&record->published, memory_order_acquire))
        osoe_fatal("%s: team %u's record here is not published", routine, team);
    if (!record->strided)
        osoe_fatal("%s: team %u's record here is not a progression of world ranks", routine,
                   team);
    uint32_t world = record->world_start, count = record->members;
    int32_t stride = record->world_stride;
    if (stride == 0 && count > 1)
        osoe_fatal("%s: team %u's ranks are all world rank %u", routine, team, world);
    for (uint32_t k = 0; k < count;) {
        int64_t w = (int64_t)world + (int64_t)k * stride;
        const struct osoe_rank_run *run =
            w >= 0 && w < job->world_size
                ? osoe_rank_run_find(job->runs, job->run_count, (uint32_t)w) : NULL;
        if (!run)
            osoe_fatal("%s: team %u's rank %u is PE %lld, which the rank map places on no "
                       "node", routine, team, k, (long long)w);
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
                   "lists %u here", routine, walk.here, team, listed);
}

/*
 * What a lead says of the gather it has entered, in the READY[0] line of the
 * lead that writes to it at step 0, past the counter (the file's comment): the
 * team's epoch, whether the result is laid out by places - collect's second
 * gather, whose blocks differ from node to node - or by a block, which every
 * node's is then, and its bytes and block. Twenty-four bytes, in a line of
 * 128 whose first eight hold the counter.
 */
struct gather_shape {
    uint32_t epoch;
    uint32_t by_places;
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
    else if (shape->by_places)
        snprintf(out, size, "epoch %u's gather of %llu bytes, at places of their own",
                 shape->epoch, (unsigned long long)shape->bytes);
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
    if (theirs.epoch == mine->epoch && theirs.by_places == mine->by_places &&
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
               "end?", gather->routine, gather->team, (unsigned long long)rank,
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
    struct gather_shape mine = { epoch, places != NULL, bytes, places ? 0 : block };
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
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, ready_counter(team, s), 8, false,
                         OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        if (s == 0)
            check_shape(team, &mine, receiver, routine);
        put_step(&gather, &team_phase->step_intervals[s], context, (int)receiver);
        osoe_transport_rmw(context, (int)receiver, OSOE_REGION_INTERNAL, data_counter(team, s),
                           OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
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
    struct lead_tree tree;
    find_lead_tree(team_phase, team, root_index, &tree, routine);
    struct pieces pieces = { team, epoch, root_index, buffer, bytes, piece_bytes(1), 1, NULL,
                             routine };
    in_pieces(team_phase, &pieces, &tree);
}

/* ---- The scans (spec §9.10.10; phase 1 step 10 slice 1) ------------------ */

/*
 * A prefix scan over a team spanning nodes, every member at its team rank's
 * place in a pre-order k-ary tree over the ranks 0 .. M - 1
 * (docs/prompts/done/phase1-step10-slice1-prompt.md, the author's design A; the
 * protocol is tools/modelcheck/scan_model.py's `per_team`). A place covering
 * the ranks [a, b) is rank a; its children split [a + 1, b) into at most k
 * consecutive blocks, the first ones a rank longer when the split is uneven,
 * so every subtree is a contiguous range of ranks. Up: a member waits for each
 * child's subtree total, adds its own source, and puts the sum into its
 * parent's up-slot for its position, then adds one, fenced, to the parent's
 * counter for that slot. Down: the root has nothing before it; every other
 * member waits for the sum of every rank before its subtree in its down-slot.
 * That sum is its exclusive result, plus its source its inclusive one, and
 * child j is sent that plus its source plus the totals of children 0 .. j - 1.
 *
 * The slots and counters are the team number's, on every member's region
 * (internal-region.h), the counters cumulative and compared as at least: a
 * team's scans all use one tree, so each of its members expects, in every
 * slot it uses, one more piece than the team's scans have moved so far
 * (`scan_pieces`, counted per team as the generic implementation's other
 * bases are). No slot is written while its reader may still need it: a child
 * puts piece p + 1 up - of this scan or the team's next - only after its down
 * for piece p arrived, which its parent sent after reading piece p's total;
 * and a parent puts a down only after the child's total arrived, so the child
 * has read its last down. Different teams use different memory, so two
 * threads may scan two teams at once (spec §9.4), which slots shared by every
 * team would not allow (the review of the slice).
 *
 * A scan larger than a slot runs a piece at a time, each piece up and down
 * before the next. Costs, with M members and k the fan-out: 2 ceil(log_k M)
 * steps a piece, each a put and a fenced add; O(k) slots and counters a team
 * number a PE; O(k log_k M) arithmetic to find a place.
 */
static uint64_t scan_slot(uint32_t team, uint32_t position)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    return osoe_internal_scan_offset(fanout, osoe.bootstrap.collective_pipeline,
                                     osoe.bootstrap.max_teams, osoe.bootstrap.node_count) +
           (uint64_t)team * osoe_internal_team_scan_bytes(fanout) +
           (uint64_t)position * OSOE_INTERNAL_SCAN_SLOT_BYTES;
}

/* Slot `position`'s counter: a child position's below the fan-out, the
 * down-slot's at it. */
static uint64_t scan_counter(uint32_t team, uint32_t position)
{
    uint32_t fanout = osoe.bootstrap.collective_fanout;
    return scan_slot(team, fanout + 1) + (uint64_t)position * OSOE_INTERNAL_LINE_BYTES;
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

/* Wait for slot `position` of `team` to hold piece `want` of the team's
 * scans, from PE `from`. */
static void scan_wait(uint32_t team, uint32_t position, uint64_t want, int from,
                      const char *routine)
{
    osoe_region_wait(routine, OSOE_REGION_INTERNAL, scan_counter(team, position), 8, false,
                     OSOE_COMPARISON_GREATER_OR_EQUAL, want, OSOE_BLOCKED_DETAIL_PE,
                     (uint64_t)from);
}

void osoe_internode_generic_scan(uint32_t team, uint32_t rank, void *dest, const void *source,
                                 size_t count, struct osoe_element element, bool inclusive,
                                 const char *routine)
{
    const struct osoe_team_record *record = &osoe_node_team(&osoe.node, team)->record;
    if (!atomic_load_explicit(&record->published, memory_order_acquire))
        osoe_fatal("%s: team %u's record here is not published", routine, team);
    if (!record->strided || rank >= record->members)
        osoe_fatal("%s: team %u's record here is %s", routine, team,
                   !record->strided ? "not a progression of world ranks"
                                    : "shorter than this PE's rank");
    osoe_combine_function add = osoe_combine_lookup(OSOE_REDUCTION_SUM, element);
    if (!add)
        osoe_fatal("%s: no sum over elements of kind %d and width %u", routine,
                   (int)element.kind, element.width);
    uint32_t k = osoe.bootstrap.collective_fanout, members = record->members;
    uint32_t a, b, parent, position;
    scan_place(members, k, rank, &a, &b, &parent, &position);
    uint32_t children = scan_children(a, b, k);
    int child_pe[OSOE_COLLECTIVE_FANOUT_MAX];
    for (uint32_t j = 0; j < children; j++) {
        uint32_t first, end;
        scan_child(a, b, k, j, &first, &end);
        child_pe[j] = (int)((int64_t)record->world_start + (int64_t)first * record->world_stride);
    }
    int parent_pe = parent == UINT32_MAX ? -1
        : (int)((int64_t)record->world_start + (int64_t)parent * record->world_stride);

    size_t width = element.width, per_piece = OSOE_INTERNAL_SCAN_SLOT_BYTES / width;
    size_t piece_bytes = (count < per_piece ? count : per_piece) * width;
    /* Own source, the running sum, and each child's subtree total, a piece each. */
    unsigned char *buffers = malloc((size_t)(children + 2) * piece_bytes);
    if (!buffers)
        osoe_fatal("%s: out of memory for a scan's pieces", routine);
    unsigned char *own = buffers, *sum = buffers + piece_bytes, *totals = buffers + 2 * piece_bytes;
    struct osoe_transport_context *context = osoe_transport_context_default();
    struct generic_team *g = &teams[team];

    for (size_t done = 0; done < count;) {
        size_t n = count - done < per_piece ? count - done : per_piece;
        size_t bytes = n * width;
        uint64_t want = ++g->scan_pieces;
        /* This PE's source first: dest may be the same array. */
        memcpy(own, (const unsigned char *)source + done * width, bytes);
        memcpy(sum, own, bytes);
        for (uint32_t j = 0; j < children; j++) {
            scan_wait(team, j, want, child_pe[j], routine);
            unsigned char *total = totals + (size_t)j * piece_bytes;
            memcpy(total, internal(scan_slot(team, j)), bytes);
            add(sum, total, n);
        }
        /* `sum` is this subtree's total. Up, then the sum before it down. */
        unsigned char *out = (unsigned char *)dest + done * width;
        if (parent_pe >= 0) {
            osoe_transport_put(context, parent_pe, OSOE_REGION_INTERNAL, scan_slot(team, position),
                               sum, bytes, OSOE_TRANSPORT_MORE);
            osoe_transport_rmw(context, parent_pe, OSOE_REGION_INTERNAL,
                               scan_counter(team, position), OSOE_RMW_ADD, 8, 1, 0,
                               OSOE_TRANSPORT_FENCED);
            scan_wait(team, k, want, parent_pe, routine);
            memcpy(sum, internal(scan_slot(team, k)), bytes);
        } else {
            memset(sum, 0, bytes);
        }
        /* `sum` is now the sum of every rank before this subtree: this PE's
         * exclusive result. */
        memcpy(out, sum, bytes);
        if (inclusive)
            add(out, own, n);
        add(sum, own, n);
        for (uint32_t j = 0; j < children; j++) {
            osoe_transport_put(context, child_pe[j], OSOE_REGION_INTERNAL, scan_slot(team, k), sum,
                               bytes, OSOE_TRANSPORT_MORE);
            osoe_transport_rmw(context, child_pe[j], OSOE_REGION_INTERNAL, scan_counter(team, k),
                               OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
            add(sum, totals + (size_t)j * piece_bytes, n);
        }
        done += n;
    }
    free(buffers);
}
