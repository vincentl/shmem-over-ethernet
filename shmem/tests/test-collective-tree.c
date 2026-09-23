/*
 * The internode tree's topology on its own (tier 1; §5.3, collective-tree.h).
 *
 * Written for an arbitrary sorted list of nodes, although only the world team
 * - every node - exists, because a split team's list is sparse and a function
 * tested only on 0 ... N-1 cannot tell an index from a node number. Every check
 * below is against a brute-force statement of the property rather than against
 * hand-computed answers: every node has exactly one parent except the root,
 * parent and child agree about each other, and the depth is what the fan-out
 * implies.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "collective-tree.h"

enum { MAX = 700 };

/* Check one list of `count` nodes with fan-out k from every node's point of view. */
static void check_list(const uint32_t *nodes, uint32_t count, uint32_t k)
{
    static struct osoe_collective_tree tree[MAX];
    static uint32_t parents_seen[MAX];
    for (uint32_t i = 0; i < count; i++) {
        CHECK(osoe_collective_tree(nodes, count, nodes[i], k, &tree[i]));
        CHECK(tree[i].index == i);
        CHECK(tree[i].count == count);
        CHECK(tree[i].children <= k);
        parents_seen[i] = 0;
    }
    uint32_t roots = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (tree[i].parent == OSOE_COLLECTIVE_TREE_NONE) {
            roots++;
            CHECK(i == 0);
            continue;
        }
        /* The parent is a node of the list, with a smaller index, that names
         * this node among its children - so the tree is sparse-list safe. */
        uint32_t p = 0;
        while (p < count && nodes[p] != tree[i].parent)
            p++;
        CHECK(p < count && p < i);
        if (p < count) {
            CHECK(osoe_collective_tree_child(&tree[p], nodes[i]) != OSOE_COLLECTIVE_TREE_NONE);
            parents_seen[i]++;
        }
    }
    CHECK(roots == 1);
    uint32_t edges = 0;
    for (uint32_t i = 0; i < count; i++) {
        CHECK(i == 0 || parents_seen[i] == 1);
        edges += tree[i].children;
        for (uint32_t j = 0; j < tree[i].children; j++) {
            /* A child names this node as its parent. */
            uint32_t c = 0;
            while (c < count && nodes[c] != tree[i].child[j])
                c++;
            CHECK(c < count && tree[c].parent == nodes[i]);
        }
    }
    CHECK(edges == count - 1);            /* a tree: N - 1 edges, all accounted for */

    /* Depth: the longest walk to the root is what the fan-out implies. */
    uint32_t deepest = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t d = 0;
        for (uint32_t at = i; at != 0; at = (at - 1) / k)
            d++;
        if (d > deepest)
            deepest = d;
    }
    CHECK(osoe_collective_tree_depth(count, k) == deepest);
}

int main(void)
{
    static uint32_t nodes[MAX];

    /* The world team's list, and the depths the brief names: 8 nodes at k = 2
     * is depth 3, 13 at k = 4 is depth 2, one node is depth 0. */
    for (uint32_t i = 0; i < MAX; i++)
        nodes[i] = i;
    CHECK(osoe_collective_tree_depth(8, 2) == 3);
    CHECK(osoe_collective_tree_depth(7, 2) == 2);
    CHECK(osoe_collective_tree_depth(13, 4) == 2);
    CHECK(osoe_collective_tree_depth(1, 4) == 0);
    CHECK(osoe_collective_tree_depth(4096, 4) == 6);
    for (uint32_t k = 2; k <= 8; k++)
        for (uint32_t n = 1; n <= 40; n++)
            check_list(nodes, n, k);
    check_list(nodes, MAX, 4);
    check_list(nodes, MAX, OSOE_COLLECTIVE_FANOUT_MAX);

    /* Sparse lists: every third node, and a pseudo-random subset. */
    for (uint32_t i = 0; i < MAX; i++)
        nodes[i] = 3 * i + 7;
    for (uint32_t k = 2; k <= 5; k++)
        check_list(nodes, 100, k);
    uint32_t n = 0, x = 20260921;
    for (uint32_t v = 0; v < 4 * MAX && n < MAX; v++) {
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 3 == 0)
            nodes[n++] = v;
    }
    check_list(nodes, n, 2);
    check_list(nodes, n, 4);

    /* A concrete sparse case, by hand, so a reader can see what "over the
     * index" means: nodes {2, 5, 9, 11, 40} at k = 2. Index 1 (node 5) has
     * parent index 0 (node 2) and children indices 3 and 4 (nodes 11 and 40). */
    uint32_t sparse[] = { 2, 5, 9, 11, 40 };
    struct osoe_collective_tree t;
    CHECK(osoe_collective_tree(sparse, 5, 5, 2, &t));
    CHECK(t.parent == 2 && t.children == 2 && t.child[0] == 11 && t.child[1] == 40);
    CHECK(osoe_collective_tree(sparse, 5, 2, 2, &t));
    CHECK(t.parent == OSOE_COLLECTIVE_TREE_NONE && t.children == 2 && t.child[0] == 5 &&
          t.child[1] == 9);
    CHECK(osoe_collective_tree(sparse, 5, 40, 2, &t));
    CHECK(t.parent == 5 && t.children == 0);

    /* Refusals: not in the list, unsorted, a repeat, fan-out out of range. */
    CHECK(!osoe_collective_tree(sparse, 5, 6, 2, &t));
    uint32_t unsorted[] = { 0, 3, 2 };
    CHECK(!osoe_collective_tree(unsorted, 3, 0, 2, &t));
    uint32_t repeat[] = { 0, 1, 1 };
    CHECK(!osoe_collective_tree(repeat, 3, 0, 2, &t));
    CHECK(!osoe_collective_tree(sparse, 5, 2, 1, &t));
    CHECK(!osoe_collective_tree(sparse, 5, 2, OSOE_COLLECTIVE_FANOUT_MAX + 1, &t));
    return CHECK_DONE();
}
