/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The mesh's start-up schedule (design §6.2; docs/design/mesh-schedule-proposal.md,
 * built by the author's decision A8 of 2026-10-11; H23): who calls whom, and in
 * which slot.
 *
 * **Who: a circulant.** Node i calls node (i + d) mod N at each offset d = 1 ..
 * (N - 1) / 2, and for an even N the offset N / 2 from the lower node of each
 * opposite pair. Every pair has exactly one caller; every node makes about N / 2
 * calls and answers the rest, where before it node 0 made N - 1 and the last
 * node none.
 *
 * **When: one offset a slot, the same on every node**, in one order shuffled
 * from the job's ids, so each slot is a permutation - every listener is called
 * once in it, but the opposite pairs' slot, where the upper half are called -
 * and the callers a late node held up go on to different nodes. A slot says
 * "not before": a node's call in slot s begins no earlier than s slots after
 * the rendezvous released it (OSOE_MESH_SLOT apart), and once its place in the
 * window of calls in flight is free, however late - never skipped.
 *
 * tools/modelcheck/mesh_model.py checks that every node's calls and accepts end
 * in a whole mesh, whatever the order and whenever each node arrives, and
 * writes the orders into shmem/tests/mesh-schedule-digest.txt, which
 * shmem.mesh-schedule holds this file to; mesh_timing.py times the schedule
 * against the order before it.
 */
#ifndef OSOE_MESH_SCHEDULE_H
#define OSOE_MESH_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The calls a node keeps in flight at once: four pairs, each its control wire
 * and its data wire. The proposal's four were one connection a pair, before
 * the control wire; mesh_timing.py has four pairs 1.4 to 1.7 times as fast as
 * four connections on the Pi-like link, at 256 and 1,024 nodes. Provisional,
 * the author's to set (H23's record).
 */
enum { OSOE_MESH_CALLS_IN_FLIGHT = 8 };

/* How many slots the schedule has: (N - 1) / 2 rounded up, one an offset. */
uint32_t osoe_mesh_schedule_slots(uint32_t node_count);

/*
 * The slots' offsets in the job's order, osoe_mesh_schedule_slots of them, into
 * `offsets`: 1 .. (N - 1) / 2 and an even N's N / 2, shuffled by Fisher-Yates
 * with draws from (job, step, N) - the same on every node of a job, without
 * telling each other, and on every run of it.
 */
void osoe_mesh_schedule_order(uint64_t job, uint32_t step, uint32_t node_count,
                              uint32_t *offsets);

/* Whether node `from` calls node `to`: the circulant's rule. */
bool osoe_mesh_schedule_calls(uint32_t from, uint32_t to, uint32_t node_count);

/* Whom `node` calls in a slot of `offset`, or node_count for nobody: the upper
 * half's in the opposite pairs' slot. */
uint32_t osoe_mesh_schedule_callee(uint32_t node, uint32_t offset, uint32_t node_count);

/* How many nodes call `node`. */
uint32_t osoe_mesh_schedule_callers(uint32_t node, uint32_t node_count);

#endif /* OSOE_MESH_SCHEDULE_H */
