/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A blocking get of more than a piece issues its pieces together
 * (docs/decisions/2026-10-10-blocking-gets.md; docs/design/blocking-get-proposal.md,
 * (b); transport-internode.c, remote_get).
 *
 * Across nodes a blocking get of more than a chunk's payload goes out as a
 * nonblocking get does - every piece with extents, a record and a token of its
 * own, on the calling thread's own context - and the thread copies each piece
 * out as it lands, through the same copy-out a nonblocking get's wait uses,
 * which counts each piece in am.gets_completed. A get of one piece keeps the
 * slot it always had, which counts nothing there. So PE 0 gets from PE 1, on
 * the other node: a get of one piece leaves am.gets_completed as it was, and a
 * get of a mebibyte and three bytes, into a destination at an odd offset,
 * moves it by a piece at least twice - and am.gets_nonblocking not at all,
 * since none of them is a nonblocking get. Every byte is checked where it
 * landed.
 *
 * And the pieces overlap: where the get is larger than the landing zone, as at
 * the default 256 KiB, its first pieces fill the zone before any is copied out,
 * so an issue finds no room - am.landing_full moves - where a get completing
 * each piece before it issues the next would never fill it (the adversarial
 * read of v0.2's blocking gets, note 4).
 *
 * OSOE_TEST_GET_PIECES=fallback, with OSOE_MAX_CONTEXTS at its most: every
 * thread place's block lies past what a token can name, so a get of more than
 * a piece takes the piece-at-a-time path instead - none of its pieces copied
 * out as a nonblocking get's, the zone never full - and its bytes are as right
 * (note 6).
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

enum { BIG = (1 << 20) + 3, SMALL = 1000, OFFSET = 5 };

static unsigned char pattern(size_t i)
{
    return (unsigned char)((i * 31 + 7) % 251);
}

/* This PE's total for a thread-scope counter, as test-get-nbi.c reads it. */
static uint64_t counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    const char *local = osoe_test_launcher("LOCALID");
    uint32_t local_id = (uint32_t)atoi(local ? local : "0");
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, local_id * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

static size_t first_wrong(const unsigned char *got, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (got[i] != pattern(i))
            return i;
    return len;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    CHECK_FATAL(shmem_n_pes() == 2);
    unsigned char *source = shmem_malloc(BIG);
    CHECK_FATAL(source != NULL);
    for (size_t i = 0; i < BIG; i++)
        source[i] = pattern(i);
    shmem_barrier_all();

    if (me == 0) {
        CHECK_FATAL(shmem_ptr(source, 1) == NULL);  /* PE 1 is on the other node */
        unsigned char *mine = malloc(BIG + OFFSET);
        CHECK_FATAL(mine != NULL);

        uint64_t completed = counter("am.gets_completed");
        uint64_t nonblocking = counter("am.gets_nonblocking");
        memset(mine, 0, SMALL);
        shmem_getmem(mine, source, SMALL, 1);
        CHECK(first_wrong(mine, SMALL) == SMALL);
        uint64_t one_piece = counter("am.gets_completed") - completed;
        printf("a get of %d bytes: %llu pieces copied out\n", SMALL,
               (unsigned long long)one_piece);
        CHECK(one_piece == 0);

        completed = counter("am.gets_completed");
        uint64_t full = counter("am.landing_full");
        memset(mine, 0, BIG + OFFSET);
        shmem_getmem(mine + OFFSET, source, BIG, 1);
        size_t wrong = first_wrong(mine + OFFSET, BIG);
        if (wrong != BIG)
            fprintf(stderr, "PE 0: byte %zu of the get read %u, not %u\n", wrong,
                    mine[OFFSET + wrong], pattern(wrong));
        CHECK(wrong == BIG);
        for (size_t i = 0; i < OFFSET; i++)
            CHECK(mine[i] == 0);
        uint64_t pieces = counter("am.gets_completed") - completed;
        uint64_t counted = counter("am.gets_nonblocking") - nonblocking;
        uint64_t no_room = counter("am.landing_full") - full;
        printf("a get of %d bytes: %llu pieces copied out, %llu counted nonblocking, "
               "%llu issues found no room\n", BIG, (unsigned long long)pieces,
               (unsigned long long)counted, (unsigned long long)no_room);
        CHECK(counted == 0);
        const char *mode = getenv("OSOE_TEST_GET_PIECES");
        if (mode && strcmp(mode, "fallback") == 0) {
            CHECK(pieces == 0);
            CHECK(no_room == 0);
            if (one_piece == 0 && wrong == BIG && pieces == 0 && counted == 0 && no_room == 0)
                printf("GET PIECES FALLBACK OK\n");
        } else {
            const char *zone = getenv("OSOE_GET_LANDING_BYTES");
            bool overfills = (zone ? strtoull(zone, NULL, 10) : 256 * 1024) < BIG;
            CHECK(pieces >= 2);
            CHECK(!overfills || no_room >= 1);
            if (one_piece == 0 && wrong == BIG && pieces >= 2 && counted == 0 &&
                (!overfills || no_room >= 1))
                printf("GET PIECES OK\n");
        }
        free(mine);
    }

    shmem_barrier_all();
    CHECK_INVARIANTS();
    shmem_free(source);
    shmem_finalize();
    return CHECK_DONE();
}
