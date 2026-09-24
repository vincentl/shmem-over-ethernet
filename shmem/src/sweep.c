/*
 * The sweep (§3.15): what every library wait does when its safety-sweep
 * deadline expires (base/sweep.h) - never on the wake path, so a healthy wait
 * pays nothing. It counts the sweep, so that a blocked PE is seen to move
 * (§3.14); while this PE's agent does not run yet - init's own waits - it polls
 * the pidfds the agent will (liveness.h), so a death during init is found; and
 * it stops at the node's abort word, in every build.
 *
 * It was also the development watchdog, which checked every sibling's pid at
 * each sweep and ended the job naming the dead PE and the routine this PE was
 * waiting in. The agents' pidfds find every such death in every build, and the
 * ending agent names the routine (abort.c), so it was retired (the author's
 * decision, 2026-09-23), and OSOE_WATCHDOG and OSOE_LIVENESS with it.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "abort.h"
#include "agent.h"
#include "base/sweep.h"
#include "fatal.h"
#include "liveness.h"
#include "metrics.h"
#include "state.h"

/*
 * If the node has aborted, end this PE as the word says: a global exit with the
 * caller's status, the way its agent would (abort.h), and anything else as a
 * fatal here too.
 */
static void stop_if_aborting(const char *routine)
{
    uint32_t pe = 0;
    int status = EXIT_FAILURE;
    enum osoe_abort_disposition how = osoe_node_abort_read(&osoe.node, &pe, &status);
    if (how == OSOE_ABORT_GLOBAL_EXIT)
        osoe_abort_end();
    if (how != OSOE_ABORT_NONE)
        osoe_fatal("%s: the job is aborting on behalf of PE %u; this PE cannot make "
                   "progress", routine, pe);
}

void osoe_sweep_expired(const char *routine)
{
    if (!osoe.node.hdr)
        return;
    /* A blocked PE's liveness signal: it is asleep, but its sweeps still rise.
     * One add, on a path that has just slept for OSOE_SWEEP_NS (§3.14). */
    atomic_fetch_add_explicit(&osoe.node.slots[osoe.bootstrap.local_id].sweeps, 1,
                              memory_order_seq_cst);

    /* Liveness (liveness.h): the agent watches once it runs, and until then -
     * init's own waits - the thread sweeping does, so a death during init is
     * found too. Before stop_if_aborting, which then acts on a death declared
     * here in this same sweep. */
    if (!osoe_agent_running())
        osoe_liveness_check();

    /* The abort word is read in every build: a PE that has ended the job
     * declared it (fatal.c), and every wait must stop at its next sweep. */
    stop_if_aborting(routine);
}

/* base/sweep.h. The epoch waits of the barriers are base's, so they report a
 * lost wake through this hook rather than naming a counter they cannot see. */
void osoe_missed_wake(const char *routine)
{
    (void)routine;
    osoe_count(OSOE_C_INVARIANT_MISSED_WAKE, 1);
}
