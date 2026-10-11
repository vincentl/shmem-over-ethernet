# 2026-10-06: Rabenseifner's reduction, Part 1 (v0.2 step 3)

The first part of v0.2's third step, on `docs/prompts/done/v0.2-step3-rabenseifner-prompt.md`
(the brief): the model and the arithmetic. Its third item - the chooser's new
shape - waits on step 2's chooser; the author's prompt for the night said that
Part 1 depends on nothing in step 2, and items 1 and 2 do not.

*Superseded (2026-10-07):* the author chose to model Träff's circulant
reduce-scatter, and to build it if the model held it (`2026-10-06.md`, items 6
and 7); it held, and replaces the fold for every node count
(`2026-10-07-circulant.md`). The fold's model stays as that comparison's
evidence (`tools/modelcheck/rabenseifner_model.py`); its C, its test and its
chooser's shape stay on the branch `rabenseifner` (rebased since, at `537d705`,
`2665f8e` and `fab0560`), not carried over. Kept as written below.

**Where it stands.** Part 1 built on the branch `rabenseifner`, itself on step
2's `tree-shape`, not merged: the arithmetic, in the model and in C (`5beed12`);
the protocol the second question below proposes, modelled (`5399266`); and the
chooser's new shape, Rabenseifner's cost beside the trees' (`d5f6447`). Waiting:
your word on the two questions, then Part 2, the data path - and the branch
merges after step 2's.

## The arithmetic

`tools/modelcheck/rabenseifner_model.py` (`model.rabenseifner`, 4 seconds) holds
the pairing and the segments in two forms that must agree: the schedule of every
step's messages, written from the brief's description, and one lead's part in one
step from its rank, the node count and the step alone, as
`shmem/src/collective-rabenseifner.c`'s `osoe_rabenseifner_step` computes it.
The fold for the 2r first leads when N = 2ᵖ + r is not a power of two, then p
halving steps and p doubling steps over the 2ᵖ leads that remain, renumbered in
order, then the unfold; a segment is a half-open interval of element indices,
split at a + (b - a) / 2, the lower lead keeping the lower half. Run over
symbolic vectors - each element the multiset of nodes combined into it, a step
reading what each lead held after the step before - for every N from 2 to 70 at
nine lengths (1, 2, 3, N - 1, N, N + 1, 2N + 3, 64 and 1000 elements): every
element of every lead's result is combined from every node exactly once. Its
broken variants fail: a fold lead combining the wrong half, and the unfold sent
before the allgather's last step.

`shmem.rabenseifner.arithmetic` runs the C's steps the same way, with a set of
nodes an element and a node taken twice an error, and holds the C's whole table
to the model's digests (`shmem/tests/rabenseifner-digest.txt`, 612 node counts
and lengths; `model.rabenseifner.digest` holds the file to the model). With the
fold's even lead sending the wrong half, 563 of them differ and the test fails.
Nothing calls the arithmetic yet.

## The protocol, modelled

The design the second question proposes, in `rabenseifner_model.py`'s second
half (`model.rabenseifner`), under the engine's SC interleavings. Each lead's
lane has R rows of W slots; step i moves its pieces into row i mod R of the
receiver's lane, piece j into slot j mod W, each a put and then an add on the
receiver's arrival counter for the row, counted over the team's life and
compared as at least. A sender puts its first piece of step i only once it
holds its partner's grant for step i and this collective; a receiver grants
step i at entry when i < R, and otherwise once it has combined every piece of
step i - R, the row's last use. Within an exchange step a lead puts its piece q
only after combining its partner's piece q - d, d = W / 2; within a one-way step
the receiver adds a credit at the sender for every piece it takes, and the
sender puts piece q only once it holds q - W + 1 of them. Every lead runs two
reductions in a row, a piece an element. Checked: every piece a lead takes is
the one it waits for - this collective's, this step's, this index - and an
unfold's comes from a lead that has finished the allgather (UP); every put lands
in a lane lent at that moment to the collective that puts (LANE); no lead is
stuck, and from every state the run can end.

| case | leads | elements | W | R | states | verdict |
|---|---|---|---|---|---|---|
| two leads, three pieces a half | 2 | 6 | 2 | 2 | 999 | holds, and can always finish |
| three leads, the fold | 3 | 6 | 2 | 2 | 16,463 | holds, and can always finish |
| four leads, a power of two | 4 | 4 | 2 | 2 | 390,789 | holds, and can always finish |
| four leads, three rows | 4 | 4 | 2 | 3 | 427,527 | holds, and can always finish |
| five leads, the fold and two halvings | 5 | 4 | 2 | 2 | 6,949,370 | holds; past the liveness check's bound |

The breakages, each of which must fail and does, with the case it fails in
first: a step's grant sent as the step before its row's
next use starts rather than ends (`row_early`, three leads: a piece of the
later step taken in place of the earlier's); no grant (two leads: a put into a
lane not yet lent); a stale grant, any grant the step ever had (two leads: a put
into the partner's lane while it is still in the first collective); the unfold
before the allgather's last step (three leads); an exchange without the lag
(two leads: a slot overwritten before its piece was combined); and a one-way
step without its credits (three leads, the fold back).

What the model does not cover: the progress thread's delivery (puts and adds
land in order on one connection, as the AM layer's models check); the combine's
arithmetic (the first half's); and a team whose collectives move between a
node's threads, which `lane_model.py` checks for the lanes they share.

## The chooser's new shape

`shape_timing.py` gains Rabenseifner's cost in closed form - its steps one
after another, each a hop, a message cost, then its pieces at the pace of the
link or of its flow (an exchange's lag of half the window, a one-way step's
credits of the window), each step's half from the steps' shape - and a
simulation of the steps on the same links as the trees (one `Network` class
now serves both). Against the simulation the closed form errs by a median of 1%
and at most 11% on the fitted links, 25% on the other link model. The chooser
takes the reduce-scatter for a reduction where it costs less than the best
tree, ties to the tree, with a window of 2 or more (half of a window of one is
no lag). `collective-shape.c` mirrors it (`osoe_shape_rabenseifner_cost`, and
`osoe_shape_choose`'s `reduce_scatter`); the grid holds the C to the model on
all 3,024 inputs, 512 of which now choose the reduce-scatter, and
`shmem.shape.choice` pins it.

**What it chooses on the Pi's four nodes**: the star to 4 KiB, the
reduce-scatter from 8 KiB - at 64 KiB 1.2 ms against the k = 2 tree's 1.9, at
1 MiB 13.9 ms against 18.9. The brief pinned "64 KiB the tree", but its own
arithmetic - 1.5n over B and four hops against 2n over B and two - crosses near
40 KB, below 64 KiB; the model, which also counts each step's message cost and
the pieces' pace, crosses lower still. Pinned instead: 2 KiB keeps the tree,
1 MiB takes the reduce-scatter. The Pi's measurement in Part 3 is what says
where the crossover is.

**What it cannot settle**: where a tree has leads with both a parent and
children - seven nodes and more - the two link models part by up to 40% on the
tree's time, and the choice between the tree and the reduce-scatter on 7 to 13
nodes at 1 MiB with them: choosing by the closed forms costs up to 38% against
the 'sender' links' best and 25% against the 'connection' links'. On four nodes
both agree. Only a cluster of more nodes can say which links the hardware has;
the record of step 2 has the same caveat for the trees alone.

## Questions for the author

### 1. The fold puts up to 3.5 vectors on a fold lead's link, not under two

**What the brief says.** "Bytes a lead sends: n(1 - 1/2ᵖ) in the reduce-scatter,
the same in the allgather, plus the fold's n and n / 2 for the leads in it:
under 2n on every link, against the tree's (k + 1) n on an interior node's
link."

**What the arithmetic gives.** The sum does not stay under 2n. An even lead of
the fold sends n/2 in the fold's exchange, n(1 - 1/2ᵖ) twice, and the whole
result, n, in the unfold; it receives n/2, then n/2 in the fold back, then the
same n(1 - 1/2ᵖ) twice. Counted from the exhaustive run, the busiest lead's
link, in vectors sent / received:

| N | 4 | 8 | 16 | 7 | 33 | 70 |
|---|---|---|---|---|---|---|
| busiest lead | 1.50 / 1.50 | 1.75 / 1.75 | 1.88 / 1.88 | 3.00 / 2.50 | 3.44 / 2.94 | 3.47 / 2.97 |

Under 2n for a power of two, as the brief says; for every other N the fold's
even leads carry about 3.5n out, which is the k = 2 tree's interior node (3n)
or worse, and with more hops (2p + 3 against 2⌈log₂ N⌉). So for N not a power
of two the algorithm as briefed is no better in bandwidth than the binary tree
it is meant to beat.

**Options.**
- (a) Build it as briefed: the chooser counts its true cost and takes it where
  it wins, which is at the powers of two, mostly; the README claims only that.
- (b) A scheme for other N that keeps every link near 2n: Rabenseifner and
  Träff, "More efficient reduction algorithms for non-power-of-two numbers of
  processors" (EuroPVM/MPI 2004) - their 3-2 and 2-1 elimination steps, which
  spread the remainder's work instead of giving it to r pairs. Not in
  `corpus/`; it would need reading before a design.
- (c) Spread the unfold: each odd fold lead takes its result from every lead's
  final segment rather than from its even partner alone - r/2ᵖ n more for
  each of those, and the even leads drop to about 2.5n: better, still over 2n.

**Recommendation: (a) now, (b) as the next design question**: the model and the
arithmetic are the same either way up to the fold, the chooser keeps the
algorithm honest, and the four-node Pi - a power of two - shows the gain the
brief promises there. **Built meanwhile:** the arithmetic as briefed.

### 2. Flow control within a step: the brief's rule covers rows between steps only

**What the brief says.** "A row is reused only when its earlier step's pieces
have all been combined ... the grant for step i is sent when step i - rows has
finished." That keeps one step out of another's row. Within one step, though,
a half of n bytes streams P pieces through the row's W slots - piece j into
slot j mod W - so a sender must know its partner has combined piece j - W before
it puts piece j. The tree has the releases for this (a node sends piece k up
only once it holds release k - D) and the broadcast's empty credits; the brief
gives Rabenseifner's steps nothing.

**The design proposed**, for the model to check before any code:
- **An exchange step** (the fold's, every halving and doubling step) needs no
  message of its own. Each lead follows one rule: it puts its piece q into its
  partner's row only after it has combined the partner's piece q - d, where
  d = ⌊W/2⌋ and W ≥ 2 (pieces 0 to d - 1 go at once). Then a piece from the
  partner certifies how far the partner has combined: the partner's piece
  q - W + d arriving means it has combined my piece q - W, whose slot my piece q
  takes, and since 2d ≤ W, the piece q - d my own rule waits for comes after it.
  No deadlock: the partner's piece q - d waited only for my piece q - 2d.
  The halves of one step differ by one element at most (the split at
  a + (b - a) / 2, the same on both sides), so their piece counts differ by one
  at most, and the rule holds to the last piece.
- **A one-way step** (the fold back, the unfold) has no reverse traffic: the
  receiver adds one to a credit counter at the sender as it combines or copies
  each piece, as a broadcast's child sends its empty credits up today, and the
  sender puts piece q only once the counter shows q - W + 1.
- **The window**: with d = ⌊W/2⌋, an exchange keeps d pieces in flight each
  way, so the link stays busy while d piece times cover a hop and a piece -
  d ≥ 1 + h/t: on the Pi, at 16 KiB pieces, d = 2, so W = 4, the capacity step 2
  proposes. The model counts the cost of d; `shape_timing.py`'s term takes it.

**Recommendation:** this design. **Checked meanwhile:** the model above holds it
on two to five leads, and each of its rules dropped - the lag, the credits - fails,
beside the brief's own breakages. Nothing is on the data path; Part 2 waits for
your word on the design.