/*
 * The self-describing metric table (§3.14).
 *
 * > The library writes introspection state into shared memory; an external tool
 * > reads it. The library serves nothing and writes no files.
 *
 * The case that matters is a hung job. Any export the job serves or writes fails
 * exactly then, because the process that would serve it is the one that is
 * stuck. So everything an investigator needs is in the node's control segment,
 * written by single writers, and `osoe-stat` maps it read-only.
 *
 * **Self-describing, so the tool need not be the job's build.** The control
 * segment carries a table naming every metric - name, type, unit, kind, scope
 * and offset - checksummed like the handler table of §3.13. A reader needs only
 * the header's frozen prefix to find the three arrays; from there it reads
 * everything by *name*. A job built from a different commit, with different
 * counters, is read correctly.
 *
 * A metric's scope says which record its offset is into:
 *   NODE        a field of the header: one per node
 *   PE          a field of a PE's slot: one per local PE, written by that PE
 *   THREAD      a counter in a thread's block: one per thread (§3.14 cost rules)
 *   CONNECTION  a field of one connection's record: one per peer node
 *   TEAM        a field of one team number's block: one per team number
 *               (layout 18), the index being the number - which is how a
 *               reader finds a team created after the table was written
 *   EPOCHS      a word of one local PE's row of published epochs, a word per
 *               team number (layout 18), the index being the local id
 *
 * The predefined teams' fields are node-scope names too, `team.world.*` and
 * the like, as they were when the header held them by name: the team blocks
 * end the header, so the node record reaches them.
 *
 * Naming. `transport.*` is the transport boundary's own vocabulary, defined here
 * rather than by each backend, so two backends are comparable in one harness
 * instead of each reporting its own words; a backend's private counters take a
 * further qualifier, `transport.local.*` and later `transport.am.*`. `shmem.*` is the API
 * layer. `pe.*`, `team.*` and `agent.*` are state. `invariant.*` must be zero at
 * quiescence, which the test suite asserts (§8.2), so they double as a
 * correctness oracle rather than only as diagnostics.
 */
#ifndef OSOE_METRICS_H
#define OSOE_METRICS_H

#include <stdint.h>

#include "base/counters.h"

/* Counters in a thread's block. Values are indices into the block, so the
 * order here is the segment's layout; the table records each one's offset, so
 * a reader never depends on it. */
enum osoe_counter {
    /* The transport boundary (transport.h), identical for every backend. */
    OSOE_C_TRANSPORT_PUT_CALLS, OSOE_C_TRANSPORT_PUT_BYTES,
    OSOE_C_TRANSPORT_GET_CALLS, OSOE_C_TRANSPORT_GET_BYTES,
    OSOE_C_TRANSPORT_RMW_CALLS, OSOE_C_TRANSPORT_RMW_FETCHING,
    OSOE_C_TRANSPORT_FENCE_CALLS,
    OSOE_C_TRANSPORT_WAIT_CALLS, OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING,
    OSOE_C_TRANSPORT_WAIT_NS, OSOE_C_TRANSPORT_WAIT_DIRTY_PEERS,
    OSOE_C_TRANSPORT_NOTIFY_CALLS, OSOE_C_TRANSPORT_NOTIFY_WAKES,
    OSOE_C_TRANSPORT_WATCH_CALLS, OSOE_C_TRANSPORT_WATCH_BLOCKED, OSOE_C_TRANSPORT_WATCH_BLOCKED_NS,
    OSOE_C_TRANSPORT_WATCH_WAKES, OSOE_C_TRANSPORT_WATCH_SPURIOUS, OSOE_C_TRANSPORT_WATCH_SWEEPS,
    OSOE_C_TRANSPORT_WATCH_SATURATED, OSOE_C_TRANSPORT_WATCH_LATE_WAKE,
    OSOE_C_TRANSPORT_SET_RANGE_CALLS,

    /* The API layer. */
    OSOE_C_SHMEM_BARRIER_CALLS, OSOE_C_SHMEM_BARRIER_NS,
    OSOE_C_SHMEM_MALLOC_CALLS, OSOE_C_SHMEM_FREE_CALLS, OSOE_C_SHMEM_REALLOC_CALLS,
    OSOE_C_SHMEM_FENCE_CALLS, OSOE_C_SHMEM_QUIET_CALLS, OSOE_C_SHMEM_PE_QUIET_CALLS,
    OSOE_C_SHMEM_WAIT_UNTIL_CALLS, OSOE_C_SHMEM_TEST_CALLS,
    OSOE_C_SHMEM_SIGNAL_UPDATE_CALLS, OSOE_C_SHMEM_SIGNAL_FETCH_CALLS,
    OSOE_C_SHMEM_SIGNAL_WAIT_CALLS,

    /* The agent thread's own block (§3.14's progress heartbeat). */
    OSOE_C_AGENT_LOOPS, OSOE_C_AGENT_WAKEUPS, OSOE_C_AGENT_LAST_WAKE_NS,
    /* A writer that found the open connection buffer full and had to ask the
     * lead to seal it and try again (§3.5, §3.6). Not an error: it is the
     * publication-window stall made visible rather than assumed rare. */
    OSOE_C_AM_RESERVE_STALLS,
    /* Wakes OSOE_TRANSPORT_MORE held back, each one a futex syscall the
     * following operation paid for instead (§4.10). */
    OSOE_C_AM_WAKES_DEFERRED,
    /* §3.10's wake of the lead, from the writer's side: sent because the lead
     * was asleep, or skipped because it was awake (lead_wake_model.py). */
    OSOE_C_AM_WAKES_SENT, OSOE_C_AM_WAKES_SKIPPED,
    /* Nonblocking gets (§4.6, as built): the requests issued, one per piece;
     * the pieces copied out of the landing arena, by a wait on their context
     * or early, by an issue on any context that found the zone full (phase 1
     * step 8: a wait completes its own context's gets, so the two differ by
     * what other contexts still have in flight); and the times an
     * issue found the landing records or arena used up and completed what was
     * in flight first - the zone's bound, working. */
    OSOE_C_AM_GETS_NONBLOCKING, OSOE_C_AM_GETS_COMPLETED, OSOE_C_AM_LANDING_FULL,

    /*
     * Must be zero at quiescence (§3.14 question 6, §8.2).
     *
     * missed_wake: a safety sweep expired and the predicate was *then* true, so
     * the update landed during the sleep, and no wake arrived - judged a margin
     * later, since a wake on its way looks the same for an instant (§4.9;
     * base/epoch.c, region-wait.h).
     *
     * It counts only waits on a variable the spec requires to be updated with
     * an AMO - every width of 4 or 8 bytes, which is every type of Table 6.
     * A late wake on a two-byte variable is expected rather than a bug: the
     * deprecated short waits have no AMO type at all, so the only remote update
     * is a put, for which notification is best effort by design (§4.10). Those
     * go to tp.watch.late_wake, which is an ordinary counter.
     *
     * Splitting them is what makes this an oracle rather than a diagnostic. A
     * single counter was nonzero for reasons that were not bugs, so the only
     * way to keep the §8.2 assertion true was to write the tests around it -
     * which is the kind of thing that stops being true without anyone noticing.
     *
     * watch_reentered: a watch published while one was already published. Under
     * SHMEM_THREAD_SERIALIZED a PE is in at most one wait at a time, so this is
     * the application calling the library from two threads at once.
     *
     * The third invariant, "a thread found no free counter block", is not here
     * but in the PE's slot: a thread that fails to claim a block writes to a
     * process-private one nobody can read, so a counter kept there could never
     * be seen by the tool or the test that exists to see it.
     */
    OSOE_C_INVARIANT_MISSED_WAKE,
    OSOE_C_INVARIANT_WATCH_REENTERED,

    /*
     * The block's own identity, in the block rather than in a parallel array so
     * that the table describes the whole segment with one mechanism. `claimed`
     * is the word a thread CASes to take the block - only this PE's own threads
     * ever claim its blocks, so the contention is process-local even though the
     * memory is shared. `tid` is what lets osoe-stat read
     * /proc/<pid>/task/<tid>/ for this thread's CPU time and involuntary
     * context switches, which is §3.14's rule that anything the kernel already
     * tracks is read by the tool and not maintained by the job.
     */
    OSOE_C_THREAD_CLAIMED,
    OSOE_C_THREAD_TID,

    OSOE_COUNTER_COUNT
};

_Static_assert((int)OSOE_COUNTER_COUNT <= (int)OSOE_COUNTERS_PER_BLOCK,
               "every counter must fit in one thread's block");

enum osoe_metric_type {
    OSOE_MTYPE_U64 = 1,
    OSOE_MTYPE_U32,
    OSOE_MTYPE_I32,
    OSOE_MTYPE_STRING,
};

enum osoe_metric_kind {
    OSOE_MKIND_COUNTER = 0,   /* monotonic; a rate is its difference over time */
    OSOE_MKIND_GAUGE,         /* a level, such as heap bytes in use */
    OSOE_MKIND_HIGHWATER,     /* the maximum ever seen */
    OSOE_MKIND_STATE,         /* not a number to add up: a rank, a pid, a phase */
    OSOE_MKIND_INVARIANT,     /* must be zero at quiescence */
};

enum osoe_metric_scope {
    OSOE_MSCOPE_NODE = 0,     /* a field of the header */
    OSOE_MSCOPE_PE,           /* a field of a PE's slot */
    OSOE_MSCOPE_THREAD,       /* a counter in a thread's block */
    OSOE_MSCOPE_CONNECTION,   /* a field of one connection's record (§3.14) */
    OSOE_MSCOPE_TEAM,         /* a field of one team number's block (layout 18) */
    OSOE_MSCOPE_EPOCHS,       /* one PE's published epoch for one team (layout 18) */
};

struct osoe_metric_descriptor {
    char     name[40];
    char     unit[8];         /* "", "bytes", "ns" */
    uint16_t type;            /* enum osoe_metric_type */
    uint16_t kind;            /* enum osoe_metric_kind */
    uint16_t scope;           /* enum osoe_metric_scope */
    uint16_t width;           /* bytes, for OSOE_MTYPE_STRING */
    uint32_t offset;          /* byte offset within that scope's record */
    uint32_t reserved;
};

_Static_assert(sizeof(struct osoe_metric_descriptor) == 64,
               "the metric descriptor is part of the segment's layout");

/*
 * The table, fixed for a build like the handler table of §3.13. The lead copies
 * it into the segment and records its checksum; every PE compares. Thread-scope
 * offsets are derived from the counter order rather than written twice, so the
 * table is assembled once on first use instead of being a compile-time array.
 */
const struct osoe_metric_descriptor *osoe_metrics_table(uint32_t *count);

/* FNV-1a over the table's bytes. */
uint32_t osoe_metric_checksum(const struct osoe_metric_descriptor *t, uint32_t n);

/*
 * This thread's counter block, claimed on first use and kept in thread-local
 * storage. A thread that finds no free block gets a process-private one and
 * bumps OSOE_C_THREADS_BLOCKS_EXHAUSTED in it: never an abort, and never a
 * block shared with another thread, so the single-writer rule holds without a
 * condition. OSOE_MAX_THREADS bounds the pool per PE.
 */
struct osoe_counter_block *osoe_metrics_block(void);

/* Bind this process to its PE's slice of the node's block pool, and release it.
 * Until osoe_metrics_init, and after osoe_metrics_fini, counting is a no-op on
 * a process-private block: the library counts the same either way and nothing
 * has to check whether it is initialized. */
void osoe_metrics_init(struct osoe_counter_block *pool, uint32_t per_pe, uint32_t local_id);
void osoe_metrics_fini(void);

/* How many blocks each PE gets: OSOE_MAX_THREADS, read at first init (§5.7). */
uint32_t osoe_metrics_blocks_per_pe(void);

/* Publish this PE's heap levels into its slot. Called by the allocator after
 * every change, which is a cold path (§4.4). */
void osoe_publish_heap_levels(void);

/* Shorthand for the common case. */
static inline void osoe_count(enum osoe_counter i, uint64_t n)
{
    osoe_counter_add(osoe_metrics_block(), (unsigned)i, n);
}

#endif /* OSOE_METRICS_H */
