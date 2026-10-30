# Cloud session 3: the author's decisions, what the cluster needs next, then the small items

*Written by the lead session on 2026-10-13 for the author's third cloud session. The environment's
setup script is unchanged from the second session's (its brief's appendix). The author opens the
session on the repository's `main` with **Claude Opus 5.5 at high effort** and sends: "Read
docs/prompts/cloud-session-3-prompt.md and do what it says."*

## What this session is for

The first two sessions (`docs/prompts/cloud-session-1-response.md`, `cloud-session-2-response.md`,
both merged) showed that this sandbox runs osoe's full build whole - every preset, tiers 1 and 2,
tier 3 over `tbf` where the kernel has no `netem`, the models - and the second built `collect`'s
places four ways. About $113 of the author's cloud credit is left, until 5 November. This
session, in three parts:

1. **The author's decisions of 2026-10-13** (`docs/decisions/2026-10-13.md`, "The author's answers
   after cloud session 2"): P35 and P34.
2. **What the cluster's later sessions need**: P32 and B14 - with B11, which this sandbox cannot
   test, the last items before the session after the next one (H25, H26, H28 and the kit's other
   sections).
3. **As the credit allows, the small items**, in batches: the diagnostics, the small tests, the
   small protocol items.

Everything goes on the branch `cloud-session-3`, with a manifest kept as before, and the session
ends with a **response file that is the complete account** (the last section).

## Read first

- **The first session's brief**, `docs/prompts/cloud-session-1-prompt.md`: its "Read first",
  "Rules the lead keeps that the repository does not spell out" and "Branch, commits, pushes"
  apply as written, with the branch `cloud-session-3`.
- **Both sessions' responses**: what the sandbox does and does not do (no `netem`; root holds
  `CAP_IPC_LOCK` but cannot raise `ulimit -l`; ThreadSanitizer as root needs `OSOE_HEAP_BASE`
  until P34), and how the second session worked.
- Each item's text in `docs/prompts/v0.2-open-items.md`, its source, and the code it names. As
  before, do **not** edit the open items, `docs/prompts/task-v0.2-to-the-tag.md`,
  `docs/prompts/README.md` or `docs/decisions/`: the response lists what closed and what the
  records need, and the lead applies them at the merge. The lead changes nothing while this
  session runs.

## Part 1: the author's decisions

1. **P35** - P31's defaults until H25 measures them: the leads' exchange capped at **16 nodes**,
   each node's total or the scan past it as the link's cost chooses. In
   `shmem/src/collective-shape.c`'s `osoe_places_choose` and `tools/modelcheck/shape_timing.py`
   alike, as a named constant; `places-grid.txt` regenerated and `shmem.shape.places-grid` holding
   the C to the model again; the README's `OSOE_COLLECTIVE_PLACES` row and design §5.8 saying the
   default and why (H24, `docs/design/evidence/v0.2-step4/2026-10-13-h24/`, provisional until
   H25); a mutant with the cap removed, failing a test.
2. **P34** - ThreadSanitizer run as root: the heap may land at address 0, where ThreadSanitizer
   dies. Find why it lands there as root, and make the smallest fix in the library - a candidate
   at 0 never taken, or refused by name - with a test that fails without it here, as root, in the
   `tsan` preset with `OSOE_HEAP_BASE` unset.

## Part 2: what the cluster needs next

3. **P32** - the landing zone from a per-node budget, as the author decided (A5,
   `docs/decisions/2026-10-12.md`, "The author's decisions, that night", item 1): a node's budget
   split among its PEs, between 256 KiB and 1 MiB a PE; the blocking get stays as built. Read the
   gets' record (`docs/decisions/2026-10-10-blocking-gets.md`), the cluster's numbers
   (`docs/fast-link-results.md`, its gets), and the README's `OSOE_GET_LANDING_BYTES` row first.
   Tests that fail without it; the README row and the design notes true.
4. **B14** - the kit's missing sections, as the item lists them: H25's `collect` section (from
   `tools/bench/collect-places.sh`, run across nodes under Slurm as the kit runs its jobs), H28's
   loop over PE counts with the zone unset in `gets.sh`, H15 to H19's runs behind a stream at each
   helper count, H13's sweep of the pipeline's window and the chooser, H27's settings and
   `facts.sh`'s busy polling, H7's io_uring flags, and H6's pre-rung trees built beside `main`
   (confirm in `git log` the four trees the lead's map named: `3ee3357`, `de3dc42`, `49c62d3`,
   `2dcf8d6`). Each in the form of the kit's existing sections - `docs/fast-link/common.sh`,
   its settings and its `run.txt` - with its section of the runbook, `docs/fast-link.md`, and a
   **dry run** on this machine's logical nodes (`docs/fast-link/dry-run.sh`) showing it runs.
   Where a section would need the library changed (H7 may), say so in the manifest and stop that
   section: do not change the library for the kit.

## Part 3: the small items, as the credit allows

In batches by the file they touch, each item with a test that fails without it and the README
true, a commit per batch and a manifest entry per item:

- **The diagnostics** - `osoe-stat`'s (D2, D3, D5, D6, D7, D8); `fatal.c`'s and `abort.c`'s (D18
  to D22); start-up's messages (D10, D11, D14, D15, D25, D28); the settings' (D16, D24, D27,
  D29); `osoe-launch`'s (D23); and D30's line splitting.
- **The small tests** - T18, T19, T22, T23, T31, T33, T34, T37, T40, T41, T43.
- **The small protocol items** - P8, P9, P10, P16, P17, P21, P22, P25, P26, P27, and P13, which
  is §3.16's ground: name the memory order and why, and change nothing else.

Read each item's text for its current wording before you start it; if one turns out to need a
decision of the author's, stop it, say why, and go on.

## The manifest, and the response that ends the session

**The manifest**, `docs/prompts/cloud-session-3-manifest.md`, as before: the per-item log, with
every command and count, current after every item, committed and pushed each time.

**The response**, `docs/prompts/cloud-session-3-response.md`, written last, is **the complete
account of the session - not a short version**. The author reads it rather than the chat, and the
lead merges from it, so whatever you would tell the author in your final message belongs in it,
in full, and nothing should be in the chat alone. It holds:

- **In short** - what was done, what was not, and why.
- **The environment** - anything that changed since the second session's report.
- **Every item** - one section each: its outcome (done, partial, skipped, stopped) and why; its
  commits; the files and functions it changed and what each change does; whether it changes the
  wire or a layout, and which; how it was shown - the test, its failing-without-the-change
  evidence, the commands and their pass counts; and anything about it left open.
- **Where the tests stand at the end** - each preset and tier run, with counts, and what was not
  run.
- **What needs the author** - every decision, with the options and a recommendation.
- **What the lead's merge needs** - the items to close, with ids; any item's text to change; the
  record and README lines that wait for the merge; and what to re-run in the development VM,
  which is aarch64, with the exact commands.
- **Decisions taken and deviations** from this brief.
- **Findings** - anything stale, wrong or missing met on the way, proposed and not fixed, each
  with where it is.
- **What remains** of Part 3, by id, for a next session.

The manifest may hold more detail; the response must not need it to be understood. Commit and push
the response as the last thing; your final chat message may be a single line pointing to it. If
the author stops the session early, write the response before anything else.

## The credit

About $113, until 5 November. Parts 1 and 2 come first; Part 3 only while credit remains, and
leave enough to write the response well. Opus for design, protocol code and the models; Sonnet
subagents, precisely instructed, for the mechanical parts and the runs. Run the models you change
and `selftest.py`, not the whole model suite, unless a change reaches the engine.
