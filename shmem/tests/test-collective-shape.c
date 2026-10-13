/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The chooser of a large collective's shape on its own (tier 1; v0.2 step 2,
 * collective-shape.h).
 *
 *   test-collective-shape choice     what the Pi's link and a loopback's choose
 *   test-collective-shape grid FILE  the C against the Python model, line by line
 *   test-collective-shape places-grid FILE
 *                                    collect's places (P31): the way and the four
 *                                    costs against the model's, line by line
 *
 * choice pins what the Pi cluster measured on four nodes (2026-09-26, finding
 * F2): the small class keeps the ceiling's tree; above it the star - k = 3 or
 * more, one level - wins at small sizes and k = 2 at 1 MiB, the two crossing
 * between 16 and 64 KiB; a loopback link, a hop five times shorter and
 * bandwidth thirty times higher, keeps the star far longer. And determinism:
 * the same inputs choose the same shape. And Träff's reduction (v0.2 step 3)
 * where the caller allows it: a 1 MiB reduction on four nodes of the Pi takes
 * it, a 2 KiB one, a broadcast, a call that does not allow it and a node count
 * whose rows a lane cannot hold do not. grid reads the answers
 * tools/modelcheck/shape_timing.py writes - op, nodes, bytes, the link, the
 * ceiling, the capacity, the small class's bytes and whether Träff's is
 * allowed, then the algorithm, fan-out, window and cost it chose - and
 * requires the same of osoe_shape_choose, osoe_shape_cost and
 * osoe_shape_circulant_cost, whose arithmetic is the model's in integers.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "collective-shape.h"
#include "internal-region.h"

static const struct osoe_link pi_link = { 86000, 7000, 116000000 };
static const struct osoe_link loopback_link = { 20000, 2000, 4000000000u };

/* The smallest power of two from 2 KiB at which a call on `nodes` leaves the
 * star for a deeper tree, or 0 up to 1 GiB. */
static uint64_t crossover(enum osoe_shape_operation op, uint32_t nodes,
                          const struct osoe_link *link)
{
    uint32_t star = nodes - 1 < 8 ? nodes - 1 : 8;
    for (uint64_t n = 2048; n <= (uint64_t)1 << 30; n *= 2)
        if (osoe_shape_choose(op, nodes, n, link, 8, 4, 1024, OSOE_INTERNAL_PIECE_BYTES, false).fanout <
            star)
            return n;
    return 0;
}

static int choice(void)
{
    const enum osoe_shape_operation ops[] = { OSOE_SHAPE_REDUCE, OSOE_SHAPE_BROADCAST };
    for (unsigned i = 0; i < 2; i++) {
        enum osoe_shape_operation op = ops[i];
        struct osoe_collective_shape s;

        /* The small class, and a team of one node: the ceiling's tree. */
        s = osoe_shape_choose(op, 4, 8, &pi_link, 8, 4, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        CHECK(s.algorithm == OSOE_SHAPE_TREE && s.fanout == 8 && s.window == 1);
        s = osoe_shape_choose(op, 1, 1 << 20, &pi_link, 8, 4, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        CHECK(s.fanout == 8 && s.window == 1);

        /* Four nodes of the Pi: the star above the small class, k = 2 at
         * 1 MiB, the window at the capacity. */
        s = osoe_shape_choose(op, 4, 2048, &pi_link, 8, 4, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        CHECK(s.fanout >= 3 && s.window == 4);
        s = osoe_shape_choose(op, 4, 1 << 20, &pi_link, 8, 4, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        CHECK(s.fanout == 2 && s.window == 4);
        uint64_t pi = crossover(op, 4, &pi_link);
        printf("%s: the Pi's crossover on four nodes at %llu bytes\n",
               op == OSOE_SHAPE_REDUCE ? "reduce" : "broadcast", (unsigned long long)pi);
        CHECK(pi >= 16384 && pi <= 65536);

        /* A loopback link keeps the star far longer. */
        uint64_t loopback = crossover(op, 4, &loopback_link);
        printf("%s: a loopback link's at %llu bytes\n",
               op == OSOE_SHAPE_REDUCE ? "reduce" : "broadcast", (unsigned long long)loopback);
        CHECK(loopback == 0 || loopback >= 8 * pi);

        /* The same inputs, the same shape. */
        struct osoe_collective_shape a =
            osoe_shape_choose(op, 1000, 65536, &pi_link, 32, 8, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        struct osoe_collective_shape b =
            osoe_shape_choose(op, 1000, 65536, &pi_link, 32, 8, 1024, OSOE_INTERNAL_PIECE_BYTES, false);
        CHECK(a.fanout == b.fanout && a.window == b.window && a.algorithm == b.algorithm);
        CHECK(a.fanout >= 2 && a.fanout <= 32 && a.window == 8);

        /* No overflow: a collective too long to time saturates. */
        CHECK(osoe_shape_cost(op, 4096, 2, 1, UINT64_MAX / 2, OSOE_INTERNAL_PIECE_BYTES,
                              &pi_link) == UINT64_MAX);
    }

    /* Träff's, where the caller allows it: a 1 MiB reduction on four nodes of
     * the Pi, and nothing else - its window four of a lane's nine rows of four
     * slots, the rows its two rounds leave it. */
    struct osoe_collective_shape s;
    s = osoe_shape_choose(OSOE_SHAPE_REDUCE, 4, 1 << 20, &pi_link, 8, 4, 1024,
                          OSOE_INTERNAL_PIECE_BYTES, true);
    CHECK(s.algorithm == OSOE_SHAPE_CIRCULANT && s.window == 16);
    s = osoe_shape_choose(OSOE_SHAPE_REDUCE, 4, 2048, &pi_link, 8, 4, 1024,
                          OSOE_INTERNAL_PIECE_BYTES, true);
    CHECK(s.algorithm == OSOE_SHAPE_TREE);
    s = osoe_shape_choose(OSOE_SHAPE_BROADCAST, 4, 1 << 20, &pi_link, 8, 4, 1024,
                          OSOE_INTERNAL_PIECE_BYTES, true);
    CHECK(s.algorithm == OSOE_SHAPE_TREE);
    s = osoe_shape_choose(OSOE_SHAPE_REDUCE, 4, 1 << 20, &pi_link, 8, 4, 1024,
                          OSOE_INTERNAL_PIECE_BYTES, false);
    CHECK(s.algorithm == OSOE_SHAPE_TREE);
    /* sixteen nodes take four rows; a ceiling of 2 gives a lane three */
    s = osoe_shape_choose(OSOE_SHAPE_REDUCE, 16, 1 << 20, &pi_link, 2, 4, 1024,
                          OSOE_INTERNAL_PIECE_BYTES, true);
    CHECK(s.algorithm == OSOE_SHAPE_TREE);
    CHECK(osoe_shape_circulant_cost(4096, 1, UINT64_MAX / 2, OSOE_INTERNAL_PIECE_BYTES,
                                    &pi_link) == UINT64_MAX);
    return CHECK_DONE();
}

static int grid(const char *path)
{
    FILE *f = fopen(path, "r");
    CHECK_FATAL(f != NULL);
    char line[512];
    unsigned lines = 0, differ = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char op[16], algorithm[16];
        unsigned nodes, ceiling, capacity, small, allowed, fanout, window;
        unsigned long long bytes, hop, message, bandwidth, cost;
        if (sscanf(line, "%15s %u %llu %llu %llu %llu %u %u %u %u %15s %u %u %llu", op, &nodes,
                   &bytes, &hop, &message, &bandwidth, &ceiling, &capacity, &small, &allowed,
                   algorithm, &fanout, &window, &cost) != 14) {
            fprintf(stderr, "%s: cannot read: %s", path, line);
            CHECK(0);
            continue;
        }
        enum osoe_shape_operation operation =
            strcmp(op, "reduce") == 0 ? OSOE_SHAPE_REDUCE : OSOE_SHAPE_BROADCAST;
        struct osoe_link link = { hop, message, bandwidth };
        struct osoe_collective_shape s =
            osoe_shape_choose(operation, nodes, bytes, &link, ceiling, capacity, small,
                              OSOE_INTERNAL_PIECE_BYTES, allowed != 0);
        bool circulant = s.algorithm == OSOE_SHAPE_CIRCULANT;
        uint64_t c = bytes <= small ? 0
                     : circulant ? osoe_shape_circulant_cost(nodes, s.window, bytes,
                                                             OSOE_INTERNAL_PIECE_BYTES, &link)
                                 : osoe_shape_cost(operation, nodes, s.fanout, s.window, bytes,
                                                   OSOE_INTERNAL_PIECE_BYTES, &link);
        const char *name = circulant ? "circulant" : "tree";
        if (strcmp(name, algorithm) != 0 || s.fanout != fanout || s.window != window ||
            c != cost) {
            if (differ++ < 10)
                fprintf(stderr, "%s: the model says %s k %u W %u %llu ns, the C %s k %u W %u "
                        "%llu ns: %s", path, algorithm, fanout, window, cost, name, s.fanout,
                        s.window, (unsigned long long)c, line);
        }
        lines++;
    }
    fclose(f);
    printf("%u lines of %s, %u where the C and the model differ\n", lines, path, differ);
    CHECK(differ == 0);
    CHECK(lines > 1000);
    return CHECK_DONE();
}

/* collect's places' choice and costs (P31) against what
 * tools/modelcheck/shape_timing.py writes - the team's nodes and members,
 * whether every node is one run and the runs are in order, the exchange's
 * room, the link, the ceiling, the capacity, the small class's bytes and
 * whether Träff's is allowed; then the way and each way's cost - requiring the
 * same of osoe_places_choose and osoe_places_cost. */
static int places_grid(const char *path)
{
    static const char *const ways[] = { "members", "totals", "scan", "exchange" };
    static const enum osoe_places_way numbered[] = { OSOE_PLACES_MEMBERS, OSOE_PLACES_TOTALS,
                                                     OSOE_PLACES_SCAN, OSOE_PLACES_EXCHANGE };
    FILE *f = fopen(path, "r");
    CHECK_FATAL(f != NULL);
    char line[512];
    unsigned lines = 0, differ = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        char way[16];
        unsigned nodes, members, runs, ordered, room, ceiling, capacity, small, allowed;
        unsigned long long hop, message, bandwidth, cost[4];
        if (sscanf(line, "%u %u %u %u %u %llu %llu %llu %u %u %u %u %15s %llu %llu %llu %llu",
                   &nodes, &members, &runs, &ordered, &room, &hop, &message, &bandwidth,
                   &ceiling, &capacity, &small, &allowed, way, &cost[0], &cost[1], &cost[2],
                   &cost[3]) != 17) {
            fprintf(stderr, "%s: cannot read: %s", path, line);
            CHECK(0);
            continue;
        }
        struct osoe_link link = { hop, message, bandwidth };
        enum osoe_places_way chosen =
            osoe_places_choose(nodes, members, runs != 0, ordered != 0, room, &link, ceiling,
                               capacity, small, OSOE_INTERNAL_PIECE_BYTES, allowed != 0);
        bool same = strcmp(ways[chosen - 1], way) == 0;
        for (unsigned w = 0; w < 4; w++)
            same &= osoe_places_cost(numbered[w], nodes, members, &link, ceiling, capacity, small,
                                     OSOE_INTERNAL_PIECE_BYTES, allowed != 0) == cost[w];
        if (!same && differ++ < 10)
            fprintf(stderr, "%s: the C chose %s: %s", path, ways[chosen - 1], line);
        lines++;
    }
    fclose(f);
    printf("%u lines of %s, %u where the C and the model differ\n", lines, path, differ);
    CHECK(differ == 0);
    CHECK(lines > 1000);
    return CHECK_DONE();
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "choice") == 0)
        return choice();
    if (argc == 3 && strcmp(argv[1], "grid") == 0)
        return grid(argv[2]);
    if (argc == 3 && strcmp(argv[1], "places-grid") == 0)
        return places_grid(argv[2]);
    fprintf(stderr, "usage: %s choice | grid FILE | places-grid FILE\n", argv[0]);
    return 2;
}
