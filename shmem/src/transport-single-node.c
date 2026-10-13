/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The refusing backend: the single-node build's internode backend
 * (OSOE_SINGLE_NODE; internode-backend.h).
 *
 * The single-node build serves jobs of one node only. It needs no liburing,
 * no io_uring and no network, and runs on any kernel with the classic futex
 * (base/futex-classic.c) - a desktop on Red Hat 9 or Debian 12, for developing
 * at small PE counts. Everything a job of one node runs is the part of the
 * transport every build compiles (transport-am.c, transport-local.c); the
 * internode backend's files - the far half, the engine, the wires and the
 * ring, the connection area, the replies and the messages, the rendezvous of
 * the node leads, internode.h over AM - are not compiled at all, and this file
 * stands in their place.
 *
 * So every entry point here is one of two kinds. Reachable at start-up: the
 * check every PE of every job makes before its node joins, where a job of more
 * than one node is refused by name before anything is made in /dev/shm - the
 * full build runs it; and internode.h's calls a job of one node makes - the
 * default fan-out, init, fini, and a team number taken again - the last three
 * handed to the generic phase as AM hands them. Unreachable: everything else, which the part
 * every job uses calls only in a job of more than one node, and which ends the
 * job naming the entry point - reaching one is a defect there, never a job
 * this build should run.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bootstrap.h"
#include "fatal.h"
#include "internode.h"
#include "internode-backend.h"
#include "internode-generic.h"

/* An entry point a job of one node never reaches. */
static _Noreturn void unreachable(const char *entry)
{
    osoe_fatal("%s was called in the single-node build, which runs a job of one node only "
               "and has no internode backend: a defect in the part of the transport every "
               "job uses, which calls it only in a job of more than one node", entry);
}

/* ---- internode-backend.h: start-up --------------------------------------- */

/*
 * The one refusal a job meets in this build, asked by every PE before its node
 * joins: the job's node count is every local PE's to see, so each refuses it
 * by itself, and the part every job uses does so telling no one, as it does
 * the full build's io_uring refusal (transport-am.c) - the start-up-is-loud
 * rule's "every PE checks before its node joins", with nothing made in
 * /dev/shm. The settings are checked beside it as values in both builds, so no
 * setting is refused by one build and taken by the other.
 */
const char *osoe_internode_backend_check(const struct osoe_bootstrap *b, char *why,
                                         size_t len)
{
    if (b->node_count <= 1)
        return NULL;
    snprintf(why, len,
             "this is osoe's single-node build (OSOE_SINGLE_NODE), which runs a job of one "
             "node, and this job spans %u nodes: the full build runs it", b->node_count);
    return why;
}

void osoe_internode_backend_advertise(void *blob)
{
    (void)blob;
    unreachable("osoe_internode_backend_advertise");
}

void osoe_internode_backend_rendezvous(const struct osoe_bootstrap *b, uint32_t session,
                                       const void *blob, struct osoe_rendezvous_result *result)
{
    (void)b;
    (void)session;
    (void)blob;
    (void)result;
    unreachable("osoe_internode_backend_rendezvous");
}

void osoe_internode_backend_start(uint32_t session, bool check_quiet)
{
    (void)session;
    (void)check_quiet;
    unreachable("osoe_internode_backend_start");
}

void osoe_internode_backend_stop(void)
{
    unreachable("osoe_internode_backend_stop");
}

void osoe_internode_backend_link(uint64_t bytes_per_second)
{
    (void)bytes_per_second;
    unreachable("osoe_internode_backend_link");
}

/* ---- internode-backend.h: the agent thread ------------------------------- */

bool osoe_internode_backend_agent_pass(void)
{
    unreachable("osoe_internode_backend_agent_pass");
}

bool osoe_internode_backend_agent_wait(_Atomic uint32_t *wake, uint32_t seen)
{
    (void)wake;
    (void)seen;
    unreachable("osoe_internode_backend_agent_wait");
}

void osoe_internode_backend_agent_exit(void)
{
    unreachable("osoe_internode_backend_agent_exit");
}

void osoe_internode_backend_agent_abort(void)
{
    unreachable("osoe_internode_backend_agent_abort");
}

bool osoe_internode_backend_node_idle(uint64_t *applied)
{
    (void)applied;
    unreachable("osoe_internode_backend_node_idle");
}

bool osoe_internode_backend_node_quiet(uint64_t *applied)
{
    (void)applied;
    unreachable("osoe_internode_backend_node_quiet");
}

void osoe_internode_backend_deadlock_send(uint32_t node, const struct osoe_deadlock_message *m)
{
    (void)node;
    (void)m;
    unreachable("osoe_internode_backend_deadlock_send");
}

/* ---- internode-backend.h: contexts and operations ------------------------ */

bool osoe_internode_backend_context_open(struct osoe_transport_context *context)
{
    (void)context;
    unreachable("osoe_internode_backend_context_open");
}

void osoe_internode_backend_context_close(struct osoe_transport_context *context)
{
    (void)context;
    unreachable("osoe_internode_backend_context_close");
}

void osoe_internode_backend_put(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, const void *src,
                                size_t len, unsigned flags)
{
    (void)context;
    (void)pe;
    (void)region;
    (void)offset;
    (void)src;
    (void)len;
    (void)flags;
    unreachable("osoe_internode_backend_put");
}

void osoe_internode_backend_get(struct osoe_transport_context *context, int pe,
                                enum osoe_region region, uint64_t offset, void *dst, size_t len,
                                unsigned flags)
{
    (void)context;
    (void)pe;
    (void)region;
    (void)offset;
    (void)dst;
    (void)len;
    (void)flags;
    unreachable("osoe_internode_backend_get");
}

uint64_t osoe_internode_backend_rmw(struct osoe_transport_context *context, int pe,
                                    enum osoe_region region, uint64_t offset,
                                    enum osoe_rmw_op op, unsigned width, uint64_t operand,
                                    uint64_t compare, unsigned flags)
{
    (void)context;
    (void)pe;
    (void)region;
    (void)offset;
    (void)op;
    (void)width;
    (void)operand;
    (void)compare;
    (void)flags;
    unreachable("osoe_internode_backend_rmw");
}

void osoe_internode_backend_fence(struct osoe_transport_context *context)
{
    (void)context;
    unreachable("osoe_internode_backend_fence");
}

void osoe_internode_backend_wait(struct osoe_transport_context *context, const int *pes,
                                 size_t npes, const char *routine)
{
    (void)context;
    (void)pes;
    (void)npes;
    (void)routine;
    unreachable("osoe_internode_backend_wait");
}

void osoe_internode_backend_wait_gets(struct osoe_transport_context *context,
                                      const char *routine)
{
    (void)context;
    (void)routine;
    unreachable("osoe_internode_backend_wait_gets");
}

/* ---- internode.h -------------------------------------------------------- */

/*
 * A job of one node lays out its collectives by the fan-out as the full build
 * does (internode-backend.h), though no tree of it ever leaves the node.
 */
uint32_t osoe_internode_fanout_default(void)
{
    return OSOE_INTERNODE_FANOUT_DEFAULT;
}

/*
 * The three calls a job of one node makes, handed to the generic phase as AM
 * hands them (internode-am.c), so that the team numbers' counters are kept
 * the same in both builds. AM's own state for the arrivals' handoff it has
 * no use for here: nothing arrives at an internode phase on one node.
 */
void osoe_internode_init(void)
{
    osoe_internode_generic_init();
}

void osoe_internode_fini(void)
{
    osoe_internode_generic_fini();
}

void osoe_internode_number_taken(uint32_t number)
{
    osoe_internode_generic_number_taken(number);
}

/* Only a team whose record says it spans nodes calls these (barrier.c,
 * collectives.c), and on one node none does. */
void osoe_internode_arrive(uint32_t team, uint32_t epoch)
{
    (void)team;
    (void)epoch;
    unreachable("osoe_internode_arrive");
}

void osoe_internode_reduce_enter(uint32_t team, uint32_t epoch, size_t count,
                                 enum osoe_reduction_operation operation,
                                 struct osoe_element element, const char *routine)
{
    (void)team;
    (void)epoch;
    (void)count;
    (void)operation;
    (void)element;
    (void)routine;
    unreachable("osoe_internode_reduce_enter");
}

void osoe_internode_reduce(uint32_t team, uint32_t epoch, void *buffer, size_t count,
                           enum osoe_reduction_operation operation,
                           struct osoe_element element, const char *routine)
{
    (void)team;
    (void)epoch;
    (void)buffer;
    (void)count;
    (void)operation;
    (void)element;
    (void)routine;
    unreachable("osoe_internode_reduce");
}

void osoe_internode_broadcast(uint32_t team, uint32_t epoch, uint32_t root_index, void *buffer,
                              size_t bytes, const char *routine)
{
    (void)team;
    (void)epoch;
    (void)root_index;
    (void)buffer;
    (void)bytes;
    (void)routine;
    unreachable("osoe_internode_broadcast");
}

void osoe_internode_collect(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                            size_t block, const uint64_t *places, const char *routine)
{
    (void)team;
    (void)epoch;
    (void)buffer;
    (void)bytes;
    (void)block;
    (void)places;
    (void)routine;
    unreachable("osoe_internode_collect");
}

void osoe_internode_scan_node_totals(uint32_t team, uint32_t epoch, uint64_t own,
                                     uint64_t *before, uint64_t *total, const char *routine)
{
    (void)team;
    (void)epoch;
    (void)own;
    (void)before;
    (void)total;
    (void)routine;
    unreachable("osoe_internode_scan_node_totals");
}

void osoe_internode_collect_ranges(uint32_t team, uint32_t epoch, void *buffer, size_t bytes,
                                   uint64_t share_start, uint64_t share_end, bool reverse,
                                   const char *routine)
{
    (void)team;
    (void)epoch;
    (void)buffer;
    (void)bytes;
    (void)share_start;
    (void)share_end;
    (void)reverse;
    (void)routine;
    unreachable("osoe_internode_collect_ranges");
}

void osoe_internode_scan(uint32_t team, uint32_t rank, void *dest, const void *source,
                         size_t count, struct osoe_element element, bool inclusive,
                         const char *routine)
{
    (void)team;
    (void)rank;
    (void)dest;
    (void)source;
    (void)count;
    (void)element;
    (void)inclusive;
    (void)routine;
    unreachable("osoe_internode_scan");
}
