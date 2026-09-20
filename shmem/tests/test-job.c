/*
 * The job-wide rendezvous and the locality function (§6.2, §8.1).
 *
 * §6.2 has only node leads cross the network, each publishing its node's rank
 * list, its SHMEM_SYMMETRIC_SIZE and its handler-table checksum, and the result
 * being the rank -> node map every PE needs before a message can be addressed
 * at all. This checks that shape directly, across real processes and several
 * node maps - including the two §8.1 calls out as the ones that break things:
 * uneven node sizes, and ranks that are not contiguous per node.
 *
 * It does not call shmem_init: the rendezvous runs before a job is usable, and
 * a multi-node job is still refused until the internode barrier lands.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap.h"
#include "check.h"
#include "job.h"

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

int main(void)
{
    struct osoe_bootstrap b = { 0 };
    osoe_bootstrap_read(&b);

    struct osoe_job job;
    osoe_job_join(&job, &b, 0);

    /* Every rank is on exactly one node, and this PE is on the node the
     * launcher said it was. */
    CHECK(job.world_size == b.world_size);
    CHECK(job.node_count == b.node_count);
    CHECK(osoe_node_of(&job, b.rank) == b.node);
    for (uint32_t r = 0; r < job.world_size; r++)
        CHECK(osoe_node_of(&job, r) < job.node_count);

    /* The map agrees with this node's own rank list, which is the half this PE
     * can check independently: OSOE_GTIDS named these ranks as local. */
    for (uint32_t i = 0; i < b.local_count; i++)
        CHECK(osoe_node_of(&job, b.gtids[i]) == b.node);

    /* Every node is used. A gap would mean the node count and the segments
     * disagree, which §8.1's map parser refuses at the launcher - so finding
     * one here would mean the two disagree about what was launched. */
    for (uint32_t n = 0; n < job.node_count; n++) {
        bool used = false;
        for (uint32_t r = 0; r < job.world_size && !used; r++)
            used = osoe_node_of(&job, r) == n;
        CHECK(used);
    }

    /*
     * The locality function. Every PE of this node is local and every PE of
     * another node is not - which is §8.1's requirement that shmem_ptr return
     * NULL across logical nodes even though the pages are physically
     * reachable, expressed at the one function everything else consults.
     */
    uint32_t local_seen = 0;
    for (uint32_t r = 0; r < job.world_size; r++) {
        bool same = osoe_node_of(&job, r) == b.node;
        CHECK(osoe_is_local(&job, r) == same);
        local_seen += same;
    }
    CHECK(local_seen == b.local_count);

    /* Out of range is nobody's PE, and must not answer "local". */
    CHECK(!osoe_is_local(&job, job.world_size));
    CHECK(osoe_node_of(&job, job.world_size) == UINT32_MAX);

    /*
     * §8.1's force-remote flag makes every PE remote, including this node's
     * own. It has to be consulted inside the locality function or half the
     * library keeps taking the short path - so this checks the function, not
     * the flag.
     */
    struct osoe_job forced = job;
    forced.force_remote = true;
    for (uint32_t r = 0; r < job.world_size; r++)
        CHECK(!osoe_is_local(&forced, r));

    osoe_job_leave(&job);
    osoe_bootstrap_free(&b);
    return CHECK_DONE();
}
