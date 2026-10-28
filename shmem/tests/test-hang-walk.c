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
 *
 * With a mode, the same walk of a data collective (D1, layout 42), which the
 * nodes' leads publish as their waits begin (node.h, team.tree.data_*): node
 * CULPRIT never calls it, and PE 0, which does, walks from a thread of its own
 * that calls nothing of the library's, following `team.tree.data_wait_node`
 * from node 0 while the node it names has the collective open, until the walk
 * ends at the culprit's node, which has none - every other node's lead in it,
 * waiting - and then ends its process. Where a lead's wait is on one counter
 * of several leads' - Träff's grants, the exchange of collect's totals - it
 * names no node, only how many are missing: those modes may end there instead.
 * The modes: reduce, a tree's one piece; reduce-large, the tree's pieces
 * through lanes, under OSOE_COLLECTIVE_SHAPE=fixed; broadcast; fcollect,
 * Bruck's steps; collect-scan and collect-exchange, collect's places by the
 * leads' scan of the totals and by their exchange (OSOE_COLLECTIVE_PLACES);
 * circulant, a reduction Träff's takes on the Pi's link set by hand; and
 * summary, two nodes and no walk, whose hang the launcher's timeout ends,
 * printing osoe-stat --summary's view of it.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
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

/* ---- the walk of a data collective (D1) ---------------------------------- */

enum data_call { REDUCE, BROADCAST, FCOLLECT, COLLECT };

struct data_case {
    const char *mode;
    enum data_call call;
    size_t count;               /* elements of a long */
    bool counts;                /* the walk may end at a count of several leads' */
    bool walk;                  /* false: the launcher's timeout shows the hang */
};

static const struct data_case data_cases[] = {
    { "reduce", REDUCE, 1, false, true },
    { "reduce-large", REDUCE, 8192, false, true },
    { "broadcast", BROADCAST, 4, false, true },
    { "fcollect", FCOLLECT, 1, false, true },
    { "collect-scan", COLLECT, 1, false, true },
    { "collect-exchange", COLLECT, 1, true, true },
    { "circulant", REDUCE, 131072, true, true },
    { "summary", REDUCE, 1, false, false },
};

static const struct data_case *walked;
static unsigned nodes, culprit;

/* Node `node`'s data collective on the world, by name. */
static int read_data(unsigned node, uint64_t *open, uint64_t *wait, uint64_t *next,
                     uint64_t *step)
{
    const char *job = osoe_test_launcher("JOB_ID"), *step_id = osoe_test_launcher("STEP_ID");
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    snprintf(name, sizeof name, "/osoe.%s.%s.%u.0", job ? job : "0", step_id ? step_id : "0",
             node);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "data walk: %s\n", err);
        return 0;
    }
    int ok = s.table_usable &&
             osoe_segment_read_name(&s, "team.tree.data_epoch_open", 0, open) &&
             osoe_segment_read_name(&s, "team.tree.data_wait", 0, wait) &&
             osoe_segment_read_name(&s, "team.tree.data_wait_node", 0, next) &&
             osoe_segment_read_name(&s, "team.tree.data_wait_step", 0, step);
    osoe_segment_close(&s);
    return ok;
}

enum walk_end { WALK_UNREAD = -1, WALK_MOVING, WALK_CULPRIT, WALK_COUNT };

/*
 * One walk from node 0: follow the node each lead names while it has the
 * collective open. It ends at a node whose lead is not in it, or waits for its
 * own members - where the culprit is, once the walk has settled - or at a
 * count, where the node names none; and anywhere else while the waits still
 * move.
 */
static enum walk_end data_walk_once(unsigned *at_out, unsigned *visits_out, uint64_t *missing,
                                    unsigned *path, int print)
{
    unsigned at = 0, visits = 0;
    *missing = 0;
    for (;;) {
        uint64_t open = 0, wait = 0, next = UINT32_MAX, step = 0;
        if (++visits > MAX_VISITS || !read_data(at, &open, &wait, &next, &step))
            return WALK_UNREAD;
        path[visits - 1] = at;
        if (print)
            printf("visit %u: node %u data_epoch_open %llu data_wait %llu data_wait_node %lld "
                   "data_wait_step %llu\n", visits, at, (unsigned long long)open,
                   (unsigned long long)wait,
                   next == UINT32_MAX ? -1LL : (long long)next, (unsigned long long)step);
        *at_out = at;
        *visits_out = visits;
        if (open != 0 && next != UINT32_MAX && next < nodes) {
            at = (unsigned)next;
            continue;
        }
        if (open == 0 || wait == OSOE_SEGMENT_DATA_WAIT_MEMBERS)
            return WALK_CULPRIT;
        *missing = step;
        return open != 0 && (wait == OSOE_SEGMENT_DATA_WAIT_GRANT ||
                             wait == OSOE_SEGMENT_DATA_WAIT_TOTALS)
                   ? WALK_COUNT
                   : WALK_MOVING;
    }
}

/* Whether every node's lead but the culprit's has the collective open, and the
 * culprit's has none. */
static bool every_other_open(void)
{
    for (unsigned node = 0; node < nodes; node++) {
        uint64_t open = 0, wait = 0, next = 0, step = 0;
        if (!read_data(node, &open, &wait, &next, &step) || (open != 0) != (node != culprit))
            return false;
    }
    return true;
}

/*
 * Walk until a walk ends where the hang is - at the culprit, or at a count -
 * every other node's lead in the collective, and the same walk again 100 ms
 * later: until every lead is in its last wait, a walk can take a longer way
 * round through waits about to end, which the visits counted would show.
 */
static void *walker(void *unused)
{
    (void)unused;
    struct timespec tick = { 0, 10 * 1000 * 1000 }, again = { 0, 100 * 1000 * 1000 };
    time_t give_up = time(NULL) + SETTLE_SECONDS;
    unsigned at = 0, visits = 0, path[MAX_VISITS], second[MAX_VISITS];
    uint64_t missing = 0;
    enum walk_end end;
    for (;;) {
        end = data_walk_once(&at, &visits, &missing, path, 0);
        if (((end == WALK_CULPRIT && at == culprit) || (end == WALK_COUNT && walked->counts)) &&
            every_other_open()) {
            unsigned at_again = 0, visits_again = 0;
            uint64_t missing_again = 0;
            nanosleep(&again, NULL);
            if (data_walk_once(&at_again, &visits_again, &missing_again, second, 0) == end &&
                visits_again == visits && !memcmp(path, second, visits * sizeof *path) &&
                missing_again == missing)
                break;
        }
        if (time(NULL) >= give_up) {
            data_walk_once(&at, &visits, &missing, path, 1);
            printf("data walk (%s): no walk settled in %d s\n", walked->mode, SETTLE_SECONDS);
            CHECK(!"a settled walk");
            fflush(stdout);
            _exit(1);
        }
        nanosleep(&tick, NULL);
    }
    /* The settled walk, printed: the visits a person with osoe-stat would make. */
    end = data_walk_once(&at, &visits, &missing, path, 1);
    if (end == WALK_CULPRIT)
        printf("data walk (%s) found node %u in %u visits, of %u nodes\n", walked->mode, at,
               visits, nodes);
    else
        printf("data walk (%s) ended at node %u in %u visits, a count still %llu short, every "
               "node's lead but node %u's in the collective\n", walked->mode, at, visits,
               (unsigned long long)missing, culprit);
    CHECK(end == WALK_CULPRIT ? at == culprit : end == WALK_COUNT && walked->counts);
    CHECK(visits <= 6);
    fflush(stdout);
    fflush(stderr);
    _exit(CHECK_DONE());
}

static int data_walk(const char *mode, int me, int n)
{
    for (size_t i = 0; i < sizeof data_cases / sizeof data_cases[0]; i++)
        if (!strcmp(mode, data_cases[i].mode))
            walked = &data_cases[i];
    CHECK_FATAL(walked != NULL);
    nodes = (unsigned)n;
    CHECK_FATAL(walked->walk ? n == NODES : n == 2);
    culprit = walked->walk ? CULPRIT : 1;
    size_t count = walked->count, total = walked->call == FCOLLECT || walked->call == COLLECT
                                              ? count * (size_t)n
                                              : count;
    long *source = shmem_malloc(count * sizeof *source);
    long *dest = shmem_malloc(total * sizeof *dest);
    CHECK_FATAL(source && dest);
    for (size_t i = 0; i < count; i++)
        source[i] = me + (long)i;
    shmem_barrier_all();

    if ((unsigned)me == culprit) {
        printf("PE %d never calls the %s\n", me, mode);
        fflush(stdout);
        sleep(600);
    }
    pthread_t thread;
    if (me == 0 && walked->walk)
        CHECK_FATAL(pthread_create(&thread, NULL, walker, NULL) == 0);
    switch (walked->call) {
    case REDUCE:
        shmem_long_sum_reduce(SHMEM_TEAM_WORLD, dest, source, count);
        break;
    case BROADCAST:
        shmem_long_broadcast(SHMEM_TEAM_WORLD, dest, source, count, 0);
        break;
    case FCOLLECT:
        shmem_long_fcollect(SHMEM_TEAM_WORLD, dest, source, count);
        break;
    case COLLECT:
        shmem_long_collect(SHMEM_TEAM_WORLD, dest, source, count);
        break;
    }
    printf("PE %d: the %s returned, which it cannot\n", me, mode);
    CHECK(!"the collective returned");
    return CHECK_DONE();
}

int main(int argc, char **argv)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    if (argc > 1)
        return data_walk(argv[1], me, n);
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
