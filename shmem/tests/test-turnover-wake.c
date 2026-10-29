/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The turnover's wake (P2; design §3.11, base/wait-word.h): every PE of node 0
 * but none of node 1 puts to the one PE of node 1 at once, so that the
 * connection's buffer is full and every turnover has writers asleep for its
 * room - the release fixes' review's S1, which H12 measured on the cluster.
 * OSOE_TURNOVER_WAKE=all, the default, wakes every writer at an open and hands
 * nothing on; hand-on wakes one, and the writers wake one another as the room
 * allows; room wakes as many as the room holds messages of a chunk, and a
 * woken writer that took less hands on (transport-internode.c,
 * reserve_waiting).
 *
 * Each writer puts its 1 MiB slice whole, 16 times - messages of a chunk - or,
 * given `small`, the slice's first 64 KiB eight bytes a put, twice: messages
 * far smaller than a chunk, a room of which holds more writers than the room
 * count counts, so that under room the woken writers hand on.
 *
 * Checked by counts, not by time: every byte where its last put left it, no
 * wake lost (CHECK_INVARIANTS), and on node 0 - summed over its counter blocks
 * once the writers are done - no hand-on and no pass under all, and some
 * hand-ons under hand-on, and under room with small puts, once the writers
 * have waited for room often enough that one of them must have reserved with
 * others asleep (STALLS_ENOUGH). The waits, hand-ons and passes are printed,
 * and not otherwise asserted: how often a writer meets a full buffer is the
 * scheduler's.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"

enum { SLICE = 1 << 20, ROUNDS = 16, SMALL_SLICE = 1 << 16, SMALL_ROUNDS = 2,
       STALLS_ENOUGH = 64 };

/* Byte j of writer w's slice as round r puts it. */
static unsigned char pattern(int w, int r, size_t j)
{
    return (unsigned char)((size_t)w * 31 + (size_t)r * 7 + j);
}

/* Word j of writer w's slice as round r puts it, eight bytes a put. */
static uint64_t small_pattern(int w, int r, size_t j)
{
    return (uint64_t)w << 40 | (uint64_t)r << 32 | (uint64_t)j;
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

int main(int argc, char **argv)
{
    bool small = argc > 1 && strcmp(argv[1], "small") == 0;
    shmem_init();
    int me = shmem_my_pe(), n = shmem_n_pes();
    CHECK_FATAL(n >= 3);
    /* The node map puts every PE but the last on node 0. */
    int target = n - 1, writers = n - 1;
    unsigned char *slices = shmem_malloc((size_t)writers * SLICE);
    unsigned char *source = malloc(SLICE);
    CHECK_FATAL(slices != NULL && source != NULL);
    shmem_barrier_all();

    if (me != target && !small)
        for (int r = 0; r < ROUNDS; r++) {
            for (size_t j = 0; j < SLICE; j++)
                source[j] = pattern(me, r, j);
            shmem_putmem(slices + (size_t)me * SLICE, source, SLICE, target);
            shmem_fence();
        }
    if (me != target && small) {
        uint64_t *mine = (uint64_t *)(void *)(slices + (size_t)me * SLICE);
        for (int r = 0; r < SMALL_ROUNDS; r++) {
            for (size_t j = 0; j < SMALL_SLICE / sizeof(uint64_t); j++)
                shmem_uint64_p(&mine[j], small_pattern(me, r, j), target);
            shmem_fence();
        }
    }
    shmem_barrier_all();

    if (me == target) {
        size_t wrong = 0;
        for (int w = 0; w < writers; w++)
            if (!small)
                for (size_t j = 0; j < SLICE; j++)
                    wrong += slices[(size_t)w * SLICE + j] != pattern(w, ROUNDS - 1, j);
            else {
                const uint64_t *words = (const uint64_t *)(void *)(slices + (size_t)w * SLICE);
                for (size_t j = 0; j < SMALL_SLICE / sizeof(uint64_t); j++)
                    wrong += words[j] != small_pattern(w, SMALL_ROUNDS - 1, j);
            }
        CHECK(wrong == 0);
    }
    if (me == 0) {
        const char *setting = getenv("OSOE_TURNOVER_WAKE");
        const char *wake = setting && (strcmp(setting, "hand-on") == 0 ||
                                       strcmp(setting, "room") == 0) ? setting : "all";
        uint64_t stalls = 0, hand_ons = 0, passes = 0;
        CHECK(thread_total("am.reserve_stalls", &stalls));
        CHECK(thread_total("am.turnover_hand_ons", &hand_ons));
        CHECK(thread_total("am.turnover_passes", &passes));
        printf("turnover wake %s: %d writers, %s, %llu waits for room, %llu hand-ons, "
               "%llu passes\n", wake, writers,
               small ? "64 KiB each in 8-byte puts, twice" : "1 MiB each, 16 times",
               (unsigned long long)stalls, (unsigned long long)hand_ons,
               (unsigned long long)passes);
        bool hands_on = strcmp(wake, "hand-on") == 0 || (strcmp(wake, "room") == 0 && small);
        if (strcmp(wake, "all") == 0)
            CHECK(hand_ons == 0 && passes == 0);
        else if (hands_on && stalls >= STALLS_ENOUGH)
            CHECK(hand_ons > 0);
        else if (hands_on)
            printf("turnover wake %s: too few waits (%llu of %d) to ask for a hand-on\n", wake,
                   (unsigned long long)stalls, STALLS_ENOUGH);
    }

    CHECK_INVARIANTS();
    free(source);
    shmem_free(slices);
    shmem_finalize();
    return CHECK_DONE();
}
