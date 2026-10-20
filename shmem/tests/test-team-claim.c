/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A team block's claim from its node's pool (tier 2; P9; node.c,
 * osoe_node_team_claim).
 *
 * A claim takes the highest free block, from the top of the pool down; a swap
 * lost to a word changed since its load moves it to the word the claiming
 * PE's local id names, and on down from there round to the top, so that leads
 * claiming at once spread over the pool rather than all trying the top word
 * again. Where a contended claim lands shows in no result, and four CPUs
 * seldom make the contention: one PE claims and gives back blocks of its
 * node's own pool by the library's calls, and loses a swap by the library's
 * rival (osoe_node_team_claim_rival), which takes the block the claim was
 * about to just before its swap. At the pool's size from OSOE_TEAM_BLOCKS:
 *
 *   - uncontended, every claim takes the highest free block until the pool is
 *     dry, whatever local id it gives;
 *   - a lost swap, at local ids naming several words - the top, word 0, past
 *     the word count: the rival's block taken, and the claim's the highest
 *     free of the named word, which on the top word is the next below the
 *     rival's;
 *   - the named word and every one below it empty: the claim goes round to
 *     the top word;
 *   - the rival's block the last free: the claim fails, the pool dry;
 *
 * and the pool whole again at the end. White-box: the pool is the library's,
 * so its sources are compiled in (CMakeLists.txt).
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <shmem.h>

#include "check.h"
#include "node.h"
#include "state.h"

/* The highest free block of word `w` but `except`, or UINT32_MAX. */
static uint32_t highest_free_in(uint32_t w, uint32_t except)
{
    uint64_t bits = atomic_load_explicit(&osoe.node.team_free[w], memory_order_acquire);
    if (except / 64 == w)
        bits &= ~(UINT64_C(1) << (except % 64));
    return bits ? w * 64 + 63 - (uint32_t)__builtin_clzll(bits) : UINT32_MAX;
}

/* The highest free block of the pool, or UINT32_MAX. */
static uint32_t highest_free(void)
{
    for (uint32_t w = osoe.node.team_free_words; w-- > 0;) {
        uint32_t block = highest_free_in(w, UINT32_MAX);
        if (block != UINT32_MAX)
            return block;
    }
    return UINT32_MAX;
}

static bool is_free(uint32_t block)
{
    return (atomic_load_explicit(&osoe.node.team_free[block / 64], memory_order_acquire) >>
            (block % 64)) & 1;
}

static uint32_t free_count(void)
{
    uint32_t count = 0;
    for (uint32_t w = 0; w < osoe.node.team_free_words; w++)
        count += (uint32_t)__builtin_popcountll(
            atomic_load_explicit(&osoe.node.team_free[w], memory_order_acquire));
    return count;
}

/* Every free block claimed uncontended, each checked the highest free, into
 * `held`; how many. */
static uint32_t claim_all(uint32_t *held)
{
    uint32_t count = 0;
    for (;;) {
        uint32_t want = highest_free();
        uint32_t got = osoe_node_team_claim(&osoe.node, count * 7 + 3);
        CHECK(got == want);
        if (got == UINT32_MAX || got != want)
            return count;
        held[count++] = got;
    }
}

/* The claim at `local_id` losing its swap to the rival, which takes `top`:
 * the claim's block `want`, and the rival's taken. Both given back. */
static void lost_swap(uint32_t local_id, uint32_t top, uint32_t want)
{
    osoe_node_team_claim_rival = true;
    uint32_t got = osoe_node_team_claim(&osoe.node, local_id);
    CHECK(!osoe_node_team_claim_rival);
    CHECK(!is_free(top));
    CHECK(got == want);
    if (got != want)
        fprintf(stderr, "local id %u: claimed block %u after the lost swap, not %u\n",
                local_id, got, want);
    osoe_node_team_return(&osoe.node, top);
    if (got != UINT32_MAX)
        osoe_node_team_return(&osoe.node, got);
}

int main(void)
{
    shmem_init();
    CHECK_FATAL(shmem_n_pes() == 1);           /* nobody else claims */
    struct osoe_node *n = &osoe.node;
    uint32_t words = n->team_free_words, top = n->team_count - 1;
    uint32_t pool = free_count();
    uint32_t *held = malloc(n->team_count * sizeof *held);
    CHECK_FATAL(held != NULL && pool == n->team_count - OSOE_PREDEFINED_TEAMS &&
                highest_free() == top);

    /* Uncontended: the highest free first, to the last. */
    uint32_t count = claim_all(held);
    CHECK(count == pool);
    for (uint32_t i = 0; i < count; i++)
        osoe_node_team_return(n, held[i]);
    CHECK(free_count() == pool);

    /* A lost swap: the word the local id names. */
    uint32_t local_ids[] = { 0, 1, words - 1, words, 2 * words + 1, 255, 256 };
    for (uint32_t i = 0; i < sizeof local_ids / sizeof local_ids[0]; i++) {
        uint32_t named = local_ids[i] % words;
        lost_swap(local_ids[i], top, highest_free_in(named, top));
    }
    CHECK(free_count() == pool);

    /* The named word and every one below it empty: round to the top. */
    if (words >= 3) {
        count = claim_all(held);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (held[i] >= 2 * 64)
                osoe_node_team_return(n, held[i]);
            else
                held[kept++] = held[i];
        }
        lost_swap(1, top, highest_free_in(words - 1, top));
        for (uint32_t i = 0; i < kept; i++)
            osoe_node_team_return(n, held[i]);
        CHECK(free_count() == pool);
    }

    /* The rival's block the last free: dry. */
    count = claim_all(held);
    CHECK(count > 0 && held[0] == top);
    osoe_node_team_return(n, top);
    lost_swap(5, top, UINT32_MAX);
    for (uint32_t i = 1; i < count; i++)
        osoe_node_team_return(n, held[i]);
    CHECK(free_count() == pool && highest_free() == top);

    printf("TEAM CLAIMS: a pool of %u blocks in %u word%s, %d failures\n", n->team_count,
           words, words == 1 ? "" : "s", check_fails);
    free(held);
    shmem_finalize();
    return CHECK_DONE();
}
