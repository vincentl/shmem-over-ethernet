/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The turnover's wake (P2; design §3.11, base/wait-word.h): every PE of node 0
 * but none of node 1 puts to the one PE of node 1 at once, so that the
 * connection's buffer is full and every turnover has writers asleep for its
 * room - the release fixes' review's S1, which H12 measured on the cluster.
 * OSOE_TURNOVER_WAKE=all, the default, wakes every writer at an open and hands
 * nothing on; hand-on wakes one, and the writers wake one another as the room
 * allows (transport-internode.c, reserve_waiting).
 *
 * Checked by counts, not by time: every byte where its last put left it, no
 * wake lost (CHECK_INVARIANTS), and on node 0 - summed over its counter blocks
 * once the writers are done - no hand-on and no pass under all, and under
 * hand-on some hand-ons once the writers have waited for room often enough
 * that one of them must have reserved with others asleep (STALLS_ENOUGH). The
 * waits, hand-ons and passes are printed, and not otherwise asserted: how
 * often a writer meets a full buffer is the scheduler's.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SLICE = 1 << 20, ROUNDS = 16, STALLS_ENOUGH = 64 };

/* Byte j of writer w's slice as round r puts it. */
static unsigned char pattern(int w, int r, size_t j)
{
    return (unsigned char)((size_t)w * 31 + (size_t)r * 7 + j);
}

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
    CHECK_FATAL(n >= 3);
    /* The node map puts every PE but the last on node 0. */
    int target = n - 1, writers = n - 1;
    unsigned char *slices = shmem_malloc((size_t)writers * SLICE);
    unsigned char *source = malloc(SLICE);
    CHECK_FATAL(slices != NULL && source != NULL);
    shmem_barrier_all();

    if (me != target)
        for (int r = 0; r < ROUNDS; r++) {
            for (size_t j = 0; j < SLICE; j++)
                source[j] = pattern(me, r, j);
            shmem_putmem(slices + (size_t)me * SLICE, source, SLICE, target);
            shmem_fence();
        }
    shmem_barrier_all();

    if (me == target) {
        size_t wrong = 0;
        for (int w = 0; w < writers; w++)
            for (size_t j = 0; j < SLICE; j++)
                wrong += slices[(size_t)w * SLICE + j] != pattern(w, ROUNDS - 1, j);
        CHECK(wrong == 0);
    }
    if (me == 0) {
        const char *setting = getenv("OSOE_TURNOVER_WAKE");
        bool hand_on = setting && strcmp(setting, "hand-on") == 0;
        uint64_t stalls = 0, hand_ons = 0, passes = 0;
        CHECK(thread_total("am.reserve_stalls", &stalls));
        CHECK(thread_total("am.turnover_hand_ons", &hand_ons));
        CHECK(thread_total("am.turnover_passes", &passes));
        printf("turnover wake %s: %d writers, %d MiB each, %llu waits for room, "
               "%llu hand-ons, %llu passes\n", hand_on ? "hand-on" : "all", writers,
               ROUNDS * SLICE >> 20, (unsigned long long)stalls,
               (unsigned long long)hand_ons, (unsigned long long)passes);
        if (!hand_on)
            CHECK(hand_ons == 0 && passes == 0);
        else if (stalls >= STALLS_ENOUGH)
            CHECK(hand_ons > 0);
        else
            printf("turnover wake hand-on: too few waits (%llu of %d) to ask for a hand-on\n",
                   (unsigned long long)stalls, STALLS_ENOUGH);
    }

    CHECK_INVARIANTS();
    free(source);
    shmem_free(slices);
    shmem_finalize();
    return CHECK_DONE();
}
