# 2026-11-02: the intranode combine sliced over the node's members, modelled and timed

## The brief

The author's answer 6 of the morning (`2026-11-01.md`): the four design changes
the large-cluster playbook names are modelled here before any cluster session,
C5 and C8 first. This is C5, the parallel intranode combine; the author's words
on starting it: "Start the C5 model".

## What was made

On the branch `intranode-combine-model`, from `main` at `e1be26bc`:

- **The proposal**, `docs/design/intranode-combine-proposal.md`: the problem (a
  node's lead combines its L members' vectors serially, O(L × bytes) on one
  thread before a byte leaves the node - 256 MiB of reads for a 1 MiB
  reduction at 256 PEs a node, 13 to 25 ms at a core's rate), the design (once
  every member has arrived, member i combines slice i of every member's source
  in the descriptor's order into slice i of the lead's dest and adds to a
  `combined` counter the lead waits on before its internode phase; the
  release and the copy-outs as built; the result bit for bit today's), how
  §5.5's three reasons for a single writer are each met, the costs (about
  sixty lines, a layout bump for the counter, invariant 16 amended), the
  probe's numbers, the alternatives weighed (a tree over the members, groups,
  leaving it), and four questions for the author.
- **The model**, `tools/modelcheck/intranode_combine_model.py`, registered as
  `model.intranode_combine` (labels `tier1;model`, as the loop gives every
  model): the sliced design at two to four members over two epochs, separate
  and in place, under SC and x86-TSO, every case holding with the engine's
  liveness check; the serial design as built, holding; five breakages, each
  failing the property it is named for - SOURCE (combining after one's own
  arrival alone), WRITER (two members one slice), RESULT (the lead not waiting
  for `combined`; a copy before the release; the lead leaving before the
  copy-outs, in place). Thirty cases, the largest 399,182 states, seven
  seconds on the Mac; 5.9 s as a ctest in the VM.
- **The probe**, `docs/design/evidence/intranode-combine/combine-timing.c`,
  standalone, with its run in the VM (`run-vm.txt`): at L up to the VM's four
  cores the sliced combine 3.5 to 4.0 times the serial at 256 KiB and 1 MiB,
  1.8 to 2.5 at 4 MiB where the memory system binds, 1.5 to 1.7 at 64 KiB
  where the barrier is a good part of 5 µs; past four processes the gain stays
  at the cores' count, 2.6 to 4.3. Its first form's barrier spun and measured
  the scheduler's slice at eight processes on four cores (8 ms); it yields now.
- The indexes: `docs/design/README.md`, `tools/modelcheck/README.md` and its
  count, the README's count of models (48), the playbook's C5 and §5 row.

## What the model found

Every member now waits on `arrived` by equality, as the lead alone did, and
§5.4's equality waits are sound only under bounded skew: a member racing into
the next epoch moves the counter past the value a slow member still waits for.
In the sliced design the skew is bounded by `combined` - the lead releases
only once every member has bumped it, and a member bumps it only after
passing its wait on `arrived`. The model's `serial` control variant, first
written with the members waiting on `arrived` but without `combined`, was
STUCK for exactly that reason; as built, members wait for nothing but the
release, and the variant now does the same and holds. The sentence is in the
model's docstring and the proposal's §3, so that a build does not drop the
counter's wait as redundant.

## Checked

The model's thirty cases on the Mac, every case as expected (`report`'s
`expect`); the registered test in the VM's release configure of this branch,
1 of 1 passed; the probe compiled with `-Wall -Wextra` on the Mac and in the
VM without a warning; the probe's run under the VM's lock. *Not run*: the
suite - nothing of the library changed; the static checks - no C of the
library changed; the model under `OSOE_MULTIPLE_MODELS` - it needs none.

## Left, and for the author

Nothing is built. The proposal's §7 puts four questions: whether and when to
build it (*recommended*: after the first cluster session's PEs-a-node rung at
64 PEs, here, for the second); the threshold below which the combine stays
serial (*recommended*: slices of at least 1 KiB, a setting only if asked for);
the arrival's wake (*recommended*: wake-all, measured); and the release's O(L)
wake beside it (*recommended*: measured in the same run). Next of the four:
C8, the two-level alltoall.

*2026-11-03*: every recommendation taken by the author
([2026-11-03](2026-11-03.md)).
