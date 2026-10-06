/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * A transport context (§4.6, §4.11; transport.h), as the AM backend's two
 * parts share it: transport-am.c, the part every job uses, makes and ends it,
 * and in a job of more than one node the internode backend keeps its own
 * state for it behind `internode` (internode-backend.h).
 *
 * Below the boundary, and included by those two files alone: the SHMEM layer
 * holds a context only by the pointer transport.h declares.
 */
#ifndef OSOE_TRANSPORT_CONTEXT_H
#define OSOE_TRANSPORT_CONTEXT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "node.h"
#include "state.h"

/* The internode backend's state for one context: on AM its dirty set and its
 * nonblocking gets in flight (transport-internode.c). */
struct osoe_internode_context;

struct osoe_transport_context {
    uint32_t block;             /* its block in this PE's pool (node.h): element index too */
    unsigned options;           /* OSOE_TRANSPORT_CONTEXT_*, from creation */
    bool live;
    /* Shared mode (§4.11; OSOE_TRANSPORT_CONTEXT_CONCURRENT): several threads
     * may call on it at once. Issue takes no lock; a wait holds `mutex`
     * (items 1, 2 and 4; phase 1 step 8 slice 3). Serial mode takes none. */
    bool concurrent;
    pthread_mutex_t mutex;
    /* NULL in a job of one node, which has nothing to keep for it: every
     * operation there is applied before its call returns. */
    struct osoe_internode_context *internode;
};

/* The context's block in this PE's pool in the node segment (node.h): its
 * counters, which the pool's state word says are taken. */
static inline struct osoe_context_block *osoe_transport_context_block(
    const struct osoe_transport_context *c)
{
    return osoe_node_context(&osoe.node, osoe.bootstrap.local_id, c->block);
}

#endif /* OSOE_TRANSPORT_CONTEXT_H */
