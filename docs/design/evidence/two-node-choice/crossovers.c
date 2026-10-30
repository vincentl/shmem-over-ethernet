/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * P4: where the chooser takes Träff's, every size from 1,025 bytes to HIGH in
 * steps of STEP, a reduction as the data path asks for it on a team that is
 * the job (the defaults: a ceiling of 8, a capacity of 8, a small class of
 * 1 KiB; a lane of osoe_internal_lane_rows), on the link given: for each node
 * count, how many sizes take Träff's, the first that does, the first from which
 * every size does, and how often the choice changes from one size to the next.
 * Built against a tree's chooser, from <repo>:
 *   cc -O2 -std=c11 -I shmem/src -I base -o crossovers crossovers.c \
 *      COLLECTIVE_SHAPE_C shmem/src/collective-circulant.c shmem/src/collective-tree.c
 * where COLLECTIVE_SHAPE_C is shmem/src/collective-shape.c, or as built before
 * P4 the file `git show cbfaa7e1:shmem/src/collective-shape.c` writes.
 *   crossovers HIGH STEP HOP_NS MESSAGE_NS BYTES_PER_SECOND NODES...
 */
#include <stdio.h>
#include <stdlib.h>

#include "collective-circulant.h"
#include "collective-shape.h"
#include "internal-region.h"

int main(int argc, char **argv)
{
    if (argc < 7)
        return 2;
    uint64_t high = strtoull(argv[1], NULL, 10), step = strtoull(argv[2], NULL, 10);
    struct osoe_link link = { strtoull(argv[3], NULL, 10), strtoull(argv[4], NULL, 10),
                              strtoull(argv[5], NULL, 10) };
    for (int i = 6; i < argc; i++) {
        uint32_t nodes = (uint32_t)strtoul(argv[i], NULL, 10);
        uint64_t circulant = 0, sizes = 0, flips = 0, first = 0, settled = 0;
        int last = -1;
        for (uint64_t n = 1025; n <= high; n += step) {
            struct osoe_collective_shape s = osoe_shape_choose(
                OSOE_SHAPE_REDUCE, nodes, n, &link, 8, osoe_internal_lane_rows(8, nodes), 8,
                1024, OSOE_INTERNAL_PIECE_BYTES, true);
            int c = s.algorithm == OSOE_SHAPE_CIRCULANT;
            circulant += (uint64_t)c;
            sizes++;
            if (c && first == 0)
                first = n;
            if (c && last != 1)
                settled = n;
            if (!c)
                settled = 0;
            if (last >= 0 && c != last)
                flips++;
            last = c;
        }
        printf("%5u nodes: Träff's at %llu of %llu sizes (%.1f%%), the first %llu, every size "
               "from %llu, %llu changes\n", nodes, (unsigned long long)circulant,
               (unsigned long long)sizes, 100.0 * (double)circulant / (double)sizes,
               (unsigned long long)first, (unsigned long long)settled,
               (unsigned long long)flips);
    }
    return 0;
}
