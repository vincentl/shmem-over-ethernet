# Implement phase 1, step 5, first slice: reductions and broadcast

You are starting **§7.1 step 5** of **osoe** (OpenSHMEM Over Ethernet),
"Collectives". Step 4 is merged: jobs run across nodes under Slurm, a PE or a
node that dies ends the job, typed RMA and nonblocking gets exist. What osoe
cannot yet do is what every application in the corpus does once an iteration:
reduce, broadcast, gather. This slice takes the two collectives §5.5's
accumulator covers - **reductions and broadcast** - on the teams that exist,
SHMEM_TEAM_WORLD and SHMEM_TEAM_SHARED.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

*Written by Claude on 2026-09-24 at the author's direction ("start
collectives"), with the Pi cluster ten hours away. Step 5 is new: §7.1 ended at
step 4 with "symmetric statics and the compiler wrapper follow", and §7.5 held
the collectives as deferred and mechanical. They come first because every code
in the corpus needs them and not every code needs statics (§8.6's U7: a
reduction per iteration everywhere). The decisions below are Claude's, for the
author's review.*

## Read first

1. `CLAUDE.md`.
2. Design §5 whole: the two-level structure (§5.1), team accounting (§5.2), the
   intranode phase (§5.3), epochs and bounded skew (§5.4), **the accumulator
   and its rules** (§5.5), and §5.8's "what the surface provides": the generic
   route over the boundary and `OSOE_REGION_INTERNAL`.
3. Invariants 10, 16 and 17 (§9): equality on epoch counters; the local lead is
   the only writer of its `dest` before release and leaves last; a peer's
   `source` is read only after that peer has called.
4. §4.10's boundary: put's local completion, the fenced rmw, the watch.
5. `barrier.c` and `team.c`: the counters and the team descriptor this builds on.

## What exists, and what does not

- **Barrier and sync**, intranode and internode: the `arrived` and `released`
  epochs per team per node, and an AM tree between node leads that the agents
  drive (§5.3).
- **Each team's `copied` counter is in the node header and nothing uses it**
  (`world_copied`, `shared_copied`): §5.4's third counter, for data
  collectives.
- **`OSOE_REGION_INTERNAL` is declared at the boundary and not built**:
  "library scratch and collective control words, never visible to the
  application".
- The spec surface of §9.10 other than barrier and sync: nothing.

## Decisions

1. **The internode phase of a data collective runs above the boundary**, over
   put, fenced rmw and a wait, between the node leads' application threads -
   §5.8's generic route - not as new AM messages the agents interpret. The
   lead is blocked in the call anyway and invariant 16 makes it the combiner,
   so an AM design would still have to land every partial result where the
   lead's thread can read it and wake that thread; the generic one does exactly
   that with operations already tested, a RoCE backend inherits it unchanged,
   and a put's local completion makes the down-sweep's forward safe with no
   by-reference hazard (§5.5's last rule). The AM tree stays the barrier's.
2. **`OSOE_REGION_INTERNAL` is the tail of each PE's heap mapping**: the heap
   object grows by the region's size past the page-rounded
   `SHMEM_SYMMETRIC_SIZE`, the allocator never sees it, and `(pe, INTERNAL,
   offset)` resolves to the heap's base plus that start plus `offset` on both
   paths. No new mapping, no new name in `/dev/shm`, and the application
   cannot address it: `shmem_ptr` and every API check see the heap only.
3. **The local lead is the team's member with the lowest local id**, which for
   the world team and the shared team is local id 0, the node's lead. §5.2 says
   the lowest team rank; the two agree for every distribution the bootstrap
   accepts, and any member would do as long as all agree.
4. **The internode phase is the node tree of §5.3**, k-ary over the team's
   nodes: children put their partial results into scratch slots at their
   parent and add to its arrival word; the root has the result; it goes down
   by put into each child's release slot and an add to its release word.
   Broadcast re-roots the tree at the root PE's node, by rotating the indices.
5. **A vector larger than a slot goes through the tree in slot-sized pieces**,
   each piece a full pass - §5.5's "chunk through fixed scratch" fallback. The
   slot is `OSOE_COLLECTIVE_CHUNK` bytes (default 16 KiB), so the internal
   region is (fan-out + 1) slots and a page of words: 84 KiB a PE at the
   default fan-out, touched only by leads. §5.8's ring and scan algorithms are
   later work.
6. **Every PE ends with bit-identical results**: each element is combined once
   along the tree and copied everywhere, never recomputed (§5.8 rule 3).

## Order of work

### Part A — the internal region

`OSOE_REGION_INTERNAL` on both paths: resolve and bounds in
`transport-local.c` and `progress-host.c`; watches and notifies for it
(`watch.c`); a wait on an internal word by offset (`wait.c`); the heap mapping
grown by the region (`node.c`); a tier-1 or tier-2 test that a put, an rmw and
a wait on it work within a node and across nodes and that nothing of the
application's reaches it.

### Part B — the accumulator: reductions

§5.5 as written: members arrive; the lead waits for them and combines their
sources into its `dest` (reading each source only after its owner arrived);
the leads meet over the tree; the lead writes the final `dest`, bumps
`released`; members copy out and bump `copied`; the lead leaves last. Every
operator and type of the spec's reduction tables, team-based
(`shmem_TYPENAME_OP_reduce`), with their `pshmem_` pairs and the C11 generic
interfaces.

### Part C — broadcast

The same machinery with the root's `source` in place of a combine, and the
tree re-rooted at the root's node. `shmem_TYPENAME_broadcast` and
`shmem_broadcastmem`, their pairs and the C11 generic.

### Part D — tests and notes

Every operator on a few types of each family, one element, a few, and more
than a slot; in place; nreduce 0; every root for broadcast; SHMEM_TEAM_SHARED;
one node at 1, 2 and 4 PEs and the multi-node maps, cyclic included; tier 3.
The design notes record what was built against §5.5 and §5.8, and step 5 in
§7.1.

## Not in this slice

collect and fcollect, alltoall and alltoalls (each needs its own design, §5.5
"Scope of this scheme"); scans (§5.8); the deprecated active-set forms
(`*_to_all`, `broadcast32`/`64`), which need arbitrary PE sets; nonblocking
collectives (invariant 10).
