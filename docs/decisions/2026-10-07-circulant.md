# 2026-10-07: Träff's circulant reduce-scatter (v0.2 step 3)

The third step of v0.2 (`docs/prompts/v0.2-step3-rabenseifner-prompt.md`, the
brief), on the author's answers of 2026-10-06 (`2026-10-06.md`): item 6, "model
Träff's and then implement if happy with model", for node counts that are not
powers of two; item 7, flow within a step as recommended, "and credits
throughout if the reduce-scatter is the circulant one, which has no swaps".
Part 1 - the fold the brief described, modelled and costed - is
`2026-10-06-rabenseifner.md`.

**Where it stands.** On the branch `circulant`, on `main`, not merged:
Träff's algorithm modelled - its arithmetic for every node count from 2 to 70,
its protocol on two to five leads - and the model holds it; its arithmetic in
C, held to the model; its cost beside the trees', in the chooser where the
caller allows it, which the data path does not yet. The data path waits on
question 1: the protocol the model holds needs words of its own in the
internal region, and grants of another form than the brief's - both on the
brief's stop-and-ask list.

## Why Träff's, and for every node count

**The fold's cost** (Part 1's question 1): for a node count that is not a power
of two, the fold puts up to 3.5 vectors on a fold lead's link - no better than
the k = 2 tree's interior lead. Träff's circulant reduce-scatter (J. L. Träff,
"Optimal, Non-pipelined Reduce-scatter and Allreduce Algorithms", arXiv
2410.14234, 2024, Algorithms 1 and 2) has no fold: the skips halve N rounding
up - 11, 6, 3, 2, 1 for 22 leads - and in each of ceil(log2 N) rounds a half,
every lead sends to one lead and receives from another, N - 1 blocks a half on
every lead, for every N.

| N | 3 | 5 | 7 | 12 | 33 | 70 | 4, 8, 16, 64 |
|---|---|---|---|---|---|---|---|
| busiest lead's link, vectors out (circulant) | 1.33 | 1.60 | 1.71 | 1.83 | 1.94 | 1.97 | the same as the fold's |
| the fold's | 2.50 | 3.00 | 3.00 | 3.25 | 3.44 | 3.47 | 1.50, 1.75, 1.88, 1.97 |

**Its time**, simulated on the Pi's link (86 µs, 7 µs, 116 bytes a µs; the
window 4) against the fold's simulation and the best tree's, 1 MiB:

| N | 3 | 4 | 5 | 7 | 8 | 12 | 16 |
|---|---|---|---|---|---|---|---|
| circulant, ms | 12.7 | 14.2 | 15.1 | 16.4 | 16.4 | 17.5 | 17.8 |
| the fold | 27.8 | 13.9 | 32.7 | 32.6 | 16.4 | 34.3 | 16.7 |
| the best tree | 18.6 | 23.9 | 29.3 | 37.2 | 38.7 | 44.6 | 49.7 |

At a power of two it is the fold's halving in its volume and its rounds; it
costs about a hop more than the fold's simulation at 16 to 64 KiB, which takes
the fold's grants as held, where Träff's counts its own. So it replaces the
fold for every node count, one algorithm and one protocol: the fold's model
stays as the evidence of the comparison (`rabenseifner_model.py`, marked not
built), and its C - on the branch `rabenseifner` - is not carried over.

## The arithmetic

`tools/modelcheck/circulant_model.py` (`model.circulant`): the rounds over
symbolic vectors - each element the multiset of the nodes combined into it -
for every N from 2 to 70 at the fold model's nine lengths: every element of
every lead's result combined from every node exactly once, every lead sending
N - 1 blocks a half, 2 ceil(log2 N) rounds. In two forms that must agree: the
rounds written out element by element, and `round_intervals()`, one lead's part
in one round as `collective-circulant.c`'s `osoe_circulant_round` computes it -
its partners, its row, whether it combines, and what it sends and receives as
at most two element intervals, the same intervals on both sides of a round,
since the blocks a lead sends are the ones its receiver combines into or copies.
`shmem.circulant.arithmetic` runs the C over the same symbolic vectors and holds
its whole table to the model's digests (612). The broken variants fail: a round
sent to the wrong lead, and the allgather's skips in the reduce-scatter's order
(599 of the 612 cases with a wrong element, 606 digests differing, in the C).

## The protocol, modelled

The model's second half, under the engine's SC interleavings:

- **Rows and pieces.** Round k of either half moves its pieces into row k of the
  receiver's lane, piece j into slot j mod W, each a put and then an add on the
  receiver's arrival counter for the row, cumulative and compared as at least.
  A reduction takes ceil(log2 N) rows, so the chooser takes it only where they
  fit a lane's ceiling + 1 - 512 nodes at the default ceiling of 8.
- **Credits throughout** (the author's item 7): a round sends to one lead and
  receives from another, so no piece says how far its sender's receiver has
  combined; the receiver adds a credit at the sender for each piece it takes, and
  the sender puts piece j only once it holds j - W + 1 of the round's credits.
  One counter a lead, cumulative over its rounds: a credit can only be for a
  piece already put, so a late one makes the sender wait longer, never less.
- **The reduce-scatter's grants.** Lead r's receivers in the reduce-scatter,
  r + s_k, are as many leads as there are rounds, each lending its own lane, so
  each grant carries a lane of its own - into field k of r's grant line - and
  one counter cannot say which of them has come. Every lead grants each of its
  senders as it enters, and starts its reduce-scatter once its count shows all
  of its own.
- **The allgather's grants.** The allgather's round k comes from r + s_k into
  r's row k, which r's reduce-scatter round k filled; r grants that sender as
  its own round k ends - a count, nothing more - and a sender starts its
  allgather once it holds all of its allgather grants, again because one
  counter cannot say which.
- **The allgather's lanes travel with the reduce-scatter.** Lead r's allgather
  round k sends to r - s_k, the lead that sent to r in reduce-scatter round k;
  that sender put its own lane into the field for row k of r's senders' lanes
  line before its first piece's add, and r read it as it took that piece - the
  row's counter is what says the field is written. Every put lands outside the
  counters a lead watches: the grant line's lanes past its counter, the
  senders' lanes on a line of their own.
- **Counters of their own.** The allgather ends with no step every lead waits
  for: a lead whose last pieces have come leaves while another still waits for
  its own, and can start the team's next collective - a small reduction, whose
  child adds to its parent's arrival counter as it enters, or a broadcast,
  whose children send their empty credits at once. So every counter the
  reduce-scatter waits on is its own, which no other collective adds to; the
  safety arguments for its grant counters lean on one fact the arithmetic
  shows: every lead's result needs every lead's reduce-scatter, so no lead
  leaves before every lead has finished it.

**Checked**: every piece a lead takes is the one it waits for - this
collective's, this half's, this round's, this index (UP); every lane a lead
reads in its senders' lanes line is the one its sender put there for this
collective and round (SENDER); every put lands in a lane lent, at that moment,
to the collective that puts (LANE); no lead is stuck; from every state the run
can end.

| case | leads | elements | W | states | verdict |
|---|---|---|---|---|---|
| a reduction twice | 2 | 4 | 1 | 1,367 | holds, and can always finish |
| a reduction twice | 3 | 6 | 1 | 55,939 | holds, and can always finish |
| a reduction, then a small tree reduction | 3 | 6 | 1 | 27,831 | holds, and can always finish |
| a small tree reduction, then a reduction | 3 | 6 | 1 | 27,127 | holds, and can always finish |
| a reduction twice | 4 | 4 | 1 | 790,023 | holds, and can always finish |
| a reduction twice | 5 | 5 | 1 | 18,292,440 | holds; past the liveness check's bound |

**The breakages**, each failing in the case named: no credit (two leads: a slot
overwritten before it was taken); no reduce-scatter grant (two leads: a put
into a lane not yet lent); a stale grant, any grant ever taken for this one
(two leads); a row's allgather grant as its round starts rather than ends (two
leads: an allgather piece where a reduce-scatter piece was waited for); the
allgather's grants waited for one a round by count (three leads: a later
round's grant counted for an earlier one, and a piece into a row still in
use); the counters shared with the tree's (three leads, a small reduction
after the reduction: a child's early add counted as a piece); and a sender's
lane put after its first piece's add (two leads: the receiver takes the first
piece and finds no lane in the field).

**What it found about the fold's design** (Part 1): the brief's one grant line
a team number cannot hold the grants of several partners at once - the
halving's partners differ from step to step, and the first R steps' grants all
arrive at entry - and its counters, the tree's, take the early adds of a lead
that has finished the recursive doubling and started a small reduction. Part
1's model counted a grant per step and ran two reductions in a row only, so it
saw neither. Both apply to any reduce-scatter that ends without a step every
lead waits for.

## The cost, and the chooser

`shape_timing.py`: Träff's reduction in closed form - the grants, a message each
and a hop, before the reduce-scatter and between the halves; then each round a
hop and a message cost and its pieces at the pace of the link, of the lead's own
messages, a piece and a credit each, or of its credits, the first W at once -
and simulated event by event on the same links as the trees (one `Network`
class, taken from the fold's branch, under both; the trees' fit and grid
unchanged by it). The closed form errs against the simulation by a median of 3%
and at most 10% on the fitted links, 16% on the others; choosing by it between
Träff's and the trees costs at most 5% against either link model's best.

`collective-shape.c`'s `osoe_shape_circulant_cost` is the same arithmetic in
integers, and `osoe_shape_choose` takes Träff's for a reduction past the small
class where it costs less than the best tree, ties to the tree, when its caller
allows it and its rows fit - and the data path does not allow it yet, so
nothing it runs changes. The grid holds the C to the model on 4,536 inputs, a
reduction both ways, 432 of them taking Träff's; with each round counted once
in the C, 634 differ. `shmem.shape.choice` pins it: on the Pi's four nodes a
1 MiB reduction takes it; a 2 KiB one, a broadcast, a caller that does not
allow it, and 16 nodes under a ceiling of 2, whose lanes hold three rows, do
not.

## Questions for the author

### 1. The reduce-scatter's words, and its grants' form

**What the brief says.** "The grant carries, as today, the epoch, the identity,
the bytes and the lane; it gains the step"; "a step's arrival counter is the
row's, cumulative as the tree's are, with the base per row". And, to stop and
ask: "Any change to the grant beyond the step field, or to the lane's layout."

**What the model found.** Neither holds for a reduce-scatter whose partners
differ from round to round, Träff's or the fold:
- a lead's one grant line takes one grant's words at a time, and the first
  rounds' receivers all grant at once, each with its own lane;
- the tree's counters take a finished lead's early adds - a small reduction's
  child, a broadcast's empty credits - while another lead still waits on them
  for its last pieces.

**The design the model holds** (above), which leaves the lane's layout and the
tree's grant as they are and adds, per team number, the reduce-scatter's own
words, sized like the gather's by the job's node count, in this order so that
every put lands outside the counters a lead watches: an arrival counter per
row, ceil(log2 N) of them; a credit counter; the allgather's grant counter; the
reduce-scatter's grant line, past whose counter each receiver puts its lane
into the field for its round, with the collective's epoch, identity and bytes;
and a line of the senders' lanes - ceil(log2 N) + 4 lines of 128 bytes, 11 at
the 70 nodes the VM runs, about 1.4 KiB a team number, 352 KiB a PE at 256
numbers (the region is 15.5 MiB at the defaults).

**Options.**
- (a) This design: its own words, the reduce-scatter's grants a lane a round
  and the allgather's a count, each waited for whole, the allgather's lanes
  carried by the reduce-scatter's first pieces.
- (b) The tree's counters, with a step at the reduction's end that every lead
  waits for, so that nobody leaves early: about a hop a call, and the grants'
  lanes still need fields of their own.
- (c) The fold rather than Träff's: the same two problems.

**Recommendation: (a).** *Built meanwhile:* the model, the arithmetic and the
chooser's shape; the data path waits for your word on the words and the grants.
**Answered** (the author, 2026-10-07, `2026-10-07.md` item 2): "accept
recommendation" - (a). The data path is the step's next part.

## Verification

The branch at its third commit, on the host: `circulant_model.py` whole - the
arithmetic, the six protocol cases and the seven breakages (about six minutes,
the five-lead case most of it) - and the digests; `shape_timing.py`'s check and
its grid; `test-circulant.c` and `test-collective-shape.c` built with
`-Wall -Wextra -Wconversion -Wshadow`, no warning, each against the mutation
above; `circulant_model.py` whole again after the allgather's lanes moved to the
reduce-scatter's first pieces (eight minutes, every case holding and the seven
breakages failing). In `osoe-load`, the branch at `14ad0c5` - the C as it stands, before the
model's last change, which touched the model alone: the four presets with the floor pins, debug
785 of 785, release 785, asan 782, tsan 546, and `tier3` 113 - step 2's suite and
`shmem.circulant.arithmetic`. And rebased onto `main` at step 2's merge
(`2489481`), in `osoe`: debug 787, release 787, asan 782, tsan 546, `tier3` 113,
with the model's check, its grid and the two C tests again on the host.
