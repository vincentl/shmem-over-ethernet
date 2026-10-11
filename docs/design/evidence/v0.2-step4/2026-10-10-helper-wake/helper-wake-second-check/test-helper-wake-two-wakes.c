/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A PE's wake reaches the owner of its wire (v0.2 step 4, Part 3; owner-map.h;
 * lead_wake_model.py's mark_wake_apart).
 *
 * With progress helpers, a PE that commits into a connection marks the
 * connection's dirty bit in its owner's bitmap and wakes that owner if it
 * sleeps (osoe_connection_area_mark, osoe_progress_host_wake). A wake that went
 * to another owner would show nowhere but in latency: the owner that holds the
 * wire still finds the bytes at its next sweep, a tenth of a second on. So this
 * counts each owner's wakes - the futex completions on its ring that saw its
 * wake word move (transport.am.wakes, layout 36) - while PE 0, alone on its
 * node, puts to each peer node in turn with a quiet after each put, and every
 * other PE waits on a word of its own. Before each put PE 0 waits for the owner
 * of that peer's wire to raise the flag it raises before it sleeps
 * (connection-area.h's `sleeping`), so the put finds it asleep, or about to be,
 * and must wake it; and again before the quiet, whose request for an
 * acknowledgement wakes the same owner, once the owner has sent the put and
 * raised its flag again. So the owner the map names is woken twice a round,
 * and every other owner of the node not at all.
 *
 * Waiting for the flag is what lets this hold on a loaded machine (the review
 * of v0.2 step 4's merge point, "The verification"): without it, an owner
 * preempted between a round's acknowledgement and its sleep was still awake at
 * the next put, and beside four busy loops on four cores the test failed 9
 * runs of 20, as if the owner spun. And the two wakes a round are what tell an
 * owner that sleeps from one that spins after raising its flag - the "spin a
 * little for latency" change, scale S1's mutants. The count is of the futex
 * waits a wake ended, and a ring keeps its wait armed across a sleep that a
 * socket's completion ended, so a spinner that slept through the round trip
 * of its last acknowledgement has a wait left armed, which the put's wake
 * completes and counts, as it would a sleeper's: one wake alone could not
 * tell them apart. The quiet's wake comes while PE 0 polls, within a
 * microsecond or two of the flag's rise on an idle machine, so it lands in the
 * spinner's spin, with no wait armed, and counts nothing; a sleeper's counts.
 * A put whose own wake is lost, or goes to another owner, leaves the round one
 * wake too (the adversarial read of this test, findings 1 and 2).
 *
 * Run on four nodes of one PE each, at OSOE_PROGRESS_HELPERS=2: node 0's peers
 * are nodes 1, 2 and 3 in slots 0, 1 and 2, whose wires are helper 1's, helper
 * 2's and helper 1's. And at no helpers, where the lead's agent, owner 0, holds
 * every wire and raises the same flag before its idle wait, which a spin there
 * would show as one in a helper does (scale S1's second mutant). The deadlock
 * detector, whose requests nudge every owner, is off (OSOE_DEADLOCK_SECONDS=0).
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "base/cacheline.h"
#include "check.h"
#include "invariants.h"

/* A put and a quiet a round, each once the owner's flag is up: two wakes. A
 * wake can still find the owner awake - its sweep's timeout, a tenth of a
 * second apart, or a pass begun again because a bit was set as it raised its
 * flag - so the bound is nine in ten, not all of them; and a spinner's one a
 * round is half. */
enum { ROUNDS = 100, WOKEN_AT_LEAST = 2 * ROUNDS * 9 / 10, OTHER_AT_MOST = 2 };

/* How long PE 0 waits for an owner's flag before it counts the owner as one
 * that never sleeps: far past any preemption. */
static const long FLAG_WAIT_NS = 10L * 1000 * 1000 * 1000;

/* Each owner's flag where this process maps its node's connection area. The
 * object's name is gone by now (§3.14: unlinked once every PE of the node has
 * it open), so the mapping is found in /proc/self/maps, and the words in it at
 * the offsets connection-area.c asserts for this test: the layout at byte 8,
 * the helpers at the fourth line and the owners' offset after them, an owner a
 * line. */
enum { AREA_LAYOUT = 5, HELPERS_MOST = 64 };
static uint32_t helpers;
static _Atomic uint32_t *flag[HELPERS_MOST + 1];

static void find_flags(void)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    CHECK_FATAL(maps != NULL);
    char line[1024];
    unsigned char *area = NULL;
    while (!area && fgets(line, sizeof line, maps)) {
        unsigned long start, offset;
        if (strstr(line, "/dev/shm/osoe.") && strstr(line, ".conn.") &&
            sscanf(line, "%lx-%*x %*s %lx", &start, &offset) == 2 && offset == 0)
            area = (unsigned char *)start;
    }
    fclose(maps);
    CHECK_FATAL(area != NULL);
    uint64_t magic, owners_offset;
    uint32_t layout;
    memcpy(&magic, area, sizeof magic);
    memcpy(&layout, area + 8, sizeof layout);
    memcpy(&helpers, area + 3 * OSOE_CACHELINE, sizeof helpers);
    memcpy(&owners_offset, area + 3 * OSOE_CACHELINE + 8, sizeof owners_offset);
    CHECK_FATAL(magic == UINT64_C(0x61657261656f736f) && layout == AREA_LAYOUT);
    CHECK_FATAL(helpers <= HELPERS_MOST);
    for (uint32_t o = 0; o <= helpers; o++)
        flag[o] = (_Atomic uint32_t *)(area + owners_offset + (uint64_t)o * OSOE_CACHELINE);
}

static long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}

/* Until owner `owner` has raised its flag; false if it never does. PE 0
 * yields rather than sleeps between looks, so that it sees the flag within a
 * microsecond or two of its rise on an idle machine - inside even a short spin
 * after it - and still lets the owner run on a loaded one. */
static bool wait_for_flag(uint32_t owner)
{
    long deadline = now_ns() + FLAG_WAIT_NS;
    while (atomic_load_explicit(flag[owner], memory_order_seq_cst) == 0) {
        if (now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

static uint64_t wakes(uint32_t owner)
{
    uint64_t value = 0;
    CHECK_FATAL(osoe_test_scoped_metric("transport.am.wakes", OSOE_MSCOPE_OWNER, owner, &value));
    return value;
}

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    long *x = shmem_malloc(sizeof *x), *done = shmem_malloc(sizeof *done);
    *x = 0;
    *done = 0;
    CHECK_FATAL(npes == 4);
    shmem_barrier_all();

    if (me == 0) {
        find_flags();
        for (int node = 1; node < npes; node++) {
            uint32_t slot = (uint32_t)node - 1, owner = helpers ? 1 + slot % helpers : 0;
            uint64_t before[HELPERS_MOST + 1], after[HELPERS_MOST + 1];
            for (uint32_t o = 0; o <= helpers; o++)
                before[o] = wakes(o);
            bool flag_rose = true;
            for (long round = 1; round <= ROUNDS; round++) {
                if (flag_rose && !wait_for_flag(owner)) {
                    printf("node %d's wire, owner %u's: its flag not up in %ld s before put "
                           "%ld\n", node, owner, FLAG_WAIT_NS / 1000000000, round);
                    flag_rose = false;
                }
                shmem_putmem(x, &round, sizeof round, node);
                if (flag_rose && !wait_for_flag(owner)) {
                    printf("node %d's wire, owner %u's: its flag not up in %ld s before quiet "
                           "%ld\n", node, owner, FLAG_WAIT_NS / 1000000000, round);
                    flag_rose = false;
                }
                shmem_quiet();
            }
            for (uint32_t o = 0; o <= helpers; o++)
                after[o] = wakes(o);
            CHECK(flag_rose);
            /* Every owner judged: with helpers the agent holds no data wire,
             * and nothing PE 0 does wakes it. */
            for (uint32_t o = 0; o <= helpers; o++) {
                uint64_t woken = after[o] - before[o];
                printf("node %d's wire, owner %u's: owner %u woken %llu times in %d rounds\n",
                       node, owner, o, (unsigned long long)woken, ROUNDS);
                CHECK(o == owner ? woken >= WOKEN_AT_LEAST : woken <= OTHER_AT_MOST);
            }
        }
        long one = 1;
        for (int node = 1; node < npes; node++)
            shmem_putmem(done, &one, sizeof one, node);
        shmem_quiet();
    } else {
        /* A wait on this PE's own word, which sends nothing: the barrier's
         * arrivals would reach node 0 inside the first window. */
        shmem_long_wait_until(done, SHMEM_CMP_EQ, 1);
    }

    shmem_barrier_all();
    if (me != 0)
        CHECK(*x == ROUNDS);
    CHECK_INVARIANTS();
    shmem_free(done);
    shmem_free(x);
    shmem_finalize();
    return CHECK_DONE();
}
