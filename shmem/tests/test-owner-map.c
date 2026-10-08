/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The map from peer to owner (owner-map.h; the progress helpers proposal's
 * §3.2), and the connection area laid out by it (connection-area.h).
 *
 * The map is arithmetic that the writers and the owners compute apart - a PE
 * to find the bitmap to mark and the owner to wake, an owner to find the peer
 * of its wire - so the two halves must agree exactly: every peer slot on one
 * owner's wire, each owner's wires numbered from 0 without a gap, and the
 * array grouped by owner holding every wire once. Checked exhaustively over
 * the counts a node can have.
 *
 * Then the area, opened in this one process for a node of 8 peers at 0 to 4
 * helpers, and of 299 at 2 and 3 - more than 64 wires an owner, so that a word
 * chosen by the slot rather than the index misses: a writer's mark sets the
 * bit of the wire's index in its owner's bitmap and nowhere else, no two
 * owners' flags share a cache line, nor do two owners' bitmaps start on one,
 * since each is another thread's.
 */
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "bootstrap.h"
#include "check.h"
#include "connection-area.h"
#include "owner-map.h"

/* connection-area.c's error path, replaced so a failure is a test failure. */
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

enum { MOST_PEERS = 300, MOST_HELPERS = 64 };

/* The map at `peers` and `helpers`, both ways and grouped. */
static void check_map(uint32_t peers, uint32_t helpers)
{
    static uint32_t seen[MOST_PEERS];
    uint32_t total = 0;
    for (uint32_t s = 0; s < peers; s++)
        seen[s] = 0;
    for (uint32_t o = 0; o <= helpers; o++) {
        uint32_t count = osoe_owner_wires(o, peers, helpers);
        total += count;
        /* No helpers: owner 0 holds every wire; otherwise it holds none, and
         * the helpers' counts differ by one at most. */
        if (helpers == 0)
            CHECK(count == peers);
        else if (o == 0)
            CHECK(count == 0);
        else
            CHECK(count == peers / helpers || count == peers / helpers + 1);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = osoe_owner_slot(o, i, helpers);
            CHECK(s < peers);
            if (s >= peers)
                continue;
            CHECK(osoe_owner_of(s, helpers) == o);
            CHECK(osoe_owner_index(s, helpers) == i);
            CHECK(osoe_owner_position(s, peers, helpers) ==
                  osoe_owner_first(o, peers, helpers) + i);
            seen[s]++;
        }
        /* Grouped: the owners follow one another without gap or overlap. */
        if (o > 0)
            CHECK(osoe_owner_first(o, peers, helpers) ==
                  osoe_owner_first(o - 1, peers, helpers) +
                      osoe_owner_wires(o - 1, peers, helpers));
    }
    CHECK(total == peers);
    for (uint32_t s = 0; s < peers; s++)
        CHECK(seen[s] == 1);
}

static uintptr_t line_of(const volatile void *p)
{
    return (uintptr_t)p / 64;
}

/* The area of a node of `nodes` - 1 peers at `helpers` helpers, in this
 * process: every mark lands in its owner's bitmap and only there. */
static void check_area(uint32_t nodes, uint32_t helpers)
{
    struct osoe_bootstrap b = {
        .job = (uint64_t)getpid(), .step = 7, .node = 4, .local_id = 0, .local_count = 1,
        .no_progress_ns = UINT64_C(30000000000),
    };
    struct osoe_connection_area area;
    osoe_connection_area_open(&area, &b, nodes, helpers, nodes * 8 + helpers,
                              OSOE_SHM_ANY_CREATOR);
    CHECK(area.helpers == helpers);
    uint32_t peers = nodes - 1;
    for (uint32_t o = 0; o <= helpers; o++) {
        CHECK(area.owner[o].dirty_words ==
              (osoe_owner_wires(o, peers, helpers) + 63) / 64);
        for (uint32_t q = 0; q < o; q++) {
            CHECK(line_of(area.owner[o].sleeping) != line_of(area.owner[q].sleeping));
            if (area.owner[o].dirty_words && area.owner[q].dirty_words)
                CHECK(line_of(area.owner[o].dirty) != line_of(area.owner[q].dirty));
        }
    }
    for (uint32_t peer = 0; peer < nodes; peer++) {
        if (peer == b.node)
            continue;
        uint32_t slot = peer < b.node ? peer : peer - 1;
        osoe_connection_area_mark(&area, peer);
        for (uint32_t o = 0; o <= helpers; o++)
            for (uint32_t w = 0; w < area.owner[o].dirty_words; w++) {
                uint64_t bits = atomic_exchange(&area.owner[o].dirty[w], 0);
                uint32_t index = osoe_owner_index(slot, helpers);
                bool here = o == osoe_owner_of(slot, helpers) && w == index / 64;
                CHECK(bits == (here ? UINT64_C(1) << (index % 64) : 0));
            }
    }
    osoe_connection_area_close(&area);
}

int main(void)
{
    for (uint32_t peers = 0; peers <= MOST_PEERS; peers++)
        for (uint32_t helpers = 0; helpers <= MOST_HELPERS; helpers++)
            check_map(peers, helpers);
    for (uint32_t helpers = 0; helpers <= 4; helpers++)
        check_area(9, helpers);
    check_area(300, 2);
    check_area(300, 3);
    if (CHECK_DONE())
        return 1;
    printf("owner map: ok\n");
    return 0;
}
