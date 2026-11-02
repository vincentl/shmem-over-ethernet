# 2026-11-02: a two-level alltoall, its cost modelled against the as-built one

## The brief

The author's answer 6 of the morning (`2026-11-01.md`): the four design changes
the large-cluster playbook names are modelled here before any cluster session,
C5 and C8 first. This is C8, the two-level alltoall; the author's words: "Start
C8". C5 was done the same day (`2026-11-02-intranode-combine-model.md`).

## What was made

On the branch `intranode-combine-model`, after C5's commit `a3e4bc89`:

- **The proposal**, `docs/design/two-level-alltoall-proposal.md`: the problem
  (a message a block, L × (N - L) into a node, an element a message with a
  stride; 33.5 million messages of 32 bytes into a node of 256 PEs on 512
  nodes at 8-byte blocks), the design (a message a member a destination node
  carrying its L_d blocks in the team's local order, a new message kind the
  owner scatters into L_d heaps; a node-order schedule; the strided form by
  the same message; the barrier and its quiet unchanged), what it costs (about
  two hundred lines, a wire version bump, no memory), the alternatives weighed
  (a message a node pair, Bruck, a barrier a step, leaving it), and four
  questions for the author.
- **The cost model**, `tools/modelcheck/alltoall_timing.py`, registered as
  `model.alltoall_timing`: the slowest node's time as the barrier, the fill,
  each PE's issue of its messages, the owners' parse a message and copy a byte
  or the link's time, a block's memcpy beside; three forms, as built, two-level
  and a node-level Bruck. `check` fits the as-built form to the third cloud
  session's twenty alltoall points (H4, H11, H12: two nodes at 16 to 256 PEs a
  node, eight at 8 to 32, 8-byte and 4 KiB blocks, no helpers and two) within
  a factor of 1.52, its variant without a per-message cost missing the 8-byte
  points by up to 3.7, and pins two of the table's answers; `table` and
  `crossover` print the forms at 8 to 4,096 nodes and 8 to 256 PEs a node.
- The indexes: `docs/design/README.md`, `tools/modelcheck/README.md` and its
  count, the README's count of models (49), the playbook's C8 and §5 row.

## What the model says

Two-level is 6 to 8 times as fast at 8- to 64-byte blocks wherever a node has
64 PEs or more (an 8-byte alltoall of 512 nodes × 256 PEs from 7.2 s to 0.85 s
by the model), 10 to 30% at 1 KiB, under 10% from 4 KiB, where the bytes bind
and are the same by any algorithm - a 4 KiB alltoall at 512 × 256 is 137 GB
into every node, 51 s on the link alone, the design's inherent O(N) a PE. The
two-level form is at least twice as fast below 256 bytes a block at 8 PEs a
node or more, never at one. Bruck wins only at 8 to 64 bytes and by half at
most, and is three to seven times slower at 1 KiB: not worth building. The
strided `alltoalls` gains nelems times the table's ratio.

## What the fit taught

The first form of the model had a fixed cost of the barrier alone and missed
the small-PE, small-block points by half: the sender's own issue of its puts -
the reservation's CAS, the memcpy, the commit, the dirty bit, the wake, on
lines the node's writers share - is about 1.5 µs a message and a good part of
an alltoall of few PEs; the two-level form pays it L times less often too.

## Checked

`alltoall_timing.py check` on the Mac, its fit within 1.52 and its broken
variant failing; the registered test in the VM's release configure, 1 of 1
passed; `shmem.license-lines` and `shmem.settings-documented` passing. *Not
run*: the suite and the static checks - nothing of the library changed.

## Left, and for the author

Nothing is built. The proposal's §6 puts four questions: whether and when
(*recommended*: after the first cluster session's alltoall row at 64 nodes,
before the second); the schedule's order (*recommended*: node order, measured
against the as-built start); the chunking (*recommended*: as a put's parts);
and the strided form (*recommended*: one change, both forms). Next of the four
models: C9, the hierarchical scan, and C17, the control tree.
