/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The library's own counters in the pacer's stream (proposal §3, "The
 * library's own counters"): what osoe did underneath a checkpoint - bytes on
 * the wire, nonblocking gets, a full landing zone, stalls for buffer space,
 * wakes, time in barriers - read from each node's control segment by the
 * node's first PE (design §3.14), through the reader osoe-stat uses, which
 * finds every field by name. Where the segment cannot be read - another
 * library, a layout without a field - the counters are left out, never
 * guessed.
 */
#ifndef PACER_LIBRARY_COUNTERS_H
#define PACER_LIBRARY_COUNTERS_H

enum { PACER_LIBRARY_COUNTERS = 6 };

/* The counters' names in the segment, and in the pacer's line. */
extern const char *const pacer_library_counter_names[PACER_LIBRARY_COUNTERS];
extern const char *const pacer_library_field_names[PACER_LIBRARY_COUNTERS];

/* On every PE, after init: this node's first PE opens its node's segment.
 * Returns 1 where it did, 0 elsewhere. */
int pacer_library_counters_open(void);

/* What each counter grew by since the last call, summed over the node's
 * threads and connections; zeros on a PE that opened nothing. */
void pacer_library_counters_growth(double growth[PACER_LIBRARY_COUNTERS]);

void pacer_library_counters_close(void);

#endif /* PACER_LIBRARY_COUNTERS_H */
