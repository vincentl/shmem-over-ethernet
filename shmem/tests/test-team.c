/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Teams and contexts (§7.3; spec §9.4, §9.5).
 *
 * The parameter is on every entry point, and the splits create teams (phase 1
 * step 7 slice 2; test-team-split.c exercises them). What this checks is that
 * the plumbing carries the argument rather than ignoring it, that a split's
 * teams answer the queries, and that the routines which must fail do fail in
 * the way the spec describes - returning nonzero and assigning
 * SHMEM_CTX_INVALID, which is a branch a spec-conforming program already has.
 *
 * Two things are more than plumbing.
 *
 * **SHMEM_TEAM_SHARED is not an alias for SHMEM_TEAM_WORLD.** On one node their
 * membership is identical, so every value check passes either way; what
 * separates them is that they are distinct teams with distinct epoch counters,
 * so a sync on one must not release a wait on the other. That is checked by
 * counting epochs out of the control segment (§3.14), which is the only way to
 * see it from outside.
 *
 * **shmem_sync_all is not shmem_barrier_all.** §5.6: barrier_all includes a
 * quiet on the default context and sync does not. Implementing sync as a
 * barrier is correct and slow, and implementing barrier_all without the quiet
 * is incorrect and silent - so the difference is checked by counting the
 * boundary's waits, where a sync must add none.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <shmem.h>

#include "arrivals.h"
#include "check.h"
#include "invariants.h"
#include "segment.h"

static int me, npes;

/* This PE's total for a thread-scope counter. */
static uint64_t counter(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    const struct osoe_metric_descriptor *m = osoe_segment_metric(&s, name);
    uint64_t total = 0;
    for (uint32_t i = 0; m && i < s.dir->blocks_per_pe; i++) {
        uint64_t v;
        if (osoe_segment_read(&s, m, (uint32_t)me * s.dir->blocks_per_pe + i, &v))
            total += v;
    }
    osoe_segment_close(&s);
    return m ? total : UINT64_MAX;
}

static void team_queries(void)
{
    CHECK(shmem_team_my_pe(SHMEM_TEAM_WORLD) == me);
    CHECK(shmem_team_n_pes(SHMEM_TEAM_WORLD) == npes);

    /* On one node SHMEM_TEAM_SHARED is every PE of the job, so its numbers
     * agree with the world's - which is why the epoch check below exists. */
    CHECK(shmem_team_n_pes(SHMEM_TEAM_SHARED) == npes);
    CHECK(shmem_team_my_pe(SHMEM_TEAM_SHARED) >= 0);
    CHECK(shmem_team_my_pe(SHMEM_TEAM_SHARED) < npes);

    /* spec §9.4.1, §9.4.2: SHMEM_TEAM_INVALID answers -1, not a failure. */
    CHECK(shmem_team_my_pe(SHMEM_TEAM_INVALID) == -1);
    CHECK(shmem_team_n_pes(SHMEM_TEAM_INVALID) == -1);

    /* §9.4.4 */
    shmem_team_config_t config = { .num_contexts = 99 };
    CHECK(shmem_team_get_config(SHMEM_TEAM_WORLD, SHMEM_TEAM_NUM_CONTEXTS, &config) == 0);
    CHECK(config.num_contexts == 0);            /* the documented default */
    config.num_contexts = 99;
    CHECK(shmem_team_get_config(SHMEM_TEAM_WORLD, 0, &config) == 0);
    CHECK(config.num_contexts == 99);           /* a mask of 0 performs no action */
    CHECK(shmem_team_get_config(SHMEM_TEAM_INVALID, SHMEM_TEAM_NUM_CONTEXTS, &config) != 0);

    /* §9.4.5: a round trip through the world, and -1 for what is not there. */
    int shared_me = shmem_team_my_pe(SHMEM_TEAM_SHARED);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_SHARED, shared_me, SHMEM_TEAM_WORLD) == me);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, me, SHMEM_TEAM_SHARED) == shared_me);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, 0, SHMEM_TEAM_WORLD) == 0);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, npes, SHMEM_TEAM_WORLD) == -1);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, -1, SHMEM_TEAM_WORLD) == -1);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_INVALID, 0, SHMEM_TEAM_WORLD) == -1);
    CHECK(shmem_team_translate_pe(SHMEM_TEAM_WORLD, 0, SHMEM_TEAM_INVALID) == -1);
}

static void splits_and_contexts(void)
{
    /* The world again, as a team of its own: every PE, in the same order. */
    shmem_team_t made = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_strided(SHMEM_TEAM_WORLD, 0, 1, npes, NULL, 0, &made) == 0);
    CHECK(made != SHMEM_TEAM_INVALID && made != SHMEM_TEAM_WORLD);
    CHECK(shmem_team_n_pes(made) == npes && shmem_team_my_pe(made) == me);
    CHECK(shmem_team_translate_pe(made, me, SHMEM_TEAM_WORLD) == me);
    CHECK(shmem_team_sync(made) == 0);

    /* §9.4.7: rows of one, so every PE its own row, and one column of all. */
    shmem_team_t x = SHMEM_TEAM_INVALID, y = SHMEM_TEAM_INVALID;
    CHECK(shmem_team_split_2d(SHMEM_TEAM_WORLD, 1, NULL, 0, &x, NULL, 0, &y) == 0);
    CHECK(x != SHMEM_TEAM_INVALID && y != SHMEM_TEAM_INVALID);
    CHECK(shmem_team_n_pes(x) == 1 && shmem_team_my_pe(x) == 0);
    CHECK(shmem_team_n_pes(y) == npes && shmem_team_my_pe(y) == me);

    /* Contexts are created on any team (§9.5; phase 1 step 8, test-context.c
     * is theirs): here that each creation answers, and each context names
     * its team (§9.5.4). */
    shmem_ctx_t ctx = SHMEM_CTX_INVALID, team_ctx = SHMEM_CTX_INVALID;
    shmem_team_t of = SHMEM_TEAM_INVALID;
    CHECK(shmem_ctx_create(0, &ctx) == 0 && ctx != SHMEM_CTX_INVALID);
    CHECK(shmem_ctx_get_team(ctx, &of) == 0 && of == SHMEM_TEAM_WORLD);
    shmem_ctx_destroy(ctx);
    CHECK(shmem_team_create_ctx(SHMEM_TEAM_WORLD, 0, &ctx) == 0 && ctx != SHMEM_CTX_INVALID);
    CHECK(shmem_ctx_get_team(ctx, &of) == 0 && of == SHMEM_TEAM_WORLD);
    shmem_ctx_destroy(ctx);
    CHECK(shmem_team_create_ctx(made, 0, &team_ctx) == 0 && team_ctx != SHMEM_CTX_INVALID);
    CHECK(shmem_ctx_get_team(team_ctx, &of) == 0 && of == made);
    /* Left for the team's destroy to destroy (§9.4.8). */
    shmem_team_destroy(made);
    shmem_team_destroy(x);
    shmem_team_destroy(y);

    /* §9.5.4: the default context belongs to the world team. */
    shmem_team_t team = SHMEM_TEAM_INVALID;
    CHECK(shmem_ctx_get_team(SHMEM_CTX_DEFAULT, &team) == 0);
    CHECK(team == SHMEM_TEAM_WORLD);
    team = SHMEM_TEAM_WORLD;
    CHECK(shmem_ctx_get_team(SHMEM_CTX_INVALID, &team) != 0);
    CHECK(team == SHMEM_TEAM_INVALID);

    /* §9.5.3, §9.4.8: the invalid handles are no-ops, not errors. */
    shmem_ctx_destroy(SHMEM_CTX_INVALID);
    shmem_team_destroy(SHMEM_TEAM_INVALID);

    /* §9.12.1, §9.12.2: and so is an invalid context here, and only here. */
    shmem_ctx_fence(SHMEM_CTX_INVALID);
    shmem_ctx_quiet(SHMEM_CTX_INVALID);
    shmem_ctx_pe_quiet(SHMEM_CTX_INVALID, NULL, 0);
}

/* Every context-taking entry point, carrying SHMEM_CTX_DEFAULT. */
static void context_forms(void)
{
    int right = (me + 1) % npes;
    uint64_t *v = shmem_malloc(sizeof *v);
    uint64_t *sig = shmem_malloc(sizeof *sig);
    unsigned char *buf = shmem_malloc(16);
    CHECK(v && sig && buf);
    *v = 0;
    *sig = 0;
    for (int i = 0; i < 16; i++)
        buf[i] = 0;
    shmem_barrier_all();

    shmem_ctx_uint64_atomic_set(SHMEM_CTX_DEFAULT, v, 3, me);
    CHECK(shmem_ctx_uint64_atomic_fetch(SHMEM_CTX_DEFAULT, v, me) == 3);
    CHECK(shmem_ctx_uint64_atomic_swap(SHMEM_CTX_DEFAULT, v, 4, me) == 3);
    CHECK(shmem_ctx_uint64_atomic_compare_swap(SHMEM_CTX_DEFAULT, v, 4, 5, me) == 4);
    shmem_ctx_uint64_atomic_inc(SHMEM_CTX_DEFAULT, v, me);
    CHECK(shmem_ctx_uint64_atomic_fetch_inc(SHMEM_CTX_DEFAULT, v, me) == 6);
    shmem_ctx_uint64_atomic_add(SHMEM_CTX_DEFAULT, v, 10, me);
    CHECK(shmem_ctx_uint64_atomic_fetch_add(SHMEM_CTX_DEFAULT, v, 1, me) == 17);
    shmem_ctx_uint64_atomic_and(SHMEM_CTX_DEFAULT, v, 0, me);
    shmem_ctx_uint64_atomic_or(SHMEM_CTX_DEFAULT, v, 0x30, me);
    CHECK(shmem_ctx_uint64_atomic_fetch_xor(SHMEM_CTX_DEFAULT, v, 0x10, me) == 0x30);
    CHECK(*v == 0x20);

    /* The C11 spellings, with and without a context: one name, two arities. */
    shmem_atomic_set(v, (uint64_t)1, me);
    CHECK(shmem_atomic_fetch(v, me) == 1);
    shmem_atomic_set(SHMEM_CTX_DEFAULT, v, (uint64_t)2, me);
    CHECK(shmem_atomic_fetch(SHMEM_CTX_DEFAULT, v, me) == 2);
    CHECK(shmem_atomic_swap(SHMEM_CTX_DEFAULT, v, (uint64_t)3, me) == 2);
    CHECK(shmem_atomic_compare_swap(SHMEM_CTX_DEFAULT, v, (uint64_t)3, (uint64_t)4, me)
          == 3);
    shmem_atomic_inc(SHMEM_CTX_DEFAULT, v, me);
    CHECK(shmem_atomic_fetch_inc(SHMEM_CTX_DEFAULT, v, me) == 5);
    shmem_atomic_add(SHMEM_CTX_DEFAULT, v, (uint64_t)4, me);
    CHECK(shmem_atomic_fetch_add(SHMEM_CTX_DEFAULT, v, (uint64_t)0, me) == 10);
    shmem_atomic_and(SHMEM_CTX_DEFAULT, v, (uint64_t)0, me);
    shmem_atomic_or(SHMEM_CTX_DEFAULT, v, (uint64_t)0x11, me);
    shmem_atomic_xor(SHMEM_CTX_DEFAULT, v, (uint64_t)0x01, me);
    CHECK(shmem_atomic_fetch_and(SHMEM_CTX_DEFAULT, v, (uint64_t)0x10, me) == 0x10);
    CHECK(shmem_atomic_fetch_or(SHMEM_CTX_DEFAULT, v, (uint64_t)0, me) == 0x10);
    CHECK(shmem_atomic_fetch_xor(SHMEM_CTX_DEFAULT, v, (uint64_t)0x10, me) == 0x10);
    CHECK(*v == 0);

    /* RMA, put-with-signal and the signal updates, all on a context. */
    unsigned char payload[16];
    for (int i = 0; i < 16; i++)
        payload[i] = (unsigned char)(i + me);
    shmem_ctx_putmem(SHMEM_CTX_DEFAULT, buf, payload, 16, right);
    shmem_ctx_quiet(SHMEM_CTX_DEFAULT);
    shmem_barrier_all();
    int left = (me + npes - 1) % npes;
    for (int i = 0; i < 16; i++)
        CHECK(buf[i] == (unsigned char)(i + left));

    unsigned char got[16];
    shmem_ctx_getmem(SHMEM_CTX_DEFAULT, got, buf, 16, me);
    for (int i = 0; i < 16; i++)
        CHECK(got[i] == buf[i]);

    shmem_ctx_putmem_signal(SHMEM_CTX_DEFAULT, buf, payload, 16, sig, 1,
                            SHMEM_SIGNAL_SET, right);
    CHECK(shmem_signal_wait_until(sig, SHMEM_CMP_EQ, 1) == 1);
    shmem_ctx_signal_add(SHMEM_CTX_DEFAULT, sig, 4, me);
    CHECK(shmem_signal_fetch(sig) == 5);
    shmem_ctx_signal_set(SHMEM_CTX_DEFAULT, sig, 0, me);
    CHECK(shmem_signal_fetch(sig) == 0);
    shmem_barrier_all();

    shmem_ctx_fence(SHMEM_CTX_DEFAULT);
    int all[1] = { right };
    shmem_ctx_pe_quiet(SHMEM_CTX_DEFAULT, all, 1);

    shmem_free(buf);
    shmem_free(sig);
    shmem_free(v);
}

/* This PE's own value for a PE-scope metric: written by nobody else, so it can
 * be read while the other PEs run. */
static uint64_t pe_value(const char *name)
{
    char segment[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(segment, sizeof segment, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, segment, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return UINT64_MAX;
    }
    uint64_t v = UINT64_MAX;
    if (!osoe_segment_read_name(&s, name, (uint32_t)me, &v))
        v = UINT64_MAX;
    osoe_segment_close(&s);
    return v;
}

/*
 * The two teams are distinct, in the two ways that matter.
 *
 * Their epoch counts advance independently: syncing SHMEM_TEAM_SHARED advances
 * that team's k and leaves the world's alone. Each PE reads only its own
 * published counts, which nobody else writes, so this needs no synchronization
 * of its own.
 *
 * And - the failure worth catching - a sync on one team must still
 * *synchronize* after any number of syncs on the other. If the two shared the
 * §5.4 counters, a world sync following a shared sync would find `released`
 * already at its target and return without waiting for anybody: a collective
 * that does not collect, which no value check would notice. The arrival table
 * notices, because PE r delays before arriving and every PE must leave after
 * the last arrival.
 */
static void teams_are_distinct(void)
{
    shmem_barrier_all();
    uint64_t w0 = pe_value("pe.world_epoch");
    uint64_t s0 = pe_value("pe.shared_epoch");
    CHECK(w0 != UINT64_MAX && s0 != UINT64_MAX);

    CHECK(shmem_team_sync(SHMEM_TEAM_SHARED) == 0);
    CHECK(pe_value("pe.shared_epoch") == s0 + 1);
    CHECK(pe_value("pe.world_epoch") == w0);

    CHECK(shmem_team_sync(SHMEM_TEAM_WORLD) == 0);
    CHECK(pe_value("pe.world_epoch") == w0 + 1);
    CHECK(pe_value("pe.shared_epoch") == s0 + 1);

    /* Interleaved, with staggered arrivals: each sync must still wait. */
    for (int round = 0; round < 3; round++) {
        arrivals_arrive(me);
        CHECK(shmem_team_sync(SHMEM_TEAM_SHARED) == 0);
        CHECK(arrivals_left_after_all(npes));

        arrivals_arrive(me);
        shmem_sync_all();
        CHECK(arrivals_left_after_all(npes));
    }
    CHECK(pe_value("pe.shared_epoch") == s0 + 4);
    CHECK(pe_value("pe.world_epoch") == w0 + 4);

    /* The C11 spelling, and the invalid team. */
    CHECK(shmem_sync(SHMEM_TEAM_SHARED) == 0);
    CHECK(shmem_team_sync(SHMEM_TEAM_INVALID) != 0);
}

/*
 * §5.6: barrier_all is a quiet followed by a sync; sync is the sync alone. The
 * difference is not visible in any value on this backend, where nothing is ever
 * outstanding, so it is counted: a sync must reach the boundary's wait zero
 * times and a barrier exactly once.
 */
static void sync_is_not_a_barrier(void)
{
    shmem_barrier_all();
    uint64_t waits0 = counter("transport.wait.calls");
    CHECK(waits0 != UINT64_MAX);

    shmem_sync_all();
    shmem_sync_all();
    CHECK(shmem_team_sync(SHMEM_TEAM_SHARED) == 0);
    uint64_t waits1 = counter("transport.wait.calls");
    CHECK(waits1 == waits0);            /* three syncs, no quiet */

    shmem_barrier_all();
    CHECK(counter("transport.wait.calls") == waits1 + 1);

    /* And a sync still synchronizes: everyone arrives before anyone leaves,
     * which is what the arrival table of the barrier test measures and what
     * the epoch equality above already required. */
    for (int i = 0; i < 50; i++)
        shmem_sync_all();
}

int main(void)
{
    arrivals_open();
    shmem_init();
    me = shmem_my_pe();
    npes = shmem_n_pes();

    team_queries();
    splits_and_contexts();
    context_forms();
    teams_are_distinct();
    sync_is_not_a_barrier();

    /* shmem_team_ptr: identical to shmem_ptr on the world team (§9.2.3). */
    uint64_t *p = shmem_malloc(sizeof *p);
    CHECK(p != NULL);
    CHECK(shmem_team_ptr(SHMEM_TEAM_WORLD, p, me) == shmem_ptr(p, me));
    CHECK(shmem_team_ptr(SHMEM_TEAM_INVALID, p, me) == NULL);
    CHECK(shmem_team_ptr(SHMEM_TEAM_WORLD, p, npes) == NULL);
    CHECK(shmem_team_ptr(SHMEM_TEAM_SHARED, p, shmem_team_my_pe(SHMEM_TEAM_SHARED))
          == shmem_ptr(p, me));
    shmem_free(p);

    CHECK_INVARIANTS();
    shmem_finalize();
    arrivals_close();
    return CHECK_DONE();
}
