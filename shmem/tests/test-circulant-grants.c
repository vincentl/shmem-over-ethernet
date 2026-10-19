/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Träff's reduce-scatter grants and senders' lanes, read and then cleared
 * (tier 2; T40; internode-generic.c's take_round_grants and take_sender_lane).
 *
 * In Träff's reduction a lead's reduce-scatter grant holds a field for each of
 * its rounds, which that round's receiver alone puts as it enters, and its
 * senders' lanes line a word for each row, which that round's sender puts with
 * its first piece. The lead reads each, then clears it, so that a field or a
 * word nobody has written since reads as none (internode-generic.c, cleared).
 * circulant_model.py tags each field with its writer and its round; the C
 * carries neither, and what makes its fields safe untagged is one writer a
 * field, which shmem.circulant.arithmetic checks at every node count from 2 to
 * 300, and the clear - which nothing else sees: with it deleted, every result
 * is still right.
 *
 * Over logical nodes, the link set by OSOE_LINK_* so that the chooser takes
 * Träff's: three sum reductions of the world at the smallest of a ladder of
 * sizes the chooser gives Träff's, each between two barriers, so that no PE
 * has entered the next while a lead reads. After each, every node's lead
 * requires its reduce-scatter grant counter to stand at the reductions' rounds
 * so far - every grant in, and Träff's taken - and every byte of its grant
 * lines past the counter, and of its senders' lanes line, to read zero,
 * through its own internal region; every PE requires its result. White-box:
 * the words are the library's, so its sources are compiled in (CMakeLists.txt).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"
#include "collective-circulant.h"
#include "collective-shape.h"
#include "internal-region.h"
#include "node.h"
#include "state.h"
#include "transport.h"

enum { REDUCTIONS = 3 };

/* What the data path asks the chooser for a reduction of `bytes` over the
 * world (internode-generic.c, shape_of). */
static struct osoe_collective_shape chosen(uint64_t bytes)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    uint32_t nodes = osoe.job.node_count;
    if (b->collective_shape == OSOE_COLLECTIVE_SHAPE_FIXED ||
        osoe.job.link_source == OSOE_LINK_NONE)
        return (struct osoe_collective_shape){ OSOE_SHAPE_TREE, b->collective_fanout,
                                               b->collective_pipeline };
    bool circulant = b->collective_shape == OSOE_COLLECTIVE_SHAPE_LINK &&
                     osoe_circulant_rows(nodes) <=
                         osoe_internal_circulant_rows(b->node_count);
    return osoe_shape_choose(OSOE_SHAPE_REDUCE, nodes, bytes, &osoe.job.link,
                             b->collective_fanout, b->collective_pipeline,
                             b->small_collective_bytes, OSOE_INTERNAL_PIECE_BYTES, circulant);
}

/* Where team number `number`'s reduce-scatter line `line` starts in a PE's
 * internal region (internode-generic.c, circulant_line). */
static uint64_t circulant_line(uint32_t number, uint32_t line)
{
    const struct osoe_bootstrap *b = &osoe.bootstrap;
    uint32_t rows = osoe_internal_circulant_rows(b->node_count);
    return osoe_internal_circulant_offset(b->collective_fanout, b->max_teams,
                                          b->small_collective_bytes, b->node_count) +
           (uint64_t)number * osoe_internal_team_circulant_bytes(rows) +
           (uint64_t)line * OSOE_INTERNAL_LINE_BYTES;
}

/* How many of `bytes` bytes at `offset` in this PE's internal region are not
 * zero, and the first of them, as an offset from `offset`. */
static uint64_t written(uint64_t offset, uint64_t bytes, uint64_t *first)
{
    const unsigned char *region =
        (const unsigned char *)osoe.node.heap_of[osoe.bootstrap.local_id] +
        osoe.node.internal_offset;
    uint64_t found = 0;
    for (uint64_t i = 0; i < bytes; i++)
        if (region[offset + i] != 0 && found++ == 0)
            *first = i;
    return found;
}

/*
 * This node's lead after `done` reductions: its grant counter at `done` times
 * the rounds of a half, and nothing left past it in the grant's lines or in
 * the senders' lanes line. The counter is the line after the credit counter
 * and the allgather's grant counter; the grant's lines follow it, and the
 * senders' lanes line them (internode-generic.c).
 */
static int lead_cleared(int me, uint32_t done)
{
    uint32_t number = OSOE_TEAM_WORLD;
    uint32_t layout = osoe_internal_circulant_rows(osoe.bootstrap.node_count);
    uint32_t rows = osoe_circulant_rows(osoe.job.node_count);
    uint32_t grant_lines = osoe_internal_circulant_grant_lines(layout);
    uint64_t counter = circulant_line(number, layout + 2);
    uint64_t senders = circulant_line(number, layout + 2 + grant_lines);
    int bad = 0;
    uint64_t grants = osoe_transport_load(OSOE_REGION_INTERNAL, counter, 8);
    if (grants != (uint64_t)done * rows) {
        fprintf(stderr, "PE %d, node %u's lead: after %u Träff's reductions its reduce-scatter "
                "grant counter is %llu, where %u rounds a reduction make %llu\n", me,
                osoe.job.node, done, (unsigned long long)grants, rows,
                (unsigned long long)((uint64_t)done * rows));
        bad = 1;
    }
    uint64_t first = 0;
    uint64_t left = written(counter + 8, (uint64_t)grant_lines * OSOE_INTERNAL_LINE_BYTES - 8,
                            &first);
    if (left != 0) {
        fprintf(stderr, "PE %d, node %u's lead: after %u Träff's reductions %llu bytes of its "
                "reduce-scatter grant's fields are not cleared, the first at byte %llu - "
                "field %llu\n", me, osoe.job.node, done, (unsigned long long)left,
                (unsigned long long)first,
                (unsigned long long)(first / OSOE_INTERNAL_CIRCULANT_GRANT_FIELD_BYTES));
        bad = 1;
    }
    left = written(senders, OSOE_INTERNAL_LINE_BYTES, &first);
    if (left != 0) {
        fprintf(stderr, "PE %d, node %u's lead: after %u Träff's reductions %llu bytes of its "
                "senders' lanes line are not cleared, the first at byte %llu\n", me,
                osoe.job.node, done, (unsigned long long)left, (unsigned long long)first);
        bad = 1;
    }
    return bad;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    CHECK_FATAL(osoe.job.node_count >= 2);

    uint64_t bytes = 0;
    for (uint64_t b = 2048; b <= (UINT64_C(1) << 20) && bytes == 0; b *= 2)
        if (chosen(b).algorithm == OSOE_SHAPE_CIRCULANT)
            bytes = b;
    if (bytes == 0) {
        if (me == 0)
            fprintf(stderr, "no size to 1 MiB takes Träff's on %u nodes on this link\n",
                    osoe.job.node_count);
        CHECK_FATAL(bytes != 0);
    }
    size_t count = (size_t)(bytes / sizeof(long));
    long *source = shmem_malloc(count * sizeof *source);
    long *dest = shmem_malloc(count * sizeof *dest);
    int *failed = shmem_malloc(2 * sizeof *failed);
    CHECK_FATAL(source && dest && failed);

    int failures = 0;
    for (uint32_t done = 1; done <= REDUCTIONS; done++) {
        for (size_t i = 0; i < count; i++)
            source[i] = (long)me * 1000 + (long)((i + done) % 7);
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, count);
        shmem_barrier_all();
        long ranks = (long)npes * (npes - 1) / 2 * 1000;
        for (size_t i = 0; i < count; i++)
            if (dest[i] != ranks + (long)npes * (long)((i + done) % 7)) {
                fprintf(stderr, "PE %d: reduction %u's element %zu is %ld, not %ld\n", me,
                        done, i, dest[i], ranks + (long)npes * (long)((i + done) % 7));
                failures++;
                break;
            }
        if (osoe.bootstrap.local_id == 0)
            failures += lead_cleared(me, done);
        shmem_barrier_all();
    }

    failed[0] = failures;
    shmem_barrier_all();
    shmem_int_sum_reduce(SHMEM_TEAM_WORLD, &failed[1], &failed[0], 1);
    CHECK(failed[1] == 0);
    if (me == 0)
        printf("CIRCULANT GRANTS CLEARED: %u Träff's reductions of %llu bytes on %u nodes, %d "
               "failures\n", REDUCTIONS, (unsigned long long)bytes, osoe.job.node_count,
               failed[1]);
    shmem_free(failed);
    shmem_free(dest);
    shmem_free(source);
    shmem_finalize();
    return CHECK_DONE();
}
