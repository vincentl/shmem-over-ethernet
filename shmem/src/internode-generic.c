/*
 * The generic internode phase of the data collectives (internode-generic.h;
 * §5.5, §5.8), moved behind internode.h from collectives.c, where phase 1 step
 * 5's first slice built it.
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

/*
 * A team's internode phase as this node's lead keeps it: the reduction's tree,
 * from the team's record, and the bases its waits count from - where each child
 * position's arrival counter and the release counter stood when the team's
 * last collective ended, by this lead's own trees (the file's comment), since
 * the counters in the internal region are cumulative over the team's life and
 * never reset. One per team number (OSOE_MAX_TEAMS), allocated at init and
 * filled the first time a call names the team.
 */
struct generic_team {
    bool registered;
    uint32_t count;                     /* how many nodes it spans */
    uint32_t index;                     /* this node's position among them */
    struct lead_tree reduction_tree;
    uint64_t arrival_base[OSOE_COLLECTIVE_FANOUT_MAX];
    uint64_t release_base;
    struct osoe_collective_state *published;    /* the team's tree state (node.h) */
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

/* A gather's: the or over unsigned words that it runs as, with the next bit
 * set too, so that one is not taken for an application's or of the same
 * length - a divergence the pieces would otherwise hide. */
enum { GATHER_BIT = 30 };
static uint32_t gather_identity(struct osoe_element element)
{
    return reduction_identity(OSOE_REDUCTION_OR, element) | UINT32_C(1) << GATHER_BIT;
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
    else if (h->collective >> 31 && (h->collective >> GATHER_BIT & 1))
        snprintf(out, size, "piece %u (%u bytes) of epoch %u's collect", h->piece, h->bytes,
                 h->epoch);
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

/*
 * Zero every place but this node's members': the or needs each byte to be one
 * node's. The members are the team's, by team rank (internode.h), which the
 * SHMEM layer lists in ascending order, so the places to zero are the gaps
 * between theirs: O(L) calls whatever the team's size, with the bytes zeroed
 * the result's less this node's own. A node whose members are one run of
 * ranks, as Slurm's block distribution makes the world's, zeroes two runs.
 */
static void zero_others(uint32_t team, unsigned char *buffer, size_t bytes, size_t block,
                        const uint64_t *places)
{
    uint32_t count = 0;
    const uint32_t *ranks = osoe_internode_team_local_ranks(team, &count);
    uint64_t from = 0;
    for (uint32_t m = 0; m < count; m++) {
        uint64_t at = place_of(places, block, ranks[m]);
        memset(buffer + from, 0, at - from);
        from = place_of(places, block, (uint64_t)ranks[m] + 1);
    }
    memset(buffer + from, 0, bytes - from);
}

/*
 * internode.h's gather, as the or of every node's buffer once the others'
 * places are zero: blocks at disjoint places, or'd, are their concatenation.
 * It runs on the reduction's tree - no protocol of its own, and nothing
 * written in another node but its internal region. In eight-byte words when
 * the buffer allows, which it does alike on every node: its length is the
 * same everywhere, and its place is, being symmetric in a page-aligned region.
 */
void osoe_internode_generic_collect(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                                    size_t block, const uint64_t *places, const char *routine)
{
    struct generic_team *team_phase = registered_team(team, routine);
    zero_others(team, buffer, bytes, block, places);
    uint32_t width = bytes % 8 == 0 && (uintptr_t)buffer % 8 == 0 ? 8 : 1;
    struct osoe_element element = { OSOE_ELEMENT_UNSIGNED_INTEGER, width };
    osoe_combine_function combine = osoe_combine_lookup(OSOE_REDUCTION_OR, element);
    struct pieces pieces = { team, epoch, gather_identity(element), buffer, bytes,
                             piece_bytes(width), width, combine, routine };
    in_pieces(team_phase, &pieces, &team_phase->reduction_tree);
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
