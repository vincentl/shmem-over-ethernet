# 2026-11-02: a control tree, its abort and detector modelled, its costs

## The brief

The author's answer 6 of the morning (`2026-11-01.md`): the four design
changes the large-cluster playbook names are modelled here before any cluster
session. This is C17, the control tree, the last of the four; the author's
words: "Start C17". C5, C8 and C9 were done the same day
(`2026-11-02-intranode-combine-model.md`,
`2026-11-02-two-level-alltoall-model.md`,
`2026-11-02-hierarchical-scan-model.md`).

## What was made

On the branch `intranode-combine-model`, after C9's commit `ce051ebb`:

- **The proposal**, `docs/design/control-tree-proposal.md`: the problem (a
  control wire to every node, 8,190 sockets a node and 16.8 million flows at
  4,096 nodes; the detector a star from node 0 and an abort's goodbyes a star
  from the aborting lead, each O(n) serial sends at one lead; what assumes a
  wire to every node - every tree's edges, the detector, the data-wire
  request, the abort's outranked rule and the carrier's wait, a death's
  relay by the mesh, the close, the silent-peer check), the design (the
  control wires of the collectives' trees alone: the world team's ceiling
  tree eager, k + 1 a lead, the rendezvous's own connections where the
  ceiling is 8; every other tree's edge made as a call first needs it by the
  schedule's caller, both ends knowing the edge; the detector's probes down
  the tree and its answers ANDed up it; an abort as a notice a node relays,
  a second allowed on a wire after the goodbye, and the ends gathering toward
  the lowest caller's node, which leaves last; the data-wire request routed
  over the tree; the goodbye still every data wire's last segment, so that
  the status is never lost to an end read first, 2026-09-23's item 12 kept),
  what does not change, the costs, the alternatives weighed and four
  questions for the author.
- **The abort's model**, `tools/modelcheck/control_tree_abort_model.py`,
  registered as `model.control_tree_abort`: every node its lead, streams per
  directed edge in order; a chain and a star of three and a tree of four; one
  to three callers and a death; LAST, STATUS, STUCK and LIVE, sixteen cases
  holding; six breakages each failing the property it is named for - no wait
  for the far side as a non-carrier node waits today (LAST), the first word
  kept, nothing back to a neighbour whose notice arrived, every neighbour
  waited for, one notice a wire as a goodbye is today (each STUCK), and
  today's rules together on a tree (LAST). Under a second.
- **The detector's model**, `deadlock_model.py`'s `tree` design, under
  `model.deadlock`: node 1 the parent of node 2, the probe passed on at the
  first half and the answer ANDed with the child's; the eight scenarios
  holding with liveness, 180,000 states the largest; `tree_no_wait`, the
  relay answering for itself alone, failing SAFETY.
- **The costs**, `tools/modelcheck/control_tree_timing.py`, registered as
  `model.control_tree_timing`: sockets a node and flows a job, the detector's
  collection and an abort's spread by the star and by the tree at the
  cloud's 40 µs hop and 35 µs message, the ends' gathering, and the control
  wires' start-up by `mesh_timing.py`'s schedule against the tree's; `check`
  pins twelve of them; `table` and `sensitivity` print them.
- The indexes: `docs/design/README.md`, `tools/modelcheck/README.md` (two
  rows and the count), the README's count of models (53), the playbook's C17
  and its §5 "Start-up" row.

## What the model settled

Three things the proposal could not have claimed without it. The direction
rule is enough for the ends: a node ends once every neighbour but the one its
word came from has ended, and no node needs the job's node count, so the
carrier's wait - today a look at every wire - becomes a look at k + 1. A node
need not know the lowest caller to end rightly, only a caller lower than its
own; so one notice a wire, today's goodbye, is wrong only where a node told a
neighbour of its own caller before a lower one arrived from a third side,
which takes three callers and an interior lead (`tree_crossing`) - the
design allows a second notice for that case alone. And a notice travels every
path before an end does, since a node tells before it ends and waits for
every neighbour but one, so the carrier's node needs no view beyond its
neighbours.

## What the costs taught

The star's cost is its serial sends, not its one hop: at 4,096 nodes a
collection is 287 ms at the root against 2.6 ms down and up the tree, and an
abort is heard everywhere in 1.6 ms against 143, the ends gathering in 0.3 ms
more - about a hundred times at every message cost from 5 to 45 µs. The star
wins below 64 nodes, by a tenth of a millisecond. The control wires' start-up
is mostly the rendezvous's hand-out either way, 0.164 s of the schedule's
0.37 and the tree's 0.165 at 4,097 nodes.

## Checked

The abort model's 22 cases on the Mac, every case as expected; the deadlock
model's nine tree cases, 11 s; `control_tree_timing.py check`, twelve pins;
in the VM's release configure of this tree `model.control_tree_abort`,
`model.control_tree_timing`, `model.deadlock`, `shmem.license-lines` and
`shmem.settings-documented`, 5 of 5 passed, 28 s. *Not run*: the suite and
the static checks - nothing of the library changed.

## Left, and for the author

Nothing is built. The proposal's §6 puts four questions: whether and when to
build it (*recommended*: after the first cluster session's start-up curve and
`ss -tm` reads at 512 nodes and above, before any run past about 2,000 nodes,
the order C5, C8, C17, C9); the other trees' wires at the first call
(*recommended*) or at a team's creation; the second notice as the same
message kind with a wire version bump (*recommended*) or a kind of its own;
and a dead node's far side, accepted (*recommended*) or waited for. The four
models the author's answer 6 asked for are done; the small-collective cost
model runs in its own session.

*2026-11-03*: every recommendation taken by the author
([2026-11-03](2026-11-03.md)).
