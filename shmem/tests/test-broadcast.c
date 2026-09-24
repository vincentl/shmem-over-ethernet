/*
 * The team-based broadcast (spec §9.10.7; design §5.5, phase 1 step 5): from
 * every root in turn, so that the tree is rooted at every node and the root is
 * sometimes its node's lead and sometimes not; one element, a few, and more
 * than one internal-region slot; the byte form and the C11 generic;
 * SHMEM_TEAM_SHARED; nelems 0; in place; and interleaved with reductions.
 *
 * "The dest object is updated on all PEs" - the root's included - so every PE,
 * the root too, must end holding the root's source. Each PE's source is a
 * function of its rank, so every PE knows what it must receive.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SMALL = 9, LARGE = 5000 };     /* LARGE longs: three 16 KiB slots' worth */

static int me, npes;

static long value(int pe, size_t i)
{
    return (long)pe * 1000003L + (long)i * 7 + 1;
}

static void from_every_root(void)
{
    long *src = shmem_malloc(LARGE * sizeof *src);
    long *dst = shmem_malloc(LARGE * sizeof *dst);
    CHECK_FATAL(src && dst);  /* NULL would be dereferenced just below */
    for (size_t i = 0; i < LARGE; i++)
        src[i] = value(me, i);
    size_t sizes[] = { 1, SMALL, LARGE };
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        for (int root = 0; root < npes; root++) {
            size_t n = sizes[s];
            for (size_t i = 0; i < n; i++)
                dst[i] = -1;
            CHECK(shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, n, root) == 0);
            size_t wrong = n;
            for (size_t i = 0; i < n && wrong == n; i++)
                if (dst[i] != value(root, i))
                    wrong = i;
            if (wrong < n)
                fprintf(stderr, "PE %d: broadcast of %zu from %d, element %zu is %ld\n", me,
                        n, root, wrong, dst[wrong]);
            CHECK(wrong == n);
        }
    }

    /* The byte form and the generic, from the last PE. */
    unsigned char *bytes = shmem_malloc(SMALL);
    unsigned char *got = shmem_malloc(SMALL);
    CHECK_FATAL(bytes && got);
    for (int i = 0; i < SMALL; i++)
        bytes[i] = (unsigned char)(me * 16 + i);
    CHECK(shmem_broadcastmem(SHMEM_TEAM_WORLD, got, bytes, SMALL, npes - 1) == 0);
    for (int i = 0; i < SMALL; i++)
        CHECK(got[i] == (unsigned char)((npes - 1) * 16 + i));
    CHECK(shmem_broadcast(SHMEM_TEAM_WORLD, dst, src, SMALL, 0) == 0);
    for (size_t i = 0; i < SMALL; i++)
        CHECK(dst[i] == value(0, i));

    /* The shared team: rooted at its last member, numbered by local position. */
    int local = shmem_team_n_pes(SHMEM_TEAM_SHARED);
    int last = shmem_team_translate_pe(SHMEM_TEAM_SHARED, local - 1, SHMEM_TEAM_WORLD);
    CHECK(shmem_long_broadcast(SHMEM_TEAM_SHARED, dst, src, SMALL, local - 1) == 0);
    for (size_t i = 0; i < SMALL; i++)
        CHECK(dst[i] == value(last, i));

    /* nelems 0: success, nothing touched. */
    dst[0] = -7;
    CHECK(shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, 0, 0) == 0);
    CHECK(dst[0] == -7);

    shmem_free(bytes);
    shmem_free(got);
    shmem_free(src);
    shmem_free(dst);
}

/*
 * In place, from every root: dest is source, so the root's own copy is a no-op
 * - and where the root is its node's lead, the lead copies nothing at all
 * (collectives.c) - while every other PE's source is overwritten unread.
 */
static void in_place(void)
{
    long *buffer = shmem_malloc(SMALL * sizeof *buffer);
    CHECK_FATAL(buffer);
    for (int root = 0; root < npes; root++) {
        for (size_t i = 0; i < SMALL; i++)
            buffer[i] = value(me, i);
        CHECK(shmem_long_broadcast(SHMEM_TEAM_WORLD, buffer, buffer, SMALL, root) == 0);
        for (size_t i = 0; i < SMALL; i++)
            CHECK(buffer[i] == value(root, i));
    }
    shmem_free(buffer);
}

/*
 * Broadcasts rooted all over the job, interleaved with reductions, at sizes
 * that go through the internode phase in pieces and in one: each broadcast's
 * tree is rooted at its root's node and each reduction's at the first, so the
 * leads' arrival and release words see a different tree from one collective to
 * the next. collectives.c argues that this cannot disturb them; this is that
 * argument's test.
 */
static void mixed_trees(void)
{
    long *src = shmem_malloc(LARGE * sizeof *src);
    long *dst = shmem_malloc(LARGE * sizeof *dst);
    long *sum = shmem_malloc(LARGE * sizeof *sum);
    CHECK_FATAL(src && dst && sum);
    for (int round = 0; round < 3 * npes + 3; round++) {
        int root = (round * 5 + 1) % npes;
        size_t n = round % 3 == 0 ? LARGE : round % 3 == 1 ? SMALL : 1;
        for (size_t i = 0; i < n; i++)
            src[i] = value(me, i) + round;
        CHECK(shmem_long_broadcast(SHMEM_TEAM_WORLD, dst, src, n, root) == 0);
        CHECK(shmem_long_sum_reduce(SHMEM_TEAM_WORLD, sum, src, n) == 0);
        size_t wrong = n;
        for (size_t i = 0; i < n && wrong == n; i++) {
            long total = 0;
            for (int p = 0; p < npes; p++)
                total += value(p, i) + round;
            if (dst[i] != value(root, i) + round || sum[i] != total)
                wrong = i;
        }
        if (wrong < n)
            fprintf(stderr, "PE %d: round %d (root %d, %zu elements), element %zu: "
                    "broadcast %ld, sum %ld\n", me, round, root, n, wrong, dst[wrong],
                    sum[wrong]);
        CHECK(wrong == n);
    }
    shmem_free(src);
    shmem_free(dst);
    shmem_free(sum);
}

int main(void)
{
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    from_every_root();
    in_place();
    mixed_trees();

    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
