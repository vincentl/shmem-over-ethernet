# Implement phase 1, step 4, second slice: the tests that could not fail

You are continuing **§7.1 step 4** of **osoe** (OpenSHMEM Over Ethernet),
"Robustness". The first slice is merged: an abort crosses nodes as an abortive
goodbye, every PE ends when its node aborts, `shmem_global_exit` exists, every
agent watches the deaths it can see with pidfds, and TCP keepalive finds a
vanished node. This slice takes the rest of step 4 — **the two tests §8.2 says
no tier can fail today** — and one refinement the first slice left open.

The author's goals, the axes to judge the work by:

- **Internode algorithms logarithmic in node count**, in time and in memory.
- **Intranode algorithms linear or better in PEs per node**, with no serial
  bottleneck where a cheap alternative exists.
- **A path to RoCE at ~10⁶ PEs (4,096 nodes × 256) that no interface blocks.**
- The Ethernet path is graded at *hundreds of nodes × 256 PPN*; the seam and the
  inherited data structures are graded at 4,096 × 256.

Machine setup: "Setting up on a new machine" in `phase1-step2-slice1-prompt.md`.
Meaning of phase/step/slice: `docs/prompts/README.md`.

*Written by Claude on 2026-09-23 while the author was away, under their mandate to
continue past slice boundaries (`docs/decisions/2026-09-23-autonomous.md`), and
brought in line with their review of that log the same evening: Part A is done
and merged (d5ca388); Part B is the Slurm path first, then fixtures captured
from Slurm installed in the VM; Part C is not being built now.*

## Read first

1. `CLAUDE.md`.
2. Design §8.2, "Tests the review found missing": the heap-base shift and the
   bootstrap fixtures. Invariant 7 (§9): *the wire format never depends on
   identical virtual addresses*. §4.1 (one heap per PE, each process maps its
   own at `B`, siblings wherever they land, the `base_of` table) and §4.2.
3. §6.1–§6.2: what the bootstrap reads from Slurm and the launcher, and how a
   node's lead chooses `B` (the candidate list, `OSOE_HEAP_BASE`).
4. §3.15's four as-built paragraphs, especially *liveness without the
   watchdog*: its last open refinement is Part C here.
5. The first slice's decisions log entries 11–18.

## What exists, and what does not

- **Every PE of a node maps its own heap at the same address**, the lead's
  `B`, and every node's lead usually chooses the same candidate. So a symmetric
  address is the same number on every PE of the job, and code that sends or
  compares raw addresses where it should use offsets passes every test there is.
  The intranode path translates through `base_of`; nothing forces it to.
- **There is no Slurm path.** `osoe_bootstrap_read` refuses a Slurm
  environment ("launch under Slurm is not yet supported") and reads only the
  `OSOE_` names `osoe-launch` sets; this brief first assumed otherwise (the
  decisions log, 20).
- **The multi-node lead finds a sibling's death within a sweep interval**, by
  polling its pidfds from its agent loop, where a completion on its ring would
  find it at once. The author decided to keep it so (Part C).

## Order of work

### Part A — the heap-base shift (**done**, merged as d5ca388)

`OSOE_HEAP_SHIFT` shifts by world rank; it found two one-base assumptions,
neither on the wire (a test comparing addresses, and `shmem_align` deciding
from the PE's own base), and `shmem.heap-shift.*` runs the address-heavy tests
shifted in every preset. What follows is the part as it was briefed.

A test mode in which **every PE maps its own heap at a different address**:
`OSOE_HEAP_SHIFT=BYTES` (a multiple of the page size, 0 by default) makes each
PE map its own heap at `B + rank × BYTES`, where `B` is the base its node's
lead chose (the lead searches its candidates for one where its own shifted heap
fits, and publishes the unshifted `B`). Say whether the shift should be by
world rank, local id or node, and why; world rank shifts both the intranode and
the internode case at once.

Then run what exercises addresses through it: the intranode RMA, AMO, wait and
collective tests at L = 2 and 4, the internode maps, force-remote, and the
differential over §8.1's four poles. Every place that assumed one base is a
finding — fix it, and say what it was. If nothing fails, break it on purpose
(compute a target as the caller's address rather than through `base_of`) and
show the mode catches it. Decide whether the mode becomes a ctest label run in
every preset or a preset of its own, by what it costs.

### Part B — the Slurm path, then captured fixtures (**decided by the author**)

First the path itself: §6.1's table read with the `SLURM_` prefix, in place of
`osoe_bootstrap_read`'s refusal. It needs no Slurm to build.

Then §8.2's *captured* `srun` environments — block, cyclic, plane and
arbitrary distributions, `-r` and `-x`, a heterogeneous job, a bare `sbatch` —
replayed into bootstrap parsing at tier 1. The author chose the source: Slurm
installed in the development VM — Debian's `slurm-wlm` (24.11) and munge, one
`slurmctld` and one `slurmd` for the single-node captures, and a small
container cluster for the multi-node ones, since a single host runs one
`slurmd` unless Slurm is built for several. The VM is shared with other
sessions: say before starting the daemons, and stop them when the captures are
done. The path is supported once the real captures pass.

### Part C — the multi-node lead's pidfds on its ring (**not now**)

The author decided against it for now: the ring is the AM backend's, below
§4.10, and `liveness.c` above it, so it needs a new seam function (and an RDMA
backend one of its own), and all it buys is the sweep interval, at most
100 ms. Revisit only if sub-100 ms death detection ever matters. As briefed: on
the lead of a multi-node job, arm each sibling's pidfd on the ring
(`IORING_OP_POLL_ADD`, oneshot), so a death is a completion that wakes the
agent at once, and measure the difference with `shmem.dead-pe` on two nodes.

### Part D — what the notes must now say

§7.1 step 4 done, or what remains; §8.2's Slurm item (the heap shift's is
done); §6.1 for the Slurm path.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md`, as amended by
the later slices, apply. In addition:

- **Complexity is a correctness criterion.** State in every commit message the
  cost in N, L, P and T, in time and memory, per PE and per node.
- **Model any new protocol before writing it.** Part C changes who wakes the
  lead's agent; if it changes an ordering `abort_exit_model.py` relies on, extend
  the model first.
- **Memory orders come from the C11 model and are minimal** — §3.16's table.
- **Run the load loop** on every test added or changed, never while editing the
  tree; and tier 3 for anything that touches the sockets or the ring.

## Working method

- State a short plan, then proceed. Stop only on a surprise, saying what you
  expected, what happened, and the options.
- Branch `phase1-step4-slice2-slurm` from main (Part A's branch is merged). Small commits, warning-free, all four presets
  green in the VM, the load loop on every test added or changed, messages that
  say what was verified and how, the design doc changed in the commit that
  makes it true.
- When done: summarize commits, verification, design-doc changes,
  measurements, deferrals, open questions; add a row to `docs/prompts/README.md`;
  and add to `docs/design/successor-api-stage-setting.md` §2 any lesson this
  slice paid for.
