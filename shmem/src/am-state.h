/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The AM layer's process-wide state (§3, §7.1 step 2): the internode backend's
 * (internode-backend.h), below the transport boundary.
 *
 * Present only in a job of more than one logical node. `area` is mapped by
 * every local PE, whose application threads reserve into its buffers (§3.5);
 * everything after it belongs to the local lead, because only the lead's
 * process runs the engine (§3.12), each part of it on the thread that owns it
 * (owner-map.h): an owner's ring, wires and connections on that owner's
 * thread, the rest on owner 0's, the lead's agent. progress-host.c owns all of
 * it, and only the internode backend's own files include this header: the
 * part of the transport every job uses reaches it through internode-backend.h,
 * and the SHMEM layer through transport.h and internode.h
 * (cmake/CheckBoundary.cmake).
 */
#ifndef OSOE_AM_STATE_H
#define OSOE_AM_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "base/cacheline.h"
#include "connection-area.h"
#include "progress.h"
#include "reply.h"
#include "ring.h"
#include "wire.h"

/*
 * One owner's host side (owner-map.h): its ring, which carries its wires -
 * created disabled by the thread that opens the job's connections, and enabled
 * by the owner's own, which is then its only submitter (ring.h) - and its half
 * of the sleep protocol, private to its thread.
 *
 * A helper's besides (owner 1 to H; progress-host.c; helper_request_model.py):
 * its thread, which the lead's agent starts and joins, and the words the two
 * ask and answer by, in the lead's process, which alone runs either. Each word
 * has one writer: `request` is the agent's - a request's sequence above its
 * kind - and `answer`, `stop` and `turns` the helper's: the request it last
 * answered with its verdict, how far its stop has gone, and its turns,
 * counted seq_cst so that the agent can tell a helper that ignores its stop
 * from one that has not yet looked. And every owner's take of the unsent limit
 * the application thread asks (osoe_progress_host_limit_unsent): the request
 * it last took, its own, and, published, the one it has applied and how many
 * of its wires that reached, which the agent says once every owner has.
 *
 * Each owner's on lines of its own, as its words in the connection area are:
 * every pass of an owner writes its ring's fields, and with helpers the next
 * owner in the array is another thread (progress-host.c allocates them so).
 */
struct osoe_am_owner {
    _Alignas(OSOE_CACHELINE) struct osoe_ring ring;
    bool flag_raised;               /* its `sleeping` is raised and not yet lowered */
    pthread_t thread;
    bool started;                   /* the agent's: the thread was created */
    uint32_t posted;                /* the agent's: the request it posted last */
    _Atomic uint32_t request;
    _Atomic uint32_t answer;
    _Atomic uint32_t stop;          /* 0, then acknowledged, then done (progress-host.c) */
    _Atomic uint64_t turns;
    uint64_t unsent_taken;
    _Atomic uint64_t unsent_applied;
    _Atomic uint32_t unsent_reached;
};

struct osoe_am_state {
    /*
     * Two facts, set first by the internode backend's start
     * (transport-internode.c) rather than by whichever component happens to
     * need them first: the engine's own start-up reads both.
     */
    bool internode;                 /* started, and not yet stopped */
    bool lead;                      /* this PE is its node's lead */
    struct osoe_connection_area area;
    struct osoe_am_owner *owner;                 /* the area's helpers + 1, by owner */
    struct osoe_wire *wire;                      /* 2 x (nodes - 1), grouped by ring (owner-map.h):
                                                  * the data wires, and owner 0's control wires */
    struct osoe_progress_connection *connection; /* each owner's, on lines of its own (lines_of) */
    struct osoe_progress_control *control;       /* nodes - 1, owner 0's, by peer slot */
    uint64_t *control_busy;                      /* their busy bitmap */
    uint64_t *busy;                              /* each owner's bitmap in turn */
    struct osoe_reply *descriptor;               /* each owner's pool in turn (§3.8) */
    struct osoe_progress_team *team;             /* OSOE_TEAM_BLOCKS: per-block tree state */
    uint32_t *pending;                           /* as many block indices (progress.h) */
    struct osoe_progress progress;               /* its owners, helpers + 1, are allocated */
};

extern struct osoe_am_state osoe_am;

#endif /* OSOE_AM_STATE_H */
