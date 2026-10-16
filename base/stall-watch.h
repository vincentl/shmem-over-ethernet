/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The no-progress rule (§3.15; decided 2026-09-20).
 *
 * No wait in the library gives up because time has passed. A wait may give up
 * only because the thing it watches has **stopped moving**: a progress word - a
 * count of acknowledged segments, of landed bytes, of joined PEs - that has not
 * changed for a configured interval. The default interval is 0, which means
 * never: a large machine is slow in ways no bound anticipates, and a job that
 * fails early under contention is worse than one that waits, since the launcher
 * can always tear a waiting job down and nothing can bring a failed one back.
 *
 * A watch is private to the waiting thread. It reads the clock only when asked,
 * which a wait does at its sweep cadence, never on a wake.
 *
 * A start-up wait also says what it waits for once it has lasted 30 s, and
 * again every minute (the round-three review's R8 and N1: a job whose start
 * had gone wrong waited in silence). That is a notice, never an end: it reads
 * how long the wait has lasted, not whether anything moved, and changes
 * nothing. A wait asks for it by beginning with osoe_stall_watch_begin_noticed,
 * which reads the clock once more; the waits of a running job never do.
 */
#ifndef OSOE_BASE_STALL_WATCH_H
#define OSOE_BASE_STALL_WATCH_H

#include <stdbool.h>
#include <stdint.h>

struct osoe_stall_watch {
    uint64_t value;             /* the progress word as last seen */
    uint64_t since_ns;          /* CLOCK_MONOTONIC when it last changed */
    uint64_t limit_ns;          /* 0: never stalled */
    uint64_t begun_ns;          /* when a noticed wait began; 0 for any other */
    uint64_t notice_ns;         /* when its next notice is due */
};

/* Begin watching `value`, with the configured limit (0 = never). */
void osoe_stall_watch_begin(struct osoe_stall_watch *w, uint64_t value, uint64_t limit_ns);

/* The same, for a start-up wait that says what it waits for once it has
 * lasted 30 s and every minute after (osoe_stall_watch_notice_due). The test
 * hook OSOE_TEST_NOTICE_SECONDS=N makes it N s and every 2N - not a setting. */
void osoe_stall_watch_begin_noticed(struct osoe_stall_watch *w, uint64_t value,
                                    uint64_t limit_ns);

/* Whether a notice of this wait is due now: true once at its first moment and
 * once at each later one, false for a wait not begun noticed. Reads the clock,
 * so a wait asks only at its sweep. */
bool osoe_stall_watch_notice_due(struct osoe_stall_watch *w);

/* How long a noticed wait has lasted, in whole seconds, for its notice. */
uint64_t osoe_stall_watch_waited_seconds(const struct osoe_stall_watch *w);

/*
 * Look at the word again. True if it has not changed for the whole limit - and
 * never when the limit is 0. Any change restarts the interval, so a wait that is
 * slow but moving is never stalled however long it takes.
 */
bool osoe_stall_watch_stalled(struct osoe_stall_watch *w, uint64_t value);

/* How long the word has been unchanged, in whole seconds, for a message. */
uint64_t osoe_stall_watch_seconds(const struct osoe_stall_watch *w);

/*
 * The limit of a start-up wait on another node: the configured one, and two
 * seconds more for each hop between - one for a lead's wait in the rendezvous,
 * two for its node-mates', which wait on their lead's - where a limit is set
 * at all (D15). A node waits for its own PEs to call shmem_init under the
 * limit itself, so a PE that never calls is named, rank and all, by its own
 * node before another node's wait on that node ends blaming the rendezvous:
 * with every limit alike either could end first, and the job's first line -
 * the one srun -K1 leaves - could be the one that names no rank.
 */
#define OSOE_STALL_WATCH_HOP_GRACE_NS UINT64_C(2000000000)
static inline uint64_t osoe_stall_watch_limit_beyond(uint64_t limit_ns, unsigned hops)
{
    return limit_ns ? limit_ns + hops * OSOE_STALL_WATCH_HOP_GRACE_NS : 0;
}

#endif /* OSOE_BASE_STALL_WATCH_H */
