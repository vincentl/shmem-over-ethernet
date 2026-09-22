/*
 * internode.h over the AM layer (§5.3, §3.12).
 *
 * The arrival is performed by the node lead's agent, because the state it
 * moves - the tree's per-team counts and the per-connection queues - is private
 * to that thread, in the lead's process, and the arriver is an application
 * thread of any local PE. So osoe_internode_arrive only leaves the epoch in the
 * team's handoff word of the node header and wakes the lead; the agent takes it
 * on its next pass (osoe_internode_am_take_arrivals, called from the engine's
 * pass) and hands it to the engine. That is §3.12's "a flag plus a futex wake",
 * and the whole of what crosses between processes here.
 *
 * A second backend replaces this file and keeps barrier.c.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "am-state.h"
#include "base/futex.h"
#include "fatal.h"
#include "internode.h"
#include "node.h"
#include "progress.h"
#include "progress-host.h"
#include "state.h"

/*
 * The epoch this agent has already taken out of each team's handoff word.
 * Private to the agent thread: nothing else reads or writes it, which is the
 * whole point of the word it shadows.
 */
static uint32_t consumed[OSOE_TEAMS];

/* Which teams have an internode phase on this node (osoe_internode_team), and
 * the two things the engine's arrival needs from the node set. */
static bool registered[OSOE_TEAMS];
static uint32_t root[OSOE_TEAMS];
static uint32_t participants[OSOE_TEAMS];

void osoe_internode_init(void)
{
    for (uint32_t team = 0; team < OSOE_TEAMS; team++) {
        consumed[team] = 0;
        registered[team] = false;
        root[team] = participants[team] = 0;
    }
}

void osoe_internode_fini(void)
{
}

void osoe_internode_team(uint32_t team, const uint32_t *nodes, uint32_t count)
{
    if (team >= OSOE_TEAMS)
        osoe_fatal("team %u has an internode phase, and this build has %u teams", team,
                   OSOE_TEAMS);
    if (count < 2)
        osoe_fatal("team %u registered an internode phase over %u node", team, count);
    registered[team] = true;
    root[team] = nodes[0];
    participants[team] = count;
}

void osoe_internode_arrive(uint32_t team, uint32_t epoch)
{
    if (team >= OSOE_TEAMS || !registered[team])
        osoe_fatal("team %u arrived at an internode phase it never registered", team);
    struct osoe_arrival_handoff *h = &osoe.node.hdr->arrival[team];
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
 * O(T) per pass, T the build's team count, and it touches only this node's own
 * header - never a connection.
 */
bool osoe_internode_am_take_arrivals(void)
{
    bool moved = false;
    for (uint32_t team = 0; team < OSOE_TEAMS; team++) {
        if (!registered[team])
            continue;
        uint32_t epoch = atomic_load_explicit(&osoe.node.hdr->arrival[team].epoch,
                                              memory_order_acquire);
        if (epoch == consumed[team])
            continue;
        consumed[team] = epoch;
        osoe_progress_arrive(&osoe_am.progress, team, epoch, root[team], participants[team]);
        moved = true;
    }
    return moved;
}
