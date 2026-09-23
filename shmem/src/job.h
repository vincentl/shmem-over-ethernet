/*
 * The job-wide rendezvous, and the one locality function (§6.2, §8.1).
 *
 * §6.2: local PEs meet on their node; **only each node's lead crosses the
 * network**. The lead of node 0 is the step's rendezvous server, every other
 * node's lead connects to it, each sends its node's rank list, its
 * SHMEM_SYMMETRIC_SIZE, its handler-table checksum and its internode tree's
 * fan-out (OSOE_COLLECTIVE_FANOUT), the server checks that
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
    uint32_t me;                    /* this PE's world rank */
    const uint32_t *node_of;        /* world rank -> logical node, world_size long */
    const unsigned char *backend_blob;  /* node_count blobs (osoe_job_backend_blob) */
    const uint16_t *local_index;    /* world rank -> local id on its node (below) */
    bool force_remote;              /* §8.1: deny the pointer to a node-mate */
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

/*
 * A backend's per-node addressing blob (§6.2; second review, roce-seam F10).
 *
 * An RDMA backend cannot form a single remote address until it knows each
 * node's queue-pair or DC target number, GID, keys and registered heap base -
 * roughly 64 bytes per node - and that has to be exchanged before the first
 * operation, which is to say at the rendezvous. So the rendezvous carries one
 * fixed-size opaque blob per node, published by the node's lead together with
 * its rank list, and readable by every PE once the job has joined. Fixed-size
 * because the rendezvous is sized before anyone has spoken; opaque because
 * nothing above the backend may read it.
 *
 * AM uses none of it and publishes zeros: AM addresses a node by its number.
 * The call that lets a backend supply its blob before the job rendezvous
 * arrives with the backend that needs one; the space arrives now, because it is
 * the rendezvous's layout and changing that later is a protocol revision
 * (layout 4 of the job segment).
 */
enum { OSOE_JOB_BACKEND_BLOB_BYTES = 128 };

static inline const void *osoe_job_backend_blob(const struct osoe_job *j, uint32_t node)
{
    return node < j->node_count ? j->backend_blob + (size_t)node * OSOE_JOB_BACKEND_BLOB_BYTES
                                : NULL;
}

/*
 * A rank's local id on its *own* node, for any rank of the job (§6.2).
 *
 * The one fact about a remote PE that an RDMA backend needs and node_of cannot
 * give: it forms a remote address as the node's registered heap base plus
 * local index x heap size plus offset, and under Slurm's cyclic or arbitrary
 * distributions the index is not derivable from the node map (second review,
 * roce-seam F7). AM addresses by rank and never reads it.
 *
 * Shared, one copy per node, 2 bytes per rank: 2 MiB at 10^6 PEs, beside
 * node_of's 4 MiB. O(P) per node, accepted for now; §6.2 records the
 * range-compressed map as the next step. For this node's own ranks the
 * library uses osoe_node_local_id, which is O(L) private.
 */
static inline uint32_t osoe_job_local_index(const struct osoe_job *j, uint32_t pe)
{
    return pe < j->world_size ? j->local_index[pe] : UINT32_MAX;
}

/* The logical node a world rank is on. */
static inline uint32_t osoe_node_of(const struct osoe_job *j, uint32_t pe)
{
    return pe < j->world_size ? j->node_of[pe] : UINT32_MAX;
}

/*
 * Is this PE reachable by ordinary loads and stores from here?
 *
 * Same logical node, and not forced remote.
 *
 * **The node map is what says which PEs are remote** (§8.1): PEs rendezvous
 * only with the PEs named in their own node's OSOE_GTIDS, so a PE never maps a
 * heap outside its logical node and there is no address to return. Running the
 * same program over both paths and diffing is two node maps, not a flag.
 *
 * §8.1's force-remote flag is the narrower thing left over: it denies the
 * *pointer* to a node-mate without changing the route, so a program cannot be
 * written against the shared-memory shortcut and then quietly depend on it, and
 * a boundary that stopped consulting this function fails a test. It is
 * consulted here, in the one locality function, or half the library would keep
 * answering from somewhere else.
 *
 * **A PE is always local to itself, force-remote or not.** There is no route
 * from a node to itself: a connection exists per node *pair* (§3.4), so
 * osoe_connection_to returns NULL for this node by construction, and an
 * operation a PE addresses to its own rank has nowhere to go. It is not a
 * special case granted to make the flag work - it is what the flag means. §8.1
 * routes traffic between PEs over the network; a PE reading and writing its own
 * symmetric memory is not traffic, and the library's own coherent load
 * (osoe_transport_load) resolves through this function on every wait_until and
 * every shmem_test.
 */
static inline bool osoe_is_local(const struct osoe_job *j, uint32_t pe)
{
    if (pe == j->me)
        return true;
    return !j->force_remote && osoe_node_of(j, pe) == j->node;
}

#endif /* OSOE_JOB_H */
