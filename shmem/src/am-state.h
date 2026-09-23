/*
 * The AM backend's process-wide state (§3, §7.1 step 2), below the transport
 * boundary.
 *
 * Present only in a job of more than one logical node. `area` is mapped by
 * every local PE, whose application threads reserve into its buffers (§3.5);
 * everything after it belongs to the local lead, because only the lead's agent
 * runs the engine (§3.12). progress-host.c owns all of it, and only the AM
 * backend's own files include this header - the SHMEM layer reaches none of it
 * except through transport.h and internode.h (cmake/CheckBoundary.cmake).
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
     * Two facts about the job, settled at the very start of osoe_transport_init
     * rather than by whichever component happens to need them first. They were
     * once set inside the engine's own start-up, *after* the per-context
     * ordering state was sized from the first, so the first internode put wrote
     * through a null dirty set.
     */
    bool internode;                 /* the job spans more than one logical node */
    bool lead;                      /* this PE is its node's lead */
    struct osoe_connection_area area;
    struct osoe_wire *wire;                      /* nodes - 1, in peer-slot order */
    struct osoe_ring ring;                       /* carries every wire (ring.h) */
    struct osoe_progress_connection *connection; /* nodes - 1, in peer-slot order */
    uint64_t *busy;                              /* (nodes - 1 + 63) / 64 words */
    struct osoe_reply *descriptor;               /* the node-wide pool (§3.8) */
    unsigned char *scratch;                      /* receive staging, one AM chunk */
    struct osoe_progress progress;
};

extern struct osoe_am_state osoe_am;

#endif /* OSOE_AM_STATE_H */
