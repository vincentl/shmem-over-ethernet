/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Pipelined pieces through the internode tree (design §5.8, "Pipelined
 * pieces"; phase 1 step 5 slice 5; internode-generic.c). A data collective's
 * buffer goes between the node leads in 16 KiB pieces, each lead keeping D of
 * them in flight - D = OSOE_COLLECTIVE_PIPELINE, which ctest sets to 1, 2 and
 * 4 on each map - so these are collectives of many pieces:
 *
 *   - a reduction, a broadcast from a root on another node than PE 0's, and an
 *     fcollect, each of 1 MiB and of an odd size whose last piece is short,
 *     every element checked;
 *   - collectives back to back with no barrier between: broadcasts from every
 *     root in turn, each tree rotated to its root, and reductions, rooted at
 *     the first node, between them, at piece counts that differ from one to
 *     the next - so each child position's counter is fed by different children
 *     over the run, and only the bases a lead advances by its own trees keep
 *     the pieces apart;
 *   - the count each lead publishes of the most pieces it had in flight
 *     (team.tree.most_pieces_in_flight), read as osoe-stat reads it, after a
 *     broadcast and a reduction of many pieces: exactly min(D, pieces) at a
 *     leaf of the collective's tree, which has no children to wait for and
 *     fills its window before it looks for a release; between 1 and that at a
 *     lead between the root and the leaves, which takes whichever of its
 *     inputs is ready first, so the schedule decides; and 0 at the root, which
 *     sends nothing up. A lead with one piece too many in flight shows it
 *     here, and a leaf with one too few.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "launcher.h"

/*
 * A piece carries 16 KiB, its 16-byte header in the lane's slot beside it
 * (internal-region.h): 2,048 longs of a reduction or a gather, or 16,384 bytes
 * of a broadcast, which goes as bytes - the same bytes either way. So 1 MiB is
 * 64 whole pieces, and the odd sizes end on a short piece. (A piece held 16 KiB
 * less its header until round six's U1, when 1 MiB was 64 pieces and one.)
 */
enum {
    PIECE_BYTES = 16 << 10,
    MEBIBYTE_LONGS = (1 << 20) / sizeof(long),      /* 64 pieces */
    ODD_LONGS = 100003,                             /* 49, the last 13,592 bytes */
    ODD_BYTES = (1 << 20) + 13,                     /* 65, the last 13 bytes */
    ODD_BLOCK_LONGS = 16411,                        /* an fcollect's, from each PE */
};

static int me, npes;

static long value(int pe, size_t i, int round)
{
    return (long)pe * 1000003L + (long)i * 7 + (long)round * 31 + 1;
}

static unsigned char byte_value(int pe, size_t i)
{
    return (unsigned char)(pe * 37 + i * 11 + i / 251);
}

/* Report the first element that differs, and count it once. */
static void first_wrong(const char *what, size_t n, size_t wrong, long got, long want)
{
    if (wrong < n)
        fprintf(stderr, "PE %d: %s of %zu elements, element %zu is %ld, not %ld\n", me, what, n,
                wrong, got, want);
    CHECK(wrong == n);
}

static void reduction(long *dst, long *src, size_t n, int round)
{
    for (size_t i = 0; i < n; i++)
        src[i] = value(me, i, round);
    CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dst, src, n) == 0);
    size_t wrong = n;
    long want = 0;
    for (size_t i = 0; i < n && wrong == n; i++) {
        want = 0;
        for (int p = 0; p < npes; p++)
            want += value(p, i, round);
        if (dst[i] != want)
            wrong = i;
    }
    first_wrong("sum", n, wrong, wrong < n ? dst[wrong] : 0, want);
}

static void broadcast_longs(long *dst, long *src, size_t n, int root, int round)
{
    for (size_t i = 0; i < n; i++) {
        src[i] = value(me, i, round);
        dst[i] = -1;
    }
    CHECK(shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, n, root) == 0);
    size_t wrong = n;
    for (size_t i = 0; i < n && wrong == n; i++)
        if (dst[i] != value(root, i, round))
            wrong = i;
    first_wrong("broadcast", n, wrong, wrong < n ? dst[wrong] : 0,
                wrong < n ? value(root, wrong, round) : 0);
}

static void broadcast_bytes(unsigned char *dst, unsigned char *src, size_t n, int root)
{
    for (size_t i = 0; i < n; i++) {
        src[i] = byte_value(me, i);
        dst[i] = (unsigned char)~byte_value(root, i);
    }
    CHECK(shmem_broadcastmem(SHMEM_TEAM_WORLD, dst, src, n, root) == 0);
    size_t wrong = n;
    for (size_t i = 0; i < n && wrong == n; i++)
        if (dst[i] != byte_value(root, i))
            wrong = i;
    first_wrong("byte broadcast", n, wrong, wrong < n ? dst[wrong] : 0,
                wrong < n ? byte_value(root, wrong) : 0);
}

/* `n` longs from every PE, into npes * n. */
static void fcollect(long *dst, long *src, size_t n, int round)
{
    for (size_t i = 0; i < n; i++)
        src[i] = value(me, i, round);
    for (size_t i = 0; i < (size_t)npes * n; i++)
        dst[i] = -1;
    CHECK(shmem_long_fcollect(SHMEM_TEAM_WORLD, dst, src, n) == 0);
    size_t total = (size_t)npes * n, wrong = total;
    for (size_t i = 0; i < total && wrong == total; i++)
        if (dst[i] != value((int)(i / n), i % n, round))
            wrong = i;
    first_wrong("fcollect", total, wrong, wrong < total ? dst[wrong] : 0,
                wrong < total ? value((int)(wrong / n), wrong % n, round) : 0);
}

/*
 * A root whose node is not PE 0's - the highest such rank - so that a
 * broadcast's tree is rotated away from the reduction's. PE 0 finds it among
 * the PEs its node does not share, and broadcasts it; on one node, the last PE.
 */
static int root_off_first_node(void)
{
    int *symmetric = shmem_malloc(2 * sizeof *symmetric);
    CHECK_FATAL(symmetric);
    if (me == 0) {
        symmetric[1] = npes - 1;
        for (int r = npes - 1; r > 0; r--)
            if (shmem_team_translate_pe(SHMEM_TEAM_WORLD, r, SHMEM_TEAM_SHARED) < 0) {
                symmetric[1] = r;
                break;
            }
    }
    CHECK(shmem_int_broadcast(SHMEM_TEAM_WORLD, &symmetric[0], &symmetric[1], 1, 0) == 0);
    int chosen = symmetric[0];
    shmem_free(symmetric);
    return chosen;
}

/* D, as the job was given it. */
static uint64_t pipeline(void)
{
    const char *d = getenv("OSOE_COLLECTIVE_PIPELINE");
    return d ? strtoull(d, NULL, 10) : 4;   /* the default, bootstrap.h */
}

/* The fan-out, likewise: the library's default is 8 (internode-backend.h). */
static uint64_t fanout(void)
{
    const char *k = getenv("OSOE_COLLECTIVE_FANOUT");
    return k ? strtoull(k, NULL, 10) : 8;
}

/* Where a node is in the tree over `nodes` nodes rooted at node `root_node`:
 * internode-generic.c's, whose node index i has children k i + 1 ... k i + k. */
enum place { ROOT, BETWEEN, LEAF };
static const char *const place_names[] = { "the root", "between root and leaves", "a leaf" };

static enum place place_in_tree(int nodes, int node, int root_node)
{
    uint64_t index = (uint64_t)((node - root_node + nodes) % nodes);
    if (index == 0)
        return ROOT;
    return fanout() * index + 1 >= (uint64_t)nodes ? LEAF : BETWEEN;
}

/*
 * The count this node's lead published for its last data collective on the
 * world team (number 0), which was of `pieces` pieces: 0 at the tree's root,
 * min(D, pieces) at a leaf, and at least 1 and at most that between them.
 * Then a barrier, so that no lead starts another data collective while a PE
 * of its node has still to read this one's.
 */
static void check_in_flight(const char *what, enum place place, uint64_t pieces)
{
    uint64_t most = UINT64_MAX;
    CHECK(osoe_test_scoped_metric("team.tree.most_pieces_in_flight", OSOE_MSCOPE_TEAM, 0,
                                  &most));
    uint64_t window = pieces < pipeline() ? pieces : pipeline();
    bool right = place == ROOT ? most == 0 : place == LEAF ? most == window
                                                           : most >= 1 && most <= window;
    if (!right)
        fprintf(stderr, "PE %d: after a %s of %llu pieces, this node's lead - %s - had at "
                "most %llu in flight; D is %llu\n", me, what, (unsigned long long)pieces,
                place_names[place], (unsigned long long)most, (unsigned long long)pipeline());
    CHECK(right);
    shmem_barrier_all();
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();
    const char *nodes_text = osoe_test_launcher("STEP_NUM_NODES");
    const char *node_text = osoe_test_launcher("NODEID");
    int nodes = nodes_text ? atoi(nodes_text) : 1;
    int node = node_text ? atoi(node_text) : 0;

    size_t largest = MEBIBYTE_LONGS;
    if (largest < ODD_LONGS)
        largest = ODD_LONGS;
    if (largest < (size_t)npes * ODD_BLOCK_LONGS)
        largest = (size_t)npes * ODD_BLOCK_LONGS;
    long *src = shmem_malloc(largest * sizeof *src);
    long *dst = shmem_malloc(largest * sizeof *dst);
    unsigned char *bytes_src = shmem_malloc(ODD_BYTES);
    unsigned char *bytes_dst = shmem_malloc(ODD_BYTES);
    CHECK_FATAL(src && dst && bytes_src && bytes_dst);
    int root = root_off_first_node();

    /* 1 MiB and an odd size of each, element by element. */
    reduction(dst, src, MEBIBYTE_LONGS, 0);
    reduction(dst, src, ODD_LONGS, 1);
    broadcast_longs(dst, src, MEBIBYTE_LONGS, root, 2);
    broadcast_longs(dst, src, ODD_LONGS, root, 3);
    broadcast_bytes(bytes_dst, bytes_src, ODD_BYTES, root);
    fcollect(dst, src, MEBIBYTE_LONGS / (size_t)npes, 4);
    fcollect(dst, src, ODD_BLOCK_LONGS, 5);

    /*
     * Back to back, no barrier: a broadcast from every root in turn and a
     * reduction after each, at 1, 3 and 49 pieces in turn, so that the next
     * collective's tree, and its piece count, differ from this one's.
     */
    static const size_t sizes[] = { 5, 3 * 2048 - 7, ODD_LONGS };
    for (int round = 0; round < 2 * npes; round++) {
        size_t n = sizes[round % 3];
        broadcast_longs(dst, src, n, round % npes, 10 + round);
        reduction(dst, src, sizes[(round + 1) % 3], 10 + round);
    }

    /*
     * The count each lead publishes, after one collective of many pieces on
     * each tree: the broadcast's, rooted at its root's node, and the
     * reduction's, at the first node.
     */
    shmem_barrier_all();
    if (nodes > 1) {
        /* The broadcast's tree is rooted at its root's node: ask the root. */
        int *node_of = shmem_malloc(sizeof *node_of);
        CHECK_FATAL(node_of);
        *node_of = node;
        shmem_barrier_all();
        int root_node = shmem_int_g(node_of, root);
        shmem_free(node_of);

        uint64_t pieces = (MEBIBYTE_LONGS * sizeof(long) + PIECE_BYTES - 1) / PIECE_BYTES;
        broadcast_longs(dst, src, MEBIBYTE_LONGS, root, 40);
        check_in_flight("broadcast", place_in_tree(nodes, node, root_node), pieces);
        reduction(dst, src, MEBIBYTE_LONGS, 41);
        check_in_flight("reduction", place_in_tree(nodes, node, 0), pieces);
    }

    shmem_free(src);
    shmem_free(dst);
    shmem_free(bytes_src);
    shmem_free(bytes_dst);
    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
