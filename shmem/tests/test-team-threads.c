/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Two splits at once on one PE, under SHMEM_THREAD_MULTIPLE (the team
 * redesign's step D, docs/design/team-resources-proposal.md D7, as redesigned
 * after the adversarial read of 2026-10-03; tools/modelcheck/
 * split_numbers_model.py). A PE's free mask is one resource, and two
 * reductions reading it at once can agree the same number: each member takes
 * its agreed numbers pending, waiting only for a split of a higher parent
 * number, and a second reduction keeps them or runs the round again.
 *
 * Contention: two threads of each PE split two parents together, ROUNDS times
 * - the world, number 0, and the world again, a higher number - starting
 * together, so that both read their masks before either keeps a number and
 * agree the same one. Both succeed on every PE, and every team made is checked
 * by a sum over it, each thread reducing over its own team while the other
 * does, and destroyed by its thread, the two at once.
 *
 * The read's program, on three PEs or more: PE 0's two threads split parent
 * {0, 2} at once and parent {0, 1} a little later; PE 1 splits {0, 1} and then
 * syncs a team {1, 2}; PE 2 syncs {1, 2} and then splits {0, 2}. The first
 * design, a mask token held across the split's reduction, deadlocked it: PE
 * 0's split of {0, 2} held the token waiting for PE 2, which waited in the
 * sync for PE 1, which ran the busy rounds of {0, 1} for ever. Here both
 * splits succeed, and the split of {0, 2}, whose mask on PE 0 was read before
 * the split of {0, 1} took the same number, loses its first round to it -
 * PE 0's shmem.team_split.lost_rounds says so.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

enum { ROUNDS = 12, LATER_MS = 100 };

static int me, npes;
static long *source, *dest;     /* [2]: each split's thread its own */
static pthread_barrier_t start;

struct split_job {
    shmem_team_t parent;
    int index;                  /* which of source and dest */
    long delay_ms;              /* before the call, in the read's program */
    bool together;              /* contention: wait at `start` first */
    shmem_team_t made;
    int status;
    long sum;
};

static void sleep_milliseconds(long milliseconds)
{
    struct timespec ts = { milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
}

/* Split the job's parent whole, and sum over the team made. */
static void run(struct split_job *job)
{
    if (job->together)
        pthread_barrier_wait(&start);
    sleep_milliseconds(job->delay_ms);
    int size = shmem_team_n_pes(job->parent);
    job->status = shmem_team_split_strided(job->parent, 0, 1, size, NULL, 0, &job->made);
    if (job->status == 0 && job->made != SHMEM_TEAM_INVALID) {
        source[job->index] = me + 1;
        dest[job->index] = 0;
        if (shmem_long_sum_reduce(job->made, &dest[job->index], &source[job->index], 1) == 0)
            job->sum = dest[job->index];
    }
}

static void *split_thread(void *argument)
{
    run(argument);
    return NULL;
}

static void *destroy_thread(void *argument)
{
    shmem_team_destroy(*(shmem_team_t *)argument);
    return NULL;
}

/* The sum of world rank + 1 over the PEs of team `t`. */
static long sum_of(shmem_team_t t)
{
    long sum = 0;
    for (int r = 0; r < shmem_team_n_pes(t); r++)
        sum += shmem_team_translate_pe(t, r, SHMEM_TEAM_WORLD) + 1;
    return sum;
}

static void check_job(const char *what, const struct split_job *job)
{
    long want = job->status == 0 ? sum_of(job->parent) : 0;
    if (job->status != 0 || job->made == SHMEM_TEAM_INVALID || job->sum != want)
        fprintf(stderr, "PE %d: %s: the split returned %d, its team's sum %ld, not %ld\n", me,
                what, job->status, job->sum, want);
    CHECK(job->status == 0 && job->made != SHMEM_TEAM_INVALID);
    CHECK(job->sum == want);
}

/* This PE's total for a thread-scope counter, summed over its blocks. */
static uint64_t my_counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    const char *local = osoe_test_launcher("LOCALID");
    uint64_t total = UINT64_MAX;
    if (m && local) {
        uint32_t id = (uint32_t)strtoul(local, NULL, 10);
        total = 0;
        for (uint32_t i = 0; i < s.dir->blocks_per_pe; i++) {
            uint64_t v;
            if (osoe_segment_read(&s, m, id * s.dir->blocks_per_pe + i, &v))
                total += v;
        }
    }
    osoe_segment_close(&s);
    return total;
}

/* Contention: the world and the world again, split together ROUNDS times. */
static void contention(void)
{
    shmem_team_t again = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &again) == 0);
    for (int round = 0; round < ROUNDS; round++) {
        struct split_job jobs[2] = {
            { .parent = SHMEM_TEAM_WORLD, .index = 0, .together = true },
            { .parent = again, .index = 1, .together = true },
        };
        pthread_t threads[2];
        for (int i = 0; i < 2; i++) {
            jobs[i].made = SHMEM_TEAM_INVALID;
            jobs[i].status = -1;
            CHECK_FATAL(pthread_create(&threads[i], NULL, split_thread, &jobs[i]) == 0);
        }
        for (int i = 0; i < 2; i++)
            CHECK_FATAL(pthread_join(threads[i], NULL) == 0);
        char what[64];
        for (int i = 0; i < 2; i++) {
            snprintf(what, sizeof what, "round %d, split %d", round, i);
            check_job(what, &jobs[i]);
        }
        /* Each team destroyed by a thread of its own, the two at once. */
        for (int i = 0; i < 2; i++)
            CHECK_FATAL(pthread_create(&threads[i], NULL, destroy_thread, &jobs[i].made) == 0);
        for (int i = 0; i < 2; i++)
            CHECK_FATAL(pthread_join(threads[i], NULL) == 0);
        shmem_barrier_all();
    }
    shmem_team_destroy(again);
}

/* The read's program, on PEs 0, 1 and 2. */
static void crossed(void)
{
    shmem_team_t low = SHMEM_TEAM_INVALID, high = SHMEM_TEAM_INVALID, other = SHMEM_TEAM_INVALID;
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, 2, NULL, 0, &low) == 0);
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 2, 2, NULL, 0, &high) == 0);
    CHECK_FATAL(shmem_team_split_strided(SHMEM_TEAM_WORLD, 1, 1, 2, NULL, 0, &other) == 0);
    uint64_t lost = my_counter("shmem.team_split.lost_rounds");
    struct split_job of_high = { .parent = high, .index = 0, .made = SHMEM_TEAM_INVALID,
                                 .status = -1 };
    struct split_job of_low = { .parent = low, .index = 1, .made = SHMEM_TEAM_INVALID,
                                .status = -1 };
    if (me == 0) {
        of_low.delay_ms = LATER_MS;
        pthread_t threads[2];
        CHECK_FATAL(pthread_create(&threads[0], NULL, split_thread, &of_high) == 0);
        CHECK_FATAL(pthread_create(&threads[1], NULL, split_thread, &of_low) == 0);
        for (int i = 0; i < 2; i++)
            CHECK_FATAL(pthread_join(threads[i], NULL) == 0);
        check_job("the split of {0, 2}", &of_high);
        check_job("the split of {0, 1}", &of_low);
        uint64_t now = my_counter("shmem.team_split.lost_rounds");
        fprintf(stderr, "PE 0: the read's program: %llu rounds lost\n",
                (unsigned long long)(now - lost));
        CHECK(now != UINT64_MAX && now > lost);
    } else if (me == 1) {
        run(&of_low);
        check_job("the split of {0, 1}", &of_low);
        shmem_team_sync(other);
    } else if (me == 2) {
        shmem_team_sync(other);
        run(&of_high);
        check_job("the split of {0, 2}", &of_high);
    }
    shmem_team_destroy(of_high.made);
    shmem_team_destroy(of_low.made);
    shmem_barrier_all();
    shmem_team_destroy(other);
    shmem_team_destroy(high);
    shmem_team_destroy(low);
}

int main(void)
{
    int provided = -1;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    npes = shmem_n_pes();
    source = shmem_malloc(2 * sizeof *source);
    dest = shmem_malloc(2 * sizeof *dest);
    CHECK_FATAL(source && dest);
    CHECK_FATAL(pthread_barrier_init(&start, NULL, 2) == 0);

    if (npes >= 3)
        crossed();
    shmem_barrier_all();
    contention();

    uint64_t calls = my_counter("shmem.team_split.calls");
    uint64_t lost = my_counter("shmem.team_split.lost_rounds");
    if (me == 0)
        fprintf(stderr, "PE 0: %llu splits, %llu rounds lost to another split's number\n",
                (unsigned long long)calls, (unsigned long long)lost);
    CHECK(calls != UINT64_MAX && lost != UINT64_MAX);
    pthread_barrier_destroy(&start);
    shmem_free(dest);
    shmem_free(source);
    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
