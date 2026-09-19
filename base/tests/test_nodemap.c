/* The §8.1 logical node map: parsing, and the per-rank bootstrap values a
 * launcher derives from it. Every one of §8.1's four poles appears here. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base/nodemap.h"
#include "check.h"

/* The node a rank is on, its local id, and its node's gtids, in one check. */
static void expect_rank(const struct osoe_node_map *m, uint32_t rank, uint32_t node,
                        uint32_t local_id, const char *gtids)
{
    CHECK(m->node_of[rank] == node);
    CHECK(osoe_node_map_local_id(m, rank) == local_id);

    uint32_t list[16];
    uint32_t n = osoe_node_map_gtids(m, node, list, 16);
    char text[64];
    size_t off = 0;
    for (uint32_t i = 0; i < n && off < sizeof text; i++)
        off += (size_t)snprintf(text + off, sizeof text - off, i ? ",%u" : "%u", list[i]);
    if (n == 0)
        text[0] = '\0';
    CHECK(strcmp(text, gtids) == 0);
    CHECK(n == osoe_node_map_local_count(m, node));
    /* The rank must sit at its own local id in its node's list (§6.2 step 4). */
    CHECK(local_id < n && list[local_id] == rank);
}

int main(void)
{
    struct osoe_node_map m;

    /* Pole 1: all PEs in one logical node - what every step-1 test runs. */
    CHECK(osoe_node_map_parse("0,0,0,0", 4, &m) == 0);
    CHECK(m.node_count == 1);
    expect_rank(&m, 0, 0, 0, "0,1,2,3");
    expect_rank(&m, 3, 0, 3, "0,1,2,3");
    osoe_node_map_free(&m);

    /* Pole 2: one PE per logical node. */
    CHECK(osoe_node_map_parse("0,1,2,3", 4, &m) == 0);
    CHECK(m.node_count == 4);
    expect_rank(&m, 0, 0, 0, "0");
    expect_rank(&m, 2, 2, 0, "2");
    osoe_node_map_free(&m);

    /* §8.1's headline example: eight PEs as four logical nodes of two. */
    CHECK(osoe_node_map_parse("0,0,1,1,2,2,3,3", 8, &m) == 0);
    CHECK(m.node_count == 4);
    expect_rank(&m, 0, 0, 0, "0,1");
    expect_rank(&m, 5, 2, 1, "4,5");
    osoe_node_map_free(&m);

    /* Pole 3: uneven node sizes. Contiguous, as §8.1 notes - which is why it
     * is not the non-contiguous case. */
    CHECK(osoe_node_map_parse("0,0,0,1,2,2", 6, &m) == 0);
    CHECK(m.node_count == 3);
    CHECK(osoe_node_map_local_count(&m, 0) == 3);
    CHECK(osoe_node_map_local_count(&m, 1) == 1);
    CHECK(osoe_node_map_local_count(&m, 2) == 2);
    expect_rank(&m, 2, 0, 2, "0,1,2");
    expect_rank(&m, 3, 1, 0, "3");
    expect_rank(&m, 4, 2, 0, "4,5");
    osoe_node_map_free(&m);

    /* Pole 4: ranks not contiguous per node - Slurm's cyclic distribution.
     * This is the shape that breaks per-node member counts (§8.1, §5.2). */
    CHECK(osoe_node_map_parse("0,1,2,0,1,0", 6, &m) == 0);
    CHECK(m.node_count == 3);
    CHECK(osoe_node_map_local_count(&m, 0) == 3);
    CHECK(osoe_node_map_local_count(&m, 1) == 2);
    CHECK(osoe_node_map_local_count(&m, 2) == 1);
    expect_rank(&m, 0, 0, 0, "0,3,5");
    expect_rank(&m, 3, 0, 1, "0,3,5");
    expect_rank(&m, 5, 0, 2, "0,3,5");
    expect_rank(&m, 1, 1, 0, "1,4");
    expect_rank(&m, 4, 1, 1, "1,4");
    expect_rank(&m, 2, 2, 0, "2");
    osoe_node_map_free(&m);

    /* Every rank accounted for exactly once, across every node. */
    CHECK(osoe_node_map_parse("2,0,2,1,0,2", 6, &m) == 0);
    uint32_t total = 0;
    for (uint32_t node = 0; node < m.node_count; node++)
        total += osoe_node_map_local_count(&m, node);
    CHECK(total == 6);
    osoe_node_map_free(&m);

    /* Refusals. */
    CHECK(osoe_node_map_parse("0,0,1", 4, &m) == -1);       /* too few entries */
    CHECK(osoe_node_map_parse("0,0,1,1,2", 4, &m) == -1);   /* too many */
    CHECK(osoe_node_map_parse("0,2", 2, &m) == -1);         /* node 1 unused */
    CHECK(osoe_node_map_parse("1,1", 2, &m) == -1);         /* node 0 unused */
    CHECK(osoe_node_map_parse("0,4294967295", 2, &m) == -1);/* gap, without overflow */
    CHECK(osoe_node_map_parse("", 2, &m) == -1);
    CHECK(osoe_node_map_parse("0,x", 2, &m) == -1);
    CHECK(osoe_node_map_parse("0,", 2, &m) == -1);
    CHECK(osoe_node_map_parse("0,0", 0, &m) == -1);         /* no ranks */
    CHECK(osoe_node_map_parse(NULL, 2, &m) == -1);

    return CHECK_DONE();
}
