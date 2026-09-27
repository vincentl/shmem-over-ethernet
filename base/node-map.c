/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include <stdlib.h>

#include "base/environment.h"
#include "base/node-map.h"

int osoe_node_map_parse(const char *s, uint32_t world_size, struct osoe_node_map *m)
{
    *m = (struct osoe_node_map){ 0 };
    if (!s || world_size == 0)
        return -1;

    uint32_t *node_of = malloc(sizeof *node_of * world_size);
    if (!node_of)
        return -1;

    size_t n;
    if (osoe_parse_u32_list(s, node_of, world_size, &n) != 0 || n != world_size) {
        free(node_of);
        return -1;
    }

    /* The ids must be 0..max with none unused: a gap would name a node that no
     * PE joins, so the node count and the number of control segments differ. */
    uint32_t max = 0;
    for (uint32_t i = 0; i < world_size; i++)
        if (node_of[i] > max)
            max = node_of[i];
    if (max >= world_size) {           /* more nodes than ranks: a gap for certain */
        free(node_of);
        return -1;
    }
    for (uint32_t id = 0; id <= max; id++) {
        uint32_t seen = 0;
        for (uint32_t i = 0; i < world_size && !seen; i++)
            seen = node_of[i] == id;
        if (!seen) {
            free(node_of);
            return -1;
        }
    }

    m->world_size = world_size;
    m->node_count = max + 1;
    m->node_of = node_of;
    return 0;
}

void osoe_node_map_free(struct osoe_node_map *m)
{
    free(m->node_of);
    m->node_of = NULL;
}

uint32_t osoe_node_map_local_id(const struct osoe_node_map *m, uint32_t rank)
{
    uint32_t id = 0;
    for (uint32_t i = 0; i < rank; i++)
        id += m->node_of[i] == m->node_of[rank];
    return id;
}

uint32_t osoe_node_map_local_count(const struct osoe_node_map *m, uint32_t node)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < m->world_size; i++)
        n += m->node_of[i] == node;
    return n;
}

uint32_t osoe_node_map_gtids(const struct osoe_node_map *m, uint32_t node,
                             uint32_t *out, uint32_t capacity)
{
    uint32_t n = osoe_node_map_local_count(m, node);
    if (n > capacity)
        return 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < m->world_size; i++)
        if (m->node_of[i] == node)
            out[k++] = i;
    return n;
}
