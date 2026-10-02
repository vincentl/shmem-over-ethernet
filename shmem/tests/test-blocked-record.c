/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The "blocked in" record read while it is being rewritten (§3.14; blocked.c).
 *
 * The library's own writer, osoe_blocked_publish, republishes as fast as it
 * can on one CPU; on another the two readers take copies in turn - the
 * library's, osoe_blocked_read, which the deadlock detector's lead uses, and
 * osoe-stat's, osoe_segment_blocked, through a segment whose metric table
 * points into the same PE slot as the library's table does. Publication k is
 *
 *     routine names[k % 4], kind k % 4, detail k, since k,
 *
 * so an accepted copy whose routine is not names[detail % 4], or whose fields
 * disagree, holds two publications: it is torn.
 *
 * This is the round-three review's litmus (K1) run on the code itself. Its
 * mutation is the readers' name loads made plain again (base/atomic-name.h),
 * as they were before the review: in osoe-load (Apple silicon, aarch64) the
 * library's reader then accepts 10,000 to 750,000 torn copies in 2 s, ten
 * runs of ten, and none with the loads as acquires. Two things the mutations
 * showed: osoe-stat's reader looks its fields up by name between loads, so
 * nothing here tears it even with plain loads, which the litmus's copy of its
 * order did; and the writer's stores made plain alone tear nothing on these
 * cores, whose read-modify-writes are single instructions (LSE). On x86-64 it
 * cannot fail at all: x86-TSO keeps loads in order - which is also why the
 * record's model, run under sequential consistency and TSO, passed while the
 * record tore - so there it checks only that both readers accept copies.
 *
 * Bounded by time: OSOE_TEST_BLOCKED_SECONDS (default 2) for each reader.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "blocked.h"
#include "check.h"
#include "node.h"
#include "test-blocked-record.h"

static const char *const names[4] = {
    "shmem_barrier_all", "shmem_quiet", "shmem_long_wait_until_all_vector", "fcollect"
};

static struct osoe_pe_slot *slot;        /* in the writer's mapping */
static _Atomic int stop;
static int writer_cpu = -1, reader_cpu = -1;

static void pin(int cpu)
{
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

/* Publications back to back, as fast as the writer goes, and every 64 of them
 * a pause of 2 us: a reader's attempt overlaps a publication almost always,
 * which is what tears a copy, and still finishes now and then even under a
 * sanitizer - a PE does not republish without end. */
static void *writer(void *unused)
{
    (void)unused;
    pin(writer_cpu);
    for (uint64_t k = 1; !atomic_load_explicit(&stop, memory_order_acquire); k++) {
        osoe_blocked_publish(slot, names[k % 4], (enum osoe_blocked_detail)(k % 4), k, k);
        if (k % 64 == 0)
            for (uint64_t until = now_ns() + 2000; now_ns() < until;)
                ;
    }
    return NULL;
}

/* The names as the record keeps them: at most OSOE_BLOCKED_ROUTINE_MAX - 1
 * characters, and the third is longer than that. */
static char kept[4][OSOE_BLOCKED_ROUTINE_MAX];

/* A copy is whole when every field came from one publication. Cheap, so that
 * the reader's loop is mostly reads, as the litmus's was. */
static bool whole(const char *routine, uint32_t kind, uint64_t detail, uint64_t since)
{
    return detail == since && kind == detail % 4 &&
           strncmp(routine, kept[detail % 4], OSOE_BLOCKED_ROUTINE_MAX) == 0;
}

struct tally {
    uint64_t reads, accepted, torn;
    char first[192];
};

static void count(struct tally *t, bool accepted, const char *routine, uint32_t kind,
                  uint64_t detail, uint64_t since)
{
    t->reads++;
    if (!accepted)
        return;
    t->accepted++;
    if (whole(routine, kind, detail, since))
        return;
    if (t->torn++ == 0)
        snprintf(t->first, sizeof t->first, "routine '%s', kind %" PRIu32 ", detail %" PRIu64
                 ", since %" PRIu64, routine, kind, detail, since);
}

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void report(const char *reader, const struct tally *t)
{
    printf("%s: %" PRIu64 " reads, %" PRIu64 " accepted, %" PRIu64 " torn%s%s\n", reader,
           t->reads, t->accepted, t->torn, t->torn ? "; the first: " : "", t->torn ? t->first : "");
    CHECK(t->accepted > 0);
    CHECK(t->torn == 0);
}

/* Two CPUs this process may run on, one for each side; none if it has one. */
static void choose_cpus(void)
{
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) != 0)
        return;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (!CPU_ISSET(c, &set))
            continue;
        if (reader_cpu < 0)
            reader_cpu = c;
        else {
            writer_cpu = c;
            return;
        }
    }
    reader_cpu = -1;
}

int main(void)
{
    const char *env = getenv("OSOE_TEST_BLOCKED_SECONDS");
    double seconds = env ? atof(env) : 2.0;
    const char *dir = getenv("TMPDIR");
    char path[256];
    snprintf(path, sizeof path, "%s/osoe-test-blocked-record.%d", dir && *dir ? dir : "/tmp",
             (int)getpid());

    const struct blocked_record_layout layout = {
        .layout = OSOE_NODE_LAYOUT,
        .slot_bytes = sizeof(struct osoe_pe_slot),
        .gen = offsetof(struct osoe_pe_slot, blocked_gen),
        .routine = offsetof(struct osoe_pe_slot, blocked_routine),
        .routine_bytes = sizeof ((struct osoe_pe_slot *)0)->blocked_routine,
        .detail = offsetof(struct osoe_pe_slot, blocked_detail),
        .kind = offsetof(struct osoe_pe_slot, blocked_detail_kind),
        .since = offsetof(struct osoe_pe_slot, blocked_since_ns),
    };
    size_t bytes = 0;
    unsigned char *map = blocked_segment_build(path, &layout, &bytes);
    CHECK_FATAL(map != NULL);
    slot = (struct osoe_pe_slot *)(void *)(map + BLOCKED_SEGMENT_HEADER_BYTES);
    osoe_blocked_publish(slot, names[0], OSOE_BLOCKED_DETAIL_NONE, 0, 0);
    void *segment = blocked_segment_open(path);
    CHECK_FATAL(segment != NULL);

    for (int i = 0; i < 4; i++)
        snprintf(kept[i], sizeof kept[i], "%s", names[i]);
    choose_cpus();
    printf("writer on CPU %d, readers on CPU %d, %.1f s each\n", writer_cpu, reader_cpu, seconds);
    pthread_t thread;
    CHECK_FATAL(pthread_create(&thread, NULL, writer, NULL) == 0);
    pin(reader_cpu);

    struct tally library = { 0 }, stat = { 0 };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        for (int i = 0; i < 4096; i++) {
            struct osoe_blocked_copy c;
            bool ok = osoe_blocked_read(slot, &c);
            count(&library, ok, c.routine, c.kind, c.detail, c.since_ns);
        }
    } while (seconds_since(&start) < seconds);

    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        for (int i = 0; i < 1024; i++) {
            struct blocked_record_copy c;
            bool ok = blocked_segment_read(segment, &c);
            count(&stat, ok, c.routine, c.kind, c.detail, c.since_ns);
        }
    } while (seconds_since(&start) < seconds);

    atomic_store_explicit(&stop, 1, memory_order_release);
    pthread_join(thread, NULL);
    blocked_segment_close(segment);
    munmap(map, bytes);
    unlink(path);

    report("osoe_blocked_read (the deadlock detector's)", &library);
    report("osoe_segment_blocked (osoe-stat's)", &stat);
    return CHECK_DONE();
}
