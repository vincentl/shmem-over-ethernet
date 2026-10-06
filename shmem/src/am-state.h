/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The AM layer's process-wide state (§3, §7.1 step 2): the internode backend's
 * (internode-backend.h), below the transport boundary.
 *
 * Present only in a job of more than one logical node. `area` is mapped by
 * every local PE, whose application threads reserve into its buffers (§3.5);
 * everything after it belongs to the local lead, because only the lead's agent
 * runs the engine (§3.12). progress-host.c owns all of it, and only the
 * internode backend's own files include this header: the part of the transport
 * every job uses reaches it through internode-backend.h, and the SHMEM layer
 * through transport.h and internode.h (cmake/CheckBoundary.cmake).
 */
#ifndef OSOE_AM_STATE_H
#define OSOE_AM_STATE_H

#include <stdbool.h>

#include "connection-area.h"
#include "progress.h"
#include "reply.h"
#include "ring.h"
#include "wire.h"

struct osoe_am_state {
    /*
     * Two facts, set first by the internode backend's start
     * (transport-internode.c) rather than by whichever component happens to
     * need them first: the engine's own start-up reads both.
     */
    bool internode;                 /* started, and not yet stopped */
    bool lead;                      /* this PE is its node's lead */
    struct osoe_connection_area area;
    struct osoe_wire *wire;                      /* nodes - 1, in peer-slot order */
    struct osoe_ring ring;                       /* carries every wire (ring.h) */
    struct osoe_progress_connection *connection; /* nodes - 1, in peer-slot order */
    uint64_t *busy;                              /* (nodes - 1 + 63) / 64 words */
    struct osoe_reply *descriptor;               /* the node-wide pool (§3.8) */
    unsigned char *scratch;                      /* receive staging, one AM chunk */
    struct osoe_progress_team *team;             /* OSOE_TEAM_BLOCKS: per-block tree state */
    uint32_t *pending;                           /* as many block indices (progress.h) */
    struct osoe_progress progress;
};

extern struct osoe_am_state osoe_am;

#endif /* OSOE_AM_STATE_H */
