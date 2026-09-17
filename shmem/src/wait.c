/*
 * Point-to-point synchronization: shmem_wait_until and shmem_test
 * (spec §9.11.1, §9.11.8; design §4.9).
 *
 * A wait is always on the calling PE's own variable, updated by an AMO from
 * any PE or thread (spec §9.11.1). The loop follows §4.9 exactly: publish the
 * watch before the first check, load the sequence word before the variable,
 * and wait on the sequence word with a safety-sweep deadline, so a lost wake
 * costs latency rather than the job.
 *
 * shmem_test never touches the slot: it is one coherent load and a compare.
 */
#include <stdbool.h>
#include <stdint.h>

#include <shmem.h>

#include "base/deadline.h"
#include "base/sweep.h"
#include "export.h"
#include "fatal.h"
#include "sym.h"
#include "transport.h"
#include "wait.h"

static bool satisfied(int cmp, uint64_t have, uint64_t want, const char *routine)
{
    switch (cmp) {
    case SHMEM_CMP_EQ: return have == want;
    case SHMEM_CMP_NE: return have != want;
    case SHMEM_CMP_GT: return have > want;
    case SHMEM_CMP_GE: return have >= want;
    case SHMEM_CMP_LT: return have < want;
    case SHMEM_CMP_LE: return have <= want;
    default:
        osoe_fatal("%s: comparison %d is not one of SHMEM_CMP_EQ, NE, GT, GE, LT, LE",
                   routine, cmp);
    }
}

/* Returns the value that satisfied the condition, which shmem_signal_wait_until
 * needs (spec §9.11.15) and the other waits discard. */
uint64_t osoe_wait_until(const char *routine, const void *ivar, unsigned width, int cmp,
                         uint64_t want)
{
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    struct osoe_tp_watch *w = osoe_tp_watch(OSOE_REGION_HEAP, offset, width);
    uint64_t have;
    for (;;) {
        uint32_t seq = osoe_tp_watch_seq(w);
        have = osoe_tp_load(OSOE_REGION_HEAP, offset, width);
        if (satisfied(cmp, have, want, routine))
            break;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        if (!osoe_tp_watch_wait(w, seq, &sweep))
            osoe_sweep_expired(routine);        /* only here, never on a wake */
    }
    osoe_tp_unwatch(w);
    return have;
}

int osoe_test(const char *routine, const void *ivar, unsigned width, int cmp,
              uint64_t want)
{
    uint64_t offset = osoe_sym_word_offset(routine, ivar, width);
    return satisfied(cmp, osoe_tp_load(OSOE_REGION_HEAP, offset, width), want, routine);
}

OSOE_API void shmem_uint64_wait_until(uint64_t *ivar, int cmp, uint64_t cmp_value)
{
    osoe_wait_until("shmem_uint64_wait_until", ivar, 8, cmp, cmp_value);
}

OSOE_API void shmem_uint32_wait_until(uint32_t *ivar, int cmp, uint32_t cmp_value)
{
    osoe_wait_until("shmem_uint32_wait_until", ivar, 4, cmp, cmp_value);
}

OSOE_API int shmem_uint64_test(uint64_t *ivar, int cmp, uint64_t cmp_value)
{
    return osoe_test("shmem_uint64_test", ivar, 8, cmp, cmp_value);
}

OSOE_API int shmem_uint32_test(uint32_t *ivar, int cmp, uint32_t cmp_value)
{
    return osoe_test("shmem_uint32_test", ivar, 4, cmp, cmp_value);
}
