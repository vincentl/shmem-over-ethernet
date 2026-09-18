/*
 * The §4.9 watched-range encoding (shmem/src/watch_range.h), on its own.
 *
 * The one property that matters is that the filter is never wrong in the
 * direction that loses a wake: osoe_watch_covers must return true for every
 * write that touched the published range. Returning true for a write that did
 * not is legal and costs a spurious wake, so the tests below assert exactness
 * where the encoding is exact and only conservatism where it is not.
 *
 * The cases the earlier power-of-two encoding got wrong are here by name: a
 * range straddling a high boundary, which it inflated to the whole region, and
 * the saturating case, where it computed a shift count of 4294967295 and
 * invoked undefined behaviour.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "watch_range.h"

enum { HEAP = 0, INTERNAL = 1 };

/* Brute force over a small window: every write that overlaps the watched range
 * must be covered. This is the property; everything else is detail. */
static void exhaustive(unsigned region, uint64_t off, uint64_t len)
{
    uint64_t w = osoe_watch_pack(region, off, len);
    uint64_t lo = off > 64 ? off - 64 : 0;
    for (uint64_t o = lo; o < off + len + 64; o++) {
        for (uint64_t n = 1; n <= 16; n++) {
            bool overlaps = o < off + len && off < o + n;
            if (overlaps && !osoe_watch_covers(w, region, o, n)) {
                fprintf(stderr, "LOST WAKE: watch [%llu,+%llu) missed write [%llu,+%llu)\n",
                        (unsigned long long)off, (unsigned long long)len,
                        (unsigned long long)o, (unsigned long long)n);
                CHECK(0);
                return;
            }
        }
    }
}

int main(void)
{
    /* No watch. */
    CHECK(osoe_watch_pack(HEAP, 0, 0) == 0);
    CHECK(!osoe_watch_covers(0, HEAP, 0, 8));

    /* A scalar ivar of each AMO width, at its natural alignment, is exact: the
     * grain is 4 bytes, so nothing else in the heap can wake it. */
    for (uint64_t off = 0; off < 4096; off += 8) {
        uint64_t w = osoe_watch_pack(HEAP, off, 8);
        CHECK(osoe_watch_covers(w, HEAP, off, 8));
        CHECK(!osoe_watch_covers(w, HEAP, off + 8, 8));
        if (off >= 8)
            CHECK(!osoe_watch_covers(w, HEAP, off - 8, 8));
    }
    for (uint64_t off = 0; off < 4096; off += 4) {
        uint64_t w = osoe_watch_pack(HEAP, off, 4);
        CHECK(osoe_watch_covers(w, HEAP, off, 4));
        CHECK(!osoe_watch_covers(w, HEAP, off + 4, 4));
    }

    /* Ranges that straddle a power-of-two boundary - what the earlier encoding
     * inflated to the whole region. The published range is the range. */
    static const uint64_t straddles[][2] = {
        { (UINT64_C(1) << 27) - 64, 128 },
        { (UINT64_C(1) << 31) - 8, 16 },
        { (UINT64_C(1) << 20) - 4, 8 },
        { 3, 2 },
        { 0, 1 },
    };
    for (unsigned i = 0; i < sizeof straddles / sizeof straddles[0]; i++) {
        uint64_t off = straddles[i][0], len = straddles[i][1];
        uint64_t w = osoe_watch_pack(HEAP, off, len);
        exhaustive(HEAP, off, len);
        /* Exact to the grain: a write one grain below the rounded-down start,
         * and one grain above the rounded-up end, must not be covered. */
        uint64_t first = off / OSOE_WATCH_GRAIN, last = (off + len - 1) / OSOE_WATCH_GRAIN;
        if (first > 0)
            CHECK(!osoe_watch_covers(w, HEAP, (first - 1) * OSOE_WATCH_GRAIN,
                                     OSOE_WATCH_GRAIN));
        CHECK(!osoe_watch_covers(w, HEAP, (last + 1) * OSOE_WATCH_GRAIN, OSOE_WATCH_GRAIN));
    }

    /* Array waits of the shape the multiple-element forms publish. */
    exhaustive(HEAP, 4096, 8192);
    exhaustive(HEAP, (UINT64_C(1) << 27) - 4096, 8192);

    /* The region is part of the filter: the same offset in another region is a
     * different place, and both regions start at 0. */
    uint64_t wh = osoe_watch_pack(HEAP, 128, 8);
    CHECK(osoe_watch_covers(wh, HEAP, 128, 8));
    CHECK(!osoe_watch_covers(wh, INTERNAL, 128, 8));
    uint64_t wi = osoe_watch_pack(INTERNAL, 128, 8);
    CHECK(osoe_watch_covers(wi, INTERNAL, 128, 8));
    CHECK(!osoe_watch_covers(wi, HEAP, 128, 8));

    /* Saturation, both kinds, must be permissive - a longer range, never
     * "none". Encoding it as none is the `satzero` lost wake in
     * docs/design/evidence/watch-options/. */
    uint64_t huge = osoe_watch_pack(HEAP, 4096, OSOE_WATCH_LEN_MAX * OSOE_WATCH_GRAIN * 2);
    CHECK(huge != 0);
    CHECK((huge & OSOE_WATCH_LEN_MAX) == OSOE_WATCH_LEN_MAX);
    CHECK(osoe_watch_covers(huge, HEAP, 4096, 8));
    CHECK(osoe_watch_covers(huge, HEAP, UINT64_C(1) << 40, 8));   /* to the end */
    CHECK(!osoe_watch_covers(huge, HEAP, 0, 8));                  /* still knows its start */

    uint64_t far = osoe_watch_pack(HEAP, OSOE_WATCH_MAX_REGION * 2, 8);
    CHECK(far != 0);
    CHECK(osoe_watch_covers(far, HEAP, 0, 8));                    /* the whole region */
    CHECK(osoe_watch_covers(far, HEAP, OSOE_WATCH_MAX_REGION * 2, 8));

    /* The largest offset the encoding names exactly, which is what bounds
     * SHMEM_SYMMETRIC_SIZE. */
    uint64_t edge = osoe_watch_pack(HEAP, OSOE_WATCH_MAX_REGION - OSOE_WATCH_GRAIN, 4);
    CHECK(osoe_watch_covers(edge, HEAP, OSOE_WATCH_MAX_REGION - OSOE_WATCH_GRAIN, 4));
    CHECK(!osoe_watch_covers(edge, HEAP, 0, 8));

    return CHECK_DONE();
}
