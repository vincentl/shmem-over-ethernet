/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The job-wide rendezvous and the locality function (§6.2, §8.1).
 *
 * §6.2 has only node leads cross the network, each publishing its node's rank
 * list, its SHMEM_SYMMETRIC_SIZE and its backend's agreement word - on AM the
 * handler table's checksum - and the result being the rank -> node map every
 * PE needs before a message can be addressed at all. This checks that shape directly, across real processes and several
 * node maps - including the two §8.1 calls out as the ones that break things:
 * uneven node sizes, and ranks that are not contiguous per node.
 *
 * It does not call shmem_init: the rendezvous runs before a job is usable. Since
 * phase 1 step 3 its leads meet over sockets, in a tree (rendezvous.h), on the
 * loopback address osoe-launch gives each logical node.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/hostlist.h"
#include "blocked.h"
#include "bootstrap.h"
#include "check.h"
#include "job.h"

/*
 * The library's two fatals, in its order (fatal.h): the message, then the last
 * act, then exit. One write(2) each, as fatal.c does, so that
 * shmem.job.duplicate-rank.slow-message can hold the message back
 * (slow-message.c); stdio's writes cannot be interposed.
 */
static void report(const char *fmt, va_list ap)
{
    char msg[512];
    int n = snprintf(msg, sizeof msg, "osoe_fatal: ");
    n += vsnprintf(msg + n, sizeof msg - (size_t)n - 1, fmt, ap);
    if (n > (int)sizeof msg - 2)
        n = (int)sizeof msg - 2;
    msg[n++] = '\n';
    ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n);
    (void)ignored;
}

/* bootstrap.c reads the fan-out's default from the backend (internode.h), which
 * this test does not link; AM's value. */
uint32_t osoe_internode_fanout_default(void);
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

/* The backend's blob (transport.h): this test has no backend, so it describes
 * nothing, and the blobs every node learns stay zero. */
void osoe_transport_advertise(void *blob);
void osoe_transport_advertise(void *blob)
{
    (void)blob;
}

/* The backend's agreement word (internode-backend.h), which the rendezvous
 * compares between the leads (P27): this test has no backend, so a word of its
 * own - or, on a node given OSOE_TEST_AGREEMENT_WORD, that one, so that
 * shmem.job.disagree.agreement-word sees two binaries told apart. */
uint32_t osoe_internode_backend_agreement_word(void);
uint32_t osoe_internode_backend_agreement_word(void)
{
    const char *word = getenv("OSOE_TEST_AGREEMENT_WORD");
    return word ? (uint32_t)strtoul(word, NULL, 0) : UINT32_C(0x0501d0e5);
}

/* The "blocked in" record (blocked.h), which job.c and rendezvous.c keep while
 * a lead waits for its peers (D7): this test has no control segment to keep it
 * in, so it records nothing. */
void osoe_blocked_enter(const char *routine, enum osoe_blocked_detail kind, uint64_t detail)
{
    (void)routine;
    (void)kind;
    (void)detail;
}

void osoe_blocked_leave(void)
{
}

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    report(fmt, ap);
    va_end(ap);
    exit(1);
}

_Noreturn void osoe_fatal_then(void (*publish)(void *), void *context, const char *fmt, ...);
_Noreturn void osoe_fatal_then(void (*publish)(void *), void *context, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    report(fmt, ap);
    va_end(ap);
    publish(context);
    exit(1);
}

int main(void)
{
    struct osoe_bootstrap b = { 0 };
    osoe_bootstrap_read(&b);

    struct osoe_job job;
    osoe_job_join(&job, &b, 0, OSOE_SHM_ANY_CREATOR);

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

    /*
     * Each rank's local index on its own node (§6.2, layout 6): this node's
     * ranks are at their position in OSOE_GTIDS, and every rank's index is
     * below its node's rank count - which is what an RDMA backend forms a
     * remote heap address from, for a PE whose node it has never joined.
     */
    for (uint32_t i = 0; i < b.local_count; i++)
        CHECK(osoe_job_local_index(&job, b.gtids[i]) == i);
    for (uint32_t r = 0; r < job.world_size; r++) {
        uint32_t on_node = 0;
        for (uint32_t q = 0; q < job.world_size; q++)
            on_node += osoe_node_of(&job, q) == osoe_node_of(&job, r);
        CHECK(osoe_job_local_index(&job, r) < on_node);
    }
    CHECK(osoe_job_local_index(&job, job.world_size) == UINT32_MAX);

    /* The per-node backend blob (layout 4): this test's backend describes
     * nothing, so every node's arrives as zeros. */
    for (uint32_t n = 0; n < job.node_count; n++) {
        const unsigned char *blob = osoe_job_backend_blob(&job, n);
        CHECK(blob != NULL);
        for (uint32_t k = 0; blob && k < OSOE_JOB_BACKEND_BLOB_BYTES; k++)
            CHECK(blob[k] == 0);
    }
    CHECK(osoe_job_backend_blob(&job, job.node_count) == NULL);

    /* Each node's address, as the rendezvous learned it from the host list:
     * osoe-launch gives logical nodes 127.0.0.[1-N] (§8.1, §6.2), and tier 3
     * its namespaces' addresses (tools/launch/tier3.sh). */
    const char *list = getenv("OSOE_STEP_NODELIST");
    for (uint32_t n = 0; job.node_count > 1 && n < job.node_count; n++) {
        char host[64];
        struct in_addr want = { 0 };
        CHECK(list && osoe_hostlist_entry(list, n, host, sizeof host) &&
              inet_pton(AF_INET, host, &want) == 1);
        CHECK(osoe_job_node_address(&job, n) == want.s_addr);
    }

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
        CHECK(osoe_routes_in_process(&job, r) == same);
        CHECK(osoe_direct_access(&job, r) == same);
        local_seen += same;
    }
    CHECK(local_seen == b.local_count);

    /* Out of range is nobody's PE, and must not answer "local". */
    CHECK(!osoe_routes_in_process(&job, job.world_size));
    CHECK(!osoe_direct_access(&job, job.world_size));
    CHECK(osoe_node_of(&job, job.world_size) == UINT32_MAX);

    /*
     * §8.1's force-remote flag makes every PE remote **except this one**. It
     * has to be consulted inside the locality function or half the library
     * keeps taking the short path - so this checks the function, not the flag.
     *
     * The exception is not a concession to make the flag work. Connections
     * exist per node pair (§3.4), so there is none from a node to itself and an
     * operation addressed to a PE's own rank has nowhere to be routed; the flag
     * routes traffic between PEs, and a PE reading its own symmetric memory is
     * not traffic. An earlier version of this check required the caller's own
     * rank to be remote too, which made osoe_transport_load - the coherent load
     * behind every shmem_test and wait_until - a null dereference under the
     * flag, on the most-travelled path in the library and on none of the paths
     * the shmem_ptr checks cover.
     */
    struct osoe_job forced = job;
    forced.force_remote = true;
    for (uint32_t r = 0; r < job.world_size; r++) {
        /* The pointer is denied to a node-mate ... */
        CHECK(osoe_direct_access(&forced, r) == (r == b.rank));
        /* ... and the route is exactly what it was without the flag, which is
         * the half three comments claimed and the code did not do. */
        CHECK(osoe_routes_in_process(&forced, r) ==
              (osoe_node_of(&job, r) == b.node));
    }
    CHECK(osoe_direct_access(&forced, b.rank));
    CHECK(!osoe_direct_access(&forced, job.world_size));   /* still nobody's PE */
    CHECK(!osoe_routes_in_process(&forced, job.world_size));

    osoe_job_leave(&job);
    osoe_bootstrap_free(&b);
    return CHECK_DONE();
}

/* The library's notice (fatal.h), which the listener's waits print once they
 * have lasted (base/stall-watch.h): to stderr, one write(2) as the library's is,
 * so that the notices of two leads that wait at once do not run into one line -
 * shmem.job.disagree.root-node-count has two, and refusal.sh reads them. */
void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    char msg[512];
    int n = snprintf(msg, sizeof msg, "osoe_notice: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(msg + n, sizeof msg - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof msg - 2)
        n = (int)sizeof msg - 2;
    msg[n++] = '\n';
    ssize_t ignored = write(STDERR_FILENO, msg, (size_t)n);
    (void)ignored;
}
