/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The job-wide rendezvous over sockets (§6.2; phase 1 step 3).
 *
 * **Only node leads take part, and they meet in a tree, not a star.** Node i's
 * lead connects to its parent, (i - 1) / 8, in a tree of fan-out 8 over the node
 * indices, rooted at node 0: §6.2's star put every lead's connection, and every
 * lead's copy of the job's tables, through node 0 - O(N) connections into one
 * listener and O(N^2) bytes out of one NIC, 2.2 GB of addresses and blobs
 * alone at 4,096 nodes. Here a
 * lead has one connection up and at most eight down, and sends each child the
 * tables once: O(N) bytes per edge, depth ceil(log8 N) - 4 at 4,096 nodes. The
 * fan-out is fixed, so no setting can make two leads disagree about who is
 * whose parent. (Decided with the author, 2026-09-22.)
 *
 * Up the tree, each lead sends its subtree's report: for every node in it, its
 * address, its ranks as progressions (rank-map.h), and its backend blob; and
 * the values every node must agree on - the job's size, SHMEM_SYMMETRIC_SIZE,
 * the handler table's checksum (§3.13), OSOE_COLLECTIVE_FANOUT (§5.3),
 * OSOE_MAX_TEAMS (§5.7) and OSOE_COLLECTIVE_PIPELINE (§5.8) - which
 * each parent checks against its own, so a disagreement is refused by the
 * lead that sees it, naming both nodes. The root builds the rank map's runs
 * from the reports (and refuses a rank claimed twice or not at all), and down
 * the tree every lead receives, and forwards, the runs, every node's address
 * and every node's blob.
 *
 * A lead's parent is found by §6.2's port rule on the address its host-list
 * entry names (listener.h); a lead listens on its own entry's address.
 *
 * **A lead calls its parent before it accepts its children**, and the hello
 * carries the node count. The count is the one value that shapes the waits,
 * since a lead expects children by it, so a disagreement about it is refused on
 * the first edge it crosses rather than leaving every lead waiting for a child
 * that does not exist (tools/modelcheck/rendezvous_model.py).
 *
 * Every read and write sweeps and gives up only by the no-progress rule
 * (§3.15). A lead that refuses the job ends it by a fatal, which the launcher
 * turns into the job's end (srun -K1, osoe-launch); its sockets close, and its
 * neighbours in the tree that were waiting on them end the job naming it.
 */
#ifndef OSOE_RENDEZVOUS_H
#define OSOE_RENDEZVOUS_H

#include <netinet/in.h>
#include <stdint.h>

#include "bootstrap.h"
#include "rank-map.h"

enum { OSOE_RENDEZVOUS_FANOUT = 8 };

/* What every lead learns: malloc'd, the caller frees. */
struct osoe_rendezvous_result {
    struct osoe_rank_run *runs;
    uint32_t run_count;
    uint32_t *ipv4;                 /* node_count addresses, network order */
    unsigned char *blobs;           /* node_count x OSOE_JOB_BACKEND_BLOB_BYTES */
};

/*
 * This lead's part (b->local_id == 0, b->node_count > 1). `blob` is this node's
 * backend blob, OSOE_JOB_BACKEND_BLOB_BYTES long, and `address` the address this
 * lead listens on - its own host-list entry's. Fatal on any disagreement.
 */
void osoe_rendezvous(const struct osoe_bootstrap *b, uint32_t session,
                     const struct sockaddr_in *address, const void *blob,
                     struct osoe_rendezvous_result *out);

#endif /* OSOE_RENDEZVOUS_H */
