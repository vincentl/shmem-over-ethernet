/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The watched range, packed into one word (§4.9, §4.11 item 3).
 *
 * A waiting PE publishes the bytes it is waiting on; every writer that applies
 * a write to that PE's memory loads the word once, after the write, and uses it
 * for one decision: might this write have touched those bytes? It is a
 * conservative filter. Saying yes to a write that touched nothing costs a
 * spurious wake, which the §4.9 loop re-checks and which is legal. Saying no to
 * a write that did touch the range loses a wake, which is not.
 *
 * One word, so it is published and read in one atomic operation and no writer
 * can see a stale half of a range. The layout is §4.11 item 3's `rng`:
 *
 *     bits 63..62  region, so a waiter on OSOE_REGION_INTERNAL (§5.8) or on
 *                  OSOE_REGION_STATICS (§4.3) is not confused with a heap
 *                  waiter at the same offset - every region starts at 0
 *     bits 61..22  the first grain of the range
 *     bits 21..0   the range's length in grains; 0 is no watch, and the maximum
 *                  saturates to "to the end of the region"
 *
 * A word of 0 is therefore "no watch", unambiguously: every real watch has a
 * length of at least one grain.
 *
 * The grain is 4 bytes, which is the width of the narrowest AMO type (spec
 * Table 6's int32_t and uint32_t), so a 4-byte ivar at its natural alignment is
 * filtered exactly and no two AMO-type objects share a grain unless they are
 * adjacent elements of one array. It is a fixed constant of the segment format
 * and deliberately not OSOE_CACHELINE: it is the precision of a filter, not a
 * false-sharing guard, and the encoding must not change with the host.
 *
 * Saturation is always the *permissive* direction - a longer range, never a
 * shorter one, and never "none". Encoding an over-long range as no watch loses
 * wakes, which docs/design/evidence/watch-options/ shows as the `satzero` case.
 *
 * This replaces the smallest-enclosing-power-of-two block of an earlier draft,
 * which had to contain both ends of the range and so inflated an 8 KiB array
 * wait to 122 KiB on average. It also removes that encoding's defect class: a
 * shift count decoded from a packed field, and a loop whose termination depended
 * on its inputs. Every shift here is a constant and there is no loop.
 */
#ifndef OSOE_WATCH_RANGE_H
#define OSOE_WATCH_RANGE_H

#include <stdbool.h>
#include <stdint.h>

#define OSOE_WATCH_GRAIN      UINT64_C(4)
#define OSOE_WATCH_GRAIN_SHIFT 2
#define OSOE_WATCH_LEN_BITS   22
#define OSOE_WATCH_OFF_BITS   40
#define OSOE_WATCH_REGION_BITS 2

#define OSOE_WATCH_LEN_MAX  ((UINT64_C(1) << OSOE_WATCH_LEN_BITS) - 1)
#define OSOE_WATCH_OFF_MAX  ((UINT64_C(1) << OSOE_WATCH_OFF_BITS) - 1)

_Static_assert(OSOE_WATCH_REGION_BITS + OSOE_WATCH_OFF_BITS + OSOE_WATCH_LEN_BITS == 64,
               "the watched range must pack into exactly one 64-bit word");
_Static_assert(OSOE_WATCH_GRAIN == (UINT64_C(1) << OSOE_WATCH_GRAIN_SHIFT),
               "the grain must be a power of two, so the division is a shift");

/*
 * The largest offset the encoding addresses exactly, and so the largest a
 * region may be. A region beyond it still works - a watch in it saturates to
 * the whole region, which only over-wakes - but the filter stops filtering, so
 * osoe_bootstrap_read refuses a SHMEM_SYMMETRIC_SIZE above this instead of silently
 * losing the property this encoding exists for.
 */
#define OSOE_WATCH_MAX_REGION ((OSOE_WATCH_OFF_MAX + 1) * OSOE_WATCH_GRAIN)

/* The range [offset, offset + len) of `region`, rounded out to whole grains.
 * len == 0 is no watch. */
static inline uint64_t osoe_watch_pack(unsigned region, uint64_t offset, uint64_t len)
{
    if (len == 0)
        return 0;
    uint64_t first = offset >> OSOE_WATCH_GRAIN_SHIFT;
    uint64_t last = (offset + len - 1) >> OSOE_WATCH_GRAIN_SHIFT;
    uint64_t grains = last - first + 1;
    uint64_t tag = (uint64_t)region << (OSOE_WATCH_OFF_BITS + OSOE_WATCH_LEN_BITS);

    if (first > OSOE_WATCH_OFF_MAX)     /* cannot name the start: watch everything */
        return tag | OSOE_WATCH_LEN_MAX;
    if (grains >= OSOE_WATCH_LEN_MAX)   /* from `first` to the end of the region */
        return tag | (first << OSOE_WATCH_LEN_BITS) | OSOE_WATCH_LEN_MAX;
    return tag | (first << OSOE_WATCH_LEN_BITS) | grains;
}

/* True if a write of len bytes at (region, offset) may have touched the
 * published range. Conservative: it never returns false for a write that did. */
static inline bool osoe_watch_covers(uint64_t word, unsigned region, uint64_t offset,
                                     uint64_t len)
{
    if (word == 0)
        return false;
    if ((unsigned)(word >> (OSOE_WATCH_OFF_BITS + OSOE_WATCH_LEN_BITS)) != region)
        return false;

    uint64_t wlen = word & OSOE_WATCH_LEN_MAX;
    uint64_t wfirst = (word >> OSOE_WATCH_LEN_BITS) & OSOE_WATCH_OFF_MAX;
    uint64_t wlast = wlen == OSOE_WATCH_LEN_MAX ? UINT64_MAX : wfirst + wlen - 1;

    uint64_t first = offset >> OSOE_WATCH_GRAIN_SHIFT;
    uint64_t last = (offset + (len ? len - 1 : 0)) >> OSOE_WATCH_GRAIN_SHIFT;
    return first <= wlast && wfirst <= last;
}

#endif /* OSOE_WATCH_RANGE_H */
