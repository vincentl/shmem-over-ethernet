/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * internode.h over the AM layer (§5.3, §3.12).
 *
 * The arrival is performed by the node lead's agent, because the state it
 * moves - the tree's per-team state and the per-connection queues - is private
 * to that thread, in the lead's process, and the arriver is an application
 * thread of any local PE. So osoe_internode_arrive only leaves the epoch in the
 * handoff word of the team's block in the node header and wakes the lead; the
 * agent takes it on its next pass (osoe_internode_am_take_arrivals, called from
 * the engine's pass) and hands it to the engine. That is §3.12's "a flag plus a
 * futex wake", and the whole of what crosses between processes here.
 *
 * A team reaches this backend as its record in the same block (node.h): the
 * engine reads it the first time it meets the team, through its host
 * (progress-host.c), and the generic data half likewise (internode-generic.c).
 *
 * The data half - reductions and broadcast - AM hands whole to the generic
 * implementation (internode-generic.h), whose puts and adds are AM's own
 * operations: the lead's thread combines, as §5.8 rule 2 wants, and AM has
 * nothing faster to offer it.
 *
 * A second backend replaces this file and keeps barrier.c.
 */
/* liburing.h, through am-state.h and ring.h, uses POSIX types that plain C11
 * hides (portability review F12: feature macros are declared per file). */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "am-state.h"
#include "base/futex.h"
#include "fatal.h"
#include "internode.h"
#include "internode-backend.h"
#include "internode-generic.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"

/*
 * The handoff word this agent has already taken out of each team block - the
 * block's generation and the epoch (node.h) - one per block of the node's pool
 * (OSOE_TEAM_BLOCKS), allocated at init. Private to the agent thread: nothing
 * else reads or writes it, which is the whole point of the word it shadows.
 */
static uint64_t *consumed;
static uint32_t consumed_count;

/*
 * 8 for AM over Ethernet, as a ceiling (v0.2 step 2; the author,
 * docs/decisions/2026-10-06.md item 5): each large reduction's and broadcast's
 * fan-out is chosen from the link under it (collective-shape.h), so the
 * ceiling is the most a call may take - on hundreds of nodes a star of 8 is
 * three levels where 4 is five - and the barrier's and the small class's trees
 * take it whole. It sizes the layout: about 4.2 times the tag's internal region
 * at the capacity of 8 (internal-region.h). An RDMA backend would choose 8-16.
 */
uint32_t osoe_internode_fanout_default(void)
{
    return OSOE_INTERNODE_FANOUT_DEFAULT;
}

void osoe_internode_init(void)
{
    consumed_count = osoe.bootstrap.team_blocks;
    consumed = calloc(consumed_count, sizeof *consumed);
    if (!consumed)
        osoe_fatal("out of memory for %u team blocks' arrivals", consumed_count);
    osoe_internode_generic_init();
}

void osoe_internode_fini(void)
{
    osoe_internode_generic_fini();
    free(consumed);
    consumed = NULL;
    consumed_count = 0;
}

void osoe_internode_number_taken(uint32_t number)
{
    osoe_internode_generic_number_taken(number);
}

void osoe_internode_arrive(uint32_t team, uint32_t epoch)
{
    /* Only a team whose record here says it spans nodes posts. The record is
     * loaded as it was published, with acquire (§3.16), and this member finds
     * it published whichever process it is: the node's lead wrote the
     * predefined teams' records before its release store to
     * lead_agent_running, which this PE loaded with acquire before its
     * shmem_init returned (init.c), and the arrival counter's release sequence
     * carries them too (node.h, the record). A created team's record is
     * carried by the split's sync over its parent, which this member left
     * before posting (node.h). */
    struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
    if (!b || !atomic_load_explicit(&b->record.published, memory_order_acquire) ||
        b->record.nodes < 2)
        osoe_fatal("a team arrived at an internode phase through block %u of the node's pool, "
                   "which holds no record of one", team);
    struct osoe_arrival_handoff *h = &b->arrival;
    /*
     * The block's generation beside the epoch (node.h): an acquire load
     * (§3.16), though this member holds the block and its generation moves
     * only while nobody does. Then a single-writer publication store (§3.16): only one member
     * posts per epoch, and the release of the previous epoch orders it after
     * the last one. Then the wake word, a locked add, which on x86-TSO also
     * drains the store buffer so the agent cannot see the bump without the
     * epoch.
     */
    uint32_t generation = atomic_load_explicit(&b->name.generation, memory_order_acquire);
    atomic_store_explicit(&h->word, (uint64_t)generation << 32 | epoch, memory_order_release);
    /* The block's bit in the posted bitmap, then its word's bit in the summary
     * (node.h, team_posted_summary; posted_bitmap_model.py): after the word, so
     * that the agent that takes a bit finds the epoch, and before the wake, so
     * that an agent that missed them both is woken to look again. Locked ors,
     * seq_cst as every read-modify-write is (§3.16). */
    atomic_fetch_or_explicit(osoe_node_team_posted(&osoe.node, team), UINT64_C(1) << (team % 64),
                             memory_order_seq_cst);
    atomic_fetch_or_explicit(osoe_node_team_posted_summary(&osoe.node, team),
                             UINT64_C(1) << (team / 64 % 64), memory_order_seq_cst);

    /* The lead is local id 0 (§6.2). It is the only agent that runs the
     * engine, so it is the only one worth waking for this. */
    struct osoe_pe_slot *lead = &osoe.node.slots[0];
    atomic_fetch_add_explicit(&lead->agent_wake, 1, memory_order_seq_cst);
    osoe_futex_wake(&lead->agent_wake, 1, OSOE_FUTEX_SHARED);
}

/*
 * Take whatever has been posted and perform the arrivals.
 *
 * **The epoch is recorded consumed before the arrival is performed, and from
 * the value just loaded rather than by re-reading the word.** On the root node
 * osoe_progress_arrive can complete the collective and release the team
 * immediately, so a local member can be back in the next barrier - and posting
 * its epoch here - before this function's next line would run. Re-reading the
 * word at that point marks an epoch consumed that was never forwarded, and
 * that node never arrives again: `consume_reread` in
 * docs/design/evidence/am-backend/arrival_model.py, where the whole job hangs
 * and every other node is the one that looks stuck.
 *
 * A word per team block, each on its own line of the node's own header -
 * never a connection - read only where the posted bitmap says a post was made
 * (node.h, team_posted_summary; tools/modelcheck/posted_bitmap_model.py): the
 * summary's words taken by exchange, each nonzero one naming bitmap words
 * taken the same way, each naming blocks; so a pass with nothing posted costs
 * a load of the summary - one word at the default pool - however many teams
 * the node holds. Until layout 30 a pass read every block at and above a
 * low-water mark of published blocks, the most the node had held at once; until
 * step D every team number below a high-water mark, every team the job had
 * made. A team is met here, and learnt from its record, at its first post; a
 * bit whose word has not moved is passed over, and a team that has no
 * internode phase never posts.
 *
 * **The whole word is compared, the generation with the epoch** (node.h, the
 * handoff): a block given back and claimed for another team starts its epochs
 * again, and the new team's first post can carry the epoch the old team's
 * last one did; it never carries its generation. The engine learns the block
 * afresh when the generation is not the one it learnt (progress.c).
 *
 * The bitmap misses no post: a member stores its word, then sets its bits,
 * then bumps the lead's wake word, and the agent loads that word before it
 * takes the bits. An agent whose load read the bump finds the bits, and through
 * their exchange the word, which was stored before them; one whose load came
 * before it finds the wait's compare fail and passes again - the rule that
 * makes the handoff sound already (§5.3). In this order no bit is taken for a
 * word already taken: a member posts again only once its last post is
 * forwarded and released, and each post sets its bit once, after its word. So
 * the comparison with the word last consumed never passes a word over here;
 * it is kept as the guard it is, should a bit ever be taken twice -
 * posted_bitmap_model.py's `kept_bits`, bits loaded rather than exchanged,
 * forwards an arrival twice without it, and ORDER fails (the step D review,
 * correctness G3).
 */
static bool take_arrival(uint32_t block)
{
    uint64_t word = atomic_load_explicit(&osoe_node_team(&osoe.node, block)->arrival.word,
                                         memory_order_acquire);
    if (word == consumed[block])
        return false;
    consumed[block] = word;
    osoe_progress_arrive(&osoe_am.progress, block, (uint32_t)(word >> 32), (uint32_t)word);
    return true;
}

bool osoe_internode_am_take_arrivals(void)
{
    bool moved = false;
    uint32_t words = (consumed_count + 63) / 64;
    for (uint32_t s = 0; s * 64 < words; s++) {
        _Atomic uint64_t *summary = &osoe.node.hdr->team_posted_summary[s];
        /* A load first: a pass with nothing posted takes no line for writing. */
        if (atomic_load_explicit(summary, memory_order_acquire) == 0)
            continue;
        uint64_t marked = atomic_exchange_explicit(summary, 0, memory_order_seq_cst);
        for (; marked != 0; marked &= marked - 1) {
            uint32_t w = s * 64 + (uint32_t)__builtin_ctzll(marked);
            if (w >= words)
                osoe_fatal("the posted summary marks word %u of a bitmap of %u", w, words);
            uint64_t posted = atomic_exchange_explicit(&osoe.node.team_posted[w], 0,
                                                       memory_order_seq_cst);
            for (; posted != 0; posted &= posted - 1) {
                uint32_t block = w * 64 + (uint32_t)__builtin_ctzll(posted);
                if (block >= consumed_count)
                    osoe_fatal("the posted bitmap marks block %u of a pool of %u", block,
                               consumed_count);
                moved |= take_arrival(block);
            }
        }
    }
    return moved;
}

/* ---- The data half: the generic implementation's ---------------------- */

void osoe_internode_reduce_enter(uint32_t team, uint32_t epoch, size_t count,
                                 enum osoe_reduction_operation operation,
                                 struct osoe_element element, const char *routine)
{
    osoe_internode_generic_reduce_enter(team, epoch, count, operation, element, routine);
}

void osoe_internode_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                           enum osoe_reduction_operation operation,
                           struct osoe_element element, const char *routine)
{
    osoe_internode_generic_reduce(team, epoch, buffer, count, operation, element, routine);
}

void osoe_internode_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index, void *buffer,
                              size_t bytes, const char *routine)
{
    osoe_internode_generic_broadcast(team, epoch, root_index, buffer, bytes, routine);
}

void osoe_internode_collect(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                            size_t block, const uint64_t *places, const char *routine)
{
    osoe_internode_generic_collect(team, epoch, buffer, bytes, block, places, routine);
}

void osoe_internode_scan_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                     uint64_t *before, uint64_t *total, const char *routine)
{
    osoe_internode_generic_scan_node_totals(team, epoch, own, before, total, routine);
}

void osoe_internode_collect_ranges(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                                   uint64_t share_start, uint64_t share_end, bool reverse,
                                   const char *routine)
{
    osoe_internode_generic_collect_ranges(team, epoch, buffer, bytes, share_start, share_end,
                                          reverse, routine);
}

void osoe_internode_exchange_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                         uint64_t *totals, const char *routine)
{
    osoe_internode_generic_exchange_node_totals(team, epoch, own, totals, routine);
}

uint32_t osoe_internode_exchange_most_nodes(void)
{
    return osoe_internode_generic_exchange_most_nodes();
}

void osoe_internode_scan(uint32_t team, uint32_t rank, void *dest, const void *source,
                         size_t count, struct osoe_element element, bool inclusive,
                         const char *routine)
{
    osoe_internode_generic_scan(team, rank, dest, source, count, element, inclusive, routine);
}
