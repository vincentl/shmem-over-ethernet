/*
 * The metric table and the per-thread counter blocks (§3.14). metrics.h states
 * the design; this is the table itself and the block claim.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base/counters.h"
#include <stdlib.h>

#include "base/env.h"
#include "fatal.h"
#include "blocked.h"
#include "metrics.h"
#include "node.h"
#include "state.h"

/* ---- The table -------------------------------------------------------- */

/*
 * Thread-scope entries are in counter order, so entry i describes counter i.
 * Its offset is i * 8 and is filled in when the table is assembled, rather than
 * written here as well, so the two cannot disagree.
 */
#define T(name, kind, unit) { name, unit, OSOE_MTYPE_U64, kind, OSOE_MSCOPE_THREAD, 0, 0, 0 }

static const struct osoe_metric_desc thread_metrics[] = {
    T("tp.put.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.put.bytes", OSOE_MKIND_COUNTER, "bytes"),
    T("tp.get.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.get.bytes", OSOE_MKIND_COUNTER, "bytes"),
    T("tp.rmw.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.rmw.fetching", OSOE_MKIND_COUNTER, ""),
    T("tp.fence.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.wait.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.wait.nothing_outstanding", OSOE_MKIND_COUNTER, ""),
    T("tp.wait.ns", OSOE_MKIND_COUNTER, "ns"),
    T("tp.wait.dirty_peers", OSOE_MKIND_COUNTER, ""),
    T("tp.notify.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.notify.wakes", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.calls", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.blocked", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.blocked_ns", OSOE_MKIND_COUNTER, "ns"),
    T("tp.watch.wakes", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.spurious", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.sweeps", OSOE_MKIND_COUNTER, ""),
    T("tp.watch.saturated", OSOE_MKIND_COUNTER, ""),
    T("shmem.barrier.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.barrier.ns", OSOE_MKIND_COUNTER, "ns"),
    T("shmem.malloc.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.free.calls", OSOE_MKIND_COUNTER, ""),
    /* Every allocating routine counts as a malloc, since that is the family
     * §3.14 question 4 asks about. realloc is counted separately as well,
     * because it is the only one that both frees and allocates. */
    T("shmem.realloc.calls", OSOE_MKIND_COUNTER, ""),
    /* The API layer's own call counts, beside the boundary's. They are not the
     * same number: shmem_barrier_all begins with a wait too (§5.6), so
     * tp.wait.calls alone cannot say how often the application asked for a
     * quiet. No `.ns` here - on this backend neither ever blocks, and §3.14
     * forbids a clock read on a path that does not sleep. */
    T("shmem.fence.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.quiet.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.pe_quiet.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.wait_until.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.test.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.update.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.fetch.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.wait.calls", OSOE_MKIND_COUNTER, ""),
    T("agent.loops", OSOE_MKIND_COUNTER, ""),
    T("agent.wakeups", OSOE_MKIND_COUNTER, ""),
    T("agent.last_wake_ns", OSOE_MKIND_STATE, "ns"),
    T("invariant.missed_wake", OSOE_MKIND_INVARIANT, ""),
    T("invariant.watch_reentered", OSOE_MKIND_INVARIANT, ""),
    T("threads.blocks_exhausted", OSOE_MKIND_INVARIANT, ""),
    T("thread.claimed", OSOE_MKIND_STATE, ""),
    T("thread.tid", OSOE_MKIND_STATE, ""),
};

_Static_assert(sizeof thread_metrics / sizeof thread_metrics[0] == OSOE_COUNTER_COUNT,
               "every counter needs a table entry, in counter order");

#define PE(name, field, type, kind, unit)                                   \
    { name, unit, type, kind, OSOE_MSCOPE_PE,                               \
      (uint16_t)sizeof(((struct osoe_pe_slot *)0)->field),                  \
      (uint32_t)offsetof(struct osoe_pe_slot, field), 0 }
#define ND(name, field, type, kind, unit)                                   \
    { name, unit, type, kind, OSOE_MSCOPE_NODE,                             \
      (uint16_t)sizeof(((struct osoe_node_header *)0)->field),              \
      (uint32_t)offsetof(struct osoe_node_header, field), 0 }
/* A field of the frozen directory, which a reader already knows how to find;
 * naming it in the table too costs nothing and keeps one way of reading. */
#define NDDIR(name, field, type, kind)                                      \
    { name, "", type, kind, OSOE_MSCOPE_NODE,                               \
      (uint16_t)sizeof(((struct osoe_segment_directory *)0)->field),        \
      (uint32_t)(offsetof(struct osoe_node_header, dir) +                   \
                 offsetof(struct osoe_segment_directory, field)), 0 }
/* An epoch counter's value is a word inside a cache-line-padded structure, and
 * the word is what a reader loads. */
#define NDEPOCH(name, field)                                                \
    { name, "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,      \
      (uint32_t)(offsetof(struct osoe_node_header, field) +                 \
                 offsetof(struct osoe_epoch, count_compare_only_by_equality)), 0 }

static const struct osoe_metric_desc record_metrics[] = {
    PE("pe.rank", rank, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.pid", pid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    PE("pe.tid", tid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    PE("pe.phase", phase, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.agent_wake", agent_wake, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.symmetric_size", symmetric_size, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "bytes"),
    PE("pe.heap_bytes", heap_bytes, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "bytes"),
    /* The heap object's name, so osoe-stat --gc can remove what a job that
     * crashed during bootstrap left behind, without knowing how the name is
     * built. Everything else in the segment is found by name; the names of the
     * other objects a job owns should be too. */
    PE("pe.heap_name", heap_name, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    PE("pe.watch_rng", watch_rng, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    PE("pe.watch_seq", watch_seq, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.world_epoch", world_epoch, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.sweeps", sweeps, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    PE("pe.blocked.gen", blocked_gen, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.routine", blocked_routine, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.detail", blocked_detail, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.since_ns", blocked_since_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "ns"),
    PE("heap.bytes_in_use", heap_in_use, OSOE_MTYPE_U64, OSOE_MKIND_GAUGE, "bytes"),
    PE("heap.bytes_high_water", heap_high_water, OSOE_MTYPE_U64, OSOE_MKIND_HIGHWATER, "bytes"),
    PE("heap.largest_free_block", heap_largest_free, OSOE_MTYPE_U64, OSOE_MKIND_GAUGE, "bytes"),
    NDEPOCH("team.world.arrived", world_arrived),
    NDEPOCH("team.world.released", world_released),
    NDEPOCH("team.world.copied", world_copied),
    ND("node.aborting", aborting, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.abort_pe", abort_pe, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.abort_status", abort_status, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    ND("node.world_size", world_size, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.job", job, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.step", step, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.id", node, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.session", session, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.heap_base", heap_base, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* The rendezvous words (§6.2). A job stuck in bootstrap is stuck in one of
     * these, and which one says how far it got. */
    ND("node.ready", ready, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.joined", joined, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.mapped", mapped, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.finalized", finalized, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.failed", failed, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    NDDIR("node.local_count", local_count, OSOE_MTYPE_U32, OSOE_MKIND_STATE),
    NDDIR("node.blocks_per_pe", blocks_per_pe, OSOE_MTYPE_U32, OSOE_MKIND_STATE),
};

enum {
    THREAD_N = sizeof thread_metrics / sizeof thread_metrics[0],
    RECORD_N = sizeof record_metrics / sizeof record_metrics[0],
    TABLE_N = THREAD_N + RECORD_N,
};

const struct osoe_metric_desc *osoe_metrics_table(uint32_t *count)
{
    static struct osoe_metric_desc table[TABLE_N];
    static bool built;

    if (!built) {
        for (uint32_t i = 0; i < THREAD_N; i++) {
            table[i] = thread_metrics[i];
            table[i].offset = i * (uint32_t)sizeof(uint64_t);
            table[i].width = (uint16_t)sizeof(uint64_t);
        }
        memcpy(table + THREAD_N, record_metrics, sizeof record_metrics);
        built = true;
    }
    *count = TABLE_N;
    return table;
}

uint32_t osoe_metric_checksum(const struct osoe_metric_desc *t, uint32_t n)
{
    const unsigned char *p = (const unsigned char *)t;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)n * sizeof *t; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* ---- Per-thread blocks ------------------------------------------------ */

#define OSOE_DEFAULT_MAX_THREADS 8

static struct osoe_counter_block *pool;         /* this PE's slice of the node pool */
static uint32_t pool_blocks;
static _Thread_local struct osoe_counter_block *my_block;

/*
 * The block a thread gets when the pool is full, and the one every thread uses
 * before init and after fini. Thread-local, so it is never shared and the
 * single-writer rule holds without a condition; nobody reads it, which is what
 * threads.blocks_exhausted records.
 */
static _Thread_local struct osoe_counter_block orphan;

uint32_t osoe_metrics_blocks_per_pe(void)
{
    uint32_t n = OSOE_DEFAULT_MAX_THREADS;
    /* A malformed value is an error, never a default (base/env.h): a typo in a
     * launch script must fail at init rather than quietly change the pool. */
    if (osoe_env_u32("OSOE_MAX_THREADS", &n) == OSOE_ENV_INVALID || n == 0)
        osoe_fatal("OSOE_MAX_THREADS='%s' is not a positive number of threads",
                   getenv("OSOE_MAX_THREADS"));
    return n;
}

void osoe_metrics_init(struct osoe_counter_block *blocks, uint32_t per_pe, uint32_t local_id)
{
    pool = blocks + (size_t)local_id * per_pe;
    pool_blocks = per_pe;
    my_block = NULL;
}

void osoe_metrics_fini(void)
{
    pool = NULL;
    pool_blocks = 0;
    my_block = NULL;
}

struct osoe_counter_block *osoe_metrics_block(void)
{
    if (my_block)
        return my_block;
    if (!pool)
        return &orphan;

    for (uint32_t i = 0; i < pool_blocks; i++) {
        uint64_t unclaimed = 0;
        if (atomic_compare_exchange_strong_explicit(&pool[i].c[OSOE_C_THREAD_CLAIMED],
                                                    &unclaimed, 1, memory_order_seq_cst,
                                                    memory_order_seq_cst)) {
            my_block = &pool[i];
            osoe_counter_set(my_block, OSOE_C_THREAD_TID, (uint64_t)(uint32_t)gettid());
            return my_block;
        }
    }
    my_block = &orphan;
    osoe_counter_add(my_block, OSOE_C_THREADS_BLOCKS_EXHAUSTED, 1);
    return my_block;
}

/* ---- The "blocked in" record (blocked.h) ------------------------------ */

uint64_t osoe_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/*
 * Bump the generation odd, write the fields, bump it even. Both bumps are
 * seq_cst read-modify-writes, which are full barriers on both targets, so no
 * field write can escape the window in either direction. §3.16 rules out the
 * textbook seqlock's relaxed stores and release fence, and this needs no rule
 * the project does not already have.
 */
static void publish(const char *routine, uint64_t detail, uint64_t since)
{
    struct osoe_pe_slot *me = &osoe.node.slots[osoe.boot.local_id];

    atomic_fetch_add_explicit(&me->blocked_gen, 1, memory_order_seq_cst);
    if (routine) {
        size_t n = strlen(routine);
        if (n >= sizeof me->blocked_routine)
            n = sizeof me->blocked_routine - 1;
        memcpy(me->blocked_routine, routine, n);
        memset(me->blocked_routine + n, 0, sizeof me->blocked_routine - n);
    } else {
        me->blocked_routine[0] = '\0';
    }
    atomic_store_explicit(&me->blocked_detail, detail, memory_order_release);
    atomic_store_explicit(&me->blocked_since_ns, since, memory_order_release);
    atomic_fetch_add_explicit(&me->blocked_gen, 1, memory_order_seq_cst);
}

void osoe_blocked_enter(const char *routine, uint64_t detail)
{
    if (osoe.node.slots)
        publish(routine, detail, osoe_now_ns());
}

void osoe_blocked_leave(void)
{
    if (osoe.node.slots)
        publish(NULL, 0, 0);
}
