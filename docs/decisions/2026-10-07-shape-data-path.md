# 2026-10-07: the shape on the data path (v0.2 step 2, Part 2)

The second part of v0.2's second step, on `docs/prompts/v0.2-step2-tree-shape-prompt.md`
(the brief), "Part 2: the shape on the data path", and the step's merge point;
Part 1's record is `2026-10-06-tree-shape.md`. Built overnight from 2026-10-06
to 07 on the author's answers of 2026-10-06 (`2026-10-06.md`, items 3 to 5):
the model's bar is its choices, the link measured at the end of `shmem_init`,
a ceiling of 8 and a capacity of 4.

**Where it stands.** Built on the branch `shape-data-path`, on Part 1's
`tree-shape`; merged with it at the step's merge point (the night's record,
`2026-10-07-overnight.md`, has the merge commit). One stop-and-ask, the scans'
shape (below), answered for the merge with the option no measured size is
slower in and put to the author.

## What was built

- **Every large reduction and broadcast takes the call's shape.** The node
  lead asks `osoe_shape_choose` for each call (`shape_of` in
  `internode-generic.c`), from the team's node count, the call's bytes, the job's
  link, the ceiling and the capacity - every input the same on every node for
  the same call - and builds that call's tree (`find_lead_tree`, now given the
  fan-out) and keeps that call's window in flight (`in_pieces`). A large
  reduction's lead grants this call's children as it enters, before its node's
  members arrive, as before. `reduction_tree`, built once per team, is gone.
- **Nothing new in the counters.** Each child position keeps its own base,
  advanced at the call's end by what this call's tree says the position
  received; a position unused in one call neither advances nor stands in for
  another, which is the argument a broadcast's rotated tree already needed
  (§5.5 as built). The layout is the ceiling's, and
  `osoe_internode_generic_number_taken` zeroes every position up to it.
- **The defaults** the author chose: `OSOE_COLLECTIVE_FANOUT` the ceiling, 8 on
  both backends; `OSOE_COLLECTIVE_PIPELINE` the capacity, 4. The settings' rows,
  the tests that assumed 4 and 2, and every memory figure of the README with
  them (below).
- **What a call took, published**: each lead's last reduction's or
  broadcast's fan-out and window in the team's tree state
  (`team.tree.shape_fanout`, `team.tree.shape_window`).
- **`shmem.shape.chosen.*`** (`test-shapes-chosen.c`): a byte sum and a
  broadcast at sizes either side of the chooser's crossovers - 1,025 bytes to
  1 MiB - over the world, their results checked, and every node's published
  shape held to the line of `shape-grid.txt` for the job's node count and link:
  the Pi's link and a loopback's, set by `OSOE_LINK_*`, on 4, 16 and 64 logical
  nodes. Every PE checks its own node's lead, so every node is checked.
- **The scans keep one shape for every call**, under `link` 0.1.0's (below).

## Choices made without the author

Each is Claude's; the undo follows.

1. **The trees are built per call, not cached.** The brief asked for the trees
   a team used to be cached by fan-out. A tree costs O(k) arithmetic and as
   many lookups of a node's lead, as a broadcast's rotated tree always has, and
   a cache would be state per team and per fan-out to keep right across team
   numbers given back. *Undo:* a cache in `struct generic_team`, keyed by the
   fan-out and the root.
2. **What a call took is published per team, not counted per fan-out.** The
   brief named a counter per k, `collectives.shape.k`. A count says how often
   each fan-out was taken, not which call took it; the test holds every call to
   the chooser through the published shape, which a count could not. *Undo:* a
   node-scope counter per fan-out beside it.
3. **The scans keep one shape for every call, where the brief has them take
   theirs per call** - question 1.

## The Pi's measurements

`docs/pi-cluster-results.md`, "v0.2 step 2", has them whole; the raw files are
`docs/design/evidence/pi-cluster/2026-10-06-v0.2-step2/`. `link` against `fixed`
at 0.1.0's defaults on the same build, and the tag's own build, alternating
three times: the e-band probe at 4 × 4 and 4 × 1, OSU's reduce, broadcast and
fcollect, `osoe-bench`.

- **The brief's claim holds**: a 1 MiB broadcast 1.43 to 1.47 times faster (the
  brief: about 1.5), a 1 MiB reduction 1.20 to 1.26 (the brief: 1.2 to 1.45),
  every size from 64 KiB up faster, and the lines below 1 KiB unchanged within
  the noise.
- **The band between 1 and 64 KiB**: on four nodes the chooser takes the star at
  every size to 16 KiB, which the sweep (below) measured fastest at each, and
  the fan-out of 2 from 16 KiB and a byte, where a call first takes two pieces -
  within F2's crossover of 16 to 64 KiB - which at 32 KiB was as fast as
  `fixed`'s star or faster, up to 1.37 times (OSU's 4 × 1 broadcast 2.6% slower,
  inside its 6.4% noise); no size there slower than `fixed` beyond the noise in
  the final run.
- **Start-up**: Part 1's measurement, 27.1 to 27.6 ms a start; `fixed` measures
  nothing.
- **The scans**, the one finding: question 1.

In the VM, the shapes chosen on 16 and 64 logical nodes, at the Pi's numbers
and a loopback's, are `shmem.shape.chosen.*`'s, every call held to the grid.

## The model, refitted to the sweep (the author's item 3)

The sweep the author asked for - OSU's reduce and broadcast at fixed shapes,
fan-outs 2, 3 and 4 and windows 2 and 4, 1 to 16 KiB at 4 × 1 - is now part of
`shape_timing.py`'s Pi table, and `model.shape_timing` holds the choices to it:
for every call a session measured under several shapes, the shape the model
picks against the fastest that session measured, at most 10% - 9.7% at worst,
1.2% at the 90th percentile, over 64 calls. The regret is taken within a
session, never across two: the sweep ran ten days of code later than the
table, its star at 16 KiB taking 369 µs where the table's took 454, and pooled
they would charge the model a 23% the Pi never showed in one run.

The refit over both tables lands at a hop of 89 µs, a message cost of 4 µs and
116 bytes a µs, against the fit in force's 86, 7 and 116: 332 of the 558 lines
within the noise against 326, and the same choice on 854 of the grid's 858
large calls - the other four at a ceiling of 32, at 1 and 2 KiB, between
neighbouring fan-outs that cost about the same. The fit in force stays, so the
grid, the pins and the tests' links stay as they are; `PI_WITHIN` is the fit's
326 over both tables, and the broken variant without a message cost, refitted,
lands 321 and still fails. What the model does not fit is the star's time at 4
to 16 KiB, which it puts up to 39% high; on four nodes no choice depends on it.

## The README's memory figures

Every figure of the internal region, the heap object, the `/dev/shm` check and
the settings' costs, recomputed from `internal-region.h`'s own functions at the
new defaults by a subagent, which reproduced each old figure at (4, 2) first and
checked the new ones live: the capacity check's own refusal gave 2,175, 4,349
and 8,697 MiB for 8, 16 and 32 PEs on one node, and the heap object matched the
formula to the byte at 1, 2, 4 and 64 nodes. A team number 23.5 KiB at 4 nodes
(13.5 before), a lane 577 KiB (160), the region 15.45 MiB a PE (6.03), a PE's
heap object 271 MiB (262), 3.6% more of the `/dev/shm` check; the node lead's
reply pool, which grows with the fan-out, 0.8 MiB (0.6). Spot-checked here: the
lane, the 17 lanes, the per-setting costs and the levels at 500 nodes, and the
region at 1,024 team numbers, which a comment quoted (35 MiB, 18 before).

## Questions for the author

### 1. The scans' shape: per call as the brief has it, or 0.1.0's

**What the brief says.** "The scan's rank tree (`scan_place`, `scan_children`)
takes k the same way, from a shape chosen on (members, n)".

**What was built, and why not that.** A scan's tree is over the team's members,
not its nodes, and its rows share one base - the pieces the team's scans have
moved - where a reduction's positions each keep their own. A row left unused by
one call would fall behind that base, and the next call to use it would wait
forever. So the scans kept one shape for every call: Part 2 first gave them the
ceiling's.

**What the Pi showed** (the brief's stop-and-ask: a line slower under `link`
than under `fixed` beyond the noise). With the ceiling's default of 8 a scan
over 16 PEs on four nodes took 1.5 to 2.0 times as long as `fixed`'s from 8 KiB
up: its root's node sends six copies over its link where a fan-out of 4 sends
three. At 0.1.0's fan-out of 4 with the capacity's four pieces in flight it was
as fast from 1 to 32 KiB, 1.30 times faster at 1 MiB and 1.10 at 32 KiB, but
6.6% slower at 64 KiB, with a spread of 1.2% between rotations. At 0.1.0's whole
shape, a fan-out of 4 and two in flight, it is `fixed`'s scan exactly, and the
final run found no line of any collective slower beyond the noise.

**Options.**
- (a) The brief's: each scan takes its shape per call. Each row then needs its
  own base, as the reductions' positions have, and the cost of a tree over
  members - local edges and remote ones, several members to a node - is not
  the node tree's the chooser has; both are protocol and model work, the
  scan model first.
- (b) 0.1.0's fan-out with the capacity's window: 1.30 times faster at 1 MiB,
  6.6% slower at 64 KiB on 4 × 4.
- (c) 0.1.0's whole shape under `link`, the ceiling and the capacity where those
  are less, and the ceiling and the capacity under `fixed`.

**Answered** (the author, 2026-10-07, `2026-10-07.md` item 1): "keep" - (c).

**Recommendation: (c) now, as built and merged** - the shape no measured size is
slower in - **and (a) as a step of its own**, its model first, which would take
(b)'s gain where the model says it wins. *Undo:* `scan_fanout` and
`scan_window` in `internode-generic.c`.

## The adversarial read

A fresh agent read the whole step, Part 2 the focus, from `f7745a0` to `3ef3ba3`
(`docs/reviews/archive/v0.2-step2-adversarial-read.md`): the rule holds - every
node computes the same shape for the same call, and no shape exceeds the layout,
measured on probes up to 64 nodes, under perturbation, the AM stress settings and
`SHMEM_THREAD_MULTIPLE` - and no defect on the data path; but the tests were
weaker than the mechanism. In `22ad0c4`:

- **Finding 1, the tree built**: the shapes test held the shape a lead
  published, so a data path that built the ceiling's tree and published the
  chooser's passed it (the read's M1). Each lead now publishes the children it
  had in the call's tree (`team.tree.shape_children`, in the tree state's
  padding, which this spends - a static assert says so), and
  `shmem.shape.chosen.*` holds them to what the chosen fan-out gives the node at
  the call's root. M1 now fails it (Verification).
- **Finding 2, grants back to back**: nothing ran collectives whose fan-out
  differs back to back, so a reduction granting another tree's children than
  the one its data path waits on passed every test (M4). The read's probe is
  now `shmem.shape.back-to-back.*`: on 16 nodes and on 8 of two PEs, the world
  and two strided teams sharing its leads, sizes across the crossovers, no
  barrier between calls. M4 now fails it.
- **Finding 3**: `SHMEM_INFO`'s text for the ceiling, the capacity and the
  shape.
- **Finding 9**: the scans' tests of four and eight pieces in flight run under
  `OSOE_COLLECTIVE_SHAPE=fixed`, where a scan keeps the capacity - under `link`
  they had quietly dropped to two - which runs the scans' fixed path too.
- **Findings 4 to 8 and 13**: the asan failure's cause put right in the
  Verification below (AddressSanitizer's own memory, not the heap); the
  decisions index's four stale lines; the README's limit, 2 on 2 to 22 nodes but
  3 on 5, and its put count, 136 to 278 by the node count; the data path's stale
  comments, and the loop's header now stating why a per-call tree is safe; the
  32 KiB sentence above.
- **After the merge** (the read's 10 to 12): multi-node tests that run on the
  measured link take shapes that vary from start to start, so a defect tied to
  one shape would fail now and then, its log not naming the link - pin
  `OSOE_LINK_*` in them, or print the link on failure; a divergence across a
  crossover now ends by the deadlock detector where one tree let a header name
  it - the call's fan-out could travel in the grant or the lane word; and the
  link is measured on two and three nodes, where no fan-out differs. Each is
  on the release checklist's after-release list.
- **Finding 14**: the scans' stop-and-ask is answered for the merge and put to
  the author (question 1); the prompts README's row is the merge's.

## Verification

The step's merge verification, both parts on one tree, in three VMs at once,
each under its own lock:

- **`f7745a0`** (Part 2 before the scans' change): in `osoe`, the four presets
  with the floor pins - debug 784 of 784, release 784, asan 781, tsan 545 -
  `tier3` 113, `osoe-pacer collectives` at three values of
  `OSOE_SMALL_COLLECTIVE_BYTES`, its teams and threads, and ten minutes of
  `mix`, no failure; in `osoe-load`, the models whole, 32 of 32, the SOS suite
  at 4 × 2 - 161 programs clean, the two C11 programs over `short *` not
  building and `shmem_team_b2b_collectives` and `shmemalign` failing as the
  runbook says - and the acceptance past 61 PEs, line for line the release
  point's: `shmem_team_reuse_teams`, `shmem_team_max`, `shmem_team_split_2d`,
  `reduce_active_set`, `broadcast_active_set` and `repeated_barriers` at 70 PEs
  on one node and on ten logical nodes of seven, `collect_active_set`'s 32
  sets at 32 PEs on one node and on four, the per-node split at 61, 62 and 64
  nodes; in `osoe-rocky`, the single-node build's four presets - 391, 391, 388
  and 383 - an install, `oshcc` from it at 8 and 32 PEs, and the refusal of a
  job of two nodes.
- **`7dbf522`** (the scans at 0.1.0's shape, and every document): in `osoe`,
  debug 784, release 784, tsan 545, `tier3` 113, the pacer's collectives, teams
  and threads and ten minutes of `mix`; asan 780 of 781 - 64 PEs of
  `shmem.shape.chosen.loopback.64` outgrew the VM's 8 GiB, and the kernel
  killed one (its log: out of memory, `shmem_test_shapes-chosen`, 55 MB of
  private memory), where the first pass had passed. AddressSanitizer's own
  memory, about 55 MB a PE, is what fills it, the heap being shared and written
  only as used (the step's adversarial read reproduced it, the two 64-PE tests
  at once leaving under 200 MiB free): `3ef3ba3` keeps the test's 64 nodes out
  of the sanitizers, and gives it the 4 MiB of heap it needs; the shape tests
  again in asan, 5 of 5, and release, 7 of 7.
- **`22ad0c4`** (the adversarial read's fixes): in `osoe`, the four presets with the floor pins - debug 786 of 786, release
  786, asan 781, tsan 545, the two back-to-back tests among them where they run -
  and `tier3` 113. Each new
  check against the mutation it was written for, in release: the data path
  building the ceiling's tree while it publishes the chooser's shape (M1) fails
  four of the five `shmem.shape.chosen.*` - `loopback.64`, where every call takes
  the ceiling anyway, cannot tell; a reduction granting the ceiling's tree's
  children while its data path waits on the call's (M4) fails
  `shmem.shape.back-to-back` on sixteen nodes, where the 23 tests before it
  passed under it, and the eight-node one with them.
- **The models** changed after `f7745a0` in `shape_timing.py` alone, the sweep
  in its table: its check and its grid, run on the host on the final tree.
- **The Pi**: above; the scans' final shape, three rotations against `fixed`.
