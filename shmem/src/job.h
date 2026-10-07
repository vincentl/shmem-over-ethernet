/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The job-wide rendezvous, and the one locality function (§6.2, §8.1).
 *
 * §6.2: local PEs meet on their node; **only each node's lead crosses the
 * network**. Since phase 1 step 3 the leads meet over sockets, in a tree rooted
 * at node 0 (rendezvous.h), and each checks that the node below it agrees about
 * the job - its size, SHMEM_SYMMETRIC_SIZE, the handler table's checksum and
 * OSOE_COLLECTIVE_FANOUT - before the root builds the rank -> node map and every
 * lead learns it, with every node's address and backend blob.
 *
 * **What a lead learns it shares with its node, not its job.** The step-2
 * stand-in kept the map in one job-wide shared-memory object every PE of the
 * job mapped, which only works on one host. Now each lead writes what it learned
 * into a node-local object, /osoe.<job>.<step>.<node>.job.<session>, which its
 * node's PEs open by name like the control segment, and whose name the last of
 * them removes (§6.2's unlink-early rule).
 *
 * **The map is runs, not ranks** (rank-map.h): O(N) under block, O(L) under
 * cyclic, where it was 6 bytes per rank - 6 MiB per node at 10^6 PEs. Which node
 * a rank is on is then a binary search, O(log R), which only the network path
 * asks; whether a rank is on *this* node - the question every put asks - is
 * this node's own rank set, O(1) when its ranks are one progression.
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
#include "collective-shape.h"
#include "rank-map.h"
#include "shm-object.h"

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
 * nothing above the backend may read it. The backend fills it through
 * osoe_transport_advertise (transport.h) - on AM since step 3, the port of the
 * lead's node-pair listener.
 */
enum { OSOE_JOB_BACKEND_BLOB_BYTES = 128 };

/* Process-private view of the job. */
struct osoe_job {
    char name[96];
    void *map;                      /* the node-local object, or NULL on one node */
    uint64_t bytes;
    uint32_t world_size;
    uint32_t node_count;
    uint32_t node;                  /* this PE's logical node */
    uint32_t me;                    /* this PE's world rank */
    const struct osoe_rank_run *runs;   /* the rank map (rank-map.h), in rank order */
    uint32_t run_count;
    const uint32_t *ipv4;           /* each node's address, network order; NULL on one node */
    const unsigned char *backend_blob;  /* node_count blobs (osoe_job_backend_blob) */
    const uint32_t *node_lead;      /* node_count leads' ranks; NULL on one node */
    uint32_t one_node_lead;         /* the lead's rank on one node */
    struct osoe_rank_set here;      /* this node's ranks: the locality question */
    struct osoe_rank_run *owned_runs;   /* the map of a job of one node, freed at leave */
    bool force_remote;              /* §8.1: deny the pointer to a node-mate */
    /* The link the large collectives' shapes are chosen from, the same on
     * every PE, and where it came from (v0.2 step 2; link-measure.h): set at
     * the end of each session's start, all zero where its source is none. */
    struct osoe_link link;
    enum osoe_link_source link_source;
    uint64_t link_ns;               /* what establishing it cost this PE's start */
};

/*
 * Meet the other nodes' leads and learn the map. Every PE calls it; only leads
 * take part, as §6.2 requires, and the others read what their own lead wrote -
 * the tables `lead_pid` made, the pid of the node's lead that created the
 * control segment this PE has joined, and no leftover a failed start by the
 * same ids left ready (shm-object.h, OSOE_SHM_ANY_CREATOR). Fatal on
 * disagreement, and on no progress for OSOE_NO_PROGRESS_TIMEOUT (§3.15).
 */
void osoe_job_join(struct osoe_job *j, const struct osoe_bootstrap *b, uint32_t session,
                   int32_t lead_pid);

/* Unmap; free what the join built. */
void osoe_job_leave(struct osoe_job *j);

static inline const void *osoe_job_backend_blob(const struct osoe_job *j, uint32_t node)
{
    return node < j->node_count ? j->backend_blob + (size_t)node * OSOE_JOB_BACKEND_BLOB_BYTES
                                : NULL;
}

/* Node `node`'s address, IPv4 in network order, as the rendezvous learned it
 * from its host-list entry; 0 on a job of one node. */
static inline uint32_t osoe_job_node_address(const struct osoe_job *j, uint32_t node)
{
    return j->ipv4 && node < j->node_count ? j->ipv4[node] : 0;
}

/*
 * A rank's local id on its *own* node, for any rank of the job (§6.2).
 *
 * The one fact about a remote PE that an RDMA backend needs and the node alone
 * cannot give: it forms a remote address as the node's registered heap base
 * plus local index x heap size plus offset, and under Slurm's cyclic or
 * arbitrary distributions the index is not derivable from the node (second
 * review, roce-seam F7). AM addresses by rank and never reads it. O(log R).
 */
static inline uint32_t osoe_job_local_index(const struct osoe_job *j, uint32_t pe)
{
    const struct osoe_rank_run *r = pe < j->world_size
                                    ? osoe_rank_run_find(j->runs, j->run_count, pe) : NULL;
    return r ? r->first_local + (uint32_t)((int64_t)(pe - r->first_rank) * r->local_step)
             : UINT32_MAX;
}

/*
 * The world rank of node `node`'s lead - its PE with local id 0 - or UINT32_MAX
 * if there is no such node. O(1): the node's lead writes every node's lead into
 * the node-local object at the rendezvous (job.c), found from the runs once in
 * O(R + N), so that a broadcast can re-root its tree on every call
 * (internode-generic.c) without a scan of the map. N words, one copy per node, beside
 * the node_count addresses and blobs already there.
 */
static inline uint32_t osoe_job_node_lead(const struct osoe_job *j, uint32_t node)
{
    if (node >= j->node_count)
        return UINT32_MAX;
    return j->node_lead ? j->node_lead[node] : j->one_node_lead;
}

/* The logical node a world rank is on. O(log R): the network path's question. */
static inline uint32_t osoe_node_of(const struct osoe_job *j, uint32_t pe)
{
    const struct osoe_rank_run *r = pe < j->world_size
                                    ? osoe_rank_run_find(j->runs, j->run_count, pe) : NULL;
    return r ? r->first_node + (uint32_t)((int64_t)(pe - r->first_rank) * r->node_step)
             : UINT32_MAX;
}

/*
 * **Two questions, not one** (§8.1; second review, roce-seam F4, premises P2).
 *
 * "Which path carries an operation to this PE" and "may this PE's memory be
 * handed out as a pointer" are different questions, and one predicate answered
 * both until slice 3. §8.1's force-remote flag is about the second only - it
 * denies the pointer to a node-mate so that a program cannot be written against
 * the shared-memory shortcut and then quietly depend on it - but answering the
 * first with it sent a put to a node-mate over a connection that does not
 * exist: "no connection toward node N", 4 runs in 4, in a flag three comments
 * said left the route alone.
 *
 * They are also the split §4.10 needs for its own reason: dispatch is per
 * (pe, operation class), so a NIC whose atomics are not coherent with the CPU
 * can carry one PE's rmw while put and get to that same PE stay local - which
 * is `direct` answering non-NULL while the route for rmw is elsewhere. The two
 * predicates are where that becomes expressible rather than claimed.
 */

/*
 * Does an operation to this PE execute in this process - the intranode path,
 * applied in the calling thread (§4.1)?
 *
 * The node map decides it, and nothing else: PEs rendezvous only with the PEs
 * named in their own node's OSOE_GTIDS, so a PE never maps a heap outside its
 * logical node. A PE is always in its own process, and connections exist per
 * node *pair* (§3.4), so there is none from a node to itself. On every put and
 * get, so it asks this node's rank set - O(1) for a block or cyclic node - and
 * not the job's map.
 */
static inline bool osoe_routes_in_process(const struct osoe_job *j, uint32_t pe)
{
    return pe == j->me || osoe_rank_set_index(&j->here, pe) >= 0;
}

/*
 * May this PE's symmetric memory be reached by ordinary loads and stores from
 * here - the answer behind shmem_ptr, shmem_team_ptr and osoe_transport_direct?
 *
 * The route, minus §8.1's force-remote flag, which denies a node-mate the
 * pointer while leaving the traffic where it was. A PE is always direct to
 * itself, flag or not: osoe_transport_load - the coherent load behind every
 * shmem_test and wait_until - resolves through this on the caller's own rank,
 * so a rule that made a PE remote from itself is a null dereference on the
 * most-travelled path in the library.
 */
static inline bool osoe_direct_access(const struct osoe_job *j, uint32_t pe)
{
    return pe == j->me || (!j->force_remote && osoe_rank_set_index(&j->here, pe) >= 0);
}

#endif /* OSOE_JOB_H */
