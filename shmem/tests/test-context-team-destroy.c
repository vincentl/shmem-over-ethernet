/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A team's destroy on one thread while another creates and destroys contexts,
 * under SHMEM_THREAD_MULTIPLE (spec §9.2: every call is thread-safe at that
 * level; §9.5.1 and §9.5.2: a context's creation is not collective). The
 * destroy ends the team's shareable contexts (§9.4.8) by scanning the process's
 * pool of contexts, which the other thread's creates and destroys write; the
 * scan took no lock until the review of step D's merge point found it
 * (conformance F1; the previous rounds' N6 and C6, left as a nit on the wrong
 * premise that the words it reads are claimed by compare-and-swap). No wrong
 * result followed in a valid program, but a ThreadSanitizer build of the
 * library reported the race: this test is what the tsan preset fails without
 * the lock (context.c, osoe_context_team_destroyed).
 *
 * Three passes. The first two are valid under both of the specification's
 * readings of a team's contexts at its destroy - §9.4.8's, that the destroy
 * ends its shareable contexts, and §9.5.2's, that a context not destroyed
 * before its team is undefined: the splitting thread first makes no context on
 * its team at all, then destroys the one it makes before the team. The third
 * takes §9.4.8's reading, which osoe implements: the splitting thread leaves
 * the shareable context it makes to the team's destroy, which ends it while the
 * other thread creates and destroys its own - the mode the pre-merge read of
 * the review's fixes found missing (docs/decisions/2026-10-04-step-d-review-
 * fixes.md). Only the splitting thread calls collectives (§9.2 item 5). After
 * the review's refutation pass (docs/reviews/archive/phase1-step7-slice3-review.md).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

#include <shmem.h>

#include "check.h"

enum { ROUNDS = 100 };

static int me;
static long *box;                       /* symmetric: shmem_malloc'ed, the creator's */
static long *left_box;                  /* and the splitter's, for the context it leaves */
static atomic_int failures;
/* What the splitter does with a context on its team: makes none, destroys the
 * one it makes before the team (§9.5.2), or leaves it to the team's destroy
 * (§9.4.8). */
static enum { NO_CONTEXT, CONTEXT_DESTROYED, CONTEXT_LEFT } context_mode;

static void *splitter(void *arg)
{
    (void)arg;
    int n = shmem_n_pes();
    for (int r = 0; r < ROUNDS; r++) {
        shmem_team_t t;
        if (shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, n, NULL, 0, &t) != 0) {
            atomic_fetch_add_explicit(&failures, 1, memory_order_seq_cst);
            continue;
        }
        if (context_mode != NO_CONTEXT) {
            shmem_ctx_t c;
            if (shmem_team_create_ctx(t, 0, &c) != 0)
                atomic_fetch_add_explicit(&failures, 1, memory_order_seq_cst);
            else if (context_mode == CONTEXT_DESTROYED)
                shmem_ctx_destroy(c);   /* §9.5.2: before the team */
            else
                shmem_ctx_long_p(c, left_box, r, me);   /* §9.4.8: the destroy ends it */
        }
        shmem_team_destroy(t);
    }
    return NULL;
}

static void *creator(void *arg)
{
    (void)arg;
    for (int r = 0; r < ROUNDS * 5; r++) {
        shmem_ctx_t c;
        if (shmem_ctx_create(SHMEM_CTX_PRIVATE, &c) != 0) {
            atomic_fetch_add_explicit(&failures, 1, memory_order_seq_cst);
            continue;
        }
        shmem_ctx_long_p(c, box, r, me);
        shmem_ctx_quiet(c);
        shmem_ctx_destroy(c);
    }
    return NULL;
}

static void pass(void)
{
    pthread_t a, b;
    CHECK_FATAL(pthread_create(&a, NULL, splitter, NULL) == 0);
    CHECK_FATAL(pthread_create(&b, NULL, creator, NULL) == 0);
    CHECK(pthread_join(a, NULL) == 0);
    CHECK(pthread_join(b, NULL) == 0);
    shmem_barrier_all();
}

int main(void)
{
    int provided = -1;
    CHECK_FATAL(shmem_init_thread(SHMEM_THREAD_MULTIPLE, &provided) == 0);
    CHECK_FATAL(provided == SHMEM_THREAD_MULTIPLE);
    me = shmem_my_pe();
    box = shmem_malloc(sizeof *box);
    left_box = shmem_malloc(sizeof *left_box);
    CHECK_FATAL(box != NULL && left_box != NULL);

    context_mode = NO_CONTEXT;
    pass();
    context_mode = CONTEXT_DESTROYED;
    pass();
    context_mode = CONTEXT_LEFT;
    pass();

    CHECK(atomic_load_explicit(&failures, memory_order_seq_cst) == 0);
    shmem_free(left_box);
    shmem_free(box);
    shmem_barrier_all();
    if (me == 0 && CHECK_DONE() == 0)
        fprintf(stderr, "PE 0: %d team destroys beside %d context creates, three times\n",
                ROUNDS, ROUNDS * 5);
    shmem_finalize();
    return CHECK_DONE();
}
