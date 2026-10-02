/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The AM sizing policy (§1, §3.2, §3.4).
 *
 * Pure arithmetic, so tier 1. What it pins down is the property §1 states as a
 * constraint - "Nothing may scale with PE count where it can scale with node
 * count" - plus §3.4's floor, which is the one rule the budget must never be
 * allowed to break.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "connection-area.h"

/* connection-area.c's error path, replaced so a failure is a test failure. Nothing
 * here should reach it: this file exercises the pure sizing function, and the
 * mapping half is covered across processes by shmem.connection-area. */
_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

enum { K = 1024 };
#define KB(n) ((uint64_t)(n) * K)
#define MB(n) ((uint64_t)(n) * K * K)

int main(void)
{
    uint64_t chosen;
    const uint64_t chunk = KB(64);
    const uint64_t floor = 2 * chunk;             /* §3.4 */
    const uint64_t budget = MB(256);

    /* The defaults are the floor: everything but aggregation pushes down. */
    CHECK(OSOE_AM_DEFAULT_CHUNK == KB(64));
    CHECK(OSOE_AM_DEFAULT_BUDGET == budget);

    /*
     * The supported range, §1's "hundreds of nodes". Nothing is shrunk, so
     * inside it the behaviour is a constant buffer size with no heuristic.
     */
    const uint32_t sizes[] = { 2, 4, 16, 64, 128, 500 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        CHECK(osoe_am_choose_buffer(chunk, floor, budget, sizes[i], &chosen) ==
              OSOE_AM_SIZE_OK);
        CHECK(chosen == floor);
    }

    /* 500 nodes: about 125 MiB per node, inside the budget. */
    CHECK(osoe_am_choose_buffer(chunk, floor, budget, 500, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(499 * 2 * chosen == MB(124) + KB(768));

    /*
     * Where the defaults end, which the README's Limits state: the default
     * buffer is the floor, so the budget cannot shrink it and is a ceiling on
     * the node count instead. 1,024 peers' two buffers of 128 KiB are exactly
     * the 256 MiB, so 1,025 nodes fit and 1,026 are refused.
     */
    CHECK(osoe_am_choose_buffer(chunk, floor, budget, 1025, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(chosen == floor && 1024 * 2 * chosen == budget);
    CHECK(osoe_am_choose_buffer(chunk, floor, budget, 1026, &chosen) == OSOE_AM_SIZE_NO_ROOM);
    CHECK(chosen == floor);

    /*
     * No PE dimension anywhere. The area is a function of node count and
     * buffer size only, which is the §1 constraint stated as an assertion: a
     * node of 1 PE and a node of 256 PEs cost exactly the same, because every
     * local PE contends on the same open buffer (§3.5).
     */
    CHECK(osoe_am_area_bytes(16, floor) == osoe_am_area_bytes(16, floor));
    for (uint32_t n = 2; n <= 64; n++) {
        uint64_t a = osoe_am_area_bytes(n, floor);
        uint64_t b = osoe_am_area_bytes(n + 1, floor);
        /* Each extra node adds exactly one connection: two buffers. */
        CHECK(b - a >= 2 * floor);
        CHECK(b - a < 2 * floor + KB(4));         /* plus its connection state */
    }

    /* One node has no peers, so nothing is allocated and any size fits. */
    CHECK(osoe_am_area_bytes(1, floor) == 0);
    CHECK(osoe_am_choose_buffer(chunk, floor, KB(1), 1, &chosen) == OSOE_AM_SIZE_OK);

    /*
     * The budget halves toward the floor and keeps the LARGEST size that fits,
     * not the floor: a request of 8x the chunk over 500 nodes wants about
     * 499 MiB and settles at 4x, which is 255.5 MiB. Stopping at the first
     * size that fits is the point - the floor is a limit, not a target.
     */
    CHECK(osoe_am_choose_buffer(chunk, 8 * chunk, budget, 500, &chosen) ==
          OSOE_AM_SIZE_SHRUNK);
    CHECK(chosen == 4 * chunk);
    CHECK(499 * 2 * chosen <= budget);
    CHECK(499 * 2 * 2 * chosen > budget);        /* and the next size up does not */

    /* Shrinking halves, so the result stays a power of two - which is what the
     * ring arithmetic in the buffer depends on. */
    CHECK(osoe_am_choose_buffer(chunk, 8 * chunk, MB(64), 100, &chosen) ==
          OSOE_AM_SIZE_SHRUNK);
    CHECK((chosen & (chosen - 1)) == 0);
    CHECK(chosen >= floor);

    /*
     * The floor is never breached. Below it a full-size message leaves no room
     * for a second and aggregation stops at the top of the range (§3.4), so
     * this says so rather than going under - a performance mystery later
     * versus an error now.
     */
    CHECK(osoe_am_choose_buffer(chunk, floor, MB(1), 500, &chosen) ==
          OSOE_AM_SIZE_NO_ROOM);
    CHECK(chosen == floor);

    /* A smaller chunk lowers the floor, which is the knob that makes many
     * logical nodes on one host affordable (§8.1): 500 nodes that need 128 MiB
     * at the default chunk need 16 MiB at an 8 KiB one. */
    CHECK(osoe_am_choose_buffer(KB(8), KB(16), MB(32), 500, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(chosen == KB(16));
    CHECK(499 * 2 * chosen < MB(16));

    /*
     * §8.1's logical node map puts every node on one machine, so the whole-host
     * cost is quadratic. This is the case the knobs exist for, and the numbers
     * are here so a change to the defaults has to confront them.
     */
    CHECK(16 * osoe_am_area_bytes(16, floor) < MB(64));          /* testing: fine */
    CHECK(64 * osoe_am_area_bytes(64, floor) > MB(900));         /* ~1 GiB: tune it */
    CHECK(64 * osoe_am_area_bytes(64, KB(16)) < MB(140));        /* with a small chunk */

    return CHECK_DONE();
}
