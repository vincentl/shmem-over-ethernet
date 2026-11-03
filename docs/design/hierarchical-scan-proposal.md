# A hierarchical prefix scan: within each node, then among the node leads

*A proposal, written by the lead on 2026-11-02 at the author's word of that
morning (`docs/decisions/2026-11-01.md`, answer 6): the third of the four
design changes the large-cluster playbook names
(`docs/prompts/large-cluster-playbook.md`, C9 and the third row of its §5) to
be modelled here before any cluster session. Modelled, and timed in hops; not
built. The author decides whether it is built, and when.*

## 1. The problem

A prefix scan on a team spanning nodes (`shmem_sum_inscan`, `shmem_sum_exscan`;
design §5.8, "Prefix scans", as built; `internode-generic.c`,
`osoe_internode_generic_scan`) runs over a pre-order tree of the team's ranks:
every member holds its rank's place on its own thread, fan-out 4 - 0.1.0's
shape, which a scan keeps under `link` since its tree cannot change from call
to call - and the scan takes no epoch of the team's node counters. A subtree
is a contiguous range of ranks, so under a block distribution the lowest
levels fall within a node, but the tree is 2 × ⌈log₄ M⌉ levels deep for M
members, and most of its upper hops cross the network whatever the layout.
`scan_timing.py`, the timing model beside `scan_model.py`, puts an 8-byte scan
at 512 nodes × 256 PEs at 12.7 network hops under a block layout where a
reduction's tree over the leads at the fan-out's ceiling of 8 is 6; on the Pi
cluster at 4 × 4 the e-band probe measured an 8-byte scan at 377 µs where a
reduction took 202 and a broadcast 209 (`docs/design/evidence/pi-cluster/
2026-10-04-step-e/merge/`, 2026-10-04). §5.8's sketch had foreseen the runs -
a tree over the maximal runs of consecutive ranks on one node, served by the
nodes' leads - and the slice's measurement found it about two hops faster
under a block layout; the rank tree was built because the runs needed
bookkeeping, a lead serving many places under a cyclic layout, and no program
in the corpus calls a scan.

Since then two things exist that did not: `osoe_team_node_runs` (`team.c`),
which a `collect` of differing lengths uses to learn whether every node's
members are one run of the team's ranks, and in which order the runs meet the
node set's positions; and `osoe_internode_scan_node_totals`
(`internode-generic.c`; `docs/design/collect-scan-proposal.md`, P30), a scan
of one eight-byte word among the node leads over a pre-order tree of the node
set's positions at the ceiling's fan-out, through the reductions' small rows
and counters, giving each lead the totals before its position and the total.

## 2. The proposal

**Where every node's members are one run of the team's ranks** - a block
distribution, in any order of the runs - a scan takes the shape of a
reduction: an intranode phase in shared memory, an internode phase among the
leads, and the members' results from their node's prefix.

1. **Arrive.** Every member says its call (`osoe_call_say`) and adds to the
   team block's `arrived`, as a reduction's members do; the lead waits for them
   all and compares the calls (P24's check, which a scan spanning nodes has
   never had, since it took no epoch).
2. **Pass 1, the node's total.** The lead sums its members' sources in team
   rank order, a chunk of elements at a time - the one-node scan's loop
   (`collectives.c`, `osoe_scan`) without its writes - and each chunk's sum is
   a piece up the leads' tree.
3. **The leads' scan over the runs.** A pre-order tree over the runs in team
   rank order at the ceiling's fan-out, run j's place served by the lead of
   `runs[j]`'s node: up, a lead waits for each child's subtree total, adds its
   own and puts the sum to its parent's slot for its position; down, the root
   has nothing before it, and every other lead receives `before`, the totals
   of the runs before its subtree, and sends child j that plus its own total
   plus the totals of children 0 .. j − 1 - P30's primitive, indexed by run
   instead of by the node set's position.
4. **Pass 2, the prefixes.** As each chunk's `before` lands, the lead writes
   every member's `dest` for that chunk: a running sum seeded with `before`
   instead of zero, then in rank order each member's exclusive result (the
   running sum) or inclusive one (after its own source is added) - the
   one-node scan's loop again, reading a member's source before writing its
   `dest`, so that the two may be one array.
5. **Release.** The lead adds to `released`; the members, which waited for it,
   return. No copy-out and no `copied` counter, as the one-node scan has none.

**The pieces.** A scan of at most `OSOE_SMALL_COLLECTIVE_BYTES` is one piece
through the team number's small rows, as the leads' scan of `collect` is
today; a larger one goes through the lead's lane in pieces, D in flight, by the
member scan's pipeline rules (`scan_pipeline_model.py`: a piece up only once
the piece D back is down; a down copied out before the next piece up; one
counter a child position; a piece down only after it went up; the sources and
totals of the D pieces in flight kept) over the leads' tree, the lane granted
as the reductions' are - the lead's lane, each child's lane word with its
first total. The primitive as built scans one word; the vector form is the
member scan's flight with the leads as its places, and both pieces of code
exist.

**The order of the runs.** The tree runs over the run indices, which are the
runs' order in team rank, and `osoe_team_node_runs` gives each run's node
(`runs[j].position`); so a block map numbered out of order, a negative stride
and the plain case are one case, and `before` is the base in rank order
without the "total less before less own" that P30 needs in reverse. The model's
`position_order` breakage shows what a tree over the node set's positions
does under a permuted layout: wrong results.

**Where it does not apply.** A team whose nodes' members are not one run each -
a cyclic or plane distribution - keeps the member scan as built, every
member a place in the rank tree: under a cyclic layout a node holds L runs and
`scan_timing.py` finds a tree over the runs served by the leads no faster than
the rank tree (45 hops either way at 512 × 256). The way a team takes is
fixed when its runs are first found, as `collect`'s places are, and is the
same on every PE, since every PE holds the team's ranks and the rank map
alike.

**What does not change.** Push, never pull (invariant 17): the lead reads a
member's source only after that member arrived, as the one-node scan does
today. Every `dest` is written by one thread, the node's lead, in one pass
(invariant 16 as it stands). The one-node team's scan, which is this design's
intranode phase. The exact sum is the specification's; a floating sum rounds
in another order than the rank tree's, which §5.8 already allows - but a user
comparing a run of this version with one of the last will see the difference,
and the README's sentence on layouts should say versions too.

**What changes.** A scan on a team spanning nodes takes an epoch of the team's
node counters - `osoe-stat`'s published epoch counts it, P24's comparison
applies to it, and the design notes' sentence "A scan on a team spanning nodes
takes no epoch" goes; it moves through the reductions' words, the small rows
and the lanes, where the member scan has rows of its own per team number,
which stay for the cyclic path; `osoe_internode_scan_node_totals` becomes a
scan of a vector in pieces, indexed by run; the tests that pin a team's epoch
after a scan; §5.8's "as built" paragraph.

## 3. The model

`tools/modelcheck/hierarchical_scan_model.py`, registered as
`model.hierarchical_scan`: N nodes of L members each, member 0 of a node its
lead, the leads' tree at fan-out 2 so that three nodes give a root with two
children and four an interior lead with a child of its own; a source a
distinct bit per (epoch, rank), so a sum is a set and a result is checked
exactly. Four properties, each with a breakage that fails it:

- **SOURCE** - the lead reads a member's source only after that member arrived
  at this epoch (invariant 17; in place, a member's `dest` of the last epoch
  is its source). `lead_before_arrived`, the lead reading after its own
  arrival alone, fails it.
- **SLOT** - a slot among the leads is written only once its last value was
  read. `down_before_up`, the root sending child j its down as soon as it
  holds the totals before it, without waiting for child j's own total, fails
  it: a root a whole scan ahead writes a down slot its child has not read.
  The rule as built - a parent writes a child's down only after the child's
  total arrived, so the child is in this scan - is what keeps the leads'
  slots safe across scans without a counter of their own.
- **VALUE** - a value a lead takes from a slot is this scan's.
- **RESULT** - every member reads, after the release, the exact prefix the
  specification defines for its rank, inclusive or exclusive.
  `release_before_prefix` (the lead releasing before pass 2),
  `member_before_release`, `no_left_sums` (child j's down without the totals
  of children 0 .. j − 1; `scan_model.py`'s breakage of the same name, over
  the leads' tree) and `position_order` (the tree over the node set's
  positions, `before` taken as the base, under a permuted layout) each fail
  it.

Sixteen cases - two to four nodes, one to three members, one or two epochs, in
place and separate, inclusive and exclusive, the runs in order, reversed and
permuted - under sequential consistency and x86-TSO, where the sources' stores,
the `dest`s' and the slots' are plain and drained by the locked add that follows
each, as the library's are (§3.16): every case of the design holds with the
engine's liveness check, the largest 739,612 states (four nodes of two under
TSO), 38 seconds for all 32 on a laptop.

**What the model settled.** The design needs no `copied` counter: the lead
writes a member's `dest` for epoch e + 1 only after that member arrived at
e + 1, which it does only after reading e's result - the two-epoch in-place
cases hold without one, where C5's sliced combine needed `combined` because
there the members write the lead's `dest`. And the leads' slots need no
protection beyond the as-built rule: the next scan's up into a parent's slot
comes only after the parent read this one's and sent the down, and the down
only after the child's up, as `down_before_up` shows by breaking it.

## 4. What it costs, and what it gains

`scan_timing.py`'s hierarchical column (registered as `model.scan_timing`,
its `check` pinning the claims below): one network hop is 1, a hop within a
node 0.05, a lead's sum or prefix of one member's element 0.002 (about 0.15 µs
at a Pi's 77 µs hop), the members' tree at fan-out 4 and the leads' at 8.

| Nodes × PEs a node, block | Members' tree, as built | Hierarchical | Ratio |
|---|---|---|---|
| 4 × 4 | 4.10 | 2.24 | 1.8 |
| 16 × 64 | 6.40 | 4.62 | 1.4 |
| 64 × 64 | 8.48 | 4.74 | 1.8 |
| 64 × 256 | 8.58 | 5.50 | 1.6 |
| 256 × 256 | 10.66 | 7.62 | 1.4 |
| 512 × 64 | 12.56 | 6.94 | 1.8 |
| 512 × 256 | 12.66 | 7.70 | 1.6 |
| cyclic, any | 45.3 at 512 × 256 | does not apply | - |

- **In hops, 1.4 to 1.9 times from 64 nodes** under a block layout, the trees'
  depths - 2 × ⌈log₄ (N × L)⌉ against 2 × ⌈log₈ N⌉ - less what the rank tree's
  lowest levels already saved within a node, plus the lead's intranode terms,
  which at 256 members add a hop. The playbook's "13 become 6" were the
  depths alone. At a Pi's 4 × 4 the model's 4.10 → 2.24 hops is the measured
  377 µs scan against the 202 µs reduction, whose tree this is.
- **For a large scan** the pieces travel the leads' tree instead of the rank
  tree: on the Pi a 1 MiB scan took 49 ms where a 1 MiB reduction - the same
  16 KiB pieces, the leads' tree up and down - took 29, so the analogy puts it
  near 30 ms plus the lead's two passes.
- **The lead's two passes** are O(L × bytes) on one thread, twice: the cost
  class of the one-node scan today and of a reduction's combine (C5). At 256
  PEs and 1 MiB that is 512 MiB of reads, 25 to 50 ms, more than the tree. C5's
  slices take both passes to O(bytes) a member - pass 1 is C5's combine; in
  pass 2 member i writes slice i of every member's prefix - and the two
  proposals share that code.
- **Memory**: nothing new; the reductions' rows and the lead's lane.
- **Code**: about a hundred and fifty lines - `osoe_scan` taking the epoch
  path where the team's runs allow, the leads' scan generalized to pieces and
  indexed by run (the member scan's flight with the leads as places), the
  design notes' §5.8, the README's prefix-scan rows and a test that fails
  without it: a scan on a block team pinning the epoch it takes, or the
  message count a node receives.

## 5. Alternatives weighed

- **§5.8's runs at fan-out 4, served by the leads** - the original sketch;
  the same shape at a smaller fan-out without the intranode phase named. This
  proposal takes the ceiling's 8 and the primitive that exists.
- **The small class alone** hierarchical, large scans through the rank tree:
  half the code, but the large scans are where the Pi's 49 against 29 ms sits.
- **A hierarchical scan under a cyclic layout** by the runs' tree with a lead
  serving L places: `scan_timing.py` finds it no faster than the rank tree,
  and `collect`'s places have the same gap (C10). Left.
- **Leaving it**: no program in the corpus calls a scan, the gain is in hops
  and the scan's share of any run is small. Of the four changes modelled this
  is the least by use.

## 6. Questions for the author

*Answered 2026-11-03*: the author took every recommendation below as his answer (`docs/decisions/2026-11-03.md`).

1. **Build it?** *Recommended*: only if the first cluster session's ladder
   (its scan row, at 1 and P PEs a node) or a program the author means to run
   makes scans matter; otherwise leave it modelled. Of the four, C5 and C8 go
   first.
2. **Both classes**, or the small scans alone? *Recommended*: both, one change,
   since the pipeline over the leads' tree is the reductions' and the member
   scan's already.
3. **The epoch.** A scan spanning nodes taking an epoch changes `osoe-stat`'s
   count and a few tests' pins, and brings P24's check. *Recommended*: yes.
4. **C5's slices for the two passes** at the same time, or the serial lead
   first? *Recommended*: C5 on the reduction first; the scan's passes take its
   code once it is in.

## 7. The record

The model: `tools/modelcheck/hierarchical_scan_model.py`,
`model.hierarchical_scan`; the timing: `tools/modelcheck/scan_timing.py`,
`model.scan_timing`; the decision record:
`docs/decisions/2026-11-02-hierarchical-scan-model.md`.
