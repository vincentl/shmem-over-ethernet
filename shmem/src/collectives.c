/*
 * The data-carrying collectives (§5.5, §5.8; phase 1 step 5): §5.5's
 * accumulator, for reductions and broadcast.
 *
 * **Within a node** it is §5.5 as written. Every member arrives with one locked
 * increment of the team's `arrived`. The local lead - the team's member with the
 * lowest local id, local id 0 for both teams that exist - waits for all of
 * them, then reads each member's source (only now: invariant 17, push never
 * pull) and combines it into its own `dest`, which it alone writes before
 * release (invariant 16). It bumps `released`; every other member copies the
 * lead's `dest` into its own and bumps `copied`; the lead waits for the last
 * copy and leaves last. `dest` is undefined until the call returns, so the
 * intermediate values the lead's `dest` holds are unobservable.
 *
 * **Between nodes** only the leads take part, over §5.3's k-ary tree of nodes,
 * and they meet above the transport boundary rather than through AM messages
 * the agents interpret: a child puts its partial result into a slot of its
 * parent's OSOE_REGION_INTERNAL and adds one to the parent's arrival word with
 * a fenced rmw, so the add lands after the bytes (invariant 5); the parent's
 * application thread waits on that word, combines each slot into its `dest`,
 * and sends up in turn. The root holds the result, and it comes down the same
 * way: a put into each child's release slot and an add to its release word.
 * The lead's thread is blocked in the call regardless and is the only writer of
 * its `dest` either way, so the AM alternative would still have had to land each
 * partial result where that thread could read it and wake it; this does exactly
 * that with operations the transport already has, and a RoCE backend inherits
 * it unchanged (§5.8). A put returns at local completion, so a result sent down
 * is the lead's to reuse at once, and §5.5's hazard of a forward that reads
 * `dest` by reference after the lead has left cannot arise.
 *
 * **A vector larger than a slot goes through the tree in slot-sized pieces**,
 * each piece a full pass up and down (§5.5's "chunk through fixed scratch").
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
 * slot while its owner reads child slots: a reduction's tree is always the
 * same, so its children cannot start the next reduction before this node has
 * released them, and a broadcast reads none. The tree barrier's model checks
 * the same up-then-down shape (tree_barrier_model.py); what is added here is
 * the data, which each release's put carries ahead of its fenced add.
 *
 * Costs, with N nodes, L local members, k the fan-out and n bytes: the lead's
 * intranode combine is O(L n), each member's copy-out O(n); the internode phase
 * is ceil(n / slot) passes of depth ceil(log_k N) each way, with O(k n) combining
 * and k puts per node per piece. A broadcast finds its rotated tree's k + 1
 * neighbouring leads in the job's table of leads, O(k) (job.h); a reduction's
 * tree is found once per team.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <shmem.h>

#include "base/cacheline.h"
#include "base/epoch.h"
#include "blocked.h"
#include "collectives.h"
#include "fatal.h"
#include "internal-region.h"
#include "job.h"
#include "node.h"
#include "rma.h"
#include "state.h"
#include "symmetric.h"
#include "team.h"
#include "transport.h"
#include "wait.h"

/* The internal region's layout (internal-region.h): the slots, then the control
 * words, per team an eight-byte arrival word (children's pieces in) and release
 * word (the parent's pieces down), each on a cache line of its own. */
enum { WORDS_PER_TEAM = 2 * OSOE_CACHELINE };
_Static_assert(OSOE_TEAMS * WORDS_PER_TEAM <= OSOE_INTERNAL_WORDS_BYTES,
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
    return child_slot(osoe.bootstrap.collective_fanout + 1) + (uint64_t)team * WORDS_PER_TEAM;
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

/* The leads around this node in a tree rooted at `root_node`. */
struct lead_tree {
    uint32_t parent;                    /* the parent lead's rank, or UINT32_MAX */
    uint32_t position;                  /* this node among the parent's children */
    uint32_t children;
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];
};

static uint32_t lead_of(uint32_t node, const char *routine)
{
    uint32_t rank = osoe_job_node_lead(&osoe.job, node);
    if (rank == UINT32_MAX)
        osoe_fatal("%s: the rank map has no lead for node %u", routine, node);
    return rank;
}

/*
 * §5.3's k-ary tree over the world team's nodes, 0 ... N - 1, with the indices
 * rotated so that `root_node` is index 0: node index i's parent is (i - 1) / k
 * and its children k i + 1 ... k i + k. The same shape as the barrier's tree
 * when the root is node 0; a broadcast roots it at its root's node, so the data
 * travels depth levels and not up to node 0 first.
 */
static void find_lead_tree(uint32_t root_node, struct lead_tree *out, const char *routine)
{
    uint32_t n = osoe.job.node_count, k = osoe.bootstrap.collective_fanout;
    uint32_t index = (osoe.job.node + n - root_node) % n;
    out->parent = UINT32_MAX;
    out->position = 0;
    if (index > 0) {
        out->parent = lead_of(((index - 1) / k + root_node) % n, routine);
        out->position = (index - 1) % k;
    }
    out->children = 0;
    for (uint32_t c = 0; c < k; c++) {
        uint64_t child = (uint64_t)k * index + 1 + c;
        if (child >= n)
            break;
        out->child[out->children++] = lead_of((uint32_t)((child + root_node) % n), routine);
    }
}

/*
 * One piece of the internode phase: `bytes` of this lead's dest at `at`. A
 * reduction's children send their partial results up, combined into `at`
 * (combine non-NULL); a broadcast's send nothing up but their arrival. Then the
 * result comes down from the parent into `at`, and goes on to the children.
 */
static void lead_piece(struct osoe_team *t, const struct lead_tree *tree, unsigned char *at,
                       size_t bytes, size_t width, osoe_combine_function combine, const char *routine)
{
    struct osoe_transport_context *context = osoe_transport_context_default();
    uint32_t team = t->number;
    uint64_t epoch = *t->epoch + 1;

    if (tree->children) {
        t->arrivals_expected += tree->children;
        osoe_wait_internal(routine, arrivals_word(team), 8, SHMEM_CMP_EQ,
                           t->arrivals_expected, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        if (combine)
            for (uint32_t c = 0; c < tree->children; c++)
                combine(at, internal(child_slot(c)), bytes / width);
    }
    if (tree->parent != UINT32_MAX) {
        if (combine)
            osoe_transport_put(context, (int)tree->parent, OSOE_REGION_INTERNAL,
                               child_slot(tree->position), at, bytes, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->parent, OSOE_REGION_INTERNAL, arrivals_word(team),
                           OSOE_RMW_ADD, 8, 1, 0, OSOE_TRANSPORT_FENCED);
        t->releases_expected += 1;
        osoe_wait_internal(routine, release_word(team), 8, SHMEM_CMP_EQ,
                           t->releases_expected, OSOE_BLOCKED_DETAIL_EPOCH, epoch);
        memcpy(at, internal(release_slot()), bytes);
    }
    for (uint32_t c = 0; c < tree->children; c++) {
        osoe_transport_put(context, (int)tree->child[c], OSOE_REGION_INTERNAL, release_slot(),
                           at, bytes, OSOE_TRANSPORT_MORE);
        osoe_transport_rmw(context, (int)tree->child[c], OSOE_REGION_INTERNAL, release_word(team),
                           OSOE_RMW_ADD, 8, 1, 0,
                           OSOE_TRANSPORT_FENCED |
                           (c + 1 < tree->children ? OSOE_TRANSPORT_MORE : 0));
    }
}

/* The leads' phase for the whole of `dest`, piece by piece. */
static void internode_phase(struct osoe_team *t, unsigned char *dest, size_t bytes, size_t width,
                            osoe_combine_function combine, int root_rank, const char *routine)
{
    if (t->number != OSOE_TEAM_WORLD)
        osoe_fatal("%s: %s spans nodes, and only the world team's leads have a tree", routine,
                   t->name);
    struct lead_tree tree;
    if (root_rank < 0) {
        /* A reduction's tree is the team's, rooted at its first node: found once. */
        if (!t->lead_tree_known) {
            struct lead_tree found;
            find_lead_tree(0, &found, routine);
            t->lead_parent = found.parent;
            t->lead_position = found.position;
            t->lead_children = found.children;
            memcpy(t->lead_child, found.child, sizeof found.child);
            t->lead_tree_known = true;
        }
        tree.parent = t->lead_parent;
        tree.position = t->lead_position;
        tree.children = t->lead_children;
        memcpy(tree.child, t->lead_child, sizeof tree.child);
    } else {
        find_lead_tree(osoe_node_of(&osoe.job, (uint32_t)root_rank), &tree, routine);
    }
    size_t piece = (OSOE_INTERNAL_SLOT_BYTES / width) * width;
    for (size_t done = 0; done < bytes; done += piece)
        lead_piece(t, &tree, dest + done, bytes - done < piece ? bytes - done : piece, width,
                   combine, routine);
}

/*
 * One data collective on `t`: a reduction if `root` is negative, else a
 * broadcast from team rank `root`. §5.5, as the file's comment walks through.
 */
static int data_collective(const char *routine, shmem_team_t team, void *dest,
                           const void *source, size_t nelems, size_t width,
                           osoe_combine_function combine, int root)
{
    struct osoe_team *t = osoe_team_of(routine, team);
    if (!t)
        return 1;                       /* SHMEM_TEAM_INVALID: undefined, answered nonzero */
    if (root >= 0 && (uint32_t)root >= t->members)
        osoe_fatal("%s: PE_root %d is not a PE of %s, which has %u", routine, root, t->name,
                   t->members);
    size_t bytes = osoe_rma_bytes(routine, nelems, width);
    if (bytes == 0)
        return 0;                       /* every member passes the same count: nothing to do */
    uint64_t dest_offset = osoe_symmetric_offset(routine, dest, bytes);
    uint64_t source_offset = osoe_symmetric_offset(routine, source, bytes);
    int root_rank = root >= 0 ? osoe_team_to_world(t, root) : -1;

    uint32_t k = *t->epoch, d = *t->data_epoch, members = t->local_members;

    /* A team of one (team.h): the caller is every member, its source the
     * result, and it touches none of the node-wide counters - they belong to
     * the node's team, not to this one, as for its sync (barrier.c). */
    if (t->singleton) {
        if (source != dest)
            memmove(dest, source, bytes);
        *t->epoch = k + 1;
        *t->data_epoch = d + 1;
        atomic_store_explicit(t->published, k + 1, memory_order_release);
        return 0;
    }

    bool lead = osoe.bootstrap.local_id == 0;
    osoe_blocked_enter(routine, OSOE_BLOCKED_DETAIL_EPOCH, k + 1);

    /*
     * Arrive; the last to arrive wakes the lead, which waits for exactly that.
     * The bump is the stamping one (base/epoch.h), because the lead waits on
     * this counter: without the stamp, a sweep expiring just as a late member
     * arrives would be counted as a lost wake.
     */
    if (osoe_epoch_release_bump_eq(t->arrived, (k + 1) * members) && !lead)
        osoe_epoch_wake_all(t->arrived);

    if (lead) {
        osoe_epoch_wait_eq(t->arrived, (k + 1) * members, routine);
        unsigned char *accumulator = dest;
        if (root < 0) {
            /* In place, the lead's source already is its dest. */
            if (source != dest)
                memcpy(accumulator, source, bytes);
            for (uint32_t m = 1; m < members; m++)
                combine(accumulator, (const unsigned char *)osoe.node.heap_of[m] + source_offset,
                        nelems);
        } else {
            int32_t root_local = osoe_node_local_id(&osoe.node, (uint32_t)root_rank);
            if (root_local >= 0 &&
                !((uint32_t)root_local == osoe.bootstrap.local_id && source == dest))
                memcpy(accumulator, (const unsigned char *)osoe.node.heap_of[root_local] +
                                source_offset, bytes);
        }
        if (t->internode)
            internode_phase(t, accumulator, bytes, width, combine, root_rank, routine);

        /* The final dest is written; now the release, and the copy-outs. */
        if (!osoe_epoch_release_bump_eq(t->released, k + 1))
            osoe_fatal("%s: %s's epochs diverged at epoch %u: PEs called different numbers "
                       "of collectives", routine, t->name, k + 1);
        osoe_epoch_wake_all(t->released);
        if (members > 1)
            osoe_epoch_wait_eq(t->copied, (d + 1) * (members - 1), routine);
    } else {
        osoe_epoch_wait_eq(t->released, k + 1, routine);
        memcpy(dest, (const unsigned char *)osoe.node.heap_of[0] + dest_offset, bytes);
        if (osoe_epoch_release_bump_eq(t->copied, (d + 1) * (members - 1)))  /* stamped: as arrived */
            osoe_epoch_wake_all(t->copied);
    }

    *t->epoch = k + 1;
    *t->data_epoch = d + 1;
    atomic_store_explicit(t->published, k + 1, memory_order_release);
    osoe_blocked_leave();
    return 0;
}

int osoe_reduce(const char *routine, shmem_team_t team, void *dest, const void *source,
                size_t nelems, size_t width, osoe_combine_function combine)
{
    return data_collective(routine, team, dest, source, nelems, width, combine, -1);
}

int osoe_broadcast(const char *routine, shmem_team_t team, void *dest, const void *source,
                   size_t nelems, size_t width, int root)
{
    if (root < 0)
        osoe_fatal("%s: PE_root %d is not a PE number", routine, root);
    return data_collective(routine, team, dest, source, nelems, width, NULL, root);
}
