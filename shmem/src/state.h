/* Process-wide library state, valid between the first shmem_init and the last
 * shmem_finalize. */
#ifndef OSOE_STATE_H
#define OSOE_STATE_H

#include <stdint.h>

#include <stdbool.h>

#include "allocator.h"
#include "bootstrap.h"
#include "connection-area.h"
#include "job.h"
#include "node.h"
#include "progress.h"
#include "reply.h"
#include "wire.h"

struct osoe_state {
    struct osoe_bootstrap bootstrap;
    struct osoe_node node;
    struct osoe_job job;
    struct osoe_allocator allocator;
    uint32_t world_epoch;           /* k for SHMEM_TEAM_WORLD (§5.4) */
    uint32_t shared_epoch;          /* and for SHMEM_TEAM_SHARED */

    /*
     * The AM backend, present only in a job of more than one logical node
     * (§7.1 step 2). `area` is mapped by every local PE, whose application
     * threads reserve into its buffers (§3.5); everything after it belongs to
     * the local lead, because only the lead's agent runs the engine (§3.12).
     * progress-host.c owns all of it.
     */
    bool internode;                 /* the job spans more than one logical node */
    bool lead;                      /* this PE is its node's lead */
    struct osoe_connection_area area;
    struct osoe_wire *wire;                      /* nodes - 1, in peer-slot order */
    struct osoe_progress_connection *connection; /* nodes - 1, in peer-slot order */
    struct osoe_reply *descriptor;               /* the node-wide pool (§3.8) */
    unsigned char *scratch;                      /* receive staging, one AM chunk */
    struct osoe_progress progress;
};

extern struct osoe_state osoe;

#endif /* OSOE_STATE_H */
