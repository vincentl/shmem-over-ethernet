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
 * (connection-area.h's `sleeping`), so every put finds it asleep, or about to
 * be, and must wake it: the owner the map names is woken in nearly every round,
 * and every other owner of the node not at all. And the put's own wake is the
 * one: PE 0's count of the wakes it claimed (am.wakes_sent) rises across nearly
 * every put. Each round's quiet wakes the same owner, so the owner's count
 * alone would pass a put whose wake was lost, or held back for good (the
 * adversarial read of this test, finding 2).
 *
 * Waiting for the flag is what lets this hold on a loaded machine (the review
 * of v0.2 step 4's merge point, "The verification"): without it, an owner
 * preempted between a round's acknowledgement and its sleep was still awake at
 * the next put, and beside four busy loops on four cores the test failed 9
 * runs of 20, as if the owner spun. A helper that does spin after raising its
 * flag - the "spin a little for latency" change, scale S1's mutants - is told
 * from one that sleeps only as far as the count can tell them apart. The count
 * is of the futex waits a wake ended, and a ring keeps its wait armed across a
 * sleep that a socket's completion ended, so a spinner that slept through the
 * round trip of its last acknowledgement has a wait left armed, which the put's
 * wake completes and counts, as it would a sleeper's. So this catches a spin
 * longer than that round trip - a 200 us spin at both helper counts, and at no
 * helpers a 30 us one - and a shorter one on some wires only (finding 1;
 * docs/decisions/2026-10-10-helper-wake.md has the counts). Counting the
 * quiet's wake too, made once the flag rose again after the put, told every
 * one of them apart on an idle machine, but beside four busy loops a correct
 * library fell short of its bound in 5 runs of 20, and this runs beside other
 * tests.
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
#include "launcher.h"

/* A put and a quiet a round, each put once the owner's flag is up. A put can
 * still find the owner awake - its sweep's timeout, a tenth of a second apart,
 * or a pass begun again because a bit was set as it raised its flag - so the
 * bound is nine rounds in ten, not all of them. */
enum { ROUNDS = 100, WOKEN_AT_LEAST = ROUNDS * 9 / 10, OTHER_AT_MOST = 2,
       CLAIMED_AT_LEAST = ROUNDS * 9 / 10 };

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

/* PE 0's count of the wakes it claimed (am.wakes_sent, its counter blocks
 * summed), through one mapping of its node's control segment, read around
 * each put - before PE 0 waits for the flag, so that the put follows the flag's
 * rise at once. */
static struct osoe_segment segment;
static const struct osoe_metric_descriptor *wakes_sent;

static void open_claims(void)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    if (osoe_segment_open(&segment, name, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        CHECK_FATAL(false);
    }
    wakes_sent = segment.table_usable ? osoe_segment_metric(&segment, "am.wakes_sent") : NULL;
    CHECK_FATAL(wakes_sent != NULL);
}

static uint64_t claimed(void)
{
    const char *local = osoe_test_launcher("LOCALID");
    uint32_t local_id = (uint32_t)atoi(local ? local : "0");
    uint64_t total = 0;
    for (uint32_t i = 0; i < segment.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&segment, wakes_sent, local_id * segment.dir->blocks_per_pe + i, &v))
            total += v;
    }
    return total;
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
        open_claims();
        for (int node = 1; node < npes; node++) {
            uint32_t slot = (uint32_t)node - 1, owner = helpers ? 1 + slot % helpers : 0;
            uint64_t before[HELPERS_MOST + 1], after[HELPERS_MOST + 1];
            for (uint32_t o = 0; o <= helpers; o++)
                before[o] = wakes(o);
            bool flag_rose = true;
            uint64_t put_claims = 0;
            for (long round = 1; round <= ROUNDS; round++) {
                uint64_t claims = claimed();
                if (flag_rose && !wait_for_flag(owner)) {
                    printf("node %d's wire, owner %u's: its flag not up in %ld s before put "
                           "%ld\n", node, owner, FLAG_WAIT_NS / 1000000000, round);
                    flag_rose = false;
                }
                shmem_putmem(x, &round, sizeof round, node);
                put_claims += claimed() - claims;
                shmem_quiet();
            }
            for (uint32_t o = 0; o <= helpers; o++)
                after[o] = wakes(o);
            CHECK(flag_rose);
            printf("node %d's wire, owner %u's: PE 0 claimed a wake at %llu of %d puts\n", node,
                   owner, (unsigned long long)put_claims, ROUNDS);
            CHECK(put_claims >= CLAIMED_AT_LEAST);
            /* Every owner judged: with helpers the agent holds no data wire,
             * and nothing PE 0 does wakes it. */
            for (uint32_t o = 0; o <= helpers; o++) {
                uint64_t woken = after[o] - before[o];
                printf("node %d's wire, owner %u's: owner %u woken %llu times in %d puts\n",
                       node, owner, o, (unsigned long long)woken, ROUNDS);
                CHECK(o == owner ? woken >= WOKEN_AT_LEAST : woken <= OTHER_AT_MOST);
            }
        }
        osoe_segment_close(&segment);
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
