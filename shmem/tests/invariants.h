/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Every invariant counter is zero at quiescence (§3.14 question 6, §8.2).
 *
 * §3.14 gives some counters the kind OSOE_MKIND_INVARIANT and says they must be
 * zero at quiescence, so that they are a correctness oracle and not only a
 * diagnostic. Until something asserts it, they are neither: `missed_wake` could
 * fire on every run of every test and nothing would say so.
 *
 * This is that assertion, and it is deliberately written the way `osoe-stat` is
 * written - through the segment reader, finding the counters by *kind* in the
 * metric table rather than by name or by index. It therefore covers every
 * invariant counter that exists now and every one added later, without being
 * edited, and it is a second user of the self-describing table, which is the
 * only way to find out whether the table really describes the segment.
 *
 * Sharing the reader with osoe-stat is the point; sharing it with the library
 * is not. A check that read the counters through the library's own headers
 * would pass whenever the library was self-consistent, which is not the
 * question.
 *
 * Quiescence is defined here as a barrier: every PE has arrived, so no PE is
 * mid-wait, and each then reads every block of every PE. Every PE reads the
 * whole node, so a violation is reported by all of them and the test fails
 * whichever PE the launcher notices first.
 */
#ifndef OSOE_TEST_INVARIANTS_H
#define OSOE_TEST_INVARIANTS_H

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "launcher.h"
#include "segment.h"

/*
 * The control segment's name, rebuilt from the launcher environment (§6.2,
 * launcher.h):
 * /osoe.<job>.<step>.<node>.<session>, session 0 for a single init/finalize
 * pair. A test that re-initializes would have to say which session it means,
 * which none does yet.
 */
static inline void osoe_test_segment_name(char *buf, size_t len, unsigned session)
{
    const char *job = osoe_test_launcher("JOB_ID");
    const char *step = osoe_test_launcher("STEP_ID");
    const char *node = osoe_test_launcher("NODEID");
    snprintf(buf, len, "/osoe.%s.%s.%s.%u", job ? job : "0", step ? step : "0",
             node ? node : "0", session);
}

/* How many records a scope has. */
static inline uint32_t osoe_test_scope_count(const struct osoe_segment *s, unsigned scope)
{
    switch (scope) {
    case OSOE_MSCOPE_NODE:   return 1;
    case OSOE_MSCOPE_PE:     return s->dir->local_count;
    case OSOE_MSCOPE_THREAD: return s->dir->block_count;
    case OSOE_MSCOPE_CONNECTION: return s->connection_count;
    default:                 return 0;
    }
}

/*
 * §3.14's unlink-early rule, checked while the job runs: once init has
 * completed, nothing this job created in /dev/shm has a name, so that no death
 * of any process can strand it. With OSOE_KEEP_CONTROL_SEGMENT=1 the control
 * segments keep theirs, and only those. Returns how many offenders it printed.
 * The launcher's own check runs after the job; this one runs inside it.
 */
static inline int osoe_test_named_objects(void)
{
    const char *job = osoe_test_launcher("JOB_ID"), *step = osoe_test_launcher("STEP_ID");
    const char *keep = getenv("OSOE_KEEP_CONTROL_SEGMENT");
    bool kept = keep && strcmp(keep, "1") == 0;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "osoe.%s.%s.", job ? job : "0", step ? step : "0");
    DIR *d = opendir("/dev/shm");
    if (!d)
        return 0;
    int bad = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0)
            continue;
        /* A kept control segment is <prefix><node>.<session> - the prefix
         * already holds the step - so two numbers and nothing after. */
        const char *rest = e->d_name + strlen(prefix);
        unsigned node, session;
        char tail;
        if (kept && sscanf(rest, "%u.%u%c", &node, &session, &tail) == 2)
            continue;
        fprintf(stderr, "invariants: /dev/shm/%s still has a name after init (§3.14)\n",
                e->d_name);
        bad++;
    }
    closedir(d);
    return bad;
}

/*
 * Nonzero if every invariant counter in the whole node is zero. Call before the
 * last shmem_finalize, which removes the segment's name. `session` counts full
 * init/finalize cycles from 0, as §6.4 numbers them; a test that runs one cycle
 * uses osoe_invariants_ok().
 */
static inline int osoe_invariants_ok_session(unsigned session)
{
    shmem_barrier_all();

    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, session);

    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "invariants: %s\n", err);
        return 0;
    }
    if (!s.table_usable) {
        fprintf(stderr, "invariants: %s has no readable metric table\n", s.name);
        osoe_segment_close(&s);
        return 0;
    }

    int bad = 0, invariants = 0;
    for (uint32_t i = 0; i < s.nmetrics; i++) {
        const struct osoe_metric_descriptor *m = &s.metrics[i];
        if (m->kind != OSOE_MKIND_INVARIANT)
            continue;
        invariants++;
        uint32_t n = osoe_test_scope_count(&s, m->scope);
        for (uint32_t k = 0; k < n; k++) {
            uint64_t v;
            if (!osoe_segment_read(&s, m, k, &v)) {
                fprintf(stderr, "invariants: %.40s[%u] is unreadable\n", m->name, k);
                bad++;
                continue;
            }
            if (v != 0) {
                fprintf(stderr, "invariants: %.40s[%u] is %llu, not zero (§8.2)\n",
                        m->name, k, (unsigned long long)v);
                bad++;
            }
        }
    }
    /* A table with no invariant counters would pass every check above, which
     * would be the loudest possible silence. */
    if (invariants == 0) {
        fprintf(stderr, "invariants: the metric table declares none\n");
        bad++;
    }
    osoe_segment_close(&s);
    bad += osoe_test_named_objects();
    return bad == 0;
}

static inline int osoe_invariants_ok(void)
{
    return osoe_invariants_ok_session(0);
}

/*
 * One node-scope metric of this PE's own node, by name, through the same reader:
 * how a test sees what the library counts but does not return, such as
 * `transport.am.header_only_sent`. Returns 0 and prints why if the segment or
 * the name cannot be read, so a test that misspells a metric fails rather than
 * reading zero.
 */
static inline int osoe_test_node_metric(const char *metric, uint64_t *value)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "node metric %s: %s\n", metric, err);
        return 0;
    }
    const struct osoe_metric_descriptor *m = s.table_usable ? osoe_segment_metric(&s, metric) : NULL;
    int ok = m && m->scope == OSOE_MSCOPE_NODE && osoe_segment_read(&s, m, 0, value);
    if (!ok)
        fprintf(stderr, "node metric %s: not a readable node-scope metric of %s\n", metric, s.name);
    osoe_segment_close(&s);
    return ok;
}

/*
 * One metric of a given scope and index of this PE's own node, by name: the
 * connection scope (§3.14) has one record per peer, so a test that wants "this
 * node's traffic toward node 3" names the metric and the peer slot.
 */
static inline int osoe_test_scoped_metric(const char *metric, unsigned scope, uint32_t index,
                                          uint64_t *value)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "metric %s: %s\n", metric, err);
        return 0;
    }
    const struct osoe_metric_descriptor *m = s.table_usable ? osoe_segment_metric(&s, metric)
                                                            : NULL;
    int ok = m && m->scope == scope && osoe_segment_read(&s, m, index, value);
    if (!ok)
        fprintf(stderr, "metric %s: not readable at scope %u index %u of %s\n", metric,
                scope, index, s.name);
    osoe_segment_close(&s);
    return ok;
}

/*
 * The block of this node's pool of teams that holds team number `number` with
 * a published record, or UINT32_MAX: the team scope is indexed by block since
 * layout 29 (the team redesign's step D), and a block's name says its team's
 * number. Until the team work's second part a number is job-wide, so one block
 * of a node holds it.
 */
static inline uint32_t osoe_test_team_block(uint32_t number)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0) {
        fprintf(stderr, "team %u's block: %s\n", number, err);
        return UINT32_MAX;
    }
    uint32_t found = UINT32_MAX;
    for (uint32_t b = 0; s.table_usable && b < s.team_count && found == UINT32_MAX; b++) {
        uint64_t n = 0, published = 0;
        if (osoe_segment_read_name(&s, "team.record.published", b, &published) && published &&
            osoe_segment_read_name(&s, "team.name.number", b, &n) && n == number)
            found = b;
    }
    if (found == UINT32_MAX)
        fprintf(stderr, "team %u: no block of %s holds it\n", number, s.name);
    osoe_segment_close(&s);
    return found;
}

/* How many records this node's connection scope has (0 on a one-node job). */
static inline uint32_t osoe_test_connection_count(void)
{
    char name[OSOE_SEGMENT_NAME_MAX], err[256];
    osoe_test_segment_name(name, sizeof name, 0);
    struct osoe_segment s;
    if (osoe_segment_open(&s, name, err, sizeof err) != 0)
        return 0;
    uint32_t n = s.connection_count;
    osoe_segment_close(&s);
    return n;
}

#define CHECK_INVARIANTS() CHECK(osoe_invariants_ok())
#define CHECK_INVARIANTS_SESSION(k) CHECK(osoe_invariants_ok_session(k))

#endif /* OSOE_TEST_INVARIANTS_H */
