/*
 * The logical node map (§8.1).
 *
 * OSOE_NODE_MAP names the logical node of every world rank:
 *
 *     OSOE_NODE_MAP=0,0,1,1,2,2,3,3
 *
 * Eight PEs on one machine become four logical nodes of two. PEs sharing a
 * logical node attach the same control segment and take the intranode path;
 * PEs in different logical nodes reach each other only through the transport,
 * so both paths run in their real form on one machine.
 *
 * **The map is the launcher's input, not the library's.** osoe-launch reads it
 * and derives the per-rank OSOE_NODEID, OSOE_LOCALID and OSOE_GTIDS of §6.2,
 * which is exactly what Slurm computes from a distribution on a real step. The
 * library therefore sees the same environment in a test as in production and
 * has no test-only bootstrap path; nothing under shmem/src reads OSOE_NODE_MAP.
 * Two of §8.1's requirements then hold by construction rather than by a check:
 * the control segment's name already carries the node id, so logical nodes
 * cannot share memory, and PEs in different logical nodes never map each
 * other's heaps, so shmem_ptr returns NULL across them.
 *
 * Local ids follow Slurm's: a rank's local id is the number of earlier ranks on
 * its node, so a node's gtids list is its ranks in increasing rank order. That
 * makes the map's four poles (§8.1) - one node, one PE per node, uneven sizes
 * 0,0,0,1,2,2, and non-contiguous ranks 0,1,2,0,1,0 - all expressible.
 */
#ifndef OSOE_BASE_NODEMAP_H
#define OSOE_BASE_NODEMAP_H

#include <stddef.h>
#include <stdint.h>

struct osoe_node_map {
    uint32_t world_size;
    uint32_t node_count;
    uint32_t *node_of;          /* world rank -> logical node id */
};

/*
 * Parse a comma-separated list of logical node ids, one per world rank.
 *
 * Returns 0, or -1 if the text is not such a list, if it does not hold exactly
 * world_size entries, or if the node ids it uses are not 0..node_count-1 with
 * every one of them used. A gap would leave a node with no PEs, whose only
 * effect is to make the node count disagree with the number of control
 * segments, so it is refused rather than silently renumbered.
 */
int osoe_node_map_parse(const char *s, uint32_t world_size, struct osoe_node_map *m);
void osoe_node_map_free(struct osoe_node_map *m);

/* The rank's local id within its node: how many earlier ranks share it. */
uint32_t osoe_node_map_local_id(const struct osoe_node_map *m, uint32_t rank);

/* How many ranks are on the node. */
uint32_t osoe_node_map_local_count(const struct osoe_node_map *m, uint32_t node);

/*
 * The node's ranks in local-id order, written to out. Returns how many there
 * are; writes nothing and returns 0 if that exceeds cap.
 */
uint32_t osoe_node_map_gtids(const struct osoe_node_map *m, uint32_t node,
                             uint32_t *out, uint32_t cap);

#endif /* OSOE_BASE_NODEMAP_H */
