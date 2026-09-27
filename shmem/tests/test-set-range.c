/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * osoe_transport_set_range (§4.4, §4.10): the allocator tells the transport which
 * ranges carry remote atomics or signals.
 *
 * The call has no observable value on a backend that ignores it - which the
 * intranode backend does, correctly, since every operation is already a memory
 * operation on a mapping it holds. So this counts the calls out of the control
 * segment (§3.14), the technique step 1 used for the same problem. What it
 * asserts is that the call site fires exactly where spec §9.3.5's hints say it
 * should and nowhere else, because a call site that never runs is the way this
 * arrives broken at the backend that needs it.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <shmem.h>

#include "check.h"
#include "invariants.h"
#include "segment.h"

/* This PE's total for a thread-scope counter, summed over its blocks. */
static uint64_t my_counter(const char *name)
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
    if (m) {
        uint32_t me = (uint32_t)shmem_my_pe();     /* rank == local id on one node */
        for (uint32_t i = 0; i < s.dir->blocks_per_pe; i++) {
            uint64_t v;
            if (osoe_segment_read(&s, m, me * s.dir->blocks_per_pe + i, &v))
                total += v;
        }
    } else
        total = UINT64_MAX;
    osoe_segment_close(&s);
    return total;
}

#define SET_RANGE_CALLS() my_counter("transport.set_range.calls")

int main(void)
{
    shmem_init();

    /* Nothing but a hinted allocation calls it, so it starts at zero however
     * much the library has already allocated for itself. */
    CHECK(SET_RANGE_CALLS() == 0);

    /* An unhinted allocation declares nothing: there is nothing to say. */
    uint64_t before = SET_RANGE_CALLS();
    void *plain = shmem_malloc(1024);
    CHECK(plain);
    CHECK(SET_RANGE_CALLS() == before);

    void *zeroed = shmem_calloc(16, 8);
    CHECK(zeroed);
    CHECK(SET_RANGE_CALLS() == before);

    void *aligned = shmem_align(64, 256);
    CHECK(aligned);
    CHECK(SET_RANGE_CALLS() == before);

    /* Each recognized hint declares its range. */
    void *atomics = shmem_malloc_with_hints(1024, SHMEM_MALLOC_ATOMICS_REMOTE);
    CHECK(atomics);
    CHECK(SET_RANGE_CALLS() == before + 1);

    void *signals = shmem_malloc_with_hints(1024, SHMEM_MALLOC_SIGNAL_REMOTE);
    CHECK(signals);
    CHECK(SET_RANGE_CALLS() == before + 2);

    /* Both hints at once are one call carrying both attributes, not two. */
    void *both = shmem_malloc_with_hints(1024, SHMEM_MALLOC_ATOMICS_REMOTE |
                                               SHMEM_MALLOC_SIGNAL_REMOTE);
    CHECK(both);
    CHECK(SET_RANGE_CALLS() == before + 3);

    /*
     * A hint this build does not recognize is kept with the block (§4.4) but
     * names no attribute, so there is nothing to declare and no call.
     */
    void *unknown = shmem_malloc_with_hints(1024, 1L << 20);
    CHECK(unknown);
    CHECK(SET_RANGE_CALLS() == before + 3);

    /*
     * spec §9.3.3 makes shmem_realloc preserve an allocation's hints, and
     * §4.10 takes the union of declared ranges rather than replacing them - so
     * a grown block is declared again, whether or not it had to move. Growing
     * into a heap where the hinted end is otherwise empty keeps it in place;
     * either way the call must happen, which is what this checks.
     */
    uint64_t at_realloc = SET_RANGE_CALLS();
    void *grown = shmem_realloc(atomics, 4096);
    CHECK(grown);
    CHECK(SET_RANGE_CALLS() == at_realloc + 1);
    atomics = grown;

    /* Shrinking is still a declaration, for the same reason: the call is not
     * conditional on the block having got bigger. */
    void *shrunk = shmem_realloc(atomics, 512);
    CHECK(shrunk);
    CHECK(SET_RANGE_CALLS() == at_realloc + 2);
    atomics = shrunk;

    /* Reallocating an unhinted block declares nothing, before or after. */
    uint64_t at_plain_realloc = SET_RANGE_CALLS();
    void *plain_grown = shmem_realloc(plain, 8192);
    CHECK(plain_grown);
    CHECK(SET_RANGE_CALLS() == at_plain_realloc);
    plain = plain_grown;

    /*
     * Freeing does not withdraw a range (§4.10): there is no un-declare, so
     * nothing is counted here either. Over-declaring is safe; a withdrawal
     * would have to be ordered against operations still in flight, which this
     * call must not do.
     */
    uint64_t at_free = SET_RANGE_CALLS();
    shmem_free(atomics);
    shmem_free(signals);
    shmem_free(both);
    shmem_free(unknown);
    shmem_free(plain);
    shmem_free(zeroed);
    shmem_free(aligned);
    CHECK(SET_RANGE_CALLS() == at_free);

    CHECK_INVARIANTS();
    shmem_finalize();
    return CHECK_DONE();
}
