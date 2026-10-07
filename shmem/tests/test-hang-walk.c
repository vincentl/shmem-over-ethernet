/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Finding the PE that never arrived, by walking down the tree (§3.14, §5.3).
 *
 * The diagnosability review's first experiment: one PE does not reach a
 * barrier, and the job hangs. With the star that was answered by visiting every
 * node; with the tree (slice 3) each node publishes the lowest child that owes
 * it an arrival, so the walk is from the root down, O(log N) visits.
 *
 * Eight logical nodes, one PE each, and node CULPRIT never calls the second
 * barrier. PE 0 walks - until the walk lands, which it may not while the
 * others are still arriving; no fixed sleep - starting at node 0
 * and follow `team.world.tree.lowest_missing` while the node it names has an
 * epoch open; the walk ends at the node whose own members have not arrived -
 * which is the culprit's - and that node's PE lines say which PE and in what.
 *
 * It reads the control segments through osoe-stat's reader, not the library's
 * headers, for the reason invariants.h gives; `osoe-stat --summary` prints the
 * same fields for a human, and the commit that added this carries that
 * transcript. Then every node's measured link, which the summary shows in its
 * headers, the same in all eight (v0.2 step 2). The job ends when PE 0 leaves without finalizing, which its node
 * declares and the launcher finishes; the test is judged on what it printed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { CULPRIT = 6, NODES = 8, MAX_VISITS = 16, SETTLE_SECONDS = 8 };

/* One node's tree state, read from its control segment by name. */
static int read_node(unsigned node, uint64_t *open, uint64_t *own, uint64_t *missing,
                     char *host, size_t hostlen)
{
    const char *job = osoe_test_launcher("JOB_ID"), *step = osoe_test_launcher("STEP_ID");
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

/* Node `node`'s measured link, as osoe-stat's summary shows it in the node's
 * header (v0.2 step 2, link-measure.h): its hop and its bandwidth. */
static int read_link(unsigned node, uint64_t *hop, uint64_t *bandwidth)
{
    const char *job = osoe_test_launcher("JOB_ID"), *step = osoe_test_launcher("STEP_ID");
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    snprintf(name, sizeof name, "/osoe.%s.%s.%u.0", job ? job : "0", step ? step : "0", node);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "hang walk: %s\n", err);
        return 0;
    }
    int ok = s.table_usable &&
             osoe_segment_read_name(&s, "node.link.measured_hop_ns", 0, hop) &&
             osoe_segment_read_name(&s, "node.link.measured_bytes_per_second", 0, bandwidth);
    osoe_segment_close(&s);
    return ok;
}

/*
 * One walk from the root: follow lowest_missing while the node it names has an
 * epoch open. Returns 1 if it ended where a walk must once the tree has
 * settled - at a node whose own members have not arrived - 0 if it ended at a
 * node whose members have (an arrival still on its way up), and -1 if a
 * segment could not be read or the walk ran too long. Prints each visit if
 * `print`.
 */
static int walk(unsigned *at_out, unsigned *visits_out, int print)
{
    unsigned at = 0, visits = 0;
    for (;;) {
        uint64_t open = 0, own = 0, missing = 0;
        char host[72] = "?";
        if (++visits > MAX_VISITS || !read_node(at, &open, &own, &missing, host, sizeof host))
            return -1;
        if (print)
            printf("visit %u: node %u (%s) epoch_open %llu own_arrived %llu lowest_missing %lld\n",
                   visits, at, host, (unsigned long long)open, (unsigned long long)own,
                   missing == UINT32_MAX ? -1LL : (long long)missing);
        if (open != 0 && missing != UINT32_MAX) {
            at = (unsigned)missing;     /* the child that owes this node */
            continue;
        }
        *at_out = at;
        *visits_out = visits;
        return own == 0;
    }
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

    /*
     * Walk until the walk lands on the culprit's node, not for a fixed time.
     * Until every other PE is in the barrier and its arrival has come up the
     * tree, a walk can end at a node that is merely slow to start, which a
     * fixed sleep only made unlikely. Bounded inside the launcher's 12 s, so a
     * tree whose walk never finds the culprit fails here and says so; what is
     * checked below is that it does, and in how many visits.
     */
    struct timespec tick = { 0, 10 * 1000 * 1000 };
    time_t give_up = time(NULL) + SETTLE_SECONDS;
    unsigned at = 0, visits = 0;
    for (;;) {
        int settled = walk(&at, &visits, 0);
        CHECK_FATAL(settled >= 0);
        if (settled && at == CULPRIT)
            break;
        CHECK_FATAL(time(NULL) < give_up);
        nanosleep(&tick, NULL);
    }
    /* The settled walk, printed: the visits a person with osoe-stat would make. */
    CHECK_FATAL(walk(&at, &visits, 1) == 1);
    printf("hang walk found node %u in %u visits, of %u nodes\n", at, visits, NODES);
    CHECK(at == CULPRIT);
    CHECK(visits <= 4);                 /* log_2 8 + 1; never all eight */
    /* And the link the shapes are chosen from, as the summary's header shows
     * it: measured at the start, and the same in every node's segment. */
    uint64_t hop0 = 0, bandwidth0 = 0;
    CHECK(read_link(0, &hop0, &bandwidth0));
    CHECK(hop0 > 0 && bandwidth0 > 0);
    for (unsigned node = 1; node < NODES; node++) {
        uint64_t hop = 0, bandwidth = 0;
        CHECK(read_link(node, &hop, &bandwidth));
        CHECK(hop == hop0 && bandwidth == bandwidth0);
    }
    printf("every node's segment shows the link measured: a hop of %llu ns, %llu bytes a second\n",
           (unsigned long long)hop0, (unsigned long long)bandwidth0);
    fflush(stdout);
    return CHECK_DONE();
}
