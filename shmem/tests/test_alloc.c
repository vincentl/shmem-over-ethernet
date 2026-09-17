/*
 * The allocator is a pure deterministic function of its call sequence (§4.4,
 * invariant 6): two instances fed the same calls return the same offsets.
 * Also: blocks never overlap, stay aligned and within capacity, coalesce back
 * to one free block, and exhaustion fails cleanly.
 */
#include <stdint.h>
#include <stdlib.h>

#include "alloc.h"
#include "check.h"

enum { OPS = 20000, LIVE_MAX = 256 };

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 33;
}

static void check_invariants(const struct osoe_alloc *a)
{
    uint64_t next = 0;
    for (size_t i = 0; i < a->count; i++) {
        CHECK(a->blocks[i].offset == next);             /* contiguous, ordered */
        CHECK(a->blocks[i].offset % OSOE_ALLOC_ALIGN == 0);
        CHECK(a->blocks[i].size > 0 && a->blocks[i].size % OSOE_ALLOC_ALIGN == 0);
        if (i > 0)
            CHECK(!(a->blocks[i].free && a->blocks[i - 1].free));   /* coalesced */
        next += a->blocks[i].size;
    }
    CHECK(next == a->capacity);
}

int main(void)
{
    const uint64_t capacity = UINT64_C(1) << 24;
    struct osoe_alloc x, y;
    CHECK(osoe_alloc_init(&x, capacity) == 0);
    CHECK(osoe_alloc_init(&y, capacity) == 0);

    uint64_t live[LIVE_MAX];
    size_t nlive = 0;
    uint64_t seed = 42;
    size_t failures = 0;

    for (int op = 0; op < OPS; op++) {
        bool do_free = nlive == LIVE_MAX || (nlive > 0 && lcg(&seed) % 2);
        if (do_free) {
            size_t k = (size_t)(lcg(&seed) % nlive);    /* out of order */
            CHECK(osoe_alloc_give(&x, live[k]) == 0);
            CHECK(osoe_alloc_give(&y, live[k]) == 0);
            live[k] = live[--nlive];
        } else {
            uint64_t size = 1 + lcg(&seed) % (lcg(&seed) % 4 ? 4096 : 1u << 20);
            uint64_t ox = UINT64_MAX, oy = UINT64_MAX;
            int rx = osoe_alloc_take(&x, size, &ox);
            int ry = osoe_alloc_take(&y, size, &oy);
            CHECK(rx == ry);
            if (rx == 0) {
                CHECK(ox == oy);
                CHECK(ox + size <= capacity);
                live[nlive++] = ox;
            } else {
                CHECK(rx == 1);
                failures++;
            }
        }
        if (op % 1000 == 0) {
            check_invariants(&x);
            CHECK(x.count == y.count);
        }
    }
    CHECK(failures > 0);                /* exhaustion was exercised */

    /* Freeing a non-block, twice, or a mid-block offset fails. */
    uint64_t o;
    CHECK(osoe_alloc_take(&x, 100, &o) == 0);
    CHECK(osoe_alloc_give(&x, o + 16) == -1);
    CHECK(osoe_alloc_give(&x, o) == 0);
    CHECK(osoe_alloc_give(&x, o) == -1);

    /* Everything freed coalesces into one block of the whole capacity. */
    while (nlive > 0)
        CHECK(osoe_alloc_give(&x, live[--nlive]) == 0);
    check_invariants(&x);
    CHECK(x.count == 1 && x.blocks[0].free);
    CHECK(osoe_alloc_take(&x, capacity, &o) == 0 && o == 0);
    CHECK(osoe_alloc_take(&x, 1, &o) == 1);
    CHECK(osoe_alloc_take(&x, UINT64_MAX, &o) == 1);

    /* A zero-capacity heap allocates nothing. */
    struct osoe_alloc z;
    CHECK(osoe_alloc_init(&z, 0) == 0);
    CHECK(osoe_alloc_take(&z, 1, &o) == 1);
    CHECK(osoe_alloc_give(&z, 0) == -1);
    osoe_alloc_fini(&z);

    osoe_alloc_fini(&x);
    osoe_alloc_fini(&y);
    return CHECK_DONE();
}
