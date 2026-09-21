/*
 * A job that spans logical nodes: init, barrier, finalize (§6.2, §5.3, §8.1).
 *
 * This is the door-opener's test. Until now shmem_init refused anything but one
 * node, so every multi-node thing was tested a layer down, against the wire and
 * the progress loop directly. Here it is the library: shmem_init brings up the
 * AM backend, shmem_barrier_all crosses node boundaries, and shmem_ptr answers
 * for §8.1's locality rule.
 *
 * **Three nodes at least, and more than one PE on a node.** Both numbers are
 * lessons paid for in this slice rather than preferences:
 *
 *   - With two nodes the root's own arrival and the single message it receives
 *     are the first and the last, so a root that released on *any* arrival
 *     behaves identically to one that waits for all. That mutation passed a
 *     two-node test of the phase below this one.
 *   - With one PE per node the intranode phase never runs: the first arriver is
 *     also the last, so an arrival target computed from the wrong member count
 *     is right by accident, and the handoff word is never contended.
 *
 * The maps below therefore include the shape that discriminates (three nodes,
 * two PEs each) as well as §8.1's four poles, and one single-node map, which is
 * how the same program exercises the path where the last local arriver releases
 * the epoch itself.
 *
 * **Many epochs, not one.** The handoff word is reused every collective, and
 * the model of it (arrival_model.py) found that its reuse defects are invisible
 * at a single epoch while its wake defects are not. A test of one barrier would
 * have the same blind spot.
 *
 * The timing check is §8.2's arrival table rather than a per-PE clock reading,
 * for the reason arrivals.h gives.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "arrivals.h"
#include "check.h"
#include "invariants.h"

enum { QUICK = 200, STAGGERED = 3, LOCAL_MAX = 256 };

/* The world ranks on this node, from the launcher's OSOE_GTIDS (§6.2). The
 * library knows this; the test works it out for itself, so that it is checking
 * the library rather than agreeing with it. */
static int local_ranks[LOCAL_MAX];
static int local_count;

static void read_local_ranks(void)
{
    const char *gtids = getenv("OSOE_GTIDS");
    if (!gtids) {
        fprintf(stderr, "OSOE_GTIDS is unset: this test needs osoe-launch\n");
        exit(2);
    }
    for (const char *at = gtids; *at; ) {
        char *end;
        long v = strtol(at, &end, 10);
        if (end == at)
            break;
        if (local_count < LOCAL_MAX)
            local_ranks[local_count++] = (int)v;
        at = (*end == ',') ? end + 1 : end;
    }
}

static int is_local(int pe)
{
    for (int i = 0; i < local_count; i++)
        if (local_ranks[i] == pe)
            return 1;
    return 0;
}

int main(void)
{
    read_local_ranks();
    const char *force = getenv("OSOE_FORCE_REMOTE");
    int forced = force && strcmp(force, "0") != 0;
    int nodes = atoi(getenv("OSOE_STEP_NUM_NODES") ? getenv("OSOE_STEP_NUM_NODES") : "1");

    arrivals_open();
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    CHECK(me >= 0 && me < n);
    CHECK(n > 0);

    /* The two predefined teams stop agreeing the moment a second node exists,
     * which is the whole reason they are two teams (§7.3). */
    CHECK(shmem_team_n_pes(SHMEM_TEAM_WORLD) == n);
    CHECK(shmem_team_n_pes(SHMEM_TEAM_SHARED) == local_count);
    CHECK(shmem_team_my_pe(SHMEM_TEAM_WORLD) == me);
    if (nodes == 1)
        CHECK(local_count == n);
    else
        CHECK(local_count < n);

    /*
     * §8.1: shmem_ptr returns NULL across logical nodes although the pages are
     * physically reachable, and NULL for a local PE too under the force-remote
     * flag - which is what lets the same program run both ways and be diffed
     * (§4.8). The pointer is checked, not just its nullness: a local one must
     * address that PE's heap and hold what that PE wrote.
     */
    long *mine = shmem_malloc(sizeof *mine);
    CHECK_FATAL(mine != NULL);  /* NULL would be dereferenced just below */
    *mine = 1000 + me;
    shmem_barrier_all();

    for (int pe = 0; pe < n; pe++) {
        void *p = shmem_ptr(mine, pe);
        /* A PE is always local to itself, force-remote or not (§8.1): there is
         * no connection from a node to itself, so an operation a PE addresses
         * to its own rank has nowhere to be routed. */
        int direct = (pe == me) || (!forced && is_local(pe));
        if (!direct) {
            CHECK(p == NULL);
        } else {
            CHECK(p != NULL);
            if (p)
                CHECK(*(long *)p == 1000 + pe);
        }
        /* shmem_team_ptr over the world team must answer identically
         * (spec §9.2.3), since it lands on the same locality function. */
        CHECK(shmem_team_ptr(SHMEM_TEAM_WORLD, mine, pe) == p);
    }
    CHECK(shmem_ptr(mine, me) == mine);

    /*
     * The library's own coherent load resolves through the same locality
     * function, on this PE's own rank (§4.10). It is reached by every
     * shmem_test and every wait_until, so a locality rule that made a PE remote
     * from itself would be a null dereference here and nowhere in the checks
     * above - which is how it was found.
     */
    long *flag = shmem_malloc(sizeof *flag);
    CHECK_FATAL(flag != NULL);  /* NULL would be dereferenced just below, and the
                                 * wait_until after it would then never return */
    *flag = 0;
    shmem_barrier_all();
    CHECK(shmem_test(flag, SHMEM_CMP_EQ, 0) != 0);
    *flag = 7;
    shmem_long_wait_until(flag, SHMEM_CMP_EQ, 7);
    CHECK(shmem_test(flag, SHMEM_CMP_NE, 0) != 0);
    shmem_free(flag);

    /*
     * Many epochs back to back. This is what reuses the handoff word, and it is
     * also where a release applied out of order would be caught: §5.4's waits
     * are equality, so an epoch that arrives twice or is skipped hangs rather
     * than returning a wrong answer, and the ctest timeout is the assertion.
     */
    for (int i = 0; i < QUICK; i++)
        shmem_barrier_all();

    /* sync_all runs the same two-level phase without the quiet (§5.6), so it
     * must cross node boundaries too. */
    for (int i = 0; i < QUICK; i++)
        shmem_sync_all();

    /* A team sync on SHMEM_TEAM_SHARED stays on this node, and interleaving it
     * with world collectives must not release either team's waiters: the two
     * teams have their own epoch counters for exactly this (§5.4). */
    for (int i = 0; i < STAGGERED; i++) {
        CHECK(shmem_team_sync(SHMEM_TEAM_SHARED) == 0);
        shmem_barrier_all();
    }

    /* Skewed arrivals: a barrier that released before every PE had arrived has
     * something to be caught by. */
    for (int i = 0; i < STAGGERED; i++) {
        arrivals_arrive(me);
        shmem_barrier_all();
        CHECK(arrivals_left_after_all(n));
    }

    CHECK_INVARIANTS();

    /* Every finalize is a barrier, the last one included (spec §9.1.4). */
    shmem_init();
    arrivals_arrive(me);
    shmem_finalize();
    CHECK(arrivals_left_after_all(n));

    arrivals_arrive(me);
    shmem_finalize();
    CHECK(arrivals_left_after_all(n));

    arrivals_close();
    return CHECK_DONE();
}
