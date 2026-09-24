/*
 * The generic internode phase of the data collectives (internode-generic.h;
 * §5.5, §5.8), moved behind internode.h from collectives.c, where phase 1 step
 * 5's first slice built it.
 *
 * **Only the node leads take part**, over §5.3's k-ary tree of nodes, and they
 * meet over the transport rather than through AM messages the agents interpret:
 * a child puts its partial result into a slot of its parent's
 * OSOE_REGION_INTERNAL and adds one to the parent's arrival word with a fenced
 * rmw, so the add lands after the bytes (invariant 5); the parent's application
 * thread waits on that word, combines each slot into its buffer, and sends up in
 * turn. The root holds the result, and it comes down the same way: a put into
 * each child's release slot and an add to its release word. The lead's thread is
 * blocked in the collective regardless and is the only writer of its buffer
 * either way, so an AM design would still have had to land each partial result
 * where that thread could read it and wake it; this does exactly that with
 * operations every transport has (§5.8). A put returns at local completion, so a
 * result sent down is the lead's to reuse at once, and §5.5's hazard of a
 * forward that reads `dest` by reference after the lead has left cannot arise.
 *
 * **A vector larger than a slot goes through the tree in slot-sized pieces**,
 * each piece a full pass up and down (§5.5's "chunk through fixed scratch").
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
 * A broadcast's pieces go up empty: a child's arrival says its release slot is
 * free, since its last piece has been copied out of it, and only then does the
 * parent write the next.
 *
 * **Every word is compared for equality** (§5.4): the arrival and release words
 * are never reset, and a lead keeps how many pieces it has seen arrive and come
 * down over the team's life. Skew is bounded as §5.4 says - a child's next piece
 * needs this piece's release, and a node's next collective needs this one's -
 * so no word runs past a value a lead waits for.
 *
 * **Trees differ from one collective to the next** - a broadcast is rooted at
 * its root's node, a reduction at the first - so a lead's next arrivals may come
 * from nodes that were not its children this time, and early. They are
 * harmless for one reason: the root releases a piece only once every node has
 * arrived for it, and a node arrives only after its own wait for its children
 * has ended; so by the time any node has this piece's release, and can start
 * the next, every node is past its wait for this piece's arrivals, and an early
 * add only counts toward the next. Nothing is written into a node's release
 * slot before that node's own arrival for the piece, and nothing into a child
 * slot while its owner reads child slots: a reduction's tree - which a
 * gather's or shares - is always the same, so its children cannot start the
 * next reduction or gather before this node has released them, and a
 * broadcast reads none. The tree barrier's model checks
 * the same up-then-down shape (tree_barrier_model.py); what is added here is
 * the data, which each release's put carries ahead of its fenced add.
 *
 * **The tree is over every node** (§7.3): the world team's node set is the
 * job's, so a node's index is its number and a broadcast's tree is the
 * reduction's rotated to its root with arithmetic alone. A split team spanning
 * some of the nodes will need its node set's positions, which are O(N) and not
 * kept (internode.h); until split teams exist that is refused by name.
 *
 * Costs, with N nodes, k the fan-out and n bytes: ceil(n / slot) passes of depth
 * ceil(log_k N) each way, with O(k n) combining and k puts per node per piece;
 * a broadcast finds its rotated tree's k + 1 neighbouring leads in the job's
 * table of leads, O(k) (job.h), and a reduction's tree is found once per team.
 * State: O(k) per team.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base/cacheline.h"
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

/* The internal region's layout (internal-region.h): the slots, then the control
 * words, per team an eight-byte arrival word (children's pieces in) and release
 * word (the parent's pieces down), each on a cache line of its own. */
enum { TEAM_WORDS_BYTES = 2 * OSOE_CACHELINE };
_Static_assert(OSOE_TEAMS * TEAM_WORDS_BYTES <= OSOE_INTERNAL_WORDS_BYTES,
               "every team's words fit the internal region's page of them");

/* Slot `position` holds that child's partial result; the one after the last
 * child's holds what the parent sends down. Only the world team spans nodes
 * today, so the slots are its; a split team (§7.3) will need its own. */
static uint64_t child_slot(uint32_t position)
{
    return (uint64_t)position * OSOE_INTERNAL_SLOT_BYTES;
}

static uint64_t release_slot(void)
{
    return child_slot(osoe.bootstrap.collective_fanout);
}

static uint64_t arrivals_word(uint32_t team)
{
    return child_slot(osoe.bootstrap.collective_fanout + 1) + (uint64_t)team * TEAM_WORDS_BYTES;
}

static uint64_t release_word(uint32_t team)
{
    return arrivals_word(team) + OSOE_CACHELINE;
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
 * found once, and how many pieces have arrived at this lead from its children
 * and come down to it from its parent over the team's life - cumulative, and
 * compared for equality (§5.4), since the counts in the internal region are
 * never reset.
 */
struct generic_team {
    bool registered;
    bool every_node;                    /* the node set is 0 ... N - 1 */
    uint32_t count;                     /* how many nodes it spans */
    bool reduction_tree_known;
    struct lead_tree reduction_tree;
    uint64_t arrivals_expected;
    uint64_t releases_expected;
};

static struct generic_team teams[OSOE_TEAMS];

void osoe_internode_generic_init(void)
{
    memset(teams, 0, sizeof teams);
}

void osoe_internode_generic_team(uint32_t team, const uint32_t *nodes, uint32_t count)
{
    (void)nodes;
    if (team >= OSOE_TEAMS)
        osoe_fatal("team %u has an internode phase, and this build has %u teams", team,
                   OSOE_TEAMS);
    struct generic_team *g = &teams[team];
    g->registered = true;
    g->count = count;
    /* The set is sorted and free of repeats (the backend's registration checks
     * it), so one as large as the job's is every node. */
    g->every_node = count == osoe.job.node_count;
}

/*
 * The team's state, for the world team only: the slots are the world team's,
 * and so is the lead a node's local id 0 is - a split team (§7.3) spanning
 * every node would pass a check on its node set and still write into the
 * wrong PE's region and share the world's slots, so the refusal is by team.
 */
static struct generic_team *registered_team(uint32_t team, const char *routine)
{
    if (team >= OSOE_TEAMS || !teams[team].registered)
        osoe_fatal("%s: team %u has no internode phase registered", routine, team);
    struct generic_team *g = &teams[team];
    if (team != OSOE_TEAM_WORLD || !g->every_node)
        osoe_fatal("%s: team %u spans %u of the job's %u nodes, and the generic internode "
                   "phase runs the world team's only (§7.3)", routine, team, g->count,
                   osoe.job.node_count);
    return g;
}

static uint32_t lead_of(uint32_t node, const char *routine)
{
    uint32_t rank = osoe_job_node_lead(&osoe.job, node);
    if (rank == UINT32_MAX)
        osoe_fatal("%s: the rank map has no lead for node %u", routine, node);
    return rank;
}

/*
 * §5.3's k-ary tree over the team's nodes, 0 ... N - 1, with the indices
 * rotated so that `root` is index 0: node index i's parent is (i - 1) / k and
 * its children k i + 1 ... k i + k. The same shape as the barrier's tree when
 * the root is node 0; a broadcast roots it at its root's node, so the data
 * travels depth levels and not up to node 0 first.
 */
static void find_lead_tree(const struct generic_team *g, uint32_t root, struct lead_tree *out,
                           const char *routine)
{
    uint32_t n = g->count, k = osoe.bootstrap.collective_fanout;
    uint32_t index = (osoe.job.node + n - root) % n;
    out->parent = UINT32_MAX;
    out->position = 0;
    if (index > 0) {
        out->parent = lead_of(((index - 1) / k + root) % n, routine);
        out->position = (index - 1) % k;
    }
    out->children = 0;
    for (uint32_t c = 0; c < k; c++) {
        uint64_t child = (uint64_t)k * index + 1 + c;
        if (child >= n)
            break;
        out->child[out->children++] = lead_of((uint32_t)((child + root) % n), routine);
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
 * One piece: `expected->bytes` of this lead's buffer at `at`. A reduction's
 * children send their partial results up, combined into `at` (combine
 * non-NULL); a broadcast's send nothing up but their arrival. Then the result
 * comes down from the parent into `at`, and goes on to the children.
 */
static void lead_piece(struct generic_team *g, uint32_t team, const struct piece_header *expected,
                       const struct lead_tree *tree, unsigned char *at, size_t width,
                       osoe_combine_function combine, const char *routine)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    size_t bytes = expected->bytes;

    if (tree->children) {
        g->arrivals_expected += tree->children;
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, arrivals_word(team), 8, false,
                         OSOE_COMPARISON_EQUAL, g->arrivals_expected,
                         OSOE_BLOCKED_DETAIL_EPOCH, expected->epoch);
        if (combine)
            for (uint32_t c = 0; c < tree->children; c++) {
                const unsigned char *slot = internal(child_slot(c));
                check_piece(slot, expected, "a child's partial result", tree->child[c],
                            routine);
                combine(at, slot + HEADER_BYTES, bytes / width);
            }
    }
    if (tree->parent != UINT32_MAX) {
        if (combine) {
            osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                               child_slot(tree->position), expected, HEADER_BYTES,
                               OSOE_TRANSPORT_MORE);
            osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                               child_slot(tree->position) + HEADER_BYTES, at, bytes,
                               OSOE_TRANSPORT_MORE);
        }
        osoe_transport_rmw(context, (int)tree->parent, OSOE_REGION_INTERNAL, arrivals_word(team),
                           OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
        g->releases_expected += 1;
        osoe_region_wait(routine, OSOE_REGION_INTERNAL, release_word(team), 8, false,
                         OSOE_COMPARISON_EQUAL, g->releases_expected,
                         OSOE_BLOCKED_DETAIL_EPOCH, expected->epoch);
        const unsigned char *slot = internal(release_slot());
        check_piece(slot, expected, "the result from the parent", tree->parent, routine);
        memcpy(at, slot + HEADER_BYTES, bytes);
    }
    for (uint32_t c = 0; c < tree->children; c++) {
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL, release_slot(),
                           expected, HEADER_BYTES, OSOE_TRANSPORT_MORE);
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL,
                           release_slot() + HEADER_BYTES, at, bytes, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->child[c], OSOE_REGION_INTERNAL, release_word(team),
                           OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (c + 1 < tree->children ? OSOE_TRANSPORT_MORE : 0));
    }
}

/* The whole buffer, piece by piece, each a whole number of elements that fits
 * a slot behind its header. */
static void in_pieces(struct generic_team *g, uint32_t team, uint32_t epoch, uint32_t collective,
                      const struct lead_tree *tree, unsigned char *buffer, size_t bytes,
                      size_t width, osoe_combine_function combine, const char *routine)
{
    size_t piece = ((OSOE_INTERNAL_SLOT_BYTES - HEADER_BYTES) / width) * width;
    uint32_t number = 0;
    for (size_t done = 0; done < bytes; done += piece, number++) {
        struct piece_header expected = {
            .epoch = epoch,
            .piece = number,
            .collective = collective,
            .bytes = (uint32_t)(bytes - done < piece ? bytes - done : piece),
        };
        lead_piece(g, team, &expected, tree, buffer + done, width, combine, routine);
    }
}

void osoe_internode_generic_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                                   enum osoe_reduction_operation operation,
                                   struct osoe_element element, const char *routine)
{
    struct generic_team *g = registered_team(team, routine);
    osoe_combine_function combine = osoe_combine_lookup(operation, element);
    if (!combine || element.width > OSOE_INTERNAL_SLOT_BYTES - HEADER_BYTES)
        osoe_fatal("%s: no reduction %d over elements of kind %d and width %u", routine,
                   (int)operation, (int)element.kind, element.width);
    if (!g->reduction_tree_known) {
        find_lead_tree(g, 0, &g->reduction_tree, routine);
        g->reduction_tree_known = true;
    }
    in_pieces(g, team, epoch, reduction_identity(operation, element), &g->reduction_tree, buffer,
              count * element.width, element.width, combine, routine);
}

/* Member r's place in a gather's result (internode.h). */
static uint64_t place_of(const uint64_t *places, size_t block, uint64_t r)
{
    return places ? places[r] : r * (uint64_t)block;
}

/*
 * Zero every place but this node's members': the or needs each byte to be one
 * node's. Only the world team spans nodes (the file's comment), so a member's
 * number is its rank. The node's ranks one block, as Slurm's block
 * distribution makes them, are two runs of zeros; otherwise each rank is asked.
 */
static void zero_others(unsigned char *buffer, size_t bytes, size_t block,
                        const uint64_t *places)
{
    const struct osoe_node *n = &osoe.node;
    if (n->local_block) {
        uint64_t from = place_of(places, block, n->first_rank);
        uint64_t to = place_of(places, block, (uint64_t)n->first_rank + n->local_count);
        memset(buffer, 0, from);
        memset(buffer + to, 0, bytes - to);
        return;
    }
    for (uint64_t r = 0; r < osoe.bootstrap.world_size; r++)
        if (osoe_node_local_id(n, (uint32_t)r) < 0)
            memset(buffer + place_of(places, block, r), 0,
                   place_of(places, block, r + 1) - place_of(places, block, r));
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
    struct generic_team *g = registered_team(team, routine);
    zero_others(buffer, bytes, block, places);
    uint32_t width = bytes % 8 == 0 && (uintptr_t)buffer % 8 == 0 ? 8 : 1;
    struct osoe_element element = { OSOE_ELEMENT_UNSIGNED_INTEGER, width };
    osoe_combine_function combine = osoe_combine_lookup(OSOE_REDUCTION_OR, element);
    if (!g->reduction_tree_known) {
        find_lead_tree(g, 0, &g->reduction_tree, routine);
        g->reduction_tree_known = true;
    }
    in_pieces(g, team, epoch, gather_identity(element), &g->reduction_tree, buffer, bytes,
              width, combine, routine);
}

void osoe_internode_generic_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index,
                                      void *buffer, size_t bytes, const char *routine)
{
    struct generic_team *g = registered_team(team, routine);
    if (root_index >= g->count)
        osoe_fatal("%s: the root's node is index %u of a team spanning %u nodes", routine,
                   root_index, g->count);
    struct lead_tree tree;
    find_lead_tree(g, root_index, &tree, routine);
    in_pieces(g, team, epoch, root_index, &tree, buffer, bytes, 1, NULL, routine);
}
