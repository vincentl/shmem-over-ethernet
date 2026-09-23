/*
 * Finding the PE that never arrived, by walking down the tree (§3.14, §5.3).
 *
 * The diagnosability review's first experiment: one PE does not reach a
 * barrier, and the job hangs. With the star that was answered by visiting every
 * node; with the tree (slice 3) each node publishes the lowest child that owes
 * it an arrival, so the walk is from the root down, O(log N) visits.
 *
 * Eight logical nodes, one PE each, and node CULPRIT never calls the second
 * barrier. PE 0 waits for the others to be in it, then walks: start at node 0
 * and follow `team.world.tree.lowest_missing` while the node it names has an
 * epoch open; the walk ends at the node whose own members have not arrived -
 * which is the culprit's - and that node's PE lines say which PE and in what.
 *
 * It reads the control segments through osoe-stat's reader, not the library's
 * headers, for the reason invariants.h gives; `osoe-stat --summary` prints the
 * same fields for a human, and the commit that added this carries that
 * transcript. The job ends when PE 0 leaves without finalizing, which its node
 * declares and the launcher finishes; the test is judged on what it printed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { CULPRIT = 6, NODES = 8, MAX_VISITS = 16 };

/* One node's tree state, read from its control segment by name. */
static int read_node(unsigned node, uint64_t *open, uint64_t *own, uint64_t *missing,
                     char *host, size_t hostlen)
{
    const char *job = getenv("OSOE_JOB_ID"), *step = getenv("OSOE_STEP_ID");
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    snprintf(name, sizeof name, "/osoe.%s.%s.%u.0", job ? job : "0", step ? step : "0", node);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "hang walk: %s\n", err);
        return 0;
    }
    int ok = s.table_usable &&
             osoe_segment_read_name(&s, "team.world.tree.epoch_open", 0, open) &&
             osoe_segment_read_name(&s, "team.world.tree.own_arrived", 0, own) &&
             osoe_segment_read_name(&s, "team.world.tree.lowest_missing", 0, missing);
    if (ok && host)
        osoe_segment_read_str(&s, "node.hostname", 0, host, hostlen);
    osoe_segment_close(&s);
    return ok;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    CHECK_FATAL(n == NODES);
    shmem_barrier_all();

    if (me == CULPRIT) {
        printf("PE %d never calls the second barrier\n", me);
        fflush(stdout);
        sleep(600);
    }
    if (me != 0) {
        shmem_barrier_all();            /* every other PE waits here for ever */
        return 0;
    }

    /* Give the others time to be in the barrier and to have forwarded what
     * they can. Their arrivals are what makes the walk's answer the culprit's
     * node rather than whichever node is slowest to start. */
    sleep(3);

    unsigned at = 0, visits = 0;
    for (;;) {
        uint64_t open = 0, own = 0, missing = 0;
        char host[72] = "?";
        visits++;
        CHECK_FATAL(visits <= MAX_VISITS);
        CHECK_FATAL(read_node(at, &open, &own, &missing, host, sizeof host));
        printf("visit %u: node %u (%s) epoch_open %llu own_arrived %llu lowest_missing %lld\n",
               visits, at, host, (unsigned long long)open, (unsigned long long)own,
               missing == UINT32_MAX ? -1LL : (long long)missing);
        if (open != 0 && missing != UINT32_MAX) {
            at = (unsigned)missing;     /* the child that owes this node */
            continue;
        }
        /* Nothing missing below: this node's own members are what is awaited. */
        CHECK(own == 0);
        break;
    }
    printf("hang walk found node %u in %u visits, of %u nodes\n", at, visits, NODES);
    CHECK(at == CULPRIT);
    CHECK(visits <= 4);                 /* log_2 8 + 1; never all eight */
    fflush(stdout);
    return CHECK_DONE();
}
