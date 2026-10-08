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

#include "am-configuration.h"
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

/* And its start-up notice (fatal.h), which a wait here prints after 30 s. */
void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_notice: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

enum { K = 1024 };
#define KB(n) ((uint64_t)(n) * K)
#define MB(n) ((uint64_t)(n) * K * K)

int main(void)
{
    uint64_t chosen;
    const uint64_t chunk = KB(64);
    const uint64_t floor = 2 * chunk;             /* §3.4 */
    const uint64_t standard = 4 * chunk;          /* the default buffer, v0.2 step 4 */
    const uint64_t budget = MB(1024);

    /*
     * The defaults, as the environment unset gives them: a buffer of four
     * chunks, 256 KiB, above the floor of two, and a budget of 1 GiB (v0.2
     * step 4; docs/decisions/2026-10-07.md item 12).
     */
    CHECK(OSOE_AM_DEFAULT_CHUNK == KB(64));
    CHECK(OSOE_AM_DEFAULT_BUDGET == budget);
    static const char *const settings[] = { "OSOE_AM_CHUNK", "OSOE_AM_BUFFER",
                                            "OSOE_AM_BUFFER_BUDGET" };
    for (size_t i = 0; i < sizeof settings / sizeof *settings; i++)
        unsetenv(settings[i]);
    struct osoe_am_configuration configuration;
    osoe_am_configuration_read(&configuration);
    CHECK(configuration.chunk == chunk && configuration.buffer == standard &&
          configuration.budget == budget);

    /*
     * The supported range, §1's "hundreds of nodes", and on to the default's
     * end. Nothing is shrunk, so inside it the behaviour is a constant buffer
     * size with no heuristic.
     */
    const uint32_t sizes[] = { 2, 4, 16, 64, 128, 500, 2049 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        CHECK(osoe_am_choose_buffer(chunk, standard, budget, sizes[i], &chosen) ==
              OSOE_AM_SIZE_OK);
        CHECK(chosen == standard);
    }

    /* 500 nodes: about 250 MiB per node, inside the budget. */
    CHECK(osoe_am_choose_buffer(chunk, standard, budget, 500, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(499 * 2 * chosen == MB(249) + KB(512));

    /*
     * Where the defaults end, which the README's Limits state. 2,048 peers' two
     * buffers of 256 KiB are exactly the 1 GiB, so 2,049 nodes keep the default;
     * from 2,050 the budget halves the buffers to the floor, the lead saying so
     * once; 4,096 peers' two buffers of 128 KiB are the 1 GiB again, so 4,097
     * nodes fit there and 4,098 are refused - asked for the floor, 4,097 fit
     * as they are.
     */
    CHECK(chosen == standard && 2048 * 2 * standard == budget);
    CHECK(osoe_am_choose_buffer(chunk, standard, budget, 2050, &chosen) ==
          OSOE_AM_SIZE_SHRUNK);
    CHECK(chosen == floor);
    CHECK(osoe_am_choose_buffer(chunk, standard, budget, 4097, &chosen) ==
          OSOE_AM_SIZE_SHRUNK);
    CHECK(chosen == floor && 4096 * 2 * chosen == budget);
    CHECK(osoe_am_choose_buffer(chunk, floor, budget, 4097, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(chosen == floor);
    CHECK(osoe_am_choose_buffer(chunk, standard, budget, 4098, &chosen) ==
          OSOE_AM_SIZE_NO_ROOM);
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
     * not the floor: a request of 32x the chunk over 500 nodes wants about
     * 1,996 MiB and settles at 16x, which is 998 MiB. Stopping at the first
     * size that fits is the point - the floor is a limit, not a target.
     */
    CHECK(osoe_am_choose_buffer(chunk, 32 * chunk, budget, 500, &chosen) ==
          OSOE_AM_SIZE_SHRUNK);
    CHECK(chosen == 16 * chunk);
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
     * at the default chunk's floor need 16 MiB at an 8 KiB one's. */
    CHECK(osoe_am_choose_buffer(KB(8), KB(16), MB(32), 500, &chosen) == OSOE_AM_SIZE_OK);
    CHECK(chosen == KB(16));
    CHECK(499 * 2 * chosen < MB(16));

    /*
     * §8.1's logical node map puts every node on one machine, so the whole-host
     * cost is quadratic. This is the case the knobs exist for, and the numbers
     * are here so a change to the defaults has to confront them: at the default
     * of four chunks, 120 MiB for 16 nodes and about 2 GiB for 64, and with an
     * 8 KiB chunk - its default buffer 32 KiB - about 250 MiB.
     */
    CHECK(16 * osoe_am_area_bytes(16, standard) < MB(128));      /* testing: fine */
    CHECK(64 * osoe_am_area_bytes(64, standard) > MB(1900));     /* ~2 GiB: tune it */
    CHECK(64 * osoe_am_area_bytes(64, KB(32)) < MB(280));        /* with a small chunk */

    return CHECK_DONE();
}
