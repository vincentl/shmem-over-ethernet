/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * No owner spins before it raises the flag it raises before it sleeps (T38;
 * round nine's scale S4; docs/decisions/2026-10-27-t38-wake-cost.md).
 *
 * shmem.helper-wake's counts cannot see such a spin: a flag raised late still
 * finds the writer that wakes its owner, and every count is right while each
 * idle pass costs the spin. Nor can a bound on the CPU a wake costs, measured
 * for T38: the as-built cost moves sevenfold to twentyfold with the machine's
 * load, a fixed bound had 1.15 times of room. What does see it is the time from
 * an idle pass's end to the flag's raise, which as built is a store or two:
 * each owner counts the raises later than a few microseconds
 * (transport.am.late_flags, node layout 42), and a spin makes one at each idle
 * pass it polls to its end - at least one a wake - where as built one needs the
 * owner preempted in that window.
 *
 * Four nodes of one PE each. PE 0 puts to nodes 1, 2 and 3 in turn, PUTS puts
 * GAP apart, so that every owner of every node is asleep at each put and woken
 * by it - node 0's by PE 0's marks of its dirty bits, which its wake count
 * counts, and the others' by their sockets, which it does not - and each
 * node's lead reads its owners' wakes, passes and late flags around the
 * trickle. Over node 0's wakes, and the puts each other node received, a
 * node's owners together must have raised at most one flag late in four - a
 * spin raises at least one for each - and passed at most twelve times, where
 * T38 measured five as built and with the spin: a spin made of passes would be
 * hundreds. Run at two helpers and at none, where the lead's agent, owner 0,
 * holds every wire and raises the same flag. The deadlock detector, whose
 * requests nudge every owner, is off (OSOE_DEADLOCK_SECONDS=0).
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { OWNERS_MOST = 65, PUTS = 300, GAP_US = 2000, PASSES_A_WAKE_MOST = 12 };

static struct osoe_segment segment;
static uint32_t owners;

struct counts {
    uint64_t wakes, passes, late_flags;
};

static void sleep_ns(long ns)
{
    struct timespec t = { ns / 1000000000L, ns % 1000000000L };
    while (nanosleep(&t, &t) != 0)
        ;
}

static uint64_t owner_metric(const char *name, uint32_t owner)
{
    uint64_t value = 0;
    CHECK_FATAL(osoe_segment_read_name(&segment, name, owner, &value));
    return value;
}

/* This node's owners' counts, summed. */
static struct counts take(void)
{
    struct counts c = { 0, 0, 0 };
    for (uint32_t o = 0; o < owners; o++) {
        c.wakes += owner_metric("transport.am.wakes", o);
        c.passes += owner_metric("transport.am.passes", o);
        c.late_flags += owner_metric("transport.am.late_flags", o);
    }
    return c;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    long *x = shmem_malloc(sizeof *x), *done = shmem_malloc(sizeof *done);
    CHECK_FATAL(x != NULL && done != NULL && npes == 4);
    *x = 0;
    *done = 0;
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    if (osoe_segment_open(&segment, name, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK_FATAL(false);
    }
    CHECK_FATAL(segment.table_usable);
    owners = segment.owner_count;
    CHECK_FATAL(owners >= 1 && owners <= OWNERS_MOST);
    shmem_barrier_all();

    /* Settled first: the barrier's own traffic, and its owners asleep. */
    sleep_ns(50L * 1000 * 1000);
    struct counts before = take(), after;
    if (me == 0) {
        for (long i = 1; i <= PUTS; i++) {
            sleep_ns(GAP_US * 1000L);
            shmem_putmem(x, &i, sizeof i, 1 + (int)((i - 1) % (npes - 1)));
        }
        shmem_quiet();
        sleep_ns(50L * 1000 * 1000);
        after = take();
        long one = 1;
        for (int node = 1; node < npes; node++)
            shmem_putmem(done, &one, sizeof one, node);
        shmem_quiet();
    } else {
        shmem_long_wait_until(done, SHMEM_CMP_EQ, 1);
        after = take();
    }
    uint64_t wakes = after.wakes - before.wakes, passes = after.passes - before.passes;
    uint64_t late = after.late_flags - before.late_flags;
    /* What woke node 0's owners is counted; the others', their sockets, is
     * not, and each received a third of the puts. */
    uint64_t woken = me == 0 ? wakes : (uint64_t)(PUTS / (npes - 1));
    printf("late flags: node %d's %u owners woke %llu times by their wake words, %llu in all, "
           "passed %llu times (%.2f each) and raised %llu flags late\n", me, owners,
           (unsigned long long)wakes, (unsigned long long)woken, (unsigned long long)passes,
           woken ? (double)passes / (double)woken : 0.0, (unsigned long long)late);
    fflush(stdout);
    CHECK(woken > 0);
    CHECK(late * 4 <= woken);
    CHECK(passes <= PASSES_A_WAKE_MOST * woken);

    shmem_barrier_all();
    osoe_segment_close(&segment);
    shmem_free(done);
    shmem_free(x);
    shmem_finalize();
    return CHECK_DONE();
}
