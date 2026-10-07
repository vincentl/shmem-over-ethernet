# 2026-10-06: the tree's shape from the link, Part 1 (v0.2 step 2)

The first part of v0.2's second step, on `docs/prompts/v0.2-step2-tree-shape-prompt.md`
(the brief), from the author's decisions for v0.2 (`docs/decisions/2026-10-05-v0.2.md`,
items 1 and 7): the cost model fitted to the Pi's table, the chooser, the link
and its settings. Part 1 chooses nothing on the data path: every collective
runs on the shape it ran on at the 0.1.0 tag.

**Where it stands.** Built on the branch `tree-shape`: the model and the chooser,
the settings - `OSOE_COLLECTIVE_SHAPE` and the link set by hand, agreed three
ways and shown by `osoe-stat` - with the README's rows, a fix to the
simulation, whose landing far into a long run could fall below the clock's
resolution and never come, and, after the author's answers of 2026-10-06, the
link's measurement and its way to every PE (below, "The link's measurement").
Three of the brief's stop-and-ask items were met before the answers, each below
with its evidence, the options and the recommendation; the author took all
three (`docs/decisions/2026-10-06.md` items 3 to 5). The measurement met a
fourth, the message cost's spread between starts, put to the author below with
a recommendation and not waited on. The branch is rebased onto step 1's merge
point and its fixes.

## What was built

1. **`tools/modelcheck/shape_timing.py`**, in `grant_timing.py`'s form, in the
   `models` preset (`model.shape_timing`, `tier1;model`, 37 seconds).
   - **The simulation**: the loop of `internode-generic.c`'s `in_pieces` over a
     k-ary tree of leads, event by event - a lead sends piece `up` once every
     child's copy of it is in and fewer than W pieces are up without their
     release, takes release `down` when it is in, copies it to each child and
     counts it, whichever is ready; a reduction's pieces go up with their bytes
     and come down with the result, a broadcast's go up empty and come down
     with the data; a message costs its sender o, then its bytes cross the
     links, then h. Each lead's link out carries one message at a time, in the
     order its one progress thread sent them; the senders into one lead share
     its link in fairly, as a switch's port merges them. It reports what OSU
     reports: the mean over PEs of each PE's time in the call, every iteration
     entered straight after a barrier whose release reaches depth d of the tree
     d hops after the root (`osu_oshm_reduce.c`).
   - **The fit**, to 2026-09-26's 402 lines (`pi_table()` reads the raw files
     under `docs/design/evidence/pi-cluster/`): h = 86 µs, o = 7 µs,
     B = 116 bytes a µs, 253 lines within the noise - question 1 below.
   - **The closed form**, `cost()`: the first piece's slowest way through the
     tree, then every later piece at the pace of the busiest link (beta pieces
     a vector piece: k + 1 on an interior lead's link) or of the window's loop,
     whichever is slower; integers, nanoseconds and bytes. Its tree terms
     (`tree_terms()`) come in closed form from the tree being full to its last
     level but one - a lead's positions on its way from the root are the
     base-k digits of its offset in its level, so the slowest way down is a
     largest digit sum - checked against a walk of every node for every fan-out
     from 2 to 32 (`tree_terms_walk()`), and beta against the pieces the
     simulation counts on each link.
   - **The chooser**, `choose()`: the small class and a team of one node keep
     the ceiling's tree; otherwise the window is the capacity and the fan-out
     the least cost from 2 to the ceiling, ties to the smaller (choice 2).
   - **Its checks**, every one in `check`: the fit's count, the regret of
     choosing by the simulation on the Pi's own table (at most 10%), the tree
     terms, beta, the closed form's choices against both link models (below),
     and the pins. **Its broken variants**, each of which must fail: no message
     cost (o = 0, h and B refitted: 244 lines within the noise, under the
     record's 253 - choice 3); and an interior lead counted k pieces rather
     than k + 1 (it fails at five nodes, k = 2, where the loop counts three).
2. **`shmem/src/collective-shape.c`**, beside `collective-tree.c`, on the
   boundary: `osoe_shape_cost` and `osoe_shape_choose`, the same arithmetic in
   C, pure - no `osoe`, no global, no floating point - a few dozen integer
   operations a call, the tree's critical paths in closed form rather than
   walked or cached. The shape carries its algorithm, a tree today, so that
   step 3's reduce-scatter can join the choice. Tests, in both builds:
   `shmem.shape.choice` pins four nodes of the Pi - the small class keeps the
   ceiling's tree, the star (k ≥ 3, one level) above it, k = 2 at 1 MiB, the
   two crossing at 32 KiB, inside F2's 16 to 64 KiB - and a loopback link's
   star, which it keeps to 1 GiB; the same inputs choose the same shape; and a
   collective too long to time saturates. `shmem.shape.grid` holds the C to the
   model on the 3,024 lines `shape_timing.py grid` wrote
   (`shmem/tests/shape-grid.txt`: two operations, four links, 1 to 4,096
   nodes, three ceilings and capacities, 8 bytes to 1 GiB); with beta's
   interior term broken in the C, 30 lines differ. `model.shape_timing.grid`
   holds the file to the model.
3. **The settings** (the brief's item 3 but the measurement, and item 4):
   `OSOE_COLLECTIVE_SHAPE` - `link`, the default, or `fixed`, the 0.1.0 tag's
   shape - and `OSOE_LINK_HOP_NS`, `OSOE_LINK_MESSAGE_NS` and
   `OSOE_LINK_BYTES_PER_SECOND`, all three or none, each in a range that keeps the
   chooser's arithmetic inside 64 bits (a hop to ten seconds, a message cost to
   one, one byte a second to ten terabytes). Read and checked by every PE,
   refused by name otherwise, one link number alone among them; held alike among
   a node's PEs by `open_control`, the header's new fields making layout 32; and
   between nodes at the rendezvous, in the up header's fifth shape (`osoeup-5`,
   80 bytes). Published as `node.collective_shape` and `node.link.*`, which
   `osoe-stat` shows in its header and as the summary's first line for each
   node: "collective shape link; link set: a hop of 86000 ns, a message cost of
   7000 ns, 116000000 bytes a second", or "link not set". `SHMEM_INFO`'s list,
   the README's two rows and the decisions index's line say that until the link
   is measured and the chooser reaches the data path, every collective runs as
   `fixed` does. Tests, in both builds: six refusals by name
   (`shmem.collective-shape.invalid.*`), the shape and the link disagreeing
   between a node's PEs (`shmem.collective-shape.local-disagree`,
   `shmem.link.local-disagree`), and `shmem.stat.link` beside `shmem.stat`'s new
   check of the default; in the full build, the two disagreeing between nodes
   (`shmem.job.disagree.collective-shape`, `.link`). Each was run against the
   mutation it was written for: the node's check of the link and of the shape
   removed, the up header carrying no link, one number alone let through.

### What the model shows beyond the brief

**Two link models, one chooser.** The brief's rule - "the busiest link's bytes
over B, plus a hop and a message cost per child at every level, plus the
window's fill" - leaves open how two transfers share a link. The fitted
simulation keeps a lead's sends in order and shares a lead's link in between
its senders ('sender'); the other end of what the hardware may do is every
connection a stream sharing both links fairly ('connection'). On four nodes the
two fit about alike (253 and 238 lines within the noise); on more they part,
in where an interior lead's link stalls. Choosing by one and running under the
other costs a median of 0, a p90 of 8.8% and up to 36%. Choosing by the closed
form costs, against 'sender', a p90 of 7.3% and at most 17%; against
'connection', 4.0% and 27%. So the closed form chooses as well as either
simulation would, whichever of them the hardware is, and it is what the C
computes. The Pi cannot tell the two apart; seven nodes and more can, which is
Part 2's VM and, later, a larger cluster.

**The window.** Neither simulation ever runs a large collective slower for
more pieces in flight, and the closed form's window term is its least exact:
chosen by cost, W came out too small (a regret of 20% at p90 on a grid). So
the window is the capacity (choice 2).

## The link's measurement

Built as the author answered the second question (item 4): at the end of
`shmem_init`, through the data path (`shmem/src/link-measure.c`, called from
`init.c`'s session start once the node's lead speaks).

- **Who measures what.** The root of every team's tree over the job's nodes is
  node 0, its first children nodes 1 to k (level order, `collective-tree.c`).
  The root's lead takes, against node 1's lead, the hop from fifteen round
  trips of an 8-byte put and its completion - halved, the median - and the
  bandwidth from the slope of a 512 KiB stream of 16 KiB puts over a 128 KiB
  one, three of each, medians; and the message cost from one put to each of
  its first `min(N - 1, ceiling)` children against one put alone, fifteen
  paired tries, the trimmed mean of the differences over the extra children.
  A burst to one child would not do: a connection carries consecutive small
  messages together, and on loopback the burst read 0 to 0.15 µs a message
  against the fan's 3 to 13. A job of two nodes has one child and no choice the
  message cost could move - every fan-out is the same tree there - and takes
  it as 0. Every number is an integer, clamped to the settings' ranges.
- **How every PE gets the root's numbers.** An AND of three words over the
  finalize team's split words (`osoe_team_split_and`), every PE but the root
  contributing all ones, so that each PE ends holding the root's: a reduction
  that borrows nothing from the heap, on counters no application collective
  touches, so the world team's epochs - which `osoe-stat` shows a user - stay
  the application's own.
- **Where it lives.** `osoe.job.link` and `osoe.job.link_source` (none, set,
  measured), on the boundary's side so that Part 2's chooser in
  `internode-generic.c` reads it; the measured numbers in the control segment
  (`node.link.measured_*`, layout 32 grown), which `osoe-stat --summary` shows
  as "link measured: ...", and `SHMEM_DEBUG`'s start-up line, which also says
  what establishing it cost the PE's start - one `write` a line now, since
  every PE says it at once and separate writes interleaved inside each other's
  lines.
- **When nothing is measured.** A job of one node, the shape `fixed`, or
  `OSOE_LINK_*` set: each is the same on every PE, so every PE takes the same
  path, and no message is sent for it.

**On the Pi** (4 × 1 and 4 × 4, fifteen starts, `build/scratch/shape/pi/link-2`
in the session's scratch tree; the first estimator's run in `link-1`):

| | 4 × 1, ten starts | 4 × 4, five starts |
|---|---|---|
| hop | 86.8 - 91.3 µs | 88.6 - 94.0 µs |
| bandwidth | 116.7 - 120.1 MB/s | 116.5 - 118.0 MB/s |
| message cost | 0 - 21.0 µs | 6.2 - 16.4 µs |
| a start's cost | 27.2 - 27.5 ms | 27.1 - 27.6 ms |

The hop agrees with the fit's 86 µs and the bandwidth with its 116 bytes a
µs, both within 8% start to start, and a start pays 27 ms, under the brief's
50. In the VM's loopback, five logical nodes: a hop of 24 to 44 µs, 4 to 40 GB/s
- one machine's four cores running five PEs and their agents is no link, as
the brief says - and 4 to 5 ms of the root's start; twenty jobs of 2 × 2 took
3,516 ms measured against 3,486 with the link set.

**The tests**: `shmem.link.{measured,set,fixed,none}.*` (`link.sh`, every PE's
`SHMEM_DEBUG` line: the same numbers on every PE, within the settings'
ranges, when measured; the given ones when set; none when fixed or on one
node) and the hang-walk's check that every node's segment shows the same
measured link. Mutated, each fails where it should: without the AND, the
measured tests and the hang-walk; with a root that contributes all ones, the
measured tests; with the segment unwritten, the hang-walk.

### A fourth stop-and-ask: the message cost between starts

**What the brief asks.** "The start-up measurement ... varying run to run by
more than a factor of two in any of the three numbers": put it to the author.

**What was found.** The message cost varies from 0 to 21 µs between starts on
the Pi, while the hop and the bandwidth agree within 8%. Within a start the
fan's extra cost comes as some tries about 20 µs late among others that cost
nothing more - the first estimator, a median, read 0 or 20 by turns (`link-1`);
the trimmed mean of paired tries (`link-2`) narrows it, and a start still
sometimes reads 0. Which mode a start is in persists through its tries, so
more tries do not settle it.

**What it moves** (`shape_timing.py`'s chooser at the Pi's hop and bandwidth,
the ceiling 8 and capacity 4 the author chose, 2 KiB to 1 MiB): nothing on
four, seven or sixteen nodes - every message cost from 0 to 21 µs chooses the
same fan-out at every size, reductions and broadcasts; on 64 and 500 nodes a
few sizes move between neighbouring fan-outs (8 against 4 at 2 KiB, 3 against
4 or 5 at 16 to 64 KiB), where the two cost about the same by being neighbours
at a crossover.

**Options.**
- (a) Keep the measurement as built: the trimmed mean, honest about its spread,
  which moves no choice the Pi can make.
- (b) Take the message cost as 0 unless set: the chooser then never charges a
  parent for its children; deterministic, and the model's fitted 7 µs - the
  average a tree pays - ignored.
- (c) Measure longer: more tries do not help where the mode persists through a
  start, and each fan costs about two round trips.

**Recommendation: (a)**, and the question again on a cluster large enough for
the message cost to move a choice. **Built meanwhile:** (a). **Merged with
(a)** at step 2's merge point, the answer not yet given: no choice the Pi can
make depends on it, and (b) is one line of `link-measure.c` (the read's finding
3; the night's record of 2026-10-07).

## Choices made without the author

Each is Claude's; the undo follows.

1. **The reference simulation's links share as 'sender'**, fitted; 'connection'
   kept beside it as the bracket the closed form is judged against. The
   queue-per-link simulation first written (each link's transfers in send
   order) fitted 230 lines and made a parent wait for its last child's first
   piece behind the first child's whole window. *Undo:* fit 'connection'.
2. **The window is the capacity**, where the brief has the chooser return the
   (k, W) of least cost with W from 1 to the capacity, ties to the smaller W.
   *Undo:* choose W by cost too, once its term is exact.
3. **The broken variant without the message cost fails the table's count, not
   "the 8-byte lines"**: on four nodes the 8-byte lines cannot tell o from h -
   with o = 0 the fit moves o's share into h (93 µs) and lands the same 18 of
   the 22. The term earns nine lines elsewhere, between 1 and 64 KiB. *Undo:*
   none needed; a cluster of more nodes could test the brief's version.
4. **The star is pinned as k ≥ 3 on four nodes**, not "k = ceiling": on four
   nodes every k from 3 builds the same star, and ties go to the smaller k. At
   8 bytes, the small class, the ceiling's tree is kept and pinned.
5. **The pins' loopback link** is h 20 µs, o 2 µs and 4 GB/s - the VM's order of
   magnitude, not a measurement.

## Questions for the author

### 1. The fit: 63% of the Pi's lines within the noise, and no fourth constant found

**What the brief asked.** "The cost model before the code, fitted to the Pi's
table and shown to predict every line of it within the noise before it
chooses anything... A model that cannot reproduce 2026-09-26's numbers chooses
nothing." And, to stop: "The fit failing to reproduce any line of the Pi's
table within its noise without a fourth constant: put the term and its meaning
to the author before building on it."

**What the table is.** 402 lines: 2026-09-26's OSU broadcasts and reductions
on four nodes of one PE, 4 bytes to 1 MiB, k = 2, 3 and 4, D = 1, 2, 4 and 8,
and the late session's f2 sweeps at D = 2. The noise, from the lines the two
sessions share: within 1% to 16 KiB, 3 to 6% at 32 and 64 KiB (F1's losses
there, once 44%), and no repeat at 1 MiB; bands of 2%, 8% and 4%. A piece then
carried 16,368 bytes, and there was no small class: every line went through
the pipelined loop.

**The fit.** h = 86 µs, o = 7 µs, B = 116 bytes a µs put 253 of the 402 lines
within the noise, median error 1.9%:

| bytes | lines | within | where it misses |
|---|---|---|---|
| 4 - 512 | 176 | 146 | 20 slow, 10 fast; median error 0.7% |
| 1 - 8 KiB | 88 | 17 | 47 slow, 24 fast; 7.2% |
| 16 KiB | 22 | 9 | 9 slow, 4 fast; 3.2% |
| 32 - 128 KiB | 62 | 41 | 7 slow, 14 fast; 6.1% |
| 256 KiB - 1 MiB | 54 | 40 | 9 slow, 5 fast; 2.6% |

**What the misses are.** Between 2 and 8 KiB the Pi's bytes cost less than any
model that moves them over the link at its measured speed: a k = 2 broadcast
on four nodes takes 406 µs at 2 KiB, 408 at 4 KiB and 418 at 8 KiB - 4 KiB more
across two hops for 2 µs - and a k = 4 reduction adds 111 µs over its 8-byte
line at 4 KiB where the root's link alone must carry three vectors in and three
out. Yet at 1 KiB a reduction costs more than the bytes: k = 2 adds 71 µs over
8 bytes where the model adds 29. Something in the transport costs a step near
1 KiB and overlaps the transmission above it; three numbers about a link cannot
say what.

**A fourth constant, tried**: that a part hs of the hop - the receiver's wake,
say - overlaps a message's own wire time, so that a message arrives at
max(t, hs) + (h - hs): a joint fit of h, o, B and hs put hs at 0. I have no
fourth term to put to you, only the misses.

**What the misses cost a chooser.** The choice, not the time, is what the
model is for, and the table judges it directly: for each call the table
measured under several shapes, the shape the model picks with the window at
the table's largest against the fastest measured - a median of 0.2%, a p90 of
3.9%, and at most 9.7%, a 32 KiB broadcast in F1's band where the noise is 8%.

**Options.**
- (a) Accept the model as the chooser's, the bar changed from "every line within
  the noise" to "chooses within the noise of the best measured shape"; Part 2's
  Pi session, which measures `link` against `fixed` at every size anyway, adds
  sizes between 1 and 16 KiB and refits before the chooser goes on the data path.
- (b) Hold Part 2 for a Pi session that finds the missing term: one put, and
  one put with a quiet, between two leads from 8 bytes to 64 KiB, with the
  progress threads' wakes counted.
- (c) Fit to the lines that matter for the choice - the crossover's band and
  the 1 MiB lines - and accept larger errors elsewhere.

**Recommendation: (a)**, with (b)'s sweep folded into Part 2's session: the
misses move no choice beyond the noise, the brief's Part 2 claim - no line
slower than `fixed` by more than the noise - is the bar that protects a program,
and a term found later changes one function in Python and one in C.

**Answered** (the author, 2026-10-06, `docs/decisions/2026-10-06.md` item 3):
(a), "accept". `model.shape_timing` already holds the bar - no choice more than
10% from the fastest measured shape - beside the fit's line count as a guard;
Part 2's Pi session adds the 1 to 16 KiB sweep.

### 2. Where the link is measured: the brief's place comes before the mesh exists

**What the brief asked.** "At start-up, once the leads' mesh is built and before
the start-up release travels down (`rendezvous.c`, `send_down`), the root
measures its link against its first child... Then the three numbers travel
down in the down header."

**What the code is.** `send_down` is the job rendezvous's down phase, inside
`job.c`'s join, and it is what tells each node the others' addresses: the mesh
is built after it, by `osoe_transport_init`, and the progress threads - half of
the hop the brief means, "through the sender's progress thread and the
receiver's" - start after that. Before `send_down` the only connection between
two leads is the rendezvous's own blocking socket.

**Options.**
- (a) Measure at the end of `shmem_init`, through the data path: the root's lead
  against its first child's, puts and fetching adds over the mesh with both
  progress threads running - the hop a collective pays - then a small broadcast
  of the three numbers over the world team (24 bytes, the small class, the
  record's tree, no shape needed to send it) before `shmem_init` returns, so
  every node holds the root's numbers before any collective can choose. The
  rendezvous and its down header stay as they are; the overrides are agreed in
  the up phase, as the brief says.
- (b) Measure over the rendezvous's socket before `send_down`, as written: the
  hop would miss both progress threads, most of the Pi's 85 µs, and the chooser
  would build trees too deep for small messages - unless a constant for the
  threads were added, a fourth constant by another name.
- (c) Measure lazily, at the first large collective: every node would have to
  agree on the moment, and the first call would pay for it.

**Recommendation: (a).** **Answered** (item 4): (a), "yes"; built as "The
link's measurement" above says.

### 3. The defaults the brief names cost a PE 2.6 times the tag's internal region

**What the brief asked.** A ceiling of 8 and a capacity of 4 by default; and,
to stop, "the default ceiling of 8 or capacity of 4 costing a PE more than
twice the tag's memory for small slots and lanes at the defaults: put the
numbers to the author with the alternatives."

**The numbers** (`internal-region.h`: 256 team numbers, small collectives of
1 KiB, 17 lanes - `OSOE_MAX_THREADS`' 16 and one - on four nodes; the region
grows by 0.4 MiB to 500 nodes in every row):

| ceiling, capacity | before the lanes | a lane | the region a PE | against the tag |
|---|---|---|---|---|
| 4, 2 (the tag) | 3.38 MiB | 160 KiB | 6.03 MiB | 1 |
| 4, 4 | 3.38 | 320 | 8.69 | 1.44 |
| 8, 2 | 5.88 | 288 | 10.66 | 1.77 |
| 6, 4 | 4.62 | 448 | 12.07 | 2.00 |
| 8, 3 | 5.88 | 432 | 13.05 | 2.16 |
| 8, 4 (the brief) | 5.88 | 576 | 15.45 | 2.56 |

The region is part of each PE's heap object in `/dev/shm`: address space,
written only as used, but counted whole by the start-up capacity check, so a
PE's figure there rises from about 263 MiB to about 272 (3.6%) at the brief's
defaults; 256 PEs on a node count 2.4 GiB more.

**What each buys.** The capacity: on the Pi a 1 MiB reduction at k = 2 took
33.5 ms at D = 2, 23.0 at D = 4 and 19.4 at D = 8 - the window is the 1 MiB
claim. The ceiling: nothing on four nodes, where every k ≥ 3 is a star; at 500
nodes an 8-byte reduction climbs ⌈log₈ 500⌉ = 3 levels against ⌈log₄ 500⌉ = 5,
about 600 µs against 960 at the Pi's link (2h + k·o a level).

**Options:** the brief's 8 and 4; 8 and 2 (under twice, but the window the
1 MiB claim needs is gone); 4 and 4 (1.44 times, the window kept, the deep
trees at hundreds of nodes kept too); 6 and 4 (twice exactly).

**Recommendation: the brief's 8 and 4**: what grows is address space and the
capacity check's count, by 3.6% of a PE's figure, and both numbers buy
something measured or computed. Part 1 keeps the tag's 4 and 2, which nothing
in it needs changed; Part 2 sets what you choose. **Answered** (item 5): 8 and 4,
"accept"; Part 2 sets them.

## The adversarial read

A fresh agent read the branch at `eefb36f` (now `b073e7a`), asked to find a PE
ending `shmem_init` with another shape or other numbers than its peers, or a
chooser past the ceiling or the capacity: the report is
`docs/reviews/archive/v0.2-step2-part1-adversarial-read.md`. The rule holds - the
same path and the same numbers on every PE, measured up to 34 nodes and under
perturbation, and the chooser held to the model on 80,000 random inputs under the
sanitizers - and it found one defect and nine smaller things. In `cd5872d`:

- **Finding 1, the defect**: the streams were sized from the root's own
  `OSOE_MAX_THREADS`, which nodes need not agree on, and ran past the region of a
  node with fewer threads, ending the job at start under a message naming the
  wrong settings. They now stay within the fewest lanes any node can have - two,
  at `OSOE_MAX_THREADS=1` - and within 64 KiB, a lane's row at the capacity, the
  longer streams wrapping as a collective's pieces wrap in a row. That also
  bounds what a start touches of node 1's lanes to 64 KiB (finding 6, now in the
  README's `OSOE_MAX_THREADS` row). `shmem.link.few-lanes.0-1` - node 1 at one
  thread and the smallest lanes the settings make - passes, and fails with the
  old sizing.
- **Finding 8**: a long stream no slower than the short gave the highest
  bandwidth a setting may, which Part 2's chooser would read as bytes for free.
  Now a slope more than four times the long stream's own rate is taken for noise,
  and that rate - which counts the hop, so is a bound below the link's, within a
  few percent of the slope where the bytes outweigh the hop - stands in for it.
- **Finding 4**: osoe-stat reads the measured bandwidth first, the last of the
  lead's three release stores, so it never shows one start's hop beside
  another's bandwidth.
- **Findings 5 and 7**, in the README: the measurement's puts count in the root's
  lead's transport counters, and its 1.9 MiB of streams cost a slower link more -
  about 170 ms at 100 Mbit/s - which set numbers or the shape `fixed` skip. The
  streams are not shortened: on the Pi the long one's 4.5 ms against the short's
  1.1 is what makes the slope steady.
- **Finding 10**: the stale comments, the decisions index's line, and the README's
  promise of a report at 30 s, which the start's last step - a collective's wait,
  shown in osoe-stat as one, its routine `shmem_init` - does not make; the cost of
  a shape no setting reaches is the most there is, not a division by 0.
- **Finding 9**: the scans keep the ceiling's tree (Part 2's record), so a message
  cost of 0 on two nodes moves none; nothing in the agent reads `osoe.job.link`.
- **Finding 3**, the fourth stop-and-ask, is the author's (below).

## Verification

`977418f` (the cost model and the chooser), in `osoe-load` (Debian 13, GCC 14):
the debug preset whole, 731 of 731, no warning - `shmem.boundary` and
`shmem.exports` with the new file among them, and `shmem.shape.choice` and
`shmem.shape.grid`; `model.shape_timing` (40 seconds) and
`model.shape_timing.grid` in the release configure; the C test also with the
host's clang. Each new test against the mutation it was written for: the C's beta
without an interior lead's parent (30 grid lines differ), and the model's two
broken variants. `4b30e34` (the settings), with liburing hidden for the
single-node build: the full build's debug preset 742 of 742, the single-node
build's 360 of 360.

`b41e729` (the measurement), the merge verification: the four presets with the
floor pins and `tier3`, failing only `shmem.memory-orders` (a relaxed store in
the measurement) and `test-internode`'s deferred-wakes check, which the init's
AND made count, with the three restart tests that run it - both fixed in
`b073e7a`; `osoe-pacer collectives` at three values of
`OSOE_SMALL_COLLECTIVE_BYTES`, its teams and threads, and ten minutes of `mix`,
no failure; the single-node build on Debian and Rocky, its four presets and an
install, failing only `shmem.memory-orders`; the models; the SOS sweep at 0,1 and
0,0,1,1, 159 programs clean on both, the known failures the runbook lists
(`shmem_team_b2b_collectives`, `shmemalign`, two C11 programs that do not build).
`b073e7a` in `osoe`: debug 778 of 778, release 778, asan 775, tsan 544, `tier3`
113, with the pins. `cd5872d` (the read's findings): the link, job, shape and
internode tests, 45 of 45, and the mutation above. The merge verification of the
whole step - both parts - runs on Part 2's tip.
