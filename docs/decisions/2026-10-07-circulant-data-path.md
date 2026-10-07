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
  own; piece j into row k of the receiver's lane at slot j mod the capacity, a
  put of its header and bytes and a fenced add on the row's counter; a
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

<!-- filled in from the mutant run -->

## Verification

<!-- filled in -->
