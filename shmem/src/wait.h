/*
 * The point-to-point wait loop (§4.9), shared by shmem_wait_until, shmem_test
 * and the signal routines (spec §9.11, §9.8).
 *
 * `routine` names the caller in diagnostics. `width` is the variable's width in
 * bytes; values are carried as uint64_t, zero-extended, which is what the
 * boundary's load and rmw take.
 */
#ifndef OSOE_WAIT_H
#define OSOE_WAIT_H

#include <stdint.h>

/* Block until the variable satisfies the comparison, and return the value that
 * did. The value matters to shmem_signal_wait_until (spec §9.11.15), which
 * returns it; every other caller discards it. */
uint64_t osoe_wait_until(const char *routine, const void *ivar, unsigned width, int cmp,
                         uint64_t want);

/* One coherent load and a compare; never blocks and never touches the watch
 * slot. */
int osoe_test(const char *routine, const void *ivar, unsigned width, int cmp,
              uint64_t want);

#endif /* OSOE_WAIT_H */
