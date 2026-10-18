/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * The metric table and the per-thread counter blocks (§3.14). metrics.h states
 * the design; this is the table itself and the block claim.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "base/counters.h"
#include <stdlib.h>

#include "base/environment.h"
#include "fatal.h"
#include "blocked.h"
#include "metrics.h"
#include "node.h"
#include "state.h"

/* ---- The table -------------------------------------------------------- */

/*
 * Thread-scope entries are in counter order, so entry i describes counter i.
 * Its offset is i * 8 and is filled in when the table is assembled, rather than
 * written here as well, so the two cannot disagree.
 */
#define T(name, kind, unit) { name, unit, OSOE_MTYPE_U64, kind, OSOE_MSCOPE_THREAD, 0, 0, 0 }

static const struct osoe_metric_descriptor thread_metrics[] = {
    T("transport.put.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.put.bytes", OSOE_MKIND_COUNTER, "bytes"),
    T("transport.get.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.get.bytes", OSOE_MKIND_COUNTER, "bytes"),
    T("transport.rmw.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.rmw.fetching", OSOE_MKIND_COUNTER, ""),
    T("transport.fence.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.wait.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.wait.nothing_outstanding", OSOE_MKIND_COUNTER, ""),
    T("transport.wait.ns", OSOE_MKIND_COUNTER, "ns"),
    T("transport.wait.dirty_peers", OSOE_MKIND_COUNTER, ""),
    T("transport.notify.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.notify.wakes", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.calls", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.blocked", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.blocked_ns", OSOE_MKIND_COUNTER, "ns"),
    T("transport.watch.wakes", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.spurious", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.sweeps", OSOE_MKIND_COUNTER, ""),
    T("transport.watch.saturated", OSOE_MKIND_COUNTER, ""),
    /* A missed wake on a variable that has no AMO type, so it can only have
     * been written with a put: expected, not an invariant (see metrics.h). */
    T("transport.watch.late_wake", OSOE_MKIND_COUNTER, ""),
    T("transport.set_range.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.barrier.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.barrier.ns", OSOE_MKIND_COUNTER, "ns"),
    T("shmem.malloc.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.free.calls", OSOE_MKIND_COUNTER, ""),
    /* Every allocating routine counts as a malloc, since that is the family
     * §3.14 question 4 asks about. realloc is counted separately as well,
     * because it is the only one that both frees and allocates. */
    T("shmem.realloc.calls", OSOE_MKIND_COUNTER, ""),
    /* The API layer's own call counts, beside the boundary's. They are not the
     * same number: shmem_barrier_all begins with a wait too (§5.6), so
     * tp.wait.calls alone cannot say how often the application asked for a
     * quiet. No `.ns` here - on this backend neither ever blocks, and §3.14
     * forbids a clock read on a path that does not sleep. */
    T("shmem.fence.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.quiet.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.pe_quiet.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.wait_until.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.test.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.update.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.fetch.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.signal.wait.calls", OSOE_MKIND_COUNTER, ""),
    /* A split's calls; its rounds run again because a PE was busy (team.c;
     * layout 30) - its node's pool empty in the first round's claim, or with a
     * block on its way back; and, under SHMEM_THREAD_MULTIPLE, its rounds lost
     * to a number another split of a member's PE held. */
    T("shmem.team_split.calls", OSOE_MKIND_COUNTER, ""),
    T("shmem.team_split.busy_rounds", OSOE_MKIND_COUNTER, ""),
    T("shmem.team_split.lost_rounds", OSOE_MKIND_COUNTER, ""),
    T("agent.loops", OSOE_MKIND_COUNTER, ""),
    T("agent.wakeups", OSOE_MKIND_COUNTER, ""),
    T("agent.last_wake_ns", OSOE_MKIND_STATE, "ns"),
    T("am.reserve_stalls", OSOE_MKIND_COUNTER, ""),
    T("am.wakes_deferred", OSOE_MKIND_COUNTER, ""),
    T("am.wakes_sent", OSOE_MKIND_COUNTER, ""),
    T("am.wakes_skipped", OSOE_MKIND_COUNTER, ""),
    T("am.gets_nonblocking", OSOE_MKIND_COUNTER, ""),
    T("am.gets_completed", OSOE_MKIND_COUNTER, ""),
    T("am.landing_full", OSOE_MKIND_COUNTER, ""),
    T("am.issue_retries", OSOE_MKIND_COUNTER, ""),
    T("am.context_locks", OSOE_MKIND_COUNTER, ""),
    T("invariant.missed_wake", OSOE_MKIND_INVARIANT, ""),
    T("invariant.watch_reentered", OSOE_MKIND_INVARIANT, ""),
    T("thread.claimed", OSOE_MKIND_STATE, ""),
    T("thread.tid", OSOE_MKIND_STATE, ""),
};

_Static_assert(sizeof thread_metrics / sizeof thread_metrics[0] == OSOE_COUNTER_COUNT,
               "every counter needs a table entry, in counter order");

#define PE(name, field, type, kind, unit)                                   \
    { name, unit, type, kind, OSOE_MSCOPE_PE,                               \
      (uint16_t)sizeof(((struct osoe_pe_slot *)0)->field),                  \
      (uint32_t)offsetof(struct osoe_pe_slot, field), 0 }
#define ND(name, field, type, kind, unit)                                   \
    { name, unit, type, kind, OSOE_MSCOPE_NODE,                             \
      (uint16_t)sizeof(((struct osoe_node_header *)0)->field),              \
      (uint32_t)offsetof(struct osoe_node_header, field), 0 }
/* A field of the frozen directory, which a reader already knows how to find;
 * naming it in the table too costs nothing and keeps one way of reading. */
#define NDDIR(name, field, type, kind)                                      \
    { name, "", type, kind, OSOE_MSCOPE_NODE,                               \
      (uint16_t)sizeof(((struct osoe_segment_directory *)0)->field),        \
      (uint32_t)(offsetof(struct osoe_node_header, dir) +                   \
                 offsetof(struct osoe_segment_directory, field)), 0 }
/* One connection's record (§3.14's fourth scope): the index is the peer slot,
 * so a reader loops over dir.connection_count of them. */
#define CONN(name, field, type, kind, unit)                                 \
    { name, unit, type, kind, OSOE_MSCOPE_CONNECTION,                       \
      (uint16_t)sizeof(((struct osoe_am_connection_metrics *)0)->field),    \
      (uint32_t)offsetof(struct osoe_am_connection_metrics, field), 0 }
/* One owner's AM record (node.h, layout 36): the index is the owner - 0 the
 * lead's agent, 1 to OSOE_PROGRESS_HELPERS its helpers (owner-map.h) - so a
 * reader loops over dir.owner_count of them, and a node's count is their sum,
 * its high-water mark their largest. */
#define OWNER(name, field, type, kind, unit)                                \
    { name, unit, type, kind, OSOE_MSCOPE_OWNER,                            \
      (uint16_t)sizeof(((struct osoe_am_owner_metrics *)0)->field),         \
      (uint32_t)offsetof(struct osoe_am_owner_metrics, field), 0 }
/*
 * A word of a team's block (node.h, layout 18), by where it is in the block:
 * an epoch counter's value - a word inside a cache-line-padded structure, and
 * the word is what a reader loads - the arrival handoff's, the tree state's and
 * the record's. Each is named twice: once in the team scope, the index being
 * the team's number, which is how a reader finds every team; and for the
 * predefined teams once more in the node scope, by the names they had when the
 * header held them, since the blocks end the header.
 */
#define BLOCK_EPOCH(field)                                                  \
    (offsetof(struct osoe_team_block, field) +                              \
     offsetof(struct osoe_epoch, count_compare_only_by_equality))
/* The handoff's epoch, the low half of its word since layout 30 (node.h): the
 * word's first four bytes, osoe running little-endian only. */
#define BLOCK_ARRIVAL                                                       \
    (offsetof(struct osoe_team_block, arrival) + offsetof(struct osoe_arrival_handoff, word))
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "a 32-bit metric reads the low half of a 64-bit word at its address");
#define BLOCK_TREE(field)                                                   \
    (offsetof(struct osoe_team_block, tree) + offsetof(struct osoe_collective_state, field))
#define BLOCK_RECORD(field)                                                 \
    (offsetof(struct osoe_team_block, record) + offsetof(struct osoe_team_record, field))
#define BLOCK_NAME(field)                                                   \
    (offsetof(struct osoe_team_block, name) + offsetof(struct osoe_team_name, field))

#define TEAM(name, where)                                                   \
    { name, "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_TEAM, 4, (uint32_t)(where), 0 }
/* A record's child ranks, a name each, as many as the largest fan-out allows:
 * a reader shows those below the record's own count (osoe-stat). */
#define RECORD_CHILD(i)                                                     \
    TEAM("team.record.child." #i, BLOCK_RECORD(child) + (i) * sizeof(uint32_t))
_Static_assert(OSOE_COLLECTIVE_FANOUT_MAX == 32,
               "one name per child rank a record can hold (record_metrics below)");
#define NDTEAM(name, number, where)                                         \
    { name, "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,      \
      (uint32_t)(offsetof(struct osoe_node_header, team) +                  \
                 (number) * sizeof(struct osoe_team_block) + (where)), 0 }

/* One local PE's published epoch count for a team (§5.4): the low half of the
 * team's word of the PE's row, the block beside it since layout 30 (node.h). */
#define EPOCH(name, number)                                                 \
    { name, "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_EPOCHS, 4,    \
      (uint32_t)((number) * sizeof(uint64_t)), 0 }

static const struct osoe_metric_descriptor record_metrics[] = {
    PE("pe.rank", rank, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.pid", pid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    PE("pe.tid", tid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    PE("pe.phase", phase, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.agent_wake", agent_wake, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.symmetric_size", symmetric_size, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "bytes"),
    PE("pe.heap_bytes", heap_bytes, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "bytes"),
    /* The heap object's name, so osoe-stat --gc can remove what a job that
     * crashed during bootstrap left behind, without knowing how the name is
     * built. Everything else in the segment is found by name; the names of the
     * other objects a job owns should be too. */
    PE("pe.heap_name", heap_name, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    /* The program's symmetric statics (§4.3, layout 17): 0 bytes and no name
     * for a program linked without oshcc's script. */
    PE("pe.statics_bytes", statics_bytes, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "bytes"),
    PE("pe.statics_name", statics_name, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    PE("pe.watch_active", watch_active, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* Each PE's k per predefined team, by the names they had in its slot;
     * since layout 18 a word of its row of published epochs. */
    EPOCH("pe.world_epoch", OSOE_TEAM_WORLD),
    EPOCH("pe.shared_epoch", OSOE_TEAM_SHARED),
    EPOCH("pe.finalize_epoch", OSOE_TEAM_FINALIZE),
    PE("pe.sweeps", sweeps, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    /* The deadlock detector's word (layout 26; node.h): the PE's threads asleep
     * in library waits in the low half, the changes in that number in the high
     * half. Named in the table since P26, so that a test and osoe-stat's dump
     * can see what the detector counts. */
    PE("pe.waits", waits, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* An invariant at PE scope, because the thread it describes is by
     * definition one whose counter block nobody can read. */
    PE("invariant.blocks_exhausted", blocks_exhausted, OSOE_MTYPE_U32,
       OSOE_MKIND_INVARIANT, ""),
    PE("pe.blocked.gen", blocked_gen, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.routine", blocked_routine, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.detail", blocked_detail, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* What `detail` is: node.h's enum osoe_blocked_detail. */
    PE("pe.blocked.detail_kind", blocked_detail_kind, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    PE("pe.blocked.since_ns", blocked_since_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "ns"),
    PE("heap.bytes_in_use", heap_in_use, OSOE_MTYPE_U64, OSOE_MKIND_GAUGE, "bytes"),
    PE("heap.bytes_high_water", heap_high_water, OSOE_MTYPE_U64, OSOE_MKIND_HIGHWATER, "bytes"),
    PE("heap.largest_free_block", heap_largest_free, OSOE_MTYPE_U64, OSOE_MKIND_GAUGE, "bytes"),
    /* The most pieces the PE's last scan across nodes had in flight (§5.8;
     * layout 25): a count to pin the scan's overlap by. */
    PE("pe.scan.most_pieces_in_flight", scan_most_in_flight, OSOE_MTYPE_U32, OSOE_MKIND_STATE,
       ""),
    /*
     * Every block of the node's pool of teams (layout 18; by block since
     * layout 29), one record per block. A reader that walks them all -
     * osoe-stat does - shows those whose record is published, under the
     * record's name: team.world.*, team.3.*.
     *
     * §5.4's three counters; then §5.3's internode arrival handoff, the epoch a
     * local member posted for the lead's agent to forward. Against `released`
     * it is where a hung multi-node barrier usually is - posted ahead of
     * released means this node's arrival has not reached the root or the
     * release has not come back, and posted equal to released means this node
     * has not arrived at all, which points at a PE that has not called the
     * collective rather than at the network.
     */
    TEAM("team.arrived", BLOCK_EPOCH(arrived)),
    TEAM("team.released", BLOCK_EPOCH(released)),
    TEAM("team.copied", BLOCK_EPOCH(copied)),
    TEAM("team.arrival_posted", BLOCK_ARRIVAL),
    /*
     * The internode tree (§5.3): where this node is in it and how far the
     * epoch in progress has got. `tree.lowest_missing` is the one a hung
     * collective is walked down by - the lowest child node that owes this node
     * an arrival - and `node.hostname` says which machine to ask next.
     */
    TEAM("team.tree.epoch_open", BLOCK_TREE(epoch_open)),
    TEAM("team.tree.epoch_closed", BLOCK_TREE(epoch_closed)),
    TEAM("team.tree.own_arrived", BLOCK_TREE(own_arrived)),
    TEAM("team.tree.children", BLOCK_TREE(children)),
    TEAM("team.tree.children_arrived", BLOCK_TREE(children_arrived)),
    TEAM("team.tree.lowest_missing", BLOCK_TREE(lowest_missing)),
    TEAM("team.tree.parent", BLOCK_TREE(parent)),
    TEAM("team.tree.fanout", BLOCK_TREE(fanout)),
    /* The most pieces of a data collective the lead had in flight in its last
     * one (§5.8; layout 19): a count to pin the pipeline's overlap by. */
    TEAM("team.tree.most_pieces_in_flight", BLOCK_TREE(most_pieces_in_flight)),
    /* The steps the lead's last gather made and the (node, step) pairs its
     * lists send (§5.8, the allgather; layout 20): counts to pin the
     * algorithm by. */
    TEAM("team.tree.gather_steps", BLOCK_TREE(gather_steps)),
    TEAM("team.tree.gather_shares", BLOCK_TREE(gather_shares)),
    /* The class of the lead's last data collective through the tree, 0 small
     * and 1 large, and the lane its thread lent to the last large one (the
     * team redesign's step E; layout 31): which way it went, and whose slots. */
    TEAM("team.tree.collective_class", BLOCK_TREE(collective_class)),
    TEAM("team.tree.shape_fanout", BLOCK_TREE(shape_fanout)),
    TEAM("team.tree.shape_window", BLOCK_TREE(shape_window)),
    TEAM("team.tree.shape_children", BLOCK_TREE(shape_children)),
    /* The algorithm of the lead's last data collective past the intranode
     * phase, 0 a tree and 1 Träff's, and the rounds its last Träff's reduction
     * made (v0.2 step 3; layout 33): which way it went, by counts. */
    TEAM("team.tree.algorithm", BLOCK_TREE(algorithm)),
    TEAM("team.tree.circulant_rounds", BLOCK_TREE(circulant_rounds)),
    TEAM("team.tree.lane", BLOCK_TREE(lane)),
    /* The lengths the lead's last collect of differing blocks summed between
     * the nodes for its places, a node's total each or a member's length each
     * (P29; layout 38): which way its places were found, by a count. */
    TEAM("team.tree.collect_lengths", BLOCK_TREE(collect_lengths)),
    /* The way that collect found its places, an enum osoe_collect_way - 1 the
     * members' vector, 2 each node's total, 3 the leads' scan (P30; layout 39),
     * 4 the leads' exchange (P33). */
    TEAM("team.tree.collect_way", BLOCK_TREE(collect_way)),
    /* The record (node.h): whether there is one, and what it says. */
    TEAM("team.record.published", BLOCK_RECORD(published)),
    TEAM("team.record.local_members", BLOCK_RECORD(local_members)),
    TEAM("team.record.nodes", BLOCK_RECORD(nodes)),
    TEAM("team.record.index", BLOCK_RECORD(index)),
    TEAM("team.record.parent", BLOCK_RECORD(parent)),
    TEAM("team.record.position", BLOCK_RECORD(position)),
    TEAM("team.record.children", BLOCK_RECORD(children)),
    RECORD_CHILD(0), RECORD_CHILD(1), RECORD_CHILD(2), RECORD_CHILD(3),
    RECORD_CHILD(4), RECORD_CHILD(5), RECORD_CHILD(6), RECORD_CHILD(7),
    RECORD_CHILD(8), RECORD_CHILD(9), RECORD_CHILD(10), RECORD_CHILD(11),
    RECORD_CHILD(12), RECORD_CHILD(13), RECORD_CHILD(14), RECORD_CHILD(15),
    RECORD_CHILD(16), RECORD_CHILD(17), RECORD_CHILD(18), RECORD_CHILD(19),
    RECORD_CHILD(20), RECORD_CHILD(21), RECORD_CHILD(22), RECORD_CHILD(23),
    RECORD_CHILD(24), RECORD_CHILD(25), RECORD_CHILD(26), RECORD_CHILD(27),
    RECORD_CHILD(28), RECORD_CHILD(29), RECORD_CHILD(30), RECORD_CHILD(31),
    { "team.record.name", "", OSOE_MTYPE_STRING, OSOE_MKIND_STATE, OSOE_MSCOPE_TEAM,
      (uint16_t)sizeof(((struct osoe_team_record *)0)->name), (uint32_t)BLOCK_RECORD(name), 0 },
    /* The team's world ranks (decision E of 2026-09-26-day): its members, the
     * first one's rank and the stride between them when `strided` is 1, a
     * progression; SHMEM_TEAM_SHARED's family, which never leaves a node,
     * records 0. With its number they name a team alike on every node, which
     * the number alone has not since the team redesign's step D gave numbers
     * back (osoe-stat labels a team by both; the review of step D's merge
     * point, readiness Q5-2). */
    TEAM("team.record.members", BLOCK_RECORD(members)),
    TEAM("team.record.world_start", BLOCK_RECORD(world_start)),
    { "team.record.world_stride", "", OSOE_MTYPE_I32, OSOE_MKIND_STATE, OSOE_MSCOPE_TEAM, 4,
      (uint32_t)BLOCK_RECORD(world_stride), 0 },
    TEAM("team.record.strided", BLOCK_RECORD(strided)),
    /* The block's name in the pool (layout 29; the team redesign's step D):
     * the local id of the team's local lead here and its number, which the map
     * finds the block by; the local members holding it; and its generation,
     * raised when a team is written into it and again when the team gives it
     * back, so odd while a team holds it (layout 30) - what osoe-stat labels a
     * block by. */
    TEAM("team.name.lead", BLOCK_NAME(lead)),
    TEAM("team.name.number", BLOCK_NAME(number)),
    TEAM("team.name.holders", BLOCK_NAME(holders)),
    TEAM("team.name.generation", BLOCK_NAME(generation)),
    TEAM("team.name.members", BLOCK_NAME(members)),
    /* The predefined teams' fields in the node scope, by the names they had
     * when the header held them, for readers that find them so. */
    NDTEAM("team.world.arrived", OSOE_TEAM_WORLD, BLOCK_EPOCH(arrived)),
    NDTEAM("team.world.released", OSOE_TEAM_WORLD, BLOCK_EPOCH(released)),
    NDTEAM("team.world.copied", OSOE_TEAM_WORLD, BLOCK_EPOCH(copied)),
    NDTEAM("team.shared.arrived", OSOE_TEAM_SHARED, BLOCK_EPOCH(arrived)),
    NDTEAM("team.shared.released", OSOE_TEAM_SHARED, BLOCK_EPOCH(released)),
    NDTEAM("team.shared.copied", OSOE_TEAM_SHARED, BLOCK_EPOCH(copied)),
    NDTEAM("team.finalize.arrived", OSOE_TEAM_FINALIZE, BLOCK_EPOCH(arrived)),
    NDTEAM("team.finalize.released", OSOE_TEAM_FINALIZE, BLOCK_EPOCH(released)),
    NDTEAM("team.world.arrival_posted", OSOE_TEAM_WORLD, BLOCK_ARRIVAL),
    NDTEAM("team.shared.arrival_posted", OSOE_TEAM_SHARED, BLOCK_ARRIVAL),
    NDTEAM("team.finalize.arrival_posted", OSOE_TEAM_FINALIZE, BLOCK_ARRIVAL),
    NDTEAM("team.world.tree.epoch_open", OSOE_TEAM_WORLD, BLOCK_TREE(epoch_open)),
    NDTEAM("team.world.tree.epoch_closed", OSOE_TEAM_WORLD, BLOCK_TREE(epoch_closed)),
    NDTEAM("team.world.tree.own_arrived", OSOE_TEAM_WORLD, BLOCK_TREE(own_arrived)),
    NDTEAM("team.world.tree.children", OSOE_TEAM_WORLD, BLOCK_TREE(children)),
    NDTEAM("team.world.tree.children_arrived", OSOE_TEAM_WORLD, BLOCK_TREE(children_arrived)),
    NDTEAM("team.world.tree.lowest_missing", OSOE_TEAM_WORLD, BLOCK_TREE(lowest_missing)),
    NDTEAM("team.world.tree.parent", OSOE_TEAM_WORLD, BLOCK_TREE(parent)),
    NDTEAM("team.world.tree.fanout", OSOE_TEAM_WORLD, BLOCK_TREE(fanout)),
    NDTEAM("team.finalize.tree.epoch_open", OSOE_TEAM_FINALIZE, BLOCK_TREE(epoch_open)),
    NDTEAM("team.finalize.tree.epoch_closed", OSOE_TEAM_FINALIZE, BLOCK_TREE(epoch_closed)),
    NDTEAM("team.finalize.tree.own_arrived", OSOE_TEAM_FINALIZE, BLOCK_TREE(own_arrived)),
    NDTEAM("team.finalize.tree.children", OSOE_TEAM_FINALIZE, BLOCK_TREE(children)),
    NDTEAM("team.finalize.tree.children_arrived", OSOE_TEAM_FINALIZE,
           BLOCK_TREE(children_arrived)),
    NDTEAM("team.finalize.tree.lowest_missing", OSOE_TEAM_FINALIZE, BLOCK_TREE(lowest_missing)),
    NDTEAM("team.finalize.tree.parent", OSOE_TEAM_FINALIZE, BLOCK_TREE(parent)),
    /* One record per connection toward a peer node, in peer-slot order. */
    CONN("connection.peer", peer, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    CONN("connection.segments_sent", segments_sent, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    CONN("connection.bytes_sent", bytes_sent, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, "bytes"),
    CONN("connection.header_only_sent", header_only_sent, OSOE_MTYPE_U64,
         OSOE_MKIND_COUNTER, ""),
    CONN("connection.segments_applied", segments_applied, OSOE_MTYPE_U64,
         OSOE_MKIND_COUNTER, ""),
    CONN("connection.bytes_applied", bytes_applied, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
         "bytes"),
    CONN("connection.messages_applied", messages_applied, OSOE_MTYPE_U64,
         OSOE_MKIND_COUNTER, ""),
    CONN("connection.control_queue_depth", control_queue_depth, OSOE_MTYPE_U32,
         OSOE_MKIND_GAUGE, ""),
    CONN("connection.bulk_queue_depth", bulk_queue_depth, OSOE_MTYPE_U32,
         OSOE_MKIND_GAUGE, ""),
    CONN("connection.control_queue_high_water", control_queue_high_water, OSOE_MTYPE_U32,
         OSOE_MKIND_HIGHWATER, ""),
    CONN("connection.bulk_queue_high_water", bulk_queue_high_water, OSOE_MTYPE_U32,
         OSOE_MKIND_HIGHWATER, ""),
    CONN("connection.value_queue_depth", value_queue_depth, OSOE_MTYPE_U32,
         OSOE_MKIND_GAUGE, ""),
    CONN("connection.value_queue_high_water", value_queue_high_water, OSOE_MTYPE_U32,
         OSOE_MKIND_HIGHWATER, ""),
    CONN("connection.acked_through", acked_through, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    CONN("connection.wanted_through", wanted_through, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    CONN("connection.sent_through", sent_through, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.aborting", aborting, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* The rank again, as the word's low 32 bits - its first four bytes on the
     * little-endian targets - under the name an osoe-stat built before layout
     * 15 reads it by, when the rank was a field of its own. */
    { "node.abort_pe", "", OSOE_MTYPE_U32, OSOE_MKIND_STATE, OSOE_MSCOPE_NODE, 4,
      (uint32_t)offsetof(struct osoe_node_header, aborting), 0 },
    ND("node.world_size", world_size, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.job", job, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.step", step, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.id", node, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.session", session, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.heap_base", heap_base, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* The shape's setting and the link it is chosen from (layout 32, v0.2 step
     * 2): 0 for link and 1 for fixed; the link as OSOE_LINK_* set it, 0 where
     * they are not set. Durations, not instants, so no "ns" unit, which marks a
     * state as an instant on CLOCK_MONOTONIC (osoe-stat): the names say it. */
    ND("node.collective_shape", collective_shape, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.link.hop_ns", link_hop_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.link.message_ns", link_message_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.link.bytes_per_second", link_bytes_per_second, OSOE_MTYPE_U64, OSOE_MKIND_STATE,
       ""),
    ND("node.link.measured_hop_ns", measured_link_hop_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    ND("node.link.measured_message_ns", measured_link_message_ns, OSOE_MTYPE_U64,
       OSOE_MKIND_STATE, ""),
    ND("node.link.measured_bytes_per_second", measured_link_bytes_per_second, OSOE_MTYPE_U64,
       OSOE_MKIND_STATE, ""),
    /* How collect finds its places (layout 39, P30 and P31): 0 link, 1 members,
     * 2 totals, 3 scan, 4 exchange - OSOE_COLLECTIVE_PLACES. */
    ND("node.collective_places", collective_places, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    /* The lead that made the segment: while no PE has joined, the segment is a
     * start under way as long as it lives, and a failed start's once it is
     * gone, which osoe-stat --gc removes (§3.14). */
    ND("node.creator_pid", creator_pid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    ND("node.launch", launch, OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    /* The rendezvous words (§6.2). A job stuck in bootstrap is stuck in one of
     * these, and which one says how far it got. */
    ND("node.ready", ready, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.joined", joined, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.mapped", mapped, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.finalized", finalized, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    ND("node.failed", failed, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    /* The posted bitmap's first summary word (layout 30, node.h): a bit for
     * each 64 team blocks of which one has an arrival posted that the lead's
     * agent has not taken - at the default pool, the only word. Until layout
     * 30, node.team_blocks_published_from, and until 29
     * node.team_numbers_published. */
    ND("node.team_posted_summary", team_posted_summary[0], OSOE_MTYPE_U64, OSOE_MKIND_STATE, ""),
    NDDIR("node.local_count", local_count, OSOE_MTYPE_U32, OSOE_MKIND_STATE),
    NDDIR("node.blocks_per_pe", blocks_per_pe, OSOE_MTYPE_U32, OSOE_MKIND_STATE),
    /* The AM layer's counters, a record an owner (node.h, layout 36; the
     * node's own record from layout 8 to 35). */
    OWNER("transport.am.tid", tid, OSOE_MTYPE_I32, OSOE_MKIND_STATE, ""),
    OWNER("transport.am.data_wires", data_wires, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    OWNER("transport.am.control_wires", control_wires, OSOE_MTYPE_U32, OSOE_MKIND_STATE, ""),
    OWNER("transport.am.segments_sent", segments_sent, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.header_only_sent", header_only_sent, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.segments_applied", segments_applied, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.messages_applied", messages_applied, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.reply_reservation_failures", reply_reservation_failures, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.control_queue_high_water", control_queue_high_water, OSOE_MTYPE_U64,
       OSOE_MKIND_HIGHWATER, ""),
    OWNER("transport.am.bulk_queue_high_water", bulk_queue_high_water, OSOE_MTYPE_U64,
       OSOE_MKIND_HIGHWATER, ""),
    OWNER("transport.am.pool_high_water", pool_high_water, OSOE_MTYPE_U64, OSOE_MKIND_HIGHWATER, ""),
    OWNER("transport.am.value_queue_high_water", value_queue_high_water, OSOE_MTYPE_U64,
       OSOE_MKIND_HIGHWATER, ""),
    OWNER("transport.am.control_segments_sent", control_segments_sent, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.control_segments_applied", control_segments_applied, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.control_messages_applied", control_messages_applied, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    ND("node.hostname", hostname, OSOE_MTYPE_STRING, OSOE_MKIND_STATE, ""),
    OWNER("transport.am.passes", passes, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.send_visits", send_visits, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
       "conns"),
    /* The ring's counters (node.h, layout 13; §3.3). */
    OWNER("transport.am.completions", completions, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.receive_buffers", receive_buffers, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
       ""),
    OWNER("transport.am.receive_enobufs", receive_enobufs, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
       ""),
    OWNER("transport.am.receive_rearms", receive_rearms, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.sends", sends, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.short_sends", short_sends, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    /* Layout 30: a local release held for its forwards (node.h). */
    OWNER("transport.am.releases_held", releases_held, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    /* Layout 34: the zero-copy send (wire-settings.h). */
    OWNER("transport.am.zero_copy_sends", zero_copy_sends, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER,
       ""),
    OWNER("transport.am.zero_copy_notifications", zero_copy_notifications, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    OWNER("transport.am.zero_copy_copied", zero_copy_copied, OSOE_MTYPE_U64,
       OSOE_MKIND_COUNTER, ""),
    /* Layout 36: the owner's waits its wake word ended (node.h). */
    OWNER("transport.am.wakes", wakes, OSOE_MTYPE_U64, OSOE_MKIND_COUNTER, ""),
    /* Layout 40: when the owner's last pass ended, an instant (node.h). */
    OWNER("transport.am.last_pass_ns", last_pass_ns, OSOE_MTYPE_U64, OSOE_MKIND_STATE, "ns"),
};

enum {
    THREAD_N = sizeof thread_metrics / sizeof thread_metrics[0],
    RECORD_N = sizeof record_metrics / sizeof record_metrics[0],
    TABLE_N = THREAD_N + RECORD_N,
};

const struct osoe_metric_descriptor *osoe_metrics_table(uint32_t *count)
{
    static struct osoe_metric_descriptor table[TABLE_N];
    static bool built;

    if (!built) {
        for (uint32_t i = 0; i < THREAD_N; i++) {
            table[i] = thread_metrics[i];
            table[i].offset = i * (uint32_t)sizeof(uint64_t);
            table[i].width = (uint16_t)sizeof(uint64_t);
        }
        memcpy(table + THREAD_N, record_metrics, sizeof record_metrics);
        built = true;
    }
    *count = TABLE_N;
    return table;
}

uint32_t osoe_metric_checksum(const struct osoe_metric_descriptor *t, uint32_t n)
{
    const unsigned char *p = (const unsigned char *)t;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)n * sizeof *t; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* ---- Per-thread blocks ------------------------------------------------ */

/* A PE's blocks: OSOE_MAX_THREADS of them, read and checked by every PE in the
 * bootstrap (bootstrap.h says why 16), and the agent's one more; the lead lays
 * them out (node.c, create_control) and every PE is held to its count. */

static struct osoe_counter_block *pool;         /* this PE's slice of the node pool */
static uint32_t pool_blocks;
static _Thread_local struct osoe_counter_block *my_block;
/*
 * Which session `pool` is, and which one this thread's `my_block` was claimed
 * in: a thread that claimed a block in a session since ended must not use a
 * pointer into a segment that is gone (phase 1 step 8 slice 2). Every session
 * counts up; a thread whose count is not the current one claims again.
 */
static _Atomic uint64_t pool_session;
static _Thread_local uint64_t my_session;
/* A thread's block is given back at its exit (phase 1 step 8 slice 2), by the
 * destructor of this key, whose value is the block. */
static pthread_key_t release_key;
static pthread_once_t release_once = PTHREAD_ONCE_INIT;

/*
 * The block every thread uses before init and after fini; a thread that finds
 * the pool full ends the job instead (osoe_metrics_block). Thread-local, so it
 * is never shared and the single-writer rule holds without a condition; nobody
 * reads it.
 */
static _Thread_local struct osoe_counter_block orphan;

/* This thread counts into `orphan` whatever the pool holds: a progress helper
 * (osoe_metrics_unpooled). */
static _Thread_local bool unpooled;

/* Held by a thread giving its block back at exit, and by finalize while it
 * lets go of the pool, so that a thread exiting during a finalize never
 * stores into a segment that finalize is about to unmap (review of slice 2,
 * S4). Only at thread exit and at finalize: nothing that counts takes it. */
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Whether the PE's threads may be in the library at once (THREAD_MULTIPLE):
 * the blocked record's owner then needs `record_mutex`, and otherwise nothing
 * takes it, since below MULTIPLE nothing takes a lock it did not before
 * (review of slice 2, S5; the slice's brief, decision 1). */
static bool records_concurrent;

void osoe_metrics_threads(bool concurrent)
{
    records_concurrent = concurrent;
}

void osoe_metrics_init(struct osoe_counter_block *blocks, uint32_t per_pe, uint32_t local_id)
{
    pool = blocks + (size_t)local_id * per_pe;
    pool_blocks = per_pe;
    atomic_fetch_add_explicit(&pool_session, 1, memory_order_seq_cst);
    my_block = NULL;
}

void osoe_metrics_fini(void)
{
    pthread_mutex_lock(&pool_mutex);
    pool = NULL;
    pool_blocks = 0;
    pthread_mutex_unlock(&pool_mutex);
    my_block = NULL;
}

/* At a thread's exit: its block back to the pool, idle - a thread cannot exit
 * inside a call, so it holds no watch. Only for a block of the session still
 * running; one of an ended session went with its segment. */
static void release_block(void *value)
{
    struct osoe_counter_block *block = value;
    pthread_mutex_lock(&pool_mutex);
    if (pool && my_session == atomic_load_explicit(&pool_session, memory_order_acquire) &&
        block >= pool && block < pool + pool_blocks)
        atomic_store_explicit(&block->c[OSOE_C_THREAD_CLAIMED], 0, memory_order_seq_cst);
    pthread_mutex_unlock(&pool_mutex);
    /* Forgotten here too: a destructor of the program's that runs after this
     * one and calls the library claims a block afresh, rather than share this
     * one with the thread that claims it next (review of slice 2, S3). */
    my_block = NULL;
    my_session = 0;
}

static void make_release_key(void)
{
    if (pthread_key_create(&release_key, release_block) != 0)
        osoe_fatal("no thread-specific key for giving counter blocks back at thread exit");
}

void osoe_metrics_unpooled(void)
{
    unpooled = true;
    my_block = &orphan;
}

bool osoe_metrics_thread_unpooled(void)
{
    return unpooled;
}

struct osoe_counter_block *osoe_metrics_block(void)
{
    if (unpooled)
        return &orphan;
    uint64_t session = atomic_load_explicit(&pool_session, memory_order_acquire);
    if (my_block && (my_block == &orphan ? !pool : my_session == session))
        return my_block;
    if (!pool)
        return my_block = &orphan;

    for (uint32_t i = 0; i < pool_blocks; i++) {
        uint64_t unclaimed = 0;
        if (atomic_compare_exchange_strong_explicit(&pool[i].c[OSOE_C_THREAD_CLAIMED],
                                                    &unclaimed, 1, memory_order_seq_cst,
                                                    memory_order_seq_cst)) {
            my_block = &pool[i];
            my_session = session;
            osoe_counter_set(my_block, OSOE_C_THREAD_TID, (uint64_t)(uint32_t)gettid());
            pthread_once(&release_once, make_release_key);
            pthread_setspecific(release_key, my_block);
            return my_block;
        }
    }
    /* No block left: every one belongs to a live thread of this PE that has
     * called the library, since a thread gives its block back only at exit,
     * called again or not. A thread with none would have no record to wait in
     * either (§4.11 item 5), so it ends the job by name at every thread level
     * (decided 2026-09-25): a program with more live calling threads raises
     * OSOE_MAX_THREADS. The PE's slot says so first, for osoe-stat. */
    atomic_fetch_add_explicit(&osoe.node.slots[osoe.bootstrap.local_id].blocks_exhausted, 1,
                              memory_order_seq_cst);
    osoe_fatal("more live threads of this PE have called the library than OSOE_MAX_THREADS=%u "
               "allows (a thread keeps its place until it exits); raise it (at most %u)",
               pool_blocks - 1, (unsigned)OSOE_MAX_THREADS_HIGHEST - 1);
}

uint32_t osoe_metrics_thread_index(void)
{
    struct osoe_counter_block *block = osoe_metrics_block();
    if (block == &orphan)
        osoe_fatal(unpooled ? "a progress helper asked for a thread record, which it has none of"
                            : "a thread asked for its record outside a session");
    return (uint32_t)(block - pool);
}

/* ---- The "blocked in" record (blocked.h) ------------------------------ */

uint64_t osoe_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* This PE's record (blocked.c has the protocol and why it holds). */
static void publish(const char *routine, enum osoe_blocked_detail kind, uint64_t detail,
                    uint64_t since)
{
    osoe_blocked_publish(&osoe.node.slots[osoe.bootstrap.local_id], routine, kind, detail,
                         since);
}

/* The routine of the record, for this process's own agent (osoe_blocked_routine):
 * a pointer to a name with static storage, published alone, so that a thread
 * of this process reads one word rather than the record's whole protocol. */
static _Atomic(const char *) waiting_in;

/*
 * Waits nest - a barrier quiets inside its own wait, and an allocation's
 * barrier inside the allocation's - so each thread keeps the waits it is in,
 * and leaving the inner one restores the outer, in the record and the pointer
 * alike. Without it an inner wait's leave cleared both, and a PE blocked in a
 * barrier after a remote put read as waiting in nothing (review of the
 * watchdog's retirement). Deeper than the stack holds, the outermost are kept.
 */
enum { BLOCKED_NESTING = 4 };
struct blocked_wait {
    const char *routine;
    enum osoe_blocked_detail kind;
    uint64_t detail, since;
};
static _Thread_local struct blocked_wait open_waits[BLOCKED_NESTING];
static _Thread_local unsigned open_count;

/*
 * The record is the PE's, and under THREAD_MULTIPLE several of its threads may
 * wait at once (phase 1 step 8 slice 2): the first to wait owns it until it
 * leaves its outermost wait, and the others do not publish, so the record
 * shows one blocked thread whole rather than two torn together. `owned` is
 * whether this thread owns it; `owner_taken` whether any thread does, both
 * changed under `record_mutex` under THREAD_MULTIPLE - on the way into and out
 * of every recorded wait, a barrier's included - and below it under nothing,
 * since then one thread at a time calls the library.
 */
static pthread_mutex_t record_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool owner_taken;
static _Thread_local bool owned;

void osoe_blocked_enter(const char *routine, enum osoe_blocked_detail kind, uint64_t detail)
{
    uint64_t since = osoe_now_ns();
    if (open_count < BLOCKED_NESTING)
        open_waits[open_count] = (struct blocked_wait){ routine, kind, detail, since };
    open_count++;
    bool lock = records_concurrent;
    if (lock)
        pthread_mutex_lock(&record_mutex);
    if (!owned && !owner_taken)
        owned = owner_taken = true;
    if (owned) {
        if (osoe.node.slots)
            publish(routine, kind, detail, since);
        atomic_store_explicit(&waiting_in, routine, memory_order_release);
    }
    if (lock)
        pthread_mutex_unlock(&record_mutex);
}

void osoe_blocked_leave(void)
{
    if (open_count > 0)
        open_count--;
    const struct blocked_wait *outer = open_count > 0 && open_count <= BLOCKED_NESTING
                                           ? &open_waits[open_count - 1] : NULL;
    bool lock = records_concurrent;
    if (lock)
        pthread_mutex_lock(&record_mutex);
    if (owned) {
        atomic_store_explicit(&waiting_in, outer ? outer->routine : NULL, memory_order_release);
        if (osoe.node.slots) {
            if (outer)
                publish(outer->routine, outer->kind, outer->detail, outer->since);
            else
                publish(NULL, OSOE_BLOCKED_DETAIL_NONE, 0, 0);
        }
        if (!outer)
            owned = owner_taken = false;
    }
    if (lock)
        pthread_mutex_unlock(&record_mutex);
}

const char *osoe_blocked_routine(void)
{
    return atomic_load_explicit(&waiting_in, memory_order_acquire);
}

/* blocked.h's finalizing word: the process's one thread in shmem_finalize
 * stores it, and any thread that ends the process loads it. */
static atomic_bool finalizing;

void osoe_blocked_finalizing(bool on)
{
    atomic_store_explicit(&finalizing, on, memory_order_release);
}

const char *osoe_blocked_where(char *buf, size_t len)
{
    const char *routine = osoe_blocked_routine();
    if (routine)
        snprintf(buf, len, "waiting in %s", routine);
    else if (atomic_load_explicit(&finalizing, memory_order_acquire))
        snprintf(buf, len, "in shmem_finalize");
    else
        snprintf(buf, len, "not waiting in the library");
    return buf;
}
