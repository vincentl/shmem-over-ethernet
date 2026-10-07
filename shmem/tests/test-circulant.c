/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Träff's circulant rounds on their own (tier 1; v0.2 step 3,
 * collective-circulant.h).
 *
 *   test-circulant FILE
 *
 * For every node count from 2 to 70, at the nine lengths the model uses (1, 2,
 * 3, N - 1, N, N + 1, 2N + 3, 64 and 1000 elements): run the rounds the C
 * gives over symbolic vectors - each element the set of nodes combined into
 * it, a round reading what each lead held after the round before - and
 * require that every lead receives exactly what its sender sends it, that no
 * element ever takes a node twice, that every element of every lead's result
 * holds every node, and that every lead sends N - 1 blocks' worth in each half
 * - no more than ceil(count / N) elements a block. Then require the C's whole
 * table, digested, to be what tools/modelcheck/circulant_model.py wrote into
 * FILE, where the same arithmetic is checked against the rounds written out
 * element by element.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "collective-circulant.h"

/* The nodes combined into one element: up to 128. */
struct set {
    uint64_t word[2];
};

static uint64_t interval_elements(const uint64_t first[2], const uint64_t end[2])
{
    return (end[0] - first[0]) + (end[1] - first[1]);
}

static int same_intervals(const uint64_t af[2], const uint64_t ae[2], const uint64_t bf[2],
                          const uint64_t be[2])
{
    return af[0] == bf[0] && ae[0] == be[0] && af[1] == bf[1] && ae[1] == be[1];
}

static int combine_check(uint32_t nodes, uint64_t count)
{
    struct set *have = calloc((size_t)nodes * count, sizeof *have);
    struct set *sent = calloc((size_t)nodes * count, sizeof *sent);
    uint64_t *out = calloc(nodes, sizeof *out);
    CHECK_FATAL(have && sent && out);
    for (uint32_t lead = 0; lead < nodes; lead++)
        for (uint64_t e = 0; e < count; e++)
            have[lead * count + e].word[lead / 64] |= (uint64_t)1 << (lead % 64);
    int bad = 0;
    uint32_t rounds = osoe_circulant_rounds(nodes), rows = osoe_circulant_rows(nodes);
    for (uint32_t round = 0; round < rounds && !bad; round++) {
        if (round == rows)
            memset(out, 0, nodes * sizeof *out);
        /* what each lead sends this round, read before anything is delivered */
        for (uint32_t lead = 0; lead < nodes; lead++) {
            struct osoe_circulant_round r = osoe_circulant_round(lead, nodes, round, count);
            for (int i = 0; i < 2; i++)
                for (uint64_t e = r.send_first[i]; e < r.send_end[i]; e++)
                    sent[lead * count + e] = have[lead * count + e];
            out[lead] += interval_elements(r.send_first, r.send_end);
        }
        for (uint32_t lead = 0; lead < nodes && !bad; lead++) {
            struct osoe_circulant_round r = osoe_circulant_round(lead, nodes, round, count);
            struct osoe_circulant_round from = osoe_circulant_round(r.from, nodes, round, count);
            if (from.to != lead || from.row != r.row || from.combines != r.combines ||
                !same_intervals(from.send_first, from.send_end, r.receive_first,
                                r.receive_end)) {
                fprintf(stderr, "N %u, %" PRIu64 " elements, round %u: lead %u receives what "
                        "lead %u does not send it\n", nodes, count, round, lead, r.from);
                bad = 1;
                break;
            }
            for (int i = 0; i < 2; i++)
                for (uint64_t e = r.receive_first[i]; e < r.receive_end[i]; e++) {
                    struct set *mine = &have[lead * count + e];
                    const struct set *theirs = &sent[r.from * count + e];
                    if (r.combines) {
                        if ((mine->word[0] & theirs->word[0]) ||
                            (mine->word[1] & theirs->word[1])) {
                            fprintf(stderr, "N %u, %" PRIu64 " elements, round %u: lead %u's "
                                    "element %" PRIu64 " takes a node twice\n", nodes, count,
                                    round, lead, e);
                            bad = 1;
                        }
                        mine->word[0] |= theirs->word[0];
                        mine->word[1] |= theirs->word[1];
                    } else {
                        *mine = *theirs;
                    }
                }
        }
        /* each half, every lead's N - 1 blocks */
        if (!bad && (round == rows - 1 || round == rounds - 1)) {
            uint64_t most = (uint64_t)(nodes - 1) * ((count + nodes - 1) / nodes);
            for (uint32_t lead = 0; lead < nodes; lead++)
                if (out[lead] > most) {
                    fprintf(stderr, "N %u, %" PRIu64 " elements: lead %u sends %" PRIu64
                            " elements in a half, more than N - 1 blocks' %" PRIu64 "\n", nodes,
                            count, lead, out[lead], most);
                    bad = 1;
                }
        }
    }
    struct set full = { { 0, 0 } };
    for (uint32_t lead = 0; lead < nodes; lead++)
        full.word[lead / 64] |= (uint64_t)1 << (lead % 64);
    for (uint32_t lead = 0; lead < nodes && !bad; lead++)
        for (uint64_t e = 0; e < count && !bad; e++)
            if (memcmp(&have[lead * count + e], &full, sizeof full) != 0) {
                fprintf(stderr, "N %u, %" PRIu64 " elements: lead %u's element %" PRIu64
                        " lacks a node\n", nodes, count, lead, e);
                bad = 1;
            }
    free(have);
    free(sent);
    free(out);
    return bad;
}

/* circulant_model.py's digest(): FNV-1a, 64 bits, over every round's every
 * lead's part, each value 8 bytes, least significant first. */
static uint64_t fold_in(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xff;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t digest(uint32_t nodes, uint64_t count)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t round = 0; round < osoe_circulant_rounds(nodes); round++)
        for (uint32_t lead = 0; lead < nodes; lead++) {
            struct osoe_circulant_round r = osoe_circulant_round(lead, nodes, round, count);
            const uint64_t values[12] = { r.to, r.from, r.row, r.combines,
                                          r.send_first[0], r.send_end[0],
                                          r.send_first[1], r.send_end[1],
                                          r.receive_first[0], r.receive_end[0],
                                          r.receive_first[1], r.receive_end[1] };
            for (int i = 0; i < 12; i++)
                h = fold_in(h, values[i]);
        }
    return h;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    CHECK_FATAL(f != NULL);
    char line[256];
    unsigned digests = 0, differ = 0, failed = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        unsigned nodes, rounds;
        uint64_t count, expected;
        if (sscanf(line, "%u %" SCNu64 " %u %" SCNx64, &nodes, &count, &rounds, &expected) != 4) {
            fprintf(stderr, "%s: cannot read: %s", argv[1], line);
            CHECK(0);
            continue;
        }
        if (combine_check(nodes, count))
            failed++;
        if (osoe_circulant_rounds(nodes) != rounds || digest(nodes, count) != expected) {
            if (differ++ < 10)
                fprintf(stderr, "%s: the C's table differs from the model's: %s", argv[1], line);
        }
        digests++;
    }
    fclose(f);
    printf("%u node counts and lengths: %u where an element is wrong, %u where the C and the "
           "model differ\n", digests, failed, differ);
    CHECK(failed == 0);
    CHECK(differ == 0);
    CHECK(digests > 500);
    return CHECK_DONE();
}
