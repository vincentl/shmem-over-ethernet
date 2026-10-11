/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The ThreadSanitizer gate's map of the internal region (internal-region.h;
 * OsoeThreadSanitizer.cmake): what place of the region an offset into it is,
 * by the library's own layout functions and defaults, so that the races the
 * gate explains there are named by the places the library lays out (round
 * eight's Q2-2). A team number's small rows and words are told apart to the
 * word - a child's lane word and a grant's words are the places whose races
 * are explained by place - and everything past them only by its part of the
 * region.
 *
 *   thread-sanitizer-place FANOUT MAX_TEAMS SMALL_BYTES OFFSET...
 *
 * Each setting 0 for the library's default; one line for each offset (hex),
 * the place's name.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bootstrap.h"
#include "internal-region.h"
#include "internode-backend.h"

static const char *place(uint32_t fanout, uint32_t max_teams, uint32_t small, uint64_t offset)
{
    uint64_t team_small = osoe_internal_team_small_bytes(fanout, small);
    uint64_t row_bytes = osoe_internal_small_row_bytes(small);
    uint64_t words = osoe_internal_words_offset(fanout, max_teams, small);
    if (offset < words) {
        uint64_t row = offset % team_small / row_bytes, in = offset % team_small % row_bytes;
        if (in >= OSOE_INTERNAL_LANE_WORD_BYTES)
            return "small-slot";
        return row < fanout ? "lane-word" : "release-row-lane-word";
    }
    uint64_t team_words = osoe_internal_team_words_bytes(fanout);
    if (offset >= words + (uint64_t)max_teams * team_words)
        return "past-the-team-words";
    uint64_t in = (offset - words) % team_words;
    uint64_t line = in / OSOE_INTERNAL_LINE_BYTES, at = in % OSOE_INTERNAL_LINE_BYTES;
    if (line < fanout)
        return "arrival-counter";
    if (line == fanout)
        return at < 8 ? "release-counter" : "release-line-spare-words";
    if (line == fanout + 1)
        return at < 8 ? "grant-counter" : at < 8 + 24 ? "grant-words" : "grant-line";
    return "split-words";
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s FANOUT MAX_TEAMS SMALL_BYTES OFFSET...\n", argv[0]);
        return 2;
    }
    uint32_t fanout = (uint32_t)strtoul(argv[1], NULL, 0);
    uint32_t max_teams = (uint32_t)strtoul(argv[2], NULL, 0);
    uint32_t small = (uint32_t)strtoul(argv[3], NULL, 0);
    if (fanout == 0)
        fanout = OSOE_INTERNODE_FANOUT_DEFAULT;
    if (max_teams == 0)
        max_teams = OSOE_MAX_TEAMS_DEFAULT;
    if (small == 0)
        small = OSOE_SMALL_COLLECTIVE_BYTES_DEFAULT;
    for (int i = 4; i < argc; i++)
        printf("%s\n", place(fanout, max_teams, small, strtoull(argv[i], NULL, 16)));
    return 0;
}
