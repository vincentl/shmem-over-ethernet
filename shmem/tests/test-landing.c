/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The landing zones in the control segment (§3.12, §4.6, layout 6).
 *
 * A get's `dest` may be private memory the lead's process cannot see, so a
 * reply lands here and the origin copies it out. That makes this the one piece
 * of get completion that every backend needs, which is why it lives in the
 * control segment and not in the AM connection area.
 *
 * What a test can check before the backend exists is that the zones are really
 * there, that each PE has its own, that they do not overlap, and that a slot
 * holds a whole AM chunk - a landing zone smaller than the wire's largest
 * message makes that message undeliverable, which is a hang rather than a short
 * read. It reads them through osoe-stat's segment reader, as every tier-2 test
 * reads the control segment: a check that used the library's own headers would
 * only prove the library agrees with itself.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

int main(void)
{
    shmem_init();
    int n = shmem_n_pes();

    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK(false);
        shmem_finalize();
        return CHECK_DONE();
    }

    /* Layout 6 or later, and the fields actually present - asked for one at a
     * time, which is what makes the append-only rule work in both directions. */
    CHECK(s.dir->layout >= 6);
    CHECK(OSOE_SEGMENT_HAS(s.dir, landing_count));
    CHECK(OSOE_SEGMENT_HAS(s.dir, landing_slot_bytes));

    /* One slot per local PE. On one node that is every PE. */
    CHECK(s.dir->landing_count == (uint32_t)n);
    CHECK(s.dir->landing_slot_bytes > 0);
    CHECK(s.dir->landing_bytes == (uint64_t)s.dir->landing_count *
                                  s.dir->landing_slot_bytes);

    /* Inside the segment, and after the counter blocks: a section that
     * overlapped another would corrupt both and is exactly what an arithmetic
     * slip in the layout produces. */
    CHECK(s.dir->landing_offset >= s.dir->blocks_offset + s.dir->blocks_bytes);
    CHECK(s.dir->landing_offset + s.dir->landing_bytes <= s.dir->segment_bytes);

    /*
     * A slot must hold a whole AM chunk beside its header. The default chunk is
     * 64 KiB (§3.2); whatever OSOE_AM_CHUNK says, the slot has to fit it, and
     * the test derives the expectation from the environment rather than from a
     * constant so that changing the chunk cannot quietly invalidate it.
     */
    const char *chunk_env = getenv("OSOE_AM_CHUNK");
    uint64_t chunk = chunk_env ? strtoull(chunk_env, NULL, 10) : (UINT64_C(64) << 10);
    CHECK(s.dir->landing_slot_bytes > chunk);

    /*
     * The slots do not overlap, and each lies inside the segment. This is the
     * arithmetic a layout slip gets wrong, and getting it wrong would deliver
     * one PE's reply into another's zone - a wrong answer rather than a crash,
     * which is the kind that survives testing.
     */
    for (uint32_t i = 0; i < s.dir->landing_count; i++) {
        uint64_t begin = s.dir->landing_offset + (uint64_t)i * s.dir->landing_slot_bytes;
        uint64_t end = begin + s.dir->landing_slot_bytes;
        CHECK(begin >= s.dir->landing_offset);
        CHECK(end <= s.dir->landing_offset + s.dir->landing_bytes);
        CHECK(end <= s.dir->segment_bytes);
        for (uint32_t j = 0; j < i; j++) {
            uint64_t other = s.dir->landing_offset +
                             (uint64_t)j * s.dir->landing_slot_bytes;
            CHECK(begin >= other + s.dir->landing_slot_bytes || begin + s.dir->landing_slot_bytes <= other);
        }
    }

    /* The counter blocks end before the landing zones begin: two sections that
     * overlapped would corrupt each other, and both are addressed by offsets
     * computed in the same function. */
    CHECK(s.dir->blocks_offset + s.dir->blocks_bytes <= s.dir->landing_offset);

    osoe_segment_close(&s);
    shmem_barrier_all();

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
