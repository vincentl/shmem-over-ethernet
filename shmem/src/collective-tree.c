/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* The k-ary tree over a team's participating nodes (collective-tree.h). */
#include <stdbool.h>
#include <stdint.h>

#include "collective-tree.h"

bool osoe_collective_tree(const uint32_t *nodes, uint32_t count, uint32_t me, uint32_t fanout,
                          struct osoe_collective_tree *tree)
{
    if (count == 0 || fanout < 2 || fanout > OSOE_COLLECTIVE_FANOUT_MAX)
        return false;
    for (uint32_t i = 1; i < count; i++)
        if (nodes[i] <= nodes[i - 1])
            return false;
    /* Binary search: the list is sorted, and this is a lookup a split team
     * would otherwise do in O(N). */
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (nodes[mid] < me)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == count || nodes[lo] != me)
        return false;

    uint32_t i = lo;
    *tree = (struct osoe_collective_tree){
        .count = count,
        .index = i,
        .fanout = fanout,
        .parent = i == 0 ? OSOE_COLLECTIVE_TREE_NONE : nodes[(i - 1) / fanout],
    };
    /* Children k*i + 1 ... k*i + k, in 64 bits: k*i can pass 2^32 at the
     * largest node counts a uint32_t list can hold. */
    for (uint32_t j = 0; j < fanout; j++) {
        uint64_t c = (uint64_t)fanout * i + 1 + j;
        if (c >= count)
            break;
        tree->child[tree->children++] = nodes[c];
    }
    return true;
}

uint32_t osoe_collective_tree_child(const struct osoe_collective_tree *tree, uint32_t node)
{
    for (uint32_t j = 0; j < tree->children; j++)
        if (tree->child[j] == node)
            return j;
    return OSOE_COLLECTIVE_TREE_NONE;
}

uint32_t osoe_collective_tree_depth(uint32_t count, uint32_t fanout)
{
    /* Index count - 1 is the deepest; count its ancestors. */
    uint32_t depth = 0;
    for (uint64_t i = count ? count - 1 : 0; i > 0; i = (i - 1) / fanout)
        depth++;
    return depth;
}
