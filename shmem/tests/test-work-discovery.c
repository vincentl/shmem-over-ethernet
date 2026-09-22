/*
 * The send side finds its work without visiting every connection (§3.10;
 * phase 1 step 2 slice 3, part C).
 *
 * Sixteen logical nodes, one PE each, and traffic on one connection only: PE 0
 * puts a burst to PE 1 while every other node idles. On node 0 the lead's
 * engine then counts its passes and the connections each pass drained and
 * flushed (transport.am.passes, transport.am.send_visits). Before the
 * dirty-connection set every pass visited all 15; now a pass visits the ones a
 * writer marked or that the lead knows are busy, so over the burst the ratio
 * must stay near one - it is checked at no more than two, which leaves room for
 * the acknowledgement the quiet asks of node 1 and nothing like the fifteen of
 * a full sweep.
 *
 * It also reports the writer's side of the wake (am.wakes_sent against
 * am.wakes_skipped): with the lead awake through a burst most wakes are
 * skipped, which is where the futex syscall per put went. That is printed,
 * not asserted - how often the lead sleeps between puts is the scheduler's.
 *
 * Receiving still polls every wire on the step-2 wire (§3.10); that is not
 * what this measures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { BURST = 20000 };

/* A thread-scope counter summed over this node's counter blocks. */
static int thread_total(const char *metric, uint64_t *total)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", metric, err);
        return 0;
    }
    const struct osoe_metric_descriptor *m = s.table_usable ? osoe_segment_metric(&s, metric) : NULL;
    int ok = m && m->scope == OSOE_MSCOPE_THREAD;
    *total = 0;
    for (uint32_t b = 0; ok && b < s.dir->block_count; b++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, b, &v))
            *total += v;
    }
    if (!ok)
        fprintf(stderr, "%s: not a thread-scope metric of %s\n", metric, s.name);
    osoe_segment_close(&s);
    return ok;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    CHECK_FATAL(n >= 16);
    long *target = shmem_malloc(sizeof *target);
    CHECK_FATAL(target != NULL);
    shmem_barrier_all();

    uint64_t passes0 = 0, visits0 = 0, sent0 = 0, skipped0 = 0;
    if (me == 0) {
        CHECK(osoe_test_node_metric("transport.am.passes", &passes0));
        CHECK(osoe_test_node_metric("transport.am.send_visits", &visits0));
        CHECK(thread_total("am.wakes_sent", &sent0));
        CHECK(thread_total("am.wakes_skipped", &skipped0));
        for (long i = 0; i < BURST; i++)
            shmem_putmem(target, &i, sizeof i, 1);
        shmem_quiet();

        uint64_t passes1 = 0, visits1 = 0, sent1 = 0, skipped1 = 0;
        CHECK(osoe_test_node_metric("transport.am.passes", &passes1));
        CHECK(osoe_test_node_metric("transport.am.send_visits", &visits1));
        CHECK(thread_total("am.wakes_sent", &sent1));
        CHECK(thread_total("am.wakes_skipped", &skipped1));
        uint64_t passes = passes1 - passes0, visits = visits1 - visits0;
        printf("work discovery: %llu passes visited %llu connections (%.2f per pass, of %d); "
               "%llu wakes sent, %llu skipped over %d puts\n",
               (unsigned long long)passes, (unsigned long long)visits,
               passes ? (double)visits / (double)passes : 0.0, n - 1,
               (unsigned long long)(sent1 - sent0), (unsigned long long)(skipped1 - skipped0),
               BURST);
        CHECK(passes > 0);
        CHECK(visits <= 2 * passes);
        /* Every put asked for the lead, one way or the other. */
        CHECK((sent1 - sent0) + (skipped1 - skipped0) >= BURST);
    }
    shmem_barrier_all();
    if (me == 1)
        CHECK(*target == BURST - 1);

    CHECK_INVARIANTS();
    shmem_free(target);
    shmem_finalize();
    return CHECK_DONE();
}
