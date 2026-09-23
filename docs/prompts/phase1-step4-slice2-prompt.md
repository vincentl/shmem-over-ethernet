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

*Written by Claude on 2026-09-23 while the author was away, under his mandate to
continue past slice boundaries (`docs/decisions/2026-09-23-autonomous.md`). Part
B needs a decision only the author can take, and says so.*

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
- **No tier feeds real `SLURM_*` values.** The bootstrap's Slurm path has been
  read against documentation and the launcher's imitation of it, never against
  what `srun` sets.
- **The multi-node lead finds a sibling's death within a sweep interval**, by
  polling its pidfds from its agent loop, where a completion on its ring would
  find it at once.

## Order of work

### Part A — the heap-base shift

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

### Part B — Slurm environment fixtures (**needs the author**)

§8.2 asks for *captured* `srun` environments — block, cyclic, plane and
arbitrary distributions, `-r` and `-x`, a heterogeneous job, a bare `sbatch` —
replayed into bootstrap parsing at tier 1. Capturing needs a Slurm: the
development VM has none (Debian's `slurm-wlm` 24.11 is available), and a
single host runs one `slurmd` unless Slurm is built for several. **Stop and ask
before installing anything**: the author chooses between the VM with Slurm
installed, a real cluster, or fixtures written from Slurm's documentation and
marked as such. Until then, write the replay harness against one fixture the
launcher produces, so the part is ready for real captures.

### Part C — the multi-node lead's pidfds on its ring

On the lead of a multi-node job, arm each sibling's pidfd on the ring
(`IORING_OP_POLL_ADD`, oneshot), so a death is a completion that wakes the
agent at once; keep the sweep-interval poll for a single-node job, which has no
ring. Update the ring's submission and completion bounds (§3.3's assertion) for
L − 1 more entries, armed once. Measure the difference with `shmem.dead-pe` on
two nodes: the time from the death to the last rank's exit.

### Part D — what the notes must now say

§7.1 step 4 done, or what remains; §8.2's two items; invariant 7's evidence;
§3.15's liveness paragraph for Part C.

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
  expected, what happened, and the options — and at Part B's decision.
- Branch `phase1-step4-slice2`. Small commits, warning-free, all four presets
  green in the VM, the load loop on every test added or changed, messages that
  say what was verified and how, the design doc changed in the commit that
  makes it true.
- When done: summarize commits, verification, design-doc changes,
  measurements, deferrals, open questions; add a row to `docs/prompts/README.md`;
  and add to `docs/design/successor-api-stage-setting.md` §2 any lesson this
  slice paid for.
