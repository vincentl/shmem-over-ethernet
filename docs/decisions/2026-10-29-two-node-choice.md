# 2026-10-29: P4 - the two-node choice between a tree and Träff's

P4 (`docs/prompts/v0.2-open-items.md`, §1; v0.2 step 3's adversarial read, finding 2,
`docs/reviews/archive/v0.2-step3-adversarial-read.md`; the release checklist's after-the-merge
item, `docs/release-checklist.md`; the README's admission). On two nodes a tree and Träff's send
the same bytes - the tree's one child sends the vector up and takes it down, Träff's two leads
send each other half of it twice - but `osoe_shape_cost` (`shmem/src/collective-shape.c`)
charged a tree's partial last 16 KiB piece a whole period, where `osoe_shape_circulant_cost`
charges Träff's its exact bytes. So 16,385 bytes cost the tree 141 µs more than 16,384 on the
Pi's link, and the chooser took the tree at every multiple of 16 KiB and Träff's at most other
sizes: 11,486,022 of the 16,776,192 sizes from 1,025 bytes to 16 MiB (68%), the choice changing
2,046 times between 1,025 bytes and 16 MiB (reproduced, `crossovers.txt` below). Neither choice
had been measured. The review named two fixes: (a) take the tree on two nodes; (b) charge the
tree's last piece by its bytes, as step 2's fit, the event simulation, does.

Done here by a session the lead started, under the author's word of 2026-10-29 ("Do P1 and P4
in parallel. Go!") and the standing mandate to proceed on its own recommendations.

**The outcome: fix (b), in the form that charges a partial last piece the larger of its own
bytes on the busiest link and the window's pace less what its shorter loop gives back.** Two
nodes of the Pi's link now take the tree at every size; from three nodes to 512 the crossover is
where it was, byte for byte; the Pi measured the tree the faster to 16 KiB and the two within 12%
of each other above it, Träff's the faster in a band, which is left to the author (below).

## What was measured

Main at `cbfaa7e1` on the Pi cluster, the release build on the head node without a warning, and
a probe of the project's own, `two-node.c`, built with its `oshcc`: a byte sum over the world
(`shmem_uint8_sum_reduce`) at 4 and 8 KiB and at 16 KiB k and 16 KiB k + 1 for k = 1, 2, 3, 4,
6, 8, 16, 32, 64, 128 and 256 - to 4 MiB + 1 - each size 20 calls and then 300 (to 64 KiB + 1),
100 (to 1 MiB + 1) or 40, each call between two barriers; the slowest PE's median reported; the
last call's algorithm read from each node's lead's record through `osoe-stat`'s reader, as
`test-circulant-reduce` reads it, and three bytes of the result checked. `srun -K1
--cpu-bind=none -N2`, one PE a node and four. Every node's TCP and generic segmentation offload
off before and after each batch, the nodes 26 hours up.

**How each algorithm was forced** - by the link's numbers, no test hook:
- *the tree*: `OSOE_COLLECTIVE_SHAPE=tree`;
- *Träff's*: the shape `link` on a link set so that Träff's is the cheaper at every size measured,
  `OSOE_LINK_HOP_NS=10000 OSOE_LINK_MESSAGE_NS=0 OSOE_LINK_BYTES_PER_SECOND=116000000`. The
  tree's arm ran on the same link, so the two differ in the algorithm alone: on two nodes every
  fan-out is the same tree, and the bandwidth leaves every data wire's unsent limit at its 16 KiB
  floor, as the Pi's measured link does. Every call of the tree's arms took the tree and every
  call of Träff's arm Träff's, on every PE.

Two batches (`docs/design/evidence/two-node-choice/`): `two-node.sh`, four series - three on
pi-001 and pi-002, one on pi-003 and pi-004 - the arms in one order (the tree, Träff's, and the
default as built, the link measured); then `two-node-again.sh`, six series on pi-001 and pi-002,
the arms' order turned one place each series, and the tree on the link measured beside the tree
on the link set. The second batch was run because the first's default arm ran the tree up to 9%
faster at 4 to 16 KiB than its tree arm: with the order turned, the tree on a measured link took
0.994 to 1.016 of the tree's time on the link set at a PE a node, 1.051 at one point - setting
the link changes nothing but the choice, and the first batch's tree arm, always run first, was
the slower for its place. The links measured: a hop of 82.7 to 90.0 µs, a message cost of 0, 116.0
to 118.5 MB/s.

## What it found

The second batch, pi-001 and pi-002, the median over its six series of the slowest PE's median,
µs, and Träff's time over the tree's - their medians' ratio, and the least and most of the
series' own:

| bytes | 1 a node: tree | Träff's | ratio (series) | 4 a node: tree | Träff's | ratio (series) |
|---:|---:|---:|---|---:|---:|---|
| 4,096 | 226.8 | 378.0 | 1.667 (1.586-1.836) | 257.3 | 345.4 | 1.342 (1.167-1.486) |
| 8,192 | 278.8 | 412.5 | 1.480 (1.422-1.616) | 322.9 | 447.9 | 1.387 (1.229-1.480) |
| 16,384 | 431.2 | 521.0 | 1.208 (1.148-1.276) | 502.2 | 517.2 | 1.030 (0.980-1.041) |
| 16,385 | 446.1 | 524.0 | 1.175 (1.131-1.245) | 510.9 | 519.1 | 1.016 (0.965-1.025) |
| 32,768 | 766.3 | 764.1 | 0.997 (0.995-1.052) | 842.6 | 836.2 | 0.992 (0.859-1.067) |
| 49,152 | 1,045.8 | 1,015.9 | 0.971 (0.953-1.049) | 1,191.3 | 1,104.1 | 0.927 (0.821-1.014) |
| 65,536 | 1,302.8 | 1,229.4 | 0.944 (0.890-1.095) | 1,441.0 | 1,368.8 | 0.950 (0.702-1.073) |
| 65,537 | 1,302.1 | 1,238.3 | 0.951 (0.891-1.095) | 1,453.2 | 1,367.5 | 0.941 (0.692-1.072) |
| 98,304 | 1,605.5 | 1,472.2 | 0.917 (0.901-1.134) | 1,893.4 | 1,814.9 | 0.959 (0.788-1.077) |
| 131,072 | 2,091.6 | 1,862.2 | 0.890 (0.876-0.952) | 2,538.7 | 2,344.1 | 0.923 (0.737-0.992) |
| 262,144 | 3,364.3 | 3,354.1 | 0.997 (0.964-1.015) | 4,157.4 | 3,991.1 | 0.960 (0.860-0.967) |
| 524,288 | 5,673.0 | 5,794.8 | 1.021 (0.920-1.030) | 7,399.6 | 7,265.1 | 0.982 (0.961-0.990) |
| 1,048,576 | 10,378.1 | 10,664.1 | 1.028 (0.997-1.038) | 13,895.8 | 13,687.1 | 0.985 (0.958-0.997) |
| 1,048,577 | 10,362.1 | 10,671.7 | 1.030 (1.025-1.032) | 13,907.5 | 13,684.2 | 0.984 (0.954-0.996) |
| 2,097,152 | 19,653.7 | 19,929.7 | 1.014 (1.004-1.030) | 27,262.7 | 26,851.7 | 0.985 (0.961-1.038) |
| 4,194,304 | 38,168.9 | 39,665.1 | 1.039 (1.024-1.047) | 51,227.0 | 52,174.6 | 1.018 (0.971-1.191) |

Every size and its + 1 are in `again/analysis.txt`, the first batch in `out/analysis.txt`, which
agrees above 16 KiB to within its own spread, pi-003 and pi-004 included.

- **The byte past a multiple of 16 KiB costs either algorithm nothing measurable**: the tree,
  at the median of the series, −29 to +34 µs at every multiple to 2 MiB in both batches and both
  PE counts, and +121 to +127 µs at 4 MiB at a PE a node, 0.3% of the call - against the 141 µs
  a period the cost model charged it; at 16,385 itself 0 to 12 µs. Träff's alike. The choice's
  dependence on the remainder had nothing under it.
- **Neither algorithm wins throughout.** To 16 KiB the tree: Träff's took up to 1.7 times as
  long. From 32 KiB the medians came within 12% of each other: Träff's the faster from 48 to
  128 KiB at a PE a node, by 3 to 12%, and at four PEs a node from 32 KiB to 2 MiB, by 1 to 8%;
  the tree the faster from 512 KiB at a PE a node, by 1.4 to 3.9%. Series spread by up to a
  fifth at 32 to 128 KiB at four PEs a node.
- **The closed forms hold at 16 KiB**: the tree 468.5 µs and Träff's 513.2 by the cost model on
  the Pi's fitted link, measured 431 to 472 and 519 to 521. Above it both run 5 to 46% slower
  than their closed forms at a PE a node, the tree the more so from 48 to 128 KiB - 37 to 46%,
  Träff's 21 to 31% - which is where Träff's band lies; and the cost model, three numbers for
  the link, has nothing that moves with the PEs a node.

## The choice

**Both fixes take the tree on two nodes of the Pi's link at every size** - the measurement's
choice to 16 KiB, and within 12% of Träff's either way beyond. They differ in what else they do:

- **(a), the tree on two nodes**, changes the 38 lines of the shape grid where two nodes took
  Träff's, on every link, and nothing else. It answers the defect where it was found and leaves
  the defect itself - a partial last piece charged a whole period - wherever else it decides
  something: at three nodes and more wherever a crossover lies above 16 KiB, as on a loopback's
  or a faster link, the tree took 16 KiB k and Träff's 16 KiB k + 1 alike.
- **(b), the last piece by its bytes**, changes every tree's cost of a size that is not a whole
  number of pieces, on every node count - and corrects the model where it was wrong, against
  step 2's simulation as against the Pi. It needs no rule for two nodes: there the closed forms
  take Träff's only where the tree's pace - a piece on the busiest link, or its window's loop -
  is slower than Träff's by more than the hops and message costs of Träff's second half: on the
  Pi's link nowhere, and on a loopback's from about 300 KiB, where the tree's window binds.

So (b). What it moves elsewhere was found with the grid, the model's checks and a count of every
size (`model-checks.txt`, `crossovers.txt`):

- **On the Pi's link at the defaults, nothing but two nodes.** At every size from 1,025 bytes to
  4 MiB on 3 to 512 nodes, the same choice as before: Träff's from 16,184 bytes on three nodes to
  3,583 on 511 and 512, and at every size above, one change each - the README's "under 4 to
  16 KiB by the node count" stands. Two nodes: Träff's at none of the 16,776,192 sizes to 16 MiB.
- **On the grids**: 1,045 of the shape grid's 11,880 lines, every one at 16,385 bytes, the one
  size of the grid with a partial last piece - their costs, 168 of them their shape (mostly the
  fan-out) and 34 their algorithm, Träff's to the tree: two nodes of the Pi's link, two and three
  of the slow-hop link, and 8 to 65,536 nodes of the loopback's and the fast link's. By the
  simulation, of the 113 changed shapes on up to 33 nodes, 81 are faster than the shapes they
  replace and 23 slower, by 2 to 15%; the geometric mean of their times 0.919 of the old. The
  places grid: 633 of its 9,360 lines, the members' vector's cost alone; no way changes.
- **On a loopback's link** the choice still changes a few times near a crossover where the window
  binds - 1 to 5 times, within about two pieces of it (289,429 to 327,857 bytes on two nodes) -
  as two costs that each step with the pieces cross.

**The form.** Three were weighed against the simulation's own increment for whole pieces plus 1,
8,192 and 16,383 bytes, on 2 to 16 nodes, fan-outs 2, 4 and 8, windows 2, 4 and 8, on the grid's
four links (`model-checks.txt`, part 1; the error a share of the call):

| form: a partial last piece adds | Pi's link: median, p90, worst | a loopback's | the slow-hop link | the fast link |
|---|---|---|---|---|
| a period, as built | 0.060, 0.359, 0.842 | 0.097, 0.395, 0.551 | 0.060, 0.359, 0.820 | 0.129, 0.451, 0.653 |
| its bytes on the busiest link | 0.000, 0.044, 0.305 | 0.040, 0.318, 0.728 | 0.000, 0.050, 0.348 | 0.049, 0.432, 0.854 |
| the period's formula at its bytes | 0.009, 0.096, 0.315 | 0.090, 0.363, 0.551 | 0.009, 0.106, 0.327 | 0.125, 0.427, 0.653 |
| **the larger of its bytes on the busiest link and the window's pace less what its shorter loop gives back** | 0.000, 0.048, 0.315 | 0.065, 0.324, 0.551 | 0.000, 0.067, 0.327 | 0.105, 0.387, 0.663 |

The bytes alone were the first built, and the best at the median; but where the window binds, a
piece past the window's first W waits a whole loop of a deep tree in the simulation - 148 µs
for one byte past 64 KiB, a broadcast on sixteen nodes of a binary tree at a window of 4 on the
loopback's link - which neither form models, and the bytes alone fell further short of it than the
period had, the worst case on those two links 73 and 85% of the call against the period's 55 and
65%. The form built is no worse than the period at the worst on any link but by a point, better
at the median and the p90 on all four, within half a point of the bytes alone on the Pi's link
and the same choices there at every size, and continuous at the multiples: at a whole piece's
bytes it is the period. Its reasoning is the window's: the last
piece goes up when the window lets it, a period after the piece before it as a whole one would,
and comes back sooner by its missing bytes' share of the loop - or, where the link holds it, it
adds its own bytes on the busiest link.

## Built

- `osoe_shape_cost` (`collective-shape.c`, its comment in `collective-shape.h`): a partial last
  piece charged as above; a size of whole pieces, and a size of one piece, cost what they did.
  `shape_timing.py`'s `cost()` the same, line for line, the old form kept as `LAST_PIECE_WHOLE`
  for its broken variant; `shape-grid.txt` and `places-grid.txt` written again from the model.
- **Tests, each with the mutation it catches** (the mutants built on the Mac against the test's
  own source and the grids, `shmem.circulant.2`'s in the VM below):

| test | what it holds | its mutation | caught |
|---|---|---|---|
| `model.shape_timing`, LAST (`check_last_piece`) | on two nodes of the Pi's link 16 KiB k + 1 takes what 16 KiB k takes, k to 256; one byte past whole pieces adds to the closed form what it adds to the simulation, within 1% of the call at the median and 15% at worst, on 2 to 16 nodes at fan-outs 2, 4 and 8 - with the fix 0.000 and 0.110 | the broken variant, a period (`LAST_PIECE_WHOLE`) | both halves: 256 k of 256; 0.184 and 0.820 |
| `shmem.shape.choice`, `check_last_piece` | on two nodes of the Pi, 16 KiB k + 1 costs the tree 9 ns more than 16 KiB k and half a piece more 70,621 ns, and 16 KiB k - 1, k and + 1 take the tree, k to 256 | the fix reverted (`cbfaa7e1`'s `collective-shape.c`) | 768 checks |
| `shmem.shape.choice`, `check_crossovers` | from three nodes to 512 on the Pi's link, Träff's from 3,500 bytes to under 16 KiB and at every size above, on a ladder to 1 MiB; on three exactly from 16,184 | the tail's bytes not multiplied by the busiest link's count (`tail = last_ns`) | three nodes: "from 16192 bytes, and the tree above it" |
| `shmem.shape.grid`, `shmem.shape.places-grid` | the C against the model, line by line | either mutation | 1,045 and 733 lines |
| `shmem.circulant.2`'s line, pinned in `shmem/tests/CMakeLists.txt` | "Träff's from 0 bytes, 0 calls checked as Träff's" - the program asks the chooser it compiles in, so a chooser that took Träff's would have the program agree | the fix reverted | in the VM: "154 calls checked as Träff's, 4 of them on teams that are not the world" - the review's 6 were at `5cbd47e`, before the phases since added |

- The README's two-node sentence; the design notes' §5.8, as built at v0.2; the release
  checklist's item from step 3; the decisions in force; P4 closed in the open items. No test was
  added to the suite's count: the new checks are in tests it has.

## Verification

In the development VM, on `88e6fe70` (the build, `ad44a6bb`, with its follow-ups: the comment and
the record naming `shmem.circulant.2`'s count on this tree, the README rewrapped, the Pi results'
section). **A pre-flight** on `ad44a6bb` first, the release build: the static checks 14 of 14 and
the shape tests, the shape models and the circulant cases 51 of 51, `shmem.circulant.2` ending
"every phase on 2 nodes, 2 PEs, Träff's from 0 bytes, 0 calls checked as Träff's, 0 of them on
teams that are not the world"; and the same tree with `cbfaa7e1`'s `collective-shape.c`, the fix
reverted, built without a warning, failing all three of `shmem.shape.choice`, `shmem.shape.grid`
and `shmem.circulant.2` - the last on its pinned line, the program counting "154 calls checked
as Träff's, 4 of them on teams that are not the world". **The whole verification** on
`88e6fe70`, P2's `verify-default.sh` less its two wake variants, which hold P2's settings, 62
minutes from 02:35 UTC:

- every preset, tier 3's and the single-node build without a warning; the static checks 14 of 14;
- release 1,205 of 1,205 (the models apart), debug 1,205 of 1,205, asan 1,165 of 1,165, tsan 837
  of 837 through its gate, the single-node suite 584 of 584;
- the internode suite in debug at no helpers, on demand and on demand at no helpers, 625 of 625
  each; on demand under asan 602 of 602 and under tsan 282 of 282;
- tier 3's suite 138 of 138; `model.shape_timing`, `.grid` and `.places-grid` 3 of 3;
- the counts P2's, no test added: debug 1,205, internode 625; tsan 837, internode 282; the
  single-node build 631; release without the models 1,205; tier 3 138; the kernel's journal since
  the run began naming no memory kill.

Every failure: none. And the static checks again on the tree of the commit that records this,
the release build without a warning: 14 of 14.

## For the author

- **A two-node rule from the measurement.** On two nodes Träff's beat the tree by 3 to 12% from
  48 to 128 KiB at a PE a node, and by 1 to 8% from 32 KiB to 2 MiB at four, where the cost model
  has the tree the cheaper by 45 µs at every size. As built the chooser took Träff's at the sizes
  in that band that are not multiples of 16 KiB - by the remainder, not the band - and the tree
  at its multiples; now the tree at both, so a size such as 131,073 bytes is 13% slower than it
  was at a PE a node, and 16,385 bytes 15% faster. A rule such as Träff's on two nodes from
  48 KiB would rest on the Pi's link alone and on the PEs a node, which the chooser does not
  know; I left it to you. My recommendation: no rule now; measure two nodes on the Google
  cluster if it runs again, where a piece takes 6 µs on the wire, not 141.
- **Where the window binds**, the closed form's window term is its least exact, as its header
  says: a piece past the window's first W waits a whole loop of a deep tree in the simulation,
  which the period's pace spreads across every piece. Not P4's, and not a choice on the Pi's link;
  noted in case a faster link's shapes are ever fitted.

## To undo

Revert the build's commit: `osoe_shape_cost` charges a partial last piece a period again, the
grids go back, and `shmem.circulant.2`'s pinned line must go with it, since two nodes take
Träff's for 154 of its calls again.
