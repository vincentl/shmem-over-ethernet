/*
 * The k-ary tree a team's internode phase runs over (§5.3; phase 1 step 2
 * slice 3).
 *
 * Over the *index* of the participating nodes, not over node numbers: a team's
 * node set is a sorted list (§5.2), node index i's parent is index (i - 1) / k
 * and its children are indices k*i + 1 ... k*i + k. The world team spans every
 * node, so its list is 0 ... N-1 and index and node number coincide; a split
 * team's list is sparse, and the same function serves it.
 *
 * Costs, with N the participating nodes and k the fan-out: O(log N) depth, so a
 * barrier's latency is 2 * ceil(log_k N) hops rather than the star's two hops
 * plus N - 1 serial message costs at the root; O(k) messages handled per node
 * per barrier; O(k) state per team per node, which is this structure. Building
 * it is O(N) once per team, for the sortedness check, and the list is not kept.
 */
#ifndef OSOE_COLLECTIVE_TREE_H
#define OSOE_COLLECTIVE_TREE_H

#include <stdbool.h>
#include <stdint.h>

enum {
    /* The fan-out's default is the backend's (osoe_internode_fanout_default in
     * internode.h), because the best k depends on the fabric (§5.3). Bounds a node's children, and so the per-team state and the bitmask of
     * children arrived. 32 is well past where a wider tree stops paying: at
     * k = 32 a node handles 33 messages per barrier to save one level of 11
     * at 4,096 nodes. */
    OSOE_COLLECTIVE_FANOUT_MAX = 32,
};

#define OSOE_COLLECTIVE_TREE_NONE UINT32_MAX

struct osoe_collective_tree {
    uint32_t count;             /* participating nodes */
    uint32_t index;             /* this node's position among them */
    uint32_t fanout;            /* k */
    uint32_t parent;            /* the parent's node number, or ..._NONE at the root */
    uint32_t children;          /* how many of child[] are used */
    uint32_t child[OSOE_COLLECTIVE_FANOUT_MAX];  /* node numbers, by index */
};

/*
 * This node's place in the tree over `nodes` (count of them, sorted ascending,
 * no repeats) with fan-out `fanout` (2 ... OSOE_COLLECTIVE_FANOUT_MAX).
 * Returns false if the list is not sorted and free of repeats, if the fan-out
 * is out of range, or if `me` is not in it - each a caller's bug the caller
 * names.
 */
bool osoe_collective_tree(const uint32_t *nodes, uint32_t count, uint32_t me, uint32_t fanout,
                          struct osoe_collective_tree *tree);

/* The position of `node` among `tree`'s children, or ..._NONE if it is not one. */
uint32_t osoe_collective_tree_child(const struct osoe_collective_tree *tree, uint32_t node);

/* How deep the tree over `count` nodes is with this fan-out: 0 for one node. */
uint32_t osoe_collective_tree_depth(uint32_t count, uint32_t fanout);

#endif /* OSOE_COLLECTIVE_TREE_H */
