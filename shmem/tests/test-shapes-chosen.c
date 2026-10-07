/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The shape each large reduction and broadcast takes on the data path (tier 2;
 * v0.2 step 2, Part 2): the fan-out and the window the chooser gives for the
 * call, the same on every node, and the results right under each shape.
 *
 *   test-shapes-chosen GRID
 *
 * Run over logical nodes, the link set by OSOE_LINK_* (so that the choice is
 * known): for each size of a list that crosses the chooser's crossovers, a
 * byte sum reduction and a broadcast over the world, their results checked
 * against what every PE contributed, then the shape this node's lead
 * published for the call (team.tree.shape_fanout, shape_window - read as
 * osoe-stat reads them) held to the line of GRID - tools/modelcheck/
 * shape_timing.py's grid, which shmem.shape.grid holds the C chooser to - for
 * this job's node count, link, ceiling and capacity. Every PE checks its own
 * node's lead's shape, so every node is checked: a node that chose otherwise
 * would have built another tree for the call, and the job would hang or sum
 * wrong first.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

/* Sizes in bytes: the small class's edge, and either side of the crossovers
 * the grid holds for the Pi's and a loopback's links. */
static const uint64_t SIZES[] = { 1025, 4096, 16384, 16385, 65536, 1048576 };
enum { SIZE_COUNT = sizeof SIZES / sizeof SIZES[0] };

static uint64_t setting(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    return value ? strtoull(value, NULL, 10) : fallback;
}

/* The grid's shape for one call, or 0 if the grid has no line for it. */
static int grid_shape(const char *path, const char *op, uint64_t nodes, uint64_t bytes,
                      uint64_t hop, uint64_t message, uint64_t bandwidth, uint64_t ceiling,
                      uint64_t capacity, uint64_t small, uint64_t *fanout, uint64_t *window)
{
    FILE *f = fopen(path, "r");
    CHECK_FATAL(f != NULL);
    char line[256];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char name[16];
        unsigned long long v[11];
        if (line[0] == '#' ||
            sscanf(line, "%15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", name,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
                   &v[10]) != 12)
            continue;
        if (strcmp(name, op) == 0 && v[0] == nodes && v[1] == bytes && v[2] == hop &&
            v[3] == message && v[4] == bandwidth && v[5] == ceiling && v[6] == capacity &&
            v[7] == small) {
            *fanout = v[8];
            *window = v[9];
            found = 1;
        }
    }
    fclose(f);
    return found;
}

int main(int argc, char **argv)
{
    CHECK_FATAL(argc == 2);
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    const char *step_nodes = osoe_test_launcher("STEP_NUM_NODES");
    uint64_t nodes = strtoull(step_nodes ? step_nodes : "1", NULL, 10);
    uint64_t hop = setting("OSOE_LINK_HOP_NS", 0), message = setting("OSOE_LINK_MESSAGE_NS", 0);
    uint64_t bandwidth = setting("OSOE_LINK_BYTES_PER_SECOND", 0);
    uint64_t ceiling = setting("OSOE_COLLECTIVE_FANOUT", 8);
    uint64_t capacity = setting("OSOE_COLLECTIVE_PIPELINE", 4);
    uint64_t small = setting("OSOE_SMALL_COLLECTIVE_BYTES", 1024);
    CHECK_FATAL(bandwidth != 0);            /* the link is set, so the choice is known */

    /* Bytes, so that every size is the grid's to the byte: a sum of uint8_t,
     * each element (PE + 1) times a small factor, wrapping as uint8_t does. */
    size_t most = SIZES[SIZE_COUNT - 1];
    uint8_t *source = shmem_malloc(most);
    uint8_t *dest = shmem_malloc(most);
    CHECK_FATAL(source && dest);
    int checked = 0;
    for (unsigned s = 0; s < SIZE_COUNT; s++) {
        size_t count = (size_t)SIZES[s];
        for (int op = 0; op < 2; op++) {
            for (size_t i = 0; i < count; i++) {
                source[i] = (uint8_t)((me + 1) * (int)(i % 7 + 1));
                dest[i] = 0;
            }
            shmem_barrier_all();
            int root = (int)(s % (unsigned)npes);
            if (op == 0)
                shmem_uint8_sum_reduce(SHMEM_TEAM_WORLD, dest, source, count);
            else
                shmem_broadcastmem(SHMEM_TEAM_WORLD, dest, source, count, root);
            int all = npes * (npes + 1) / 2;
            for (size_t i = 0; i < count; i++) {
                uint8_t want = op == 0 ? (uint8_t)(all * (int)(i % 7 + 1))
                                       : (uint8_t)((root + 1) * (int)(i % 7 + 1));
                if (dest[i] != want) {
                    fprintf(stderr, "PE %d: %s of %zu bytes: byte %zu is %u, not %u\n", me,
                            op == 0 ? "a sum" : "a broadcast", count, i, dest[i], want);
                    CHECK(0);
                    break;
                }
            }
            /* The node's lead published the call's shape before it released
             * its members (node.h), so it is there once the call returns. */
            uint64_t fanout = 0, window = 0, want_fanout = 0, want_window = 0;
            CHECK(osoe_test_scoped_metric("team.tree.shape_fanout", OSOE_MSCOPE_TEAM, 0, &fanout));
            CHECK(osoe_test_scoped_metric("team.tree.shape_window", OSOE_MSCOPE_TEAM, 0, &window));
            const char *name = op == 0 ? "reduce" : "broadcast";
            uint64_t bytes = (uint64_t)count;
            if (!grid_shape(argv[1], name, nodes, bytes, hop, message, bandwidth, ceiling,
                            capacity, small, &want_fanout, &want_window)) {
                fprintf(stderr, "PE %d: the grid has no %s of %llu bytes on %llu nodes\n", me,
                        name, (unsigned long long)bytes, (unsigned long long)nodes);
                CHECK(0);
                continue;
            }
            if (fanout != want_fanout || window != want_window) {
                fprintf(stderr, "PE %d: a %s of %llu bytes on %llu nodes took fan-out %llu and "
                        "window %llu, the chooser's %llu and %llu\n", me, name,
                        (unsigned long long)bytes, (unsigned long long)nodes,
                        (unsigned long long)fanout, (unsigned long long)window,
                        (unsigned long long)want_fanout, (unsigned long long)want_window);
                CHECK(0);
            }
            if (me == 0)
                printf("%s %llu bytes on %llu nodes: fan-out %llu, window %llu\n", name,
                       (unsigned long long)bytes, (unsigned long long)nodes,
                       (unsigned long long)fanout, (unsigned long long)window);
            checked++;
        }
    }
    shmem_barrier_all();
    if (me == 0)
        printf("SHAPES CHOSEN: %d calls checked\n", checked);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return CHECK_DONE();
}
