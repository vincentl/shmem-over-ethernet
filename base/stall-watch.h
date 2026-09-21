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
 */
#ifndef OSOE_BASE_STALL_WATCH_H
#define OSOE_BASE_STALL_WATCH_H

#include <stdbool.h>
#include <stdint.h>

struct osoe_stall_watch {
    uint64_t value;             /* the progress word as last seen */
    uint64_t since_ns;          /* CLOCK_MONOTONIC when it last changed */
    uint64_t limit_ns;          /* 0: never stalled */
};

/* Begin watching `value`, with the configured limit (0 = never). */
void osoe_stall_watch_begin(struct osoe_stall_watch *w, uint64_t value, uint64_t limit_ns);

/*
 * Look at the word again. True if it has not changed for the whole limit - and
 * never when the limit is 0. Any change restarts the interval, so a wait that is
 * slow but moving is never stalled however long it takes.
 */
bool osoe_stall_watch_stalled(struct osoe_stall_watch *w, uint64_t value);

/* How long the word has been unchanged, in whole seconds, for a message. */
uint64_t osoe_stall_watch_seconds(const struct osoe_stall_watch *w);

#endif /* OSOE_BASE_STALL_WATCH_H */
