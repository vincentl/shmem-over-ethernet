# Phase 1, step 10, slice 1: the prefix scans, and the team's record carrying its world ranks

**Status: approved by the author on 2026-09-26 (evening): question S, design
A, every member at its team rank's place. Built; its review moved the slots
from the PE's, shared by every team with READY, to the team number's, with no
READY - see design §5.8 and `docs/decisions/2026-09-26-evening.md` §5.** It departs from design §5.8's
sketch of runs, which §5.8 is updated to say. Question E (the record, not the
upcall) was decided earlier the same day and is folded in as decided.

*Written by Claude (Opus 5.5) on 2026-09-26.* Evidence:
`tools/modelcheck/scan_model.py` (every design case passes; each of its four
breakages fails with a trace) and `tools/modelcheck/scan_timing.py`.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node.**
- **A path to RoCE at ~10⁶ PEs that no interface blocks** - a constraint on
  the seams, not work in this project (design §1).

## What the spec asks (§9.10.10)

`shmem_sum_inscan` and `shmem_sum_exscan`, typed for every integer, real and
complex type Table 10 allows SUM on, and the C11 generics: PE i's `dest[j]` is
the sum of `source[j]` over team ranks 0..i (inclusive) or 0..i-1 (exclusive,
0 at rank 0). Collective over the team; `source` and `dest` the same or
disjoint; before a PE calls, every PE's `dest` is ready and its own `source`
readable by any PE, with no synchronization needed. Returns 0. Floating
results may differ between layouts: the spec defines the sum mathematically
(design §5.8 already says to document it).

## The open point, resolved: where a node's many runs land

§5.8 left open how a node holding many runs - a cyclic layout gives a run per
PE - takes their messages, since the internal region has one tree position's
slots per node. **Every PE has its own internal region** (the tail of its heap
mapping, `internal-region.h`), so any design can land a tree position's
messages in the region of the PE that holds it, and nothing grows with the
job. Two rules make the slots safe, and the model breaks each:

- **The slots are the PE's, shared by every team**: k up-slots and one
  down-slot of 16 KiB, and a counter per slot, cumulative and compared as at
  least, each PE counting its own scans of any team (`team_counts` breaks
  it). They cannot be the reduction's per-team rows: a node that finished a
  scan may start a reduction and put into its tree parent's row while that
  PE, in the scan, still waits in the same memory - trees of different
  collectives are different trees.
- **READY, per team number**: a PE entering a scan on team t adds one to
  READY[t] at each of its children, and a child puts up only once READY[t]
  shows its parent entered. Without it a member of the new team that was not
  in the last one writes into a parent still in another scan (`no_ready`);
  with one READY per PE, a parent in a third team can satisfy the wait
  (`shared_ready`, which needs three teams to show). A down put needs no
  READY: the child's total arrived, so the child is in this scan.

Cost: (k + 1) × 16 KiB a PE and a line per team number - 80 KiB and 8 KiB at
the defaults - address space, touched only by scans.

## Question S: which tree

**A. Every member at its team rank's place (recommended).** A pre-order k-ary
tree over the team's ranks: a place covering [a, b) is rank a, its children
split [a + 1, b) into at most k consecutive blocks, so every subtree is a
contiguous range of ranks. Up: each member waits for its children's subtree
totals, adds its own source, puts the sum up. Down: it receives the sum of
every rank before its subtree; that is its exclusive result, plus its source
its inclusive one, and child j gets that plus its source plus children 0..j-1's
totals. Each member works on its own thread, reads only its own source, writes
only its own `dest` (in place is safe), and waits only on its own region. Nothing
is computed about the layout: a member's neighbours are ranks, by arithmetic,
and ranks are world ranks by the team's progression.

**B. §5.8's runs.** A pre-order tree over the team's runs - maximal stretches
of consecutive ranks on one node - each run's head holding its place, the
run's local prefix computed by its node, and each run's offset sent down.
Its places need every run's index and its neighbours' heads: a walk of the
team through the rank map once per team, as the allgather's (O(P) time under
a cyclic layout, O(N + runs) under a block one, O(runs here × k) kept), and
either the node's lead serving every run of its node (one thread, several
places, which must be served deepest first up and shallowest first down to
be free of deadlock), or each run's head serving its own and a node-level
join to release the members.

**What the simulation says** (`scan_timing.py`; a network hop 1, about 77 µs
on a Pi; a hop within a node 0.05; k = 4):

| Layout | N × L | A: members | B: runs |
|---|---|---|---|
| block | 4 × 4 | 4.1 | 2.1 |
| block | 64 × 4 | 8.3 | 6.3 |
| block | 1,024 × 64 | 12.6 | 10.6 |
| cyclic | 4 × 4 | 4.3 | 4.4 |
| cyclic | 1,024 × 4 | 12.8 | 12.9 |
| cyclic | 1,024 × 64 | 22.8 | 23.0 |

Both are 2 ⌈log_k⌉ of what they build a tree over, and equal under a cyclic
layout, where every run is one PE. Under a block layout A pays about two
network hops more at every size, because its subtrees do not line up with
nodes: at the Pi's 4 × 4 about 320 µs against 165. B is also O(P) at a team's
first scan under a cyclic layout, for the walk.

**Why A anyway**: no corpus program calls a scan (§5.8); A is one small
routine every member runs, with no layout walk, no run bookkeeping, no
multi-place service loop and no new node-level join, where B is all four; and
both are logarithmic, so the goals do not choose. *If the author prefers B*,
the slots and READY above carry over unchanged (a run's head is a PE), and
the brief grows the walk and the service loop.

## Decisions, under A (the ones that hold under B are marked)

1. **The seam** (holds under B): `osoe_internode_scan` in `internode.h`'s
   data half, the generic implementation in `internode-generic.c`, so a
   backend may offload it as it may a reduction. It is the one data-half call
   **every member** makes, on its own thread, not the team's local lead:
   addressed by (team, epoch) as every call there is, and by the member's team
   rank. A team on one node never calls it (it has no internode phase, as for
   every collective): its local lead computes every member's result in rank
   order, reading each source before writing that member's `dest`, between the
   intranode arrival and release the reductions use.
2. **E, as decided**: a team's node-block record gains `world_start`,
   `world_stride` and `members`, published with the rest; the backend maps
   team ranks to world ranks from it alone; `osoe_internode_team_world_ranks`
   goes from `internode.h` and `team.c`, and the allgather's walk reads the
   record. Every team with an internode phase is strided in world ranks; only
   `SHMEM_TEAM_SHARED`'s family is not, and it never leaves a node, so its
   record says no progression and nothing below asks. Layout bump.
3. **The slots and READY** (holds under B): as above, in the internal region
   after the allgather's words, so nothing before them moves; the fan-out is
   the agreed `OSOE_COLLECTIVE_FANOUT`.
4. **Pieces**: a scan of more than a slot runs piece by piece, each piece up
   and down before the next; a slot is reused only after the protocol above
   says so (the model's consecutive scans on one team are the same argument).
   Pipelined pieces are not built: no program needs a large scan.
5. **Every type**: the integer, floating and complex types Table 10 allows
   SUM on, typed and C11 generic, generated as the reductions are; the combine
   is `combine.c`'s sum. `pshmem_` strong, `shmem_` weak.
6. **Epochs**: a scan is a data collective and takes the team's next epoch,
   as a reduction does, so osoe-stat and a divergence check see it.

## Tests

By value, the source of rank r element j distinct per (r, j) so a wrong rank's
contribution shows: inclusive and exclusive; in place and not; nelems 1, a
slot's worth, and past it; every type once; the world, a strided split, a
negative stride, SHARED, a team of one; two scans on different teams back to
back with members of one not in the other (the model's `CA` case) and three
teams (`ZXY`); L1, L2, L4, 0,0,1,1, 0,1,2,0,1,0, 0,0,0,1,2,2, force-remote;
tier 3; the SOS scan programs (`shmemx_` renamed, as for `ibput`). Mutations
from the model's breakages: no READY, one READY per PE, counts per team,
children's left sums dropped. The layout bump seen by osoe-stat.

## Stop and ask

If a scan needs memory that grows with the job's size; if the seam call cannot
be what every backend implements without AM's help; or if `SHMEM_TEAM_SHARED`
turns out to need an internode phase.
