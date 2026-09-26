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
#include "internode-generic.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"

/*
 * The epoch this agent has already taken out of each team's handoff word, one
 * per team number (OSOE_MAX_TEAMS), allocated at init. Private to the agent
 * thread: nothing else reads or writes it, which is the whole point of the
 * word it shadows.
 */
static uint32_t *consumed;
static uint32_t consumed_count;

/*
 * 4 for AM over Ethernet: h/o is about 4 on a server's TCP (h ~ 20 us one way,
 * o ~ 5 us per message) and about 8 on a Pi's 1 GbE, and the cost model's
 * optimum for those is k = 4 to 6 (§5.3). An RDMA backend would choose 8-16.
 */
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

void osoe_internode_init(void)
{
    consumed_count = osoe.bootstrap.max_teams;
    consumed = calloc(consumed_count, sizeof *consumed);
    if (!consumed)
        osoe_fatal("out of memory for %u teams' arrivals", consumed_count);
    osoe_internode_generic_init();
}

void osoe_internode_fini(void)
{
    osoe_internode_generic_fini();
    free(consumed);
    consumed = NULL;
    consumed_count = 0;
}

void osoe_internode_arrive(uint32_t team, uint32_t epoch)
{
    /* Only a team whose record here says it spans nodes posts: the record is
     * published before any member can arrive (node.h), and loaded as it was
     * published, with acquire (§3.16). */
    struct osoe_team_block *b = osoe_node_team(&osoe.node, team);
    if (!b || !atomic_load_explicit(&b->record.published, memory_order_acquire) ||
        b->record.nodes < 2)
        osoe_fatal("team %u arrived at an internode phase it has no record of on this node",
                   team);
    struct osoe_arrival_handoff *h = &b->arrival;
    /*
     * A single-writer publication store (§3.16): only one member posts per
     * epoch, and the release of the previous epoch orders it after the last
     * one. Then the wake word, a locked add, which on x86-TSO also drains the
     * store buffer so the agent cannot see the bump without the epoch.
     */
    atomic_store_explicit(&h->epoch, epoch, memory_order_release);

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
 * A word per team number, OSOE_MAX_TEAMS of them, each on its own line of the
 * node's own header - never a connection: a team is met here, and learnt from
 * its record, at its first post, so every number is looked at. That is O(T)
 * loads per pass with T the job's team count, 32 by default - a cost that
 * grows with OSOE_MAX_TEAMS, not with the teams in use. A word that has not
 * moved is skipped at once; a team that has no internode phase never posts.
 */
bool osoe_internode_am_take_arrivals(void)
{
    bool moved = false;
    for (uint32_t team = 0; team < consumed_count; team++) {
        uint32_t epoch = atomic_load_explicit(&osoe_node_team(&osoe.node, team)->arrival.epoch,
                                              memory_order_acquire);
        if (epoch == consumed[team])
            continue;
        consumed[team] = epoch;
        osoe_progress_arrive(&osoe_am.progress, team, epoch);
        moved = true;
    }
    return moved;
}

/* ---- The data half: the generic implementation's ---------------------- */

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
