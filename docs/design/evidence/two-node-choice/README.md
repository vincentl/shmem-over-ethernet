# P4: the two-node choice between a tree and Träff's - the evidence

For `docs/decisions/2026-10-29-two-node-choice.md`. Measured on the Pi cluster on 2026-10-29,
on main at `cbfaa7e1`, which `~/osoe` held (`.osoe-commit`), its release build on the head node
without a warning; every node's TCP and generic segmentation offload off before and after each
batch, the nodes 26 hours up.

**The Pi** (run on the head node from `~/osoe`, the probe and the scripts copied to
`~/osoe/build/p4/`, one foreground ssh holding the session while each batch ran):

- `two-node.c` - the probe: a byte sum over the world at 4 and 8 KiB and at 16 KiB k and
  16 KiB k + 1, k from 1 to 256; each size's slowest PE's median, and what its calls took, read
  from each node's lead's record. Its header says how it is built.
- `two-node.sh` - the first batch: the tree (`OSOE_COLLECTIVE_SHAPE=tree`), Träff's (the shape
  `link` on a link set so that it is the cheaper at every size) and the default as built, at one
  PE a node and four, three series on pi-001 and pi-002 and a fourth on pi-003 and pi-004.
  Output in `out/`: a file a run, `summary.txt`, and `analysis.txt` (`analyse.py out 4`).
- `two-node-again.sh` - the second batch: six series on pi-001 and pi-002, the arms' order
  turned each series, with the tree on the link measured beside the tree on the link set. Output
  in `again/`, `analysis.txt` by `analyse.py again`.
- `analyse.py` - the tables: each size's median over the series, Träff's over the tree, and the
  series' least and most ratios; and any call that took another algorithm than its arm's, or
  whose PEs read different ones (none did).

**The model** (on the Mac, from the repository):

- `model-checks.py` and its output `model-checks.txt` - the cost of a partial last piece against
  step 2's simulation in the four forms weighed, on the grid's four links; and what the form
  built changes on the shape and places grids against the old, the changed shapes judged by the
  simulation.
- `crossovers.c` and its output `crossovers.txt` - every size from 1,025 bytes: where Träff's is
  taken on each node count, as built (`cbfaa7e1`'s `collective-shape.c`) and now - the review's
  68% of sizes on two nodes reproduced, none now, and the crossovers from three nodes to 512
  unchanged. Its header says how it is built.

Evidence paths name the repository `<repo>` and the home `~`.
