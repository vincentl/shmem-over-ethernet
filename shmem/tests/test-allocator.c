/*
 * The allocator is a pure deterministic function of its call sequence (§4.4,
 * invariant 6): two instances fed the same calls return the same offsets.
 * Also: blocks never overlap, stay aligned and within capacity, coalesce back
 * to one free block, and exhaustion fails cleanly.
 */
#include <stdint.h>
#include <stdlib.h>

#include "allocator.h"
#include "check.h"

enum { OPS = 20000, LIVE_MAX = 256 };

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 33;
}

static void check_invariants(const struct osoe_allocator *a)
{
    uint64_t next = 0;
    for (size_t i = 0; i < a->count; i++) {
        CHECK(a->blocks[i].offset == next);             /* contiguous, ordered */
        CHECK(a->blocks[i].offset % OSOE_ALLOCATOR_ALIGN == 0);
        CHECK(a->blocks[i].size > 0 && a->blocks[i].size % OSOE_ALLOCATOR_ALIGN == 0);
        if (i > 0)
            CHECK(!(a->blocks[i].free && a->blocks[i - 1].free));   /* coalesced */
        next += a->blocks[i].size;
    }
    CHECK(next == a->capacity);
}

int main(void)
{
    const uint64_t capacity = UINT64_C(1) << 24;
    struct osoe_allocator x, y;
    CHECK(osoe_allocator_init(&x, capacity) == 0);
    CHECK(osoe_allocator_init(&y, capacity) == 0);

    uint64_t live[LIVE_MAX];
    size_t nlive = 0;
    uint64_t seed = 42;
    size_t failures = 0;

    for (int op = 0; op < OPS; op++) {
        bool do_free = nlive == LIVE_MAX || (nlive > 0 && lcg(&seed) % 2);
        if (do_free) {
            size_t k = (size_t)(lcg(&seed) % nlive);    /* out of order */
            CHECK(osoe_allocator_give(&x, live[k]) == 0);
            CHECK(osoe_allocator_give(&y, live[k]) == 0);
            live[k] = live[--nlive];
        } else {
            uint64_t size = 1 + lcg(&seed) % (lcg(&seed) % 4 ? 4096 : 1u << 20);
            uint64_t ox = UINT64_MAX, oy = UINT64_MAX;
            int rx = osoe_allocator_take(&x, size, OSOE_ALLOCATOR_ALIGN, 0, &ox);
            int ry = osoe_allocator_take(&y, size, OSOE_ALLOCATOR_ALIGN, 0, &oy);
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
    CHECK(osoe_allocator_take(&x, 100, OSOE_ALLOCATOR_ALIGN, 0, &o) == 0);
    CHECK(osoe_allocator_give(&x, o + 16) == -1);
    CHECK(osoe_allocator_give(&x, o) == 0);
    CHECK(osoe_allocator_give(&x, o) == -1);

    /* Everything freed coalesces into one block of the whole capacity. */
    while (nlive > 0)
        CHECK(osoe_allocator_give(&x, live[--nlive]) == 0);
    check_invariants(&x);
    CHECK(x.count == 1 && x.blocks[0].free);
    CHECK(osoe_allocator_take(&x, capacity, OSOE_ALLOCATOR_ALIGN, 0, &o) == 0 && o == 0);
    CHECK(osoe_allocator_take(&x, 1, OSOE_ALLOCATOR_ALIGN, 0, &o) == 1);
    CHECK(osoe_allocator_take(&x, UINT64_MAX, OSOE_ALLOCATOR_ALIGN, 0, &o) == 1);

    /*
     * Alignment (spec §9.3.4). The padding below an aligned block becomes a
     * free block, so the list stays contiguous and every invariant above holds.
     */
    while (x.count > 1 || !x.blocks[0].free) {
        for (size_t i = 0; i < x.count; i++)
            if (!x.blocks[i].free) {
                CHECK(osoe_allocator_give(&x, x.blocks[i].offset) == 0);
                break;
            }
    }
    CHECK(osoe_allocator_take(&x, 8, OSOE_ALLOCATOR_ALIGN, 0, &o) == 0 && o == 0);
    uint64_t aligned;
    CHECK(osoe_allocator_take(&x, 64, 4096, 0, &aligned) == 0);
    CHECK(aligned % 4096 == 0);
    CHECK(aligned >= 4096);             /* the 8 bytes at 0 pushed it up */
    check_invariants(&x);
    CHECK(osoe_allocator_give(&x, aligned) == 0);
    CHECK(osoe_allocator_give(&x, 0) == 0);
    check_invariants(&x);
    CHECK(x.count == 1);

    /*
     * An alignment no position but offset 0 can satisfy. Offset 0 is a multiple
     * of every alignment, so the request has to be made with 0 already taken
     * for the failure to be about alignment rather than about space.
     */
    CHECK(osoe_allocator_take(&x, 16, OSOE_ALLOCATOR_ALIGN, 0, &o) == 0 && o == 0);
    CHECK(osoe_allocator_take(&x, 16, capacity, 0, &aligned) == 1);
    CHECK(osoe_allocator_give(&x, o) == 0);
    check_invariants(&x);

    /*
     * Hints (§4.4): a hinted allocation is placed from the high end and an
     * unhinted one from the low end, which is what makes "the hints cannot be
     * preserved" a case that can happen at all.
     */
    uint64_t plain, hinted;
    CHECK(osoe_allocator_take(&x, 1000, OSOE_ALLOCATOR_ALIGN, 0, &plain) == 0);
    CHECK(plain == 0);
    CHECK(osoe_allocator_take(&x, 1000, OSOE_ALLOCATOR_ALIGN, 1, &hinted) == 0);
    CHECK(hinted == capacity - 1008);   /* 1000 rounded up to the 16-byte grain */
    check_invariants(&x);

    uint64_t size_out = 0;
    long hints_out = -1;
    CHECK(osoe_allocator_query(&x, hinted, &size_out, &hints_out) == 0);
    CHECK(size_out == 1008 && hints_out == 1);
    CHECK(osoe_allocator_query(&x, plain, &size_out, &hints_out) == 0 && hints_out == 0);
    CHECK(osoe_allocator_query(&x, hinted + 16, &size_out, &hints_out) == -1);

    /* Freeing clears the hint, so the space is available to anyone. */
    CHECK(osoe_allocator_give(&x, hinted) == 0);
    CHECK(osoe_allocator_query(&x, hinted, &size_out, &hints_out) == -1);

    /*
     * Resizing in place. Shrinking always works; growing works only into a free
     * neighbour above, which is exactly the case that lets a program grow one
     * array to the whole heap instead of half of it.
     */
    CHECK(osoe_allocator_resize(&x, plain, 100) == 0);
    CHECK(osoe_allocator_query(&x, plain, &size_out, &hints_out) == 0 && size_out == 112);
    check_invariants(&x);
    CHECK(osoe_allocator_resize(&x, plain, capacity) == 0);        /* nothing above it */
    CHECK(osoe_allocator_query(&x, plain, &size_out, &hints_out) == 0 && size_out == capacity);
    CHECK(x.count == 1);
    CHECK(osoe_allocator_resize(&x, plain, 4096) == 0);
    check_invariants(&x);

    /* Blocked above: a second allocation sits immediately after. */
    uint64_t blocker;
    CHECK(osoe_allocator_take(&x, 16, OSOE_ALLOCATOR_ALIGN, 0, &blocker) == 0);
    CHECK(blocker == 4096);
    CHECK(osoe_allocator_resize(&x, plain, 8192) == 1);            /* cannot, without moving */
    CHECK(osoe_allocator_query(&x, plain, &size_out, &hints_out) == 0 && size_out == 4096);
    CHECK(osoe_allocator_resize(&x, blocker + 16, 32) == -1);      /* not a block */
    CHECK(osoe_allocator_give(&x, blocker) == 0);
    CHECK(osoe_allocator_give(&x, plain) == 0);
    check_invariants(&x);

    /* Mixed hints and alignments are still a pure function of the sequence. */
    {
        struct osoe_allocator p, q;
        CHECK(osoe_allocator_init(&p, capacity) == 0);
        CHECK(osoe_allocator_init(&q, capacity) == 0);
        uint64_t seed2 = 7;
        uint64_t held[64];
        size_t nheld = 0;
        for (int i = 0; i < 4000; i++) {
            if (nheld == 64 || (nheld > 0 && lcg(&seed2) % 3 == 0)) {
                size_t k = (size_t)(lcg(&seed2) % nheld);
                CHECK(osoe_allocator_give(&p, held[k]) == 0);
                CHECK(osoe_allocator_give(&q, held[k]) == 0);
                held[k] = held[--nheld];
            } else {
                uint64_t sz = 1 + lcg(&seed2) % 8192;
                uint64_t al = UINT64_C(16) << (lcg(&seed2) % 6);
                long h = (long)(lcg(&seed2) % 3);
                uint64_t op = UINT64_MAX, oq = UINT64_MAX;
                int rp = osoe_allocator_take(&p, sz, al, h, &op);
                int rq = osoe_allocator_take(&q, sz, al, h, &oq);
                CHECK(rp == rq);
                if (rp == 0) {
                    CHECK(op == oq);
                    CHECK(op % al == 0);
                    held[nheld++] = op;
                }
            }
            if (nheld > 0 && lcg(&seed2) % 5 == 0) {
                uint64_t sz = 1 + lcg(&seed2) % 8192;
                CHECK(osoe_allocator_resize(&p, held[0], sz) ==
                      osoe_allocator_resize(&q, held[0], sz));
            }
        }
        check_invariants(&p);
        CHECK(p.count == q.count);
        for (size_t i = 0; i < p.count && i < q.count; i++) {
            CHECK(p.blocks[i].offset == q.blocks[i].offset);
            CHECK(p.blocks[i].size == q.blocks[i].size);
            CHECK(p.blocks[i].free == q.blocks[i].free);
            CHECK(p.blocks[i].hints == q.blocks[i].hints);
        }
        osoe_allocator_fini(&p);
        osoe_allocator_fini(&q);
    }

    /* A zero-capacity heap allocates nothing. */
    struct osoe_allocator z;
    CHECK(osoe_allocator_init(&z, 0) == 0);
    CHECK(osoe_allocator_take(&z, 1, OSOE_ALLOCATOR_ALIGN, 0, &o) == 1);
    CHECK(osoe_allocator_give(&z, 0) == -1);
    osoe_allocator_fini(&z);

    osoe_allocator_fini(&x);
    osoe_allocator_fini(&y);
    return CHECK_DONE();
}
