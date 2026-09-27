/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A quiesced pair of nodes falls silent (§3.4; review round two, C1).
 *
 * One put and one quiet, then half a second in which nobody calls the library
 * at all. Every segment either node sends in that half second is overhead, and
 * after the first few there is nothing left for one to carry: the data is
 * applied, the quiet is satisfied, and the only thing a header-only segment
 * could still be doing is answering a request. What must not happen is an
 * answer that is itself a request, because then the pair answers each other
 * forever - about 2,500 segments a second per direction on this wire, measured
 * by the review, and on TCP that many packets from an idle job.
 *
 * The bound is not zero, and ack_model.py says why (docs/design/evidence/
 * am-backend/): a lead that receives a request must answer it, and cannot know
 * that the asking node's quiet was satisfied meanwhile by some other segment's
 * cumulative ack, so one last header-only segment into a quiesced pair is the
 * protocol working. The model needs at most three per node in its worst
 * interleaving. The check allows eight: tight against an echo, which makes
 * hundreds even on an oversubscribed VM, and loose against a scheduler.
 *
 * Counted from the node's own `transport.am.header_only_sent`, which exists for
 * this (node.h, layout 8): header-only segments have no other observable effect.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { IDLE_MS = 500, HEADER_ONLY_BOUND = 8 };

int main(void)
{
    shmem_init();
    int me = shmem_my_pe();
    long *x = shmem_malloc(sizeof *x);
    *x = 0;
    shmem_barrier_all();

    if (me == 0) {
        long v = 42;
        shmem_putmem(x, &v, sizeof v, 1);
        shmem_quiet();
    }

    uint64_t before = 0, after = 0;
    CHECK_FATAL(osoe_test_node_metric("transport.am.header_only_sent", &before));
    struct timespec idle = { 0, IDLE_MS * 1000000L };
    nanosleep(&idle, NULL);
    CHECK_FATAL(osoe_test_node_metric("transport.am.header_only_sent", &after));

    uint64_t sent = after - before;
    printf("PE %d: %llu header-only segments in %d ms of idleness\n", me,
           (unsigned long long)sent, IDLE_MS);
    CHECK(sent <= HEADER_ONLY_BOUND);

    shmem_barrier_all();
    if (me == 1)
        CHECK(*x == 42);
    CHECK_INVARIANTS();
    shmem_free(x);
    shmem_finalize();
    return CHECK_DONE();
}
