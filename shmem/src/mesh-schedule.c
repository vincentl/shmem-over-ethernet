/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mesh-schedule.h"
#include "wire-settings.h"

uint32_t osoe_mesh_schedule_slots(uint32_t node_count)
{
    if (node_count < 2)
        return 0;
    return (node_count - 1) / 2 + (node_count % 2 == 0);
}

void osoe_mesh_schedule_order(uint64_t job, uint32_t step, uint32_t node_count,
                              uint32_t *offsets)
{
    uint32_t slots = osoe_mesh_schedule_slots(node_count);
    for (uint32_t s = 0; s < (node_count - 1) / 2 && s < slots; s++)
        offsets[s] = s + 1;
    if (slots && node_count % 2 == 0)
        offsets[slots - 1] = node_count / 2;
    /* The draws are osoe_wire_hash's, MurmurHash3's finalizer, listener.c's
     * port rule's mix too: mesh_model.py's `order`, line for line. */
    uint64_t seed = osoe_wire_hash(job ^ osoe_wire_hash(((uint64_t)step << 32) | node_count));
    for (uint32_t k = slots ? slots - 1 : 0; k > 0; k--) {
        uint32_t r = (uint32_t)(osoe_wire_hash(seed ^ k) % ((uint64_t)k + 1));
        uint32_t d = offsets[k];
        offsets[k] = offsets[r];
        offsets[r] = d;
    }
}

bool osoe_mesh_schedule_calls(uint32_t from, uint32_t to, uint32_t node_count)
{
    if (from >= node_count || to >= node_count || from == to)
        return false;
    uint32_t delta = (to + node_count - from) % node_count;
    if (delta <= (node_count - 1) / 2)
        return true;
    return node_count % 2 == 0 && delta == node_count / 2 && from < to;
}

uint32_t osoe_mesh_schedule_callee(uint32_t node, uint32_t offset, uint32_t node_count)
{
    if (offset == 0 || offset > node_count / 2 ||
        (node_count % 2 == 0 && offset == node_count / 2 && node >= node_count / 2))
        return node_count;
    return (uint32_t)(((uint64_t)node + offset) % node_count);
}

uint32_t osoe_mesh_schedule_callers(uint32_t node, uint32_t node_count)
{
    if (node_count < 2)
        return 0;
    return (node_count - 1) / 2 + (node_count % 2 == 0 && node >= node_count / 2);
}
