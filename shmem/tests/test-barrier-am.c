/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The internode phase of a barrier, over the AM layer (§5.1, §5.3, §8.2).
 *
 * The intranode phase runs on the node's epoch counters and is step 1's. This
 * is the other half: the last local member arrives, the nodes meet up the
 * k-ary tree, and the release comes back down it and is applied by each node's
 * progress thread.
 *
 * **Three nodes, not two.** With two, the root's own arrival and the one
 * message it receives are the first and the last, so a root that released on
 * *any* arrival behaves identically to one that waits for all - and that
 * mutation passed a two-node test. Three is the smallest configuration where
 * "released too early" and "released correctly" differ.
 *
 * **The timing check is §8.2's arrival table**, not a clock reading per PE. A
 * barrier promises only that no PE leaves before every PE has arrived, so each
 * node records its arrival in a table outside the library and then requires
 * that its release came no earlier than the latest arrival. Ranks stagger their
 * arrival, so a release that did not wait has something to be caught by; a
 * check that measured each PE's own wait would fail when a PE was descheduled,
 * which is the mistake §8.2 records having made once already.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am.h"
#include "arrivals.h"
#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "collective-tree.h"
#include "connection-area.h"
#include "node.h"
#include "progress.h"
#include "reply.h"
#include "wire.h"

/* bootstrap.c reads the fan-out's default from the backend (internode.h), which
 * this test does not link; AM's value. */
uint32_t osoe_internode_fanout_default(void);
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

enum { MAX_NODES = 16, DESCRIPTORS = 64, TEAM = 0 };

static uint32_t releases, released_epoch;

static void *resolve(void *c, uint32_t pe, uint16_t r, uint64_t o, uint32_t n)
{ (void)c; (void)pe; (void)r; (void)o; (void)n; return NULL; }
static void notify(void *c, uint32_t pe, uint16_t r, uint64_t o, uint32_t n)
{ (void)c; (void)pe; (void)r; (void)o; (void)n; }

/* Stands in for advancing the team's `released` counter and waking every local
 * waiter (§5.3). What matters here is when it happens, not what it moves. */
static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context; (void)team;
    released_epoch = epoch;
    releases++;
}

int main(void)
{
    struct osoe_bootstrap b = { 0 };
    osoe_bootstrap_read(&b);
    /* Out of range here overruns the fixed-size wires[MAX_NODES] below, or
     * under-populates it against the node_count the internode phase then
     * arrives and waits against. */
    CHECK_FATAL(b.node_count >= 3 && b.node_count <= MAX_NODES);

    struct osoe_connection_area area;
    osoe_connection_area_open(&area, &b, b.node_count, 0, 0, OSOE_SHM_ANY_CREATOR);
    arrivals_open();

    /* Only the node lead touches a wire or runs the loop (§3.12). The other
     * PEs of a node have nothing to do in a barrier's internode phase - their
     * arrival is the node's, counted by the intranode phase. */
    if (b.local_id != 0) {
        osoe_connection_area_close(&area);
        osoe_bootstrap_free(&b);
        return CHECK_DONE();
    }

    /* This lead's connections to every other node, two a peer - its data wires,
     * then its control wires, on which the tree's messages go (owner-map.h) -
     * and the ring that carries them: the mesh a lead makes (§6.2). This thread
     * drives the engine, so it enables the ring and is its only submitter
     * (ring.h). */
    static struct osoe_wire wires[2 * MAX_NODES];
    static struct osoe_ring ring;
    uint32_t peers = b.node_count - 1;
    osoe_wires_open(wires, (struct osoe_ring *[]){ &ring }, 0, &b, 0, NULL);
    osoe_ring_enable(&ring);

    static struct osoe_reply descriptors[DESCRIPTORS];
    static struct osoe_progress_connection connections[MAX_NODES];
    static uint64_t busy[(MAX_NODES + 63) / 64];
    struct osoe_progress_host host = { resolve, notify, release, NULL, NULL, NULL, NULL,
                                       NULL, NULL };
    /* One owner, the lead's agent, holding every wire (owner-map.h). */
    struct osoe_progress p;
    static struct osoe_progress_owner owner;
    static struct osoe_progress_control controls[MAX_NODES];
    static uint64_t control_busy[(MAX_NODES + 63) / 64];
    osoe_progress_init(&p, &area, &owner, &host);
    osoe_progress_attach(&p, 0, &ring, wires, connections, busy, descriptors, DESCRIPTORS);
    osoe_progress_attach_control(&p, &wires[peers], controls, control_busy);

    /* The world team's tree: every node, at the fan-out the environment gives
     * (OSOE_COLLECTIVE_FANOUT, so the same binary runs every shape). */
    uint32_t nodes[MAX_NODES];
    for (uint32_t n = 0; n < b.node_count; n++)
        nodes[n] = n;
    struct osoe_collective_tree tree;
    CHECK_FATAL(osoe_collective_tree(nodes, b.node_count, b.node, b.collective_fanout, &tree));
    osoe_progress_team(&p, TEAM, &tree);

    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(25000000000));

    /*
     * Every node must have a buffer open before any arrival is posted, or a
     * node could still be opening one while another has already released - and
     * the timing check below would then be measuring setup, not the barrier.
     */
    for (int i = 0; i < 64; i++)
        osoe_progress_poll(&owner);

    enum { EPOCH = 5 };
    /*
     * The table is indexed by *node*, because the internode phase is between
     * nodes: a node arrives once, however many local members it has. Staggering
     * by rank spreads those arrivals out, so a release that did not wait has
     * something to be caught by.
     */
    arrivals_arrive((int)b.rank);
    arrivals_row()[b.node] = arrivals_now();
    osoe_progress_arrive(&p, TEAM, 0, EPOCH);

    while (releases == 0) {
        if (!osoe_progress_poll(&owner))
            osoe_ring_wait(&ring, UINT64_C(1000000));
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "node %u: the barrier never released\n", b.node);
            CHECK(false);
            goto done;
        }
    }

    /*
     * The property, and the only one a barrier has: this node left no earlier
     * than the latest node arrived. A root that released on any arrival rather
     * than on all of them fails here, and fails nowhere else.
     */
    CHECK(arrivals_left_after_all((int)b.node_count));
    CHECK(released_epoch == EPOCH);
    CHECK(releases == 1);

    /*
     * Keep the engine turning: the root's own wait is satisfied while the
     * releases it owes every other node are still in its outbox, and only this
     * thread sends them.
     *
     * **Until it owes nothing, not until a pass moves nothing.** A pass moves
     * nothing whenever the wire is momentarily full, which under load happens
     * while a sealed segment is still going out - so the older form here walked
     * away from a release the peer was waiting for. It was caught in
     * shmem.progress, at about one run in thirty and only in the full suite;
     * this file has the same shape with three nodes and two peers to owe
     * rather than one. osoe_progress_idle is the library's own predicate for
     * exactly this question.
     */
    while (!osoe_progress_idle(&owner)) {
        osoe_progress_poll(&owner);
        if (osoe_deadline_expired(&give_up))
            break;
    }

    /*
     * Then the close a lead makes (§6.4, close_model.py): goodbye, end of
     * stream, and read until every peer's has ended - which is also what keeps
     * this node reading until the nodes it releases have taken their release.
     */
    osoe_progress_close(&owner);
    while (!osoe_progress_closed(&owner) && !osoe_deadline_expired(&give_up))
        if (!osoe_progress_poll(&owner))
            osoe_ring_wait(&ring, UINT64_C(1000000));
    CHECK(osoe_progress_closed(&owner));

done:
    for (uint32_t i = 0; i < 2 * peers; i++)
        osoe_wire_close(&wires[i]);
    osoe_ring_close(&ring);
    osoe_connection_area_close(&area);
    osoe_bootstrap_free(&b);
    return CHECK_DONE();
}

/* The library's notice (fatal.h), which the listener's waits print once they
 * have lasted (base/stall-watch.h): here, to stderr as it is. */
void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_notice: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
