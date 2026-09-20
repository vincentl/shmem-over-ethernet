/*
 * The internode phase of a barrier, over the AM layer (§5.1, §5.3, §8.2).
 *
 * The intranode phase runs on the node's epoch counters and is step 1's. This
 * is the other half: the last local member arrives, the root counts the nodes,
 * and the release comes back and is applied by each node's progress thread.
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
#include "connection-area.h"
#include "node.h"
#include "progress.h"
#include "reply.h"
#include "wire.h"

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

enum { MAX_NODES = 8, DESCRIPTORS = 64, SCRATCH = 4096, ROOT = 0, TEAM = 0 };

static uint32_t releases, released_epoch;

static void *resolve(void *c, uint32_t pe, uint16_t r, uint64_t o, uint32_t n)
{ (void)c; (void)pe; (void)r; (void)o; (void)n; return NULL; }
static struct osoe_landing_slot *landing(void *c, uint32_t pe)
{ (void)c; (void)pe; return NULL; }
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
    CHECK(b.node_count >= 3 && b.node_count <= MAX_NODES);

    struct osoe_connection_area area;
    osoe_connection_area_open(&area, &b, b.node_count, 0);
    arrivals_open();

    /* Only the node lead touches a wire or runs the loop (§3.12). The other
     * PEs of a node have nothing to do in a barrier's internode phase - their
     * arrival is the node's, counted by the intranode phase. */
    if (b.local_id != 0) {
        osoe_connection_area_close(&area);
        osoe_bootstrap_free(&b);
        return CHECK_DONE();
    }

    static struct osoe_wire wires[MAX_NODES];
    uint32_t peers = 0;
    for (uint32_t n = 0; n < b.node_count; n++) {
        if (n == b.node)
            continue;
        osoe_wire_open(&wires[peers++], &b, n, 0);
    }
    CHECK(peers == b.node_count - 1);

    static struct osoe_reply descriptors[DESCRIPTORS];
    static unsigned char scratch[SCRATCH];
    static struct osoe_progress_connection connections[MAX_NODES];
    struct osoe_progress_host host = { resolve, landing, notify, release, NULL };
    struct osoe_progress p;
    osoe_progress_init(&p, &area, wires, descriptors, DESCRIPTORS, scratch, SCRATCH,
                       &host);
    osoe_progress_attach(&p, connections);

    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(25000000000));

    /*
     * Every node must have a buffer open before any arrival is posted, or a
     * node could still be opening one while another has already released - and
     * the timing check below would then be measuring setup, not the barrier.
     */
    for (int i = 0; i < 64; i++)
        osoe_progress_poll(&p);

    enum { EPOCH = 5 };
    /*
     * The table is indexed by *node*, because the internode phase is between
     * nodes: a node arrives once, however many local members it has. Staggering
     * by rank spreads those arrivals out, so a release that did not wait has
     * something to be caught by.
     */
    arrivals_arrive((int)b.rank);
    arrivals_row()[b.node] = arrivals_now();
    osoe_progress_arrive(&p, TEAM, EPOCH, ROOT, b.node_count);

    while (releases == 0) {
        osoe_progress_poll(&p);
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

    /* Keep the engine turning: the root's own wait is satisfied while the
     * releases it owes every other node are still in its outbox, and only this
     * thread sends them. */
    while (osoe_progress_poll(&p))
        if (osoe_deadline_expired(&give_up))
            break;

done:
    for (uint32_t i = 0; i < peers; i++)
        osoe_wire_close(&wires[i]);
    osoe_connection_area_close(&area);
    osoe_bootstrap_free(&b);
    return CHECK_DONE();
}
