# 2026-11-02: the small collectives' cost model

## The brief

The author's answer 15 of the morning (`2026-11-01.md`): a small-collective
cost model is written here before any cluster session - hops and message costs
for the tree at each fan-out, the dissemination barrier, the binomial tree and
recursive doubling - so that the ladder's barrier and small-reduction times have
a prediction beside them. This is that model, written by a session the lead
started under that answer, in `shape_timing.py`'s pattern; the brief named what
done looks like, and every item of it is here. No change to the C library.

## What was made

On the branch `claude/upbeat-fermi-9c9109`, from `main` at `e1be26bc`:

- **The model**, `tools/modelcheck/small_collective_timing.py`, registered as
  `model.small_collective_timing` (labels `tier1;model`, timeout 600 s; about
  a second on the Mac). The inputs are the hop *h*, the message cost *o*, a
  lead's wake *w* for the data path and an intranode cost a PE; the shapes
  the level-order k-ary tree as built at every fan-out from 2 to 32, a
  binomial tree, a dissemination barrier, recursive doubling and a broadcast
  without its up pass; each a closed form and an event simulation of a loop
  of calls, in two measures - a loop's period, which `osoe-bench` reports,
  and one call's time - agreeing to the microsecond on every grid the test
  runs. The simulation alone adds a receiver's cost a message (`disagree`).
- **The fit**, in the test (FIT): the tree at k = 8 against the records'
  barriers on four Pi nodes and on two and eight cloud nodes, every row
  within the sessions' spread, the 8-byte reduce and broadcast as their gap
  over the barrier. The constants and where each comes from are the model's
  head comment; `fit` re-derives them.
- **The crossover tables** (`crossover`): every shape at 8, 64, 512, 4,096 and
  32,768 nodes on the Pi's and the cloud's fitted links, the Pi at the shape
  fit's message cost, and the brief's two guessed links; and the ratio of
  message cost to hop at which each alternative beats the tree at 8.
- **Four planted errors**, each failing the check written for it: a parent's
  sends counted as one message cost a level (TERMS), the binomial's sends
  smallest subtree first (PINS), the cloud's message cost the trimmed mean's
  23 µs (FIT), no intranode cost a PE (FIT).
- **The note**, `docs/design/small-collective-model.md`: the model, its
  inputs, the fitted table, the crossover tables and what it does not model.
  A row in `tools/modelcheck/README.md`; the playbook's §5 "Decided by" cells
  for the barrier and the small reductions and broadcasts name the model and
  what it says, its C16 and its phase B row carry the model's line; the
  indexes; the README's count of models, 48.

## What it found

1. **The tree at 8 stands on both measured links below 512 nodes.** At 8 and
   64 nodes it is a star and a two-level tree; the dissemination costs a
   third more, the binomial twice. From 585 nodes a wider tree saves a level:
   at 4,096 nodes 16 is three levels where 8 is four, 374 against 430 µs on
   the cloud's fitted link, 13%; the best fan-out is the widest that saves a
   level. At 512 nodes 16 pays eight sends more a level for nothing.
2. **The dissemination and recursive doubling win only from a message cost
   about a fifth of the hop** - R(h + o) against about (2/3)R h + 3R o for the
   tree at 8, so o/h > 1/6 - at every node count. The fitted ratios are 0.012
   on the Pi and 0.047 on the cloud. The binomial tree never pays below 1.5
   hops: its root's sends are serial too.
3. **The barriers the sessions measured imply a message cost of about 2 µs on
   the cloud**, where `link-measure.c`'s trimmed mean read 23 to 45 (the third
   cluster session) and P38's least-less-least 0 to 36. A RELEASE to seven
   children leaving 23 µs apart would make the 8 × 8 barrier 250 µs; it
   measured 99 to 132. The playbook's C16 reasoned from the trimmed mean that
   a fan-out of 8 "pays as much in its parent's sends as in its hop"; by the
   barriers it pays a twentieth. If the trimmed mean were right, the
   dissemination would be a third faster than the tree at every rung (the
   `loaded` link's rows); so the ladder's first barrier rows measure the
   message cost itself: near the model's line - 222 µs at 64 nodes and 8 PEs
   a node - and the tree stands; far above it and the dissemination is worth
   building.
4. **A first closed form of recursive doubling past a power of two was
   wrong**, charging two whole rounds for the extra leads; the simulation
   showed a lead with no extra sending its first round at once, the extras'
   rounds overlapping the core's. The closed form is now the per-lead
   recursion and agrees. The playbook's row says "costs a round at
   non-powers of two"; the truth is less than two rounds and more than none.
5. **A broadcast without its up pass is 0.55 to 0.6 of one call's time** and
   has no period of its own in a loop; the gain is for spaced broadcasts and
   needs a slot-ownership protocol.

## Not modelled

The fabric's topology; contention at the root beyond its serial sends - the
simulation shows a receiver's cost equal to the message cost adding 4 to 5% to
the tree and three times it 38%, where the dissemination takes 13%; the idle
agents' wakes; the intranode phase past the cores; the link's bandwidth, the
helpers, the control wire's buffer; the small class's 1 KiB end, which
`shape_timing.py`'s closed form prices. The note's last section says each.

## For the author

*Answered 2026-11-04*: the author took every recommendation ([2026-11-04](2026-11-04.md)).

1. **Should the chooser shape the small class from the link too** - the fan-out
   from this model's closed form, as the large class's comes from
   `shape_timing.py`'s? *What the model says*: nothing to gain below 585
   nodes on either measured link; a level, 13%, at 4,096. *Recommended*: not
   before the ladder's top rungs time `OSOE_COLLECTIVE_FANOUT=16` against 8,
   which is the same change as a setting (C16's first step); if 16 wins there
   by the model's margin, the chooser's small-class rule is a few lines,
   `k = the widest fan-out to 32 that does not deepen the tree`, and a
   setting's agreement at the rendezvous as today.
2. **The message cost's two readings.** The barriers say 2 µs; the data path's
   fan says 23 to 45, and `collective-shape.c` takes Träff's by the latter,
   which the third session found it should have taken more often (P37's
   round with a cost of 21 µs ran Träff's 1.4 to 2.6 times faster).
   *Recommended*: on the cluster, read the barrier beside the start-up's
   message cost in the same job; if the barrier's implied cost stays an order
   of magnitude under the fan's, the fan's measurement is measuring the idle
   wakes P38 named, and the chooser is being told a cost the collectives do
   not pay. A question for the ladder, not a change now.
3. **Build the dissemination barrier?** *Recommended*: no, unless the ladder's
   barrier sits far above the model's line and the receiver's cost (the
   `disagree` table) does not explain it; then it is the cheaper barrier at
   every rung, and a control-wire protocol of its own.

## To undo

Delete `tools/modelcheck/small_collective_timing.py`, its `add_test` block in
`tools/modelcheck/CMakeLists.txt`, its row in `tools/modelcheck/README.md` and
the note `docs/design/small-collective-model.md`; put the playbook's two "Decided
by" cells back to "`OSOE_COLLECTIVE_FANOUT=16` timed against 8 at the top
rungs; a small-message model with both costs" and "the same model, with this
cluster's hop and message cost; the up pass a protocol"; the counts of models
back to 47. Nothing in the library was changed.
