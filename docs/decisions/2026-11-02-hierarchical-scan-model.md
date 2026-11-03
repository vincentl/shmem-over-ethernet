# 2026-11-02: a hierarchical prefix scan, modelled and timed in hops

## The brief

The author's answer 6 of the morning (`2026-11-01.md`): the four design changes
the large-cluster playbook names are modelled here before any cluster session.
This is C9, the hierarchical scan; the author's words: "Start C9". C5 and C8
were done the same day (`2026-11-02-intranode-combine-model.md`,
`2026-11-02-two-level-alltoall-model.md`).

## What was made

On the branch `intranode-combine-model`, after C8's commit `8f6737f8`:

- **The proposal**, `docs/design/hierarchical-scan-proposal.md`: the problem (a
  scan over a pre-order tree of the team's ranks at fan-out 4, 12.7 network
  hops at 512 nodes × 256 PEs under a block layout where a reduction's tree is
  6; on the Pi at 4 × 4 an 8-byte scan 377 µs against a reduction's 202), the
  design (where every node's members are one run of the team's ranks: the
  members arrive as a reduction's do, the lead sums its members' sources and
  scans the nodes' totals with the other leads over a pre-order tree of the
  runs in rank order - `collect`'s leads' scan, indexed by run instead of by
  the node set's position - then writes every member's prefix from `before`
  and releases; small scans through the reductions' small rows, large ones
  through the lead's lane by the member scan's pipeline rules; the member
  scan kept for cyclic and plane layouts), what changes (a scan spanning
  nodes takes an epoch, and P24's check with it), the costs, the alternatives
  weighed and four questions for the author.
- **The model**, `tools/modelcheck/hierarchical_scan_model.py`, registered as
  `model.hierarchical_scan`: N nodes of L members at fan-out 2, a source a
  distinct bit per (epoch, rank); SOURCE, SLOT, VALUE and RESULT; sixteen
  cases of the design - two to four nodes, one to three members, one or two
  epochs, in place and separate, inclusive and exclusive, the runs in order,
  reversed and permuted - under SC and x86-TSO, every one holding with the
  engine's liveness check, the largest 739,612 states; six breakages, each
  failing the property it is named for - the lead reading before every arrival
  (SOURCE), the release before the prefixes, a member's read before the
  release, a down without the totals of the children before it and the tree
  over the node set's positions under a permuted layout (RESULT), and the
  root's down to a child before that child's own total (SLOT). 32 cases in 38
  seconds on the Mac.
- **The timing**, `tools/modelcheck/scan_timing.py` extended with the
  hierarchical design beside the members' and the runs' trees, and a `check`
  registered as `model.scan_timing`: under a block layout the hierarchical
  design faster at every node count from 4 and every PEs-a-node count from 4,
  1.35 times or better from 64 nodes, 12.66 → 7.70 hops at 512 × 256; the
  cyclic layout's 45 hops by the members' tree, where the design does not
  apply, pinned.
- The indexes: `docs/design/README.md`, `tools/modelcheck/README.md` (its
  `scan_model.py` row and a new one, and the count), the README's count of
  models (51), the playbook's C9 and §5 row - the "13 become 6" there were the
  trees' depths alone; with the lead's intranode terms at 256 members the
  model says 12.7 to 7.7.

## What the model settled

Two things the proposal could not have claimed without it. The design needs
no `copied` counter: the lead writes a member's `dest` for the next epoch
only after that member arrived at it, which it does only after reading this
one's result - the two-epoch in-place cases hold without one, where C5's
sliced combine needed `combined` because there the members write the lead's
`dest`. And the leads' slots need nothing beyond the rule as built, that a
parent writes a child's down only after the child's total arrived: the
breakage that drops it, the root sending child j its down as soon as the
totals before it are in, lets a root a whole scan ahead write a down slot its
child has not read, and SLOT catches it.

## What the timing taught

The playbook's "about 13 hops become about 6" counted the trees' depths; the
members' tree already keeps its lowest levels within a node under a block
layout, and the hierarchical design pays the lead's sum and prefixes over its
members, a hop at 256 of them. The honest numbers are 12.7 to 7.7 at 512 ×
256, 1.4 to 1.9 times from 64 nodes; and the primitive the playbook leaned on
scans one 8-byte word, so a scan of a vector takes the member scan's pieces
over the leads' tree, which the proposal says.

## Checked

The model's 32 cases on the Mac, every case as expected; `scan_timing.py
check` on the Mac, 26 pins; both registered tests in the VM's release
configure of this tree, 2 of 2 passed. *Not run*: the suite and the static
checks - nothing of the library changed.

## Left, and for the author

Nothing is built. The proposal's §6 puts four questions: whether to build it
(*recommended*: only if the ladder's scan row or a program of the author's
makes scans matter - of the four changes it is the least by use, since no
corpus program calls a scan); both classes or the small one (*recommended*:
both); the epoch a scan would take (*recommended*: yes, P24's check comes with
it); and C5's slices for the lead's two passes (*recommended*: C5 on the
reduction first). Last of the four models: C17, the control tree.

*2026-11-03*: every recommendation taken by the author
([2026-11-03](2026-11-03.md)).
