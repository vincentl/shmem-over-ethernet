/*
 * OSOE_REGION_INTERNAL itself (§5.8; internal-region.h), below the collectives
 * that use it. Every PE puts a word into its right neighbour's region and adds
 * one to a count beside it with a fenced rmw, then waits for its own count and
 * reads the word its left neighbour put. Between node-mates that is the
 * intranode path (transport-local.c), which no collective takes - a node has
 * one lead - and across nodes the AM path (progress-host.c). Then the
 * application's reach: shmem_addr_accessible and shmem_ptr refuse the region's
 * address, which the API checks bound by the heap's size.
 *
 * White-box: it calls the transport, so the library's sources are compiled into
 * it (CMakeLists.txt), the shared library exporting nothing but the API. The
 * words it uses are the region's last 64 bytes: the second half of the last
 * team number's release-word line (internal-region.h), or the page rounding
 * past it where the region does not end on a page - never a word a collective
 * counts on, and no team numbered that high exists here.
 * It does not check the invariants: osoe-stat's view of the segment
 * (invariants.h) and the library's (node.h) cannot share a translation unit,
 * and every other tier-2 test checks them.
 */
#include <stdint.h>
#include <stdio.h>

#include <shmem.h>

#include "check.h"
#include "region-wait.h"
#include "state.h"
#include "transport.h"

int main(void)
{
    shmem_init();
    int me = shmem_my_pe(), npes = shmem_n_pes();
    int right = (me + 1) % npes, left = (me + npes - 1) % npes;

    uint64_t count = osoe.node.internal_bytes - 64, word = count + 8;
    struct osoe_transport_context *context = osoe_transport_context_default();
    uint64_t mine = UINT64_C(0x0501d00000000) + (uint64_t)me;
    osoe_transport_put(context, right, OSOE_REGION_INTERNAL, word, &mine, sizeof mine, 0);
    osoe_transport_rmw(context, right, OSOE_REGION_INTERNAL, count, OSOE_RMW_ADD, 8, 1, 0,
                       OSOE_TRANSPORT_FENCED);
    osoe_region_wait("test-internal-region", OSOE_REGION_INTERNAL, count, 8, false,
                     OSOE_COMPARISON_EQUAL, 1, OSOE_BLOCKED_DETAIL_EPOCH, 1);
    /* Read back two ways: the transport's load, and an address computed here
     * from the mapping, so that a transport resolving the region to the wrong
     * place for its writes and its reads alike is still caught. */
    unsigned char *region = (unsigned char *)osoe.node.heap_of[osoe.bootstrap.local_id] +
                            osoe.node.internal_offset;
    uint64_t want = UINT64_C(0x0501d00000000) + (uint64_t)left;
    uint64_t got = osoe_transport_load(OSOE_REGION_INTERNAL, word, 8);
    uint64_t direct = *(const volatile uint64_t *)(region + word);
    if (got != want || direct != want)
        fprintf(stderr, "PE %d: the word from PE %d reads %#llx, and %#llx in the mapping\n",
                me, left, (unsigned long long)got, (unsigned long long)direct);
    CHECK(got == want);
    CHECK(direct == want);

    /* The application cannot name it: the region starts past the heap. */
    CHECK(!shmem_addr_accessible(region, right));
    CHECK(shmem_ptr(region, me) == NULL);

    shmem_barrier_all();
    shmem_finalize();
    return CHECK_DONE();
}
