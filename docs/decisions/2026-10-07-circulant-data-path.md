# 2026-10-07: Träff's on the data path (v0.2 step 3, Part 2)

The second part of v0.2's third step, on `docs/prompts/v0.2-step3-rabenseifner-prompt.md`
(the brief) and its section "Amended on 2026-10-07", which wins over the rest:
Träff's circulant reduce-scatter and allgather as the record of Part 1 designs
it (`2026-10-07-circulant.md`) and the author accepted it on 2026-10-07
(`2026-10-07.md`, item 2), built into the data path. The model,
`tools/modelcheck/circulant_model.py`, is the specification.

**Where it stands.** Built on the branch `circulant`, with Part 1. One
stop-and-ask, the credits (question 1, below): the design as written credits
every piece, and so leaves the last credits of each round a write into a lead's
words that the lead never waits for - the brief's first stop-and-ask. Built
meanwhile with the recommendation, which the model holds and checks.

## What was built

- **The reduce-scatter's words** (`internal-region.h`), per team number past
  the allgather's, ⌈log₂ N⌉ + 4 lines of 128 bytes at the job's N, in the
  accepted order: an arrival counter a row, the credit counter, the allgather's
  grant counter, the reduce-scatter's grant line - its counter, the reduction's
  epoch, identity and bytes, and a lane field a round - and the senders' lanes
  line, a lane word a row. At most 16 rows, which the senders' lanes line holds:
  a team of more than 65,536 nodes takes the trees (`shape_of`). They move the
  scans' area and the lanes; `osoe_internode_generic_number_taken` zeroes them.
- **The grants** (`circulant_grants`, from `osoe_internode_generic_reduce_enter`,
  before the lead waits for its node's members): to each reduce-scatter sender
  r - s_k, the head of the grant and this thread's lane in field k, then a
  fenced add. A lead starts its reduce-scatter once its grant counter shows all
  ⌈log₂ N⌉ of its own, reads and clears the line, and holds every field to this
  reduction (`take_round_grants`). As each reduce-scatter round ends it adds one
  to the allgather grant counter of the round's receiver, and starts its
  allgather once its own shows all ⌈log₂ N⌉.
- **The rounds** (`in_rounds`, beside `in_pieces`): each of the 2⌈log₂ N⌉ rounds
  in order, its intervals cut into pieces of a lane's slot, each interval on its
  own; piece j into round k's block of the receiver's lane at the block's slot j
  mod the window (choice 6), a put of its header and bytes and a fenced add on
  the round's counter; a
  reduce-scatter round's first piece preceded by the sender's lane word in the
  receiver's senders' lanes line, which the receiver reads, clears and checks as
  it takes that piece. The lead takes whichever is ready, its next piece out
  first, and waits on one watch over its counters when neither is. Every piece's
  header is checked - its number carries the round in its top five bits, so no
  piece of another round or half passes - and combined or copied into the
  accumulator on the lead's own thread, then cleared.
- **Credits**, for the pieces a sender's window waits on - the first P - W of a
  round of P (question 1).
- **The chooser on**: `shape_of` allows Träff's under `link`; a new value of the
  setting, `OSOE_COLLECTIVE_SHAPE=tree`, chooses among the trees alone - the
  brief's Part 3 setting, built now because the tree's own pipeline tests need
  it (choice 4).
- **What a call took, published** (layout 33): `team.tree.algorithm` and
  `team.tree.circulant_rounds` (choice 1).

## Choices made without the author

Each is Claude's; the undo follows.

1. **The algorithm a call took is published in a second line of the tree
   state**, which grows from 64 to 128 bytes on x86-64 and stays one 128-byte
   line on aarch64: `algorithm` - 0 a tree, 1 Träff's - and the rounds the last
   Träff's reduction made, counted as they ran. The brief left a second line or
   a counter to the record. A counter would say how often, where the tests need
   to know which call took which - the reason step 2 chose per-team
   publication over per-fan-out counters. For Träff's the existing fields say
   its class, lane, the chooser's fan-out and window, no tree children, and in
   `most_pieces_in_flight` the most pieces the lead had put in a round and not
   yet held the credit for - at most the window, which the tests pin. *Undo:* a
   thread-scope counter of Träff's calls in place of the two fields.
2. **A Träff's piece's header names its round** in the top five bits of its
   piece number and its index in the round below: the model's piece tag is
   (collective, half, round, index), and a header with the index alone would let
   a reduce-scatter piece left in a row pass for the allgather's piece of the
   same index. *Undo:* the piece's index alone, with the half and round unchecked.
3. **A grant's lane field holds the lane plus one**, so that a field never
   written, or cleared by its reader, reads as none: the internal region starts
   at zero, and lane 0 is a real lane. *Undo:* the lane itself, with the region's
   fields set to an impossible lane at start.
4. **`OSOE_COLLECTIVE_SHAPE=tree` now, not in Part 3**: the brief's setting for
   the measurement and, after it, the way to turn the algorithm off by name. With
   Träff's on, the tree's pipeline tests - which pin the trees' counts at 1 MiB
   - saw some reductions take Träff's on a measured loopback link; they run under
   `tree` now. *Undo:* none needed; Part 3 measures with it.
5. **A round's two intervals are cut into pieces each on its own**, so a round
   whose blocks wrap past the vector's end takes at most one more piece than a
   cut across the wrap, and no piece is in two places. *Undo:* one cursor across
   both intervals.
6. **Each round takes the lane's rows its rounds leave it, and keeps them all
   in flight.** The design had round k in row k at the capacity's window, four
   pieces; but a lane has the ceiling's nine rows and a reduction over N nodes
   uses ⌈log₂ N⌉, two on four nodes. Built so, Träff's was 1.14 to 1.19 times
   faster than step 2's trees at 1 MiB on the Pi, where the model said 1.34
   (`comparison-window-4/`, three rotations); a
   window of eight (`OSOE_COLLECTIVE_PIPELINE=8`) took 9.4% off it, and taking
   an arrived piece before sending the next took nothing (the Pi's evidence,
   `experiment-1/`). So the chooser gives Träff's a window of the capacity
   times (ceiling + 1) / ⌈log₂ N⌉ - 16 pieces on three or four nodes, 12 on five
   to eight, 8 on nine to sixteen, the capacity from seventeen - and round k
   takes the k-th block of that many of the lane's slots, which costs no memory:
   at 1 MiB it is 1.35 to 1.42 times faster than the trees, 1.14 to 1.24 times
   faster than built at the capacity from 256 KiB up, and unchanged below
   (`experiment-2/`, three rotations). The protocol is the model's at a window W
   of the block's size, the model's row being the block; the words, the grants
   and the lane's layout are as accepted. *Undo:* `osoe_shape_choose`'s window
   back to the capacity, which `round_slot` then maps to row k.

## Questions for the author

### 1. The credits: for every piece, or for the pieces a sender waits on

**What the design says** (Part 1's record, which the author accepted, and the
brief's amendment): "the receiver adds a credit at the sender for each piece it
takes ... one counter a lead, cumulative; a late credit only makes a sender
wait longer". And among the brief's stop-and-asks: "a write into a lead's words
or lane it does not wait for".

**What building it found.** Piece j's credit is what lets piece j + W go; a
round of P pieces has nothing after its last W, so their credits are writes
into the sender's credit counter that the sender never waits for. Within a
team's life the model's argument holds - a late credit counts toward the next
round and only makes the sender wait longer. But a team number is given back at
destroy and zeroed for the next team on the word that nothing of the last is
still to arrive (`internal-region.h`; the team redesign's "Why giving a number
back is safe"): a credit still in flight when its sender's node zeroes the word
would count for the new team's first Träff's reduction, whose sender would then
put a piece W slots early, over one its receiver has not taken. The model ran
two reductions in a row on one team and never gave a number back, so it could
not see it.

**Options.**
- (a) **Credit only the pieces the sender waits on**, the first P - W of a round,
  and count the base by them: the sender's last put of a round waits for every
  credit of the round, so none outlives it. W fewer messages a round, and no
  wait added anywhere.
- (b) Credit every piece, and have each lead wait at its reduction's end for
  every credit of its last rounds: the design as written, made safe by a wait of
  about a hop at every call's end.
- (c) As written: safe within a team's life, a slot overwritten in the rare
  case where a credit is still in flight as the team is destroyed and its number
  taken again.

**Recommendation: (a).** *Built meanwhile*, the model first:
`circulant_model.py`'s receivers credit only those pieces, its base counts them,
and an observer checks the property itself - once every lead has returned from
its last collective, no counter or field of any lead's words has moved since that
lead returned (QUIET). The design holds in all six cases (five leads, 14.4
million states against 18.3 million before); the design as written, kept as the
breakage `credit_every_piece`, fails QUIET on two leads. *Undo:* (b) is a wait
after `in_rounds`' last round for the credit counter to reach the base plus every
piece sent; (c) is the breakage.

## Tests, each with the mutation it catches

**`shmem.circulant.*`** (`test-circulant-reduce.c`), the link set to the Pi's so
that the test asks the chooser itself what each call takes: on 2, 3, 4, 5, 6,
7, 8, 9, 12, 16, 17, 33, 64 and 70 logical nodes of one PE, and 8 of two; and
on 5 and 16 under `OSOE_TEST_PERTURB`, 5 under the AM stress settings, 4 at
`OSOE_MAX_TEAMS=4` and `OSOE_MAX_THREADS=1`, 4 and 16 with two threads a PE
reducing two teams at once, and 16 under `OSOE_COLLECTIVE_SHAPE=tree`. Its
phases: a byte sum at 13 sizes from 1,025 bytes to 1 MiB and 13 bytes, either
side of the crossover, each result checked and each call's published algorithm
held to the chooser's - and for Träff's its rounds, no tree children, and at
most the window in flight; every typed reduction of the specification, 142
routines, at the smallest size Träff's takes, of values no order of combining
can round; in place; two Träff's reductions with the PEs entering some
milliseconds apart, in one order and then the other; and Träff's reductions
back to back, no barrier between, with small sums, broadcasts and the trees'
large sums on the world and on a copy of it whose leads are the world's; and
a copy of the world made, given two Träff's reductions and destroyed, three
times, each copy taking the number its last gave back, whose words every lead
must have zeroed. With two threads it also takes one team's reductions in turn
from each thread, so that the lane a lead lends changes from call to call. On
two nodes the chooser takes Träff's where a piece's rounding favours it - 16,385
and 1,048,589 bytes of the ladder, and 68% of all sizes on the Pi's link, never
a multiple of 16 KiB (the adversarial read's finding 2) - and the test holds
every call to it either way.
**`shmem.shape.chosen.*`** now holds every
reduction's algorithm to the grid's line with Träff's allowed, and
**`shmem.shape.back-to-back.*`** counts the world calls that took Träff's and
requires one at least (its 16 nodes take it from 8 KiB).

**The model's breakages as C mutations** of `internode-generic.c`
(`build/scratch/circulant-data/vm/mutate.py`), each against eleven of the tests
- 3, 5, 7, 12 and 8 × 2 nodes, both perturbed cases, the threads on 4, the
stress settings and both back-to-back maps:

| mutation | caught | how |
|---|---|---|
| no credit: any piece of a round may go | 10 of 11 (not the threads on 4, whose rounds are a piece) | a header named piece 4 where piece 0 was due |
| no reduce-scatter grant: no wait for it | 11 of 11 | the grant line "for nothing", or a field naming no lane |
| a stale grant: any count stands for this one | 11 of 11 | a field naming no lane, from the second reduction on |
| a row's allgather grant as its round starts | 11 of 11 | an allgather piece taken as the reduce-scatter's first, whose senders' lane field was not yet put |
| the allgather's grants one a round, by count | 11 of 11 | the same |
| the rows on the tree's arrival counters | 10 of 11 | a count the trees had left read as a piece, its senders' lane field empty |
| the sender's lane after its first piece's add | 11 of 11 | a senders' lane field read before it was put |
| every piece credited | 0 of 11 | no test can see it: its harm needs a credit still in flight as a team is destroyed and its number taken again; the model's QUIET is its evidence |

Each is caught by the library's own checks - a header, a grant's words, a lane
field - which end the job naming the lead that put it, or for a grant's words
the lead that holds them, not by a wrong result. The table was measured at
`4b9767e`, a round's window the capacity's four; the adversarial read ran
twenty mutations of its own at `5cbd47e`'s wider windows
(`docs/reviews/archive/v0.2-step3-adversarial-read.md`, "Mutations").

## The Pi's measurements (Part 3)

`docs/pi-cluster-results.md`, "v0.2 step 3", has them whole; the raw files are
`docs/design/evidence/pi-cluster/2026-10-07-v0.2-step3/`. `link` - Träff's
where the model takes it - against `tree`, step 2's trees, the setting the
brief asked for, on one build, alternating three times: the e-band probe at
4 × 4 and 4 × 1, OSU's reduce, `osoe-bench`.

- **The brief's claim holds as merged**: a 1 MiB reduction 1.32 to 1.41 times
  faster than step 2's trees (the brief: about 1.3), 2 and 4 MiB 1.43 and 1.46;
  no size slower beyond the noise; every size to 8 KiB the trees', unchanged.
  From 16 KiB, the smallest measured size Träff's takes, every size faster -
  1.04 to 1.58 times by OSU and the probe.
- **As Part 2 first built it**, a round in one row at the capacity's window,
  1.14 to 1.19 at 1 MiB: the window held it back, which choice 6 answers
  (`experiment-1/`, `experiment-2/`).
- **`osoe-bench`'s back-to-back loop**: at 4 × 4 its 64 KiB reduction's median
  is the trees' (2.26 against 2.24 ms, within the noise) and its fastest call
  1.17 times faster - reductions called with no barrier between them, which
  neither OSU nor the probe does. Not a size slower; worth a look when the
  progress helpers (step 4) change what a lead's node can carry both ways.
- **The model** against these: the chooser's closed forms put the 1 MiB ratio
  at 1.34 on four nodes, and OSU measured 1.36 at 4 × 1; both closed forms run
  16 to 18% under the measured times at that size. The event simulation's
  Träff's is 11 to 22% optimistic from 256 KiB - it takes a lead's link both
  ways at once at the full rate - so on seven nodes the README cites the
  closed form's 1.7, not the simulation's 2.7, and names it the model's.

## The adversarial read

A fresh agent read the whole step, Part 2 the focus, at `4b9767e` and then at
`5cbd47e` (`docs/reviews/archive/v0.2-step3-adversarial-read.md`), asked for a
piece taken from a slot its receiver did not wait for, a row reused early, a
write into a lead's words or lane after it returns, a counter another collective
adds to, and the C against the model. **Its verdict: the protocol holds** at
both - the C is the model's lead program, the model holds whole and at windows
of 2 and 3, which its own cases never use, and nothing the reviewer ran broke
it: leads lending different lanes, a number reused 30 times, fewer elements than
nodes, windows from 1 to 128, a block holding a lane's release row, perturbation,
divergent counts. Its twenty mutations at `5cbd47e` are its table. What it
found, and what was done:

- **Finding 1**, `shmem.circulant.4.max-teams-4` red at `5cbd47e`: the reuse
  phase split a fifth team while the copy held the fourth number. Fixed in
  `9bc596b`: the copy gives its number back first, and the reuse takes it.
- **Finding 2**, the README's two-node sentence false: on two nodes, where both
  send the same bytes, the cost model takes Träff's at 68% of sizes on the Pi's
  link and the tree at every multiple of 16 KiB, since a tree's cost charges a
  partial last piece whole. The README says so now (`8ce8a20`); the choice
  itself is left to after the merge (`docs/release-checklist.md`).
- **Finding 3**, this record: the verification below; the mutation table
  dated to `4b9767e` and the read's own named; the "naming both leads" line
  corrected - a grant's words name the lead that holds them.
- **Finding 4**, the lanes the grants carry reached only through threads'
  start-up order and timing - every single-threaded lead lends one index, so
  three of the read's mutations (`to_lane_own`, `granted_reversed`,
  `grant_receivers`) passed every single-threaded test. Built now rather than
  after: **`shmem.circulant.{5,7}.lanes`**, where on node i, i helper threads
  hold thread records so that every lead lends a lane of its own, the test
  checking they differ and holding every call to the chooser. All three
  mutations fail both, 3 runs of 3; `number_taken_keeps` fails every
  reuse-bearing case. And the threads mode runs at 1 MiB, where every round has
  more pieces than its window and credits flow.
- **Finding 5**, the reduce-scatter grant's head is the last receiver's to
  land, so a divergence among a lead's receivers can pass it, to be named a
  round later or by the deadlock detector - named every time the reviewer tried.
  Left to after the merge (the checklist): a field per receiver with its epoch
  would name it at once, which the grant line has room for.
- **Finding 6**, fixed before the read ended: no test reused a number (the
  reuse phase), two presets red (the lanes tests under `tree`, the sanitizers'
  cap), the memory figures.
- **Finding 7**, configurations only probes ran - registered now: a ceiling of
  2 on five nodes, whose three rounds take every row of a lane, the release
  row's too (`5.ceiling-2`); a capacity of 1 (`5.capacity-1`); a window of 128
  (`3.window-128`); fewer elements than nodes on a link slow enough that 144 and
  240 bytes take Träff's, one and seven blocks empty (`16.tiny`); and a divergent
  call, which must end by name (`4.diverge`). What no test can see:
  `no_round_in_header`, a header without its round, which only a protocol fault
  would put in the wrong slot.

## Verification

<!-- filled in -->
