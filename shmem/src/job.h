/*
 * The job-wide rendezvous, and the one locality function (§6.2, §8.1).
 *
 * §6.2: local PEs meet on their node; **only each node's lead crosses the
 * network**. The lead of node 0 is the step's rendezvous server, every other
 * node's lead connects to it, each sends its node's rank list, its
 * SHMEM_SYMMETRIC_SIZE and its handler-table checksum, the server checks that
 * they agree, and then broadcasts the rank -> node map.
 *
 * §7.1 step 2 has no sockets, so the meeting place here is a shared-memory
 * object rather than a listener, and the leads meet in memory. Everything else
 * is the same shape: only leads take part, each publishes exactly the three
 * things §6.2 says it publishes, the agreement checks are the same checks, and
 * the result is the same map. Step 3 replaces the object with the socket and
 * the handshake; nothing above this header changes.
 *
 * **Bootstrap is out of band in every build, and always will be** (§6.2). An
 * RDMA transport cannot bootstrap itself, and neither can this one: the map
 * that says which node a rank is on has to exist before any message can be
 * addressed. So this is not built on the AM layer, and it is not a place where
 * a backend gets to be clever.
 *
 * **The locality decision lives in exactly one function** (§8.1). Everything
 * that needs to know whether a PE is reachable by load and store consults
 * osoe_transport_direct, which consults this. An implementation that derived
 * locality from gethostname(), or from comparing heap pointers, would be right
 * on a cluster and wrong under the logical node map - and the node map is how
 * both paths get tested at all.
 */
#ifndef OSOE_JOB_H
#define OSOE_JOB_H

#include <stdbool.h>
#include <stdint.h>

#include "bootstrap.h"

/* Process-private view of the job. */
struct osoe_job {
    char name[96];
    void *map;
    uint64_t bytes;
    uint32_t world_size;
    uint32_t node_count;
    uint32_t node;                  /* this PE's logical node */
    const uint32_t *node_of;        /* world rank -> logical node, world_size long */
    bool force_remote;              /* §8.1: route even same-node traffic remotely */
    bool creator;                   /* node 0's lead made the object */
};

/*
 * Meet the other nodes' leads and learn the map. Every PE calls it; only leads
 * publish, as §6.2 requires, and the others read what their own lead wrote.
 * Fatal on disagreement or timeout (§3.15).
 */
void osoe_job_join(struct osoe_job *j, const struct osoe_bootstrap *b, uint32_t session);

/* Unmap; node 0's lead removes the name. */
void osoe_job_leave(struct osoe_job *j);

/* The logical node a world rank is on. */
static inline uint32_t osoe_node_of(const struct osoe_job *j, uint32_t pe)
{
    return pe < j->world_size ? j->node_of[pe] : UINT32_MAX;
}

/*
 * Is this PE reachable by ordinary loads and stores from here?
 *
 * Same logical node, and not forced remote. §8.1's force-remote flag routes
 * same-node traffic over the network so that the same program can be run both
 * ways and the results diffed - the strongest differential test available, and
 * the answer to "is the §4.10 boundary really carrying two backends" that is
 * running code rather than analysis. It has to be consulted *here*, in the one
 * locality function, or half the library would keep taking the short path.
 */
static inline bool osoe_is_local(const struct osoe_job *j, uint32_t pe)
{
    return !j->force_remote && osoe_node_of(j, pe) == j->node;
}

#endif /* OSOE_JOB_H */
