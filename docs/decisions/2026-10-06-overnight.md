# 2026-10-06, overnight: v0.2's steps 1 to 3 - where each stands

The author's prompt of 2026-10-05's evening: steps 1, 2 and 3 of
`docs/prompts/task-v0.2.md` in order, not step 4 (its proposal's §8 is
unanswered); stop-and-ask items written down with a recommendation and the work
carried on around them; `main` merged at green checkpoints only; no tag, no
push. This is where each step stands at the session's end; each step's own
record has the detail.

## Step 1: the single-node build - done

**Merged** (`main` from `5709bda` to `7053819`, and the review's fixes to
`ca04a58`): Part 1, the internode backend behind `internode-backend.h`
(`docs/decisions/2026-10-06-single-node-seam.md`), and Parts 2 and 3,
`OSOE_SINGLE_NODE` - the refusing backend, the classic futex, the `internode`
label, the Rocky Linux 9 VM, the README's desktop section
(`docs/decisions/2026-10-06-single-node-build.md`). Both builds verified whole
on the tree that merged: the full build's four presets, `tier3`, the pacer's
runs and ten minutes of `mix`, the models; the single-node build's four presets
on Debian 13 with liburing hidden and on Rocky Linux 9.8, the install, `oshcc`,
8 and 32 PEs on four cores, and the refusal. Two adversarial reads before the
merge; their fixes include waits a signal ended early in the single-node build
and a start-up check that tried the private futex scope alone.

**The merge point's full review**, the arc's first, at `7053819`
(`docs/reviews/archive/v0.2-step1-review.md`): five investigators, every one
finding that the single-node build can be offered to desktop users as the README
describes it and that the full build is still what 0.1.0 was; nothing blocks.
The one finding rated *fix before release* - `osoe-stat --summary` walking
nothing on a job of one node, which the README described as each team's barrier
in progress - held under a fresh refuter, and is fixed: the summary now says a
job of one node has no tree to walk and points at the PE lines. The rest: tests
the single-node build lacked or that could not fail (the one-node half of the
global exit and the deadlock detector, the pid fallback below Linux 5.3, the
agent's one wake, a spin before a sleep, the one-node quiet's fence on x86-64),
words grown stale with the step, and one defect the lead confirmed from an
investigator's note - a launcher whose output's reader had gone died of SIGPIPE
before it removed a refused start's segment. All fixed and merged but three left
for after the release - a test that nothing spins, best before step 4; the
fence's litmus test; the seam's note for a backend not of AM's shape - on the
checklist (`docs/decisions/2026-10-06-single-node-review-fixes.md`, with the
verification). **Step 1 is done**; its brief is in `docs/prompts/done/`.

## Step 2: the tree's shape from the link

**Part 1 on the branch `tree-shape`, not merged**: the cost model
(`tools/modelcheck/shape_timing.py`), the chooser (`collective-shape.c`), and
the settings - `OSOE_COLLECTIVE_SHAPE` and the link set by hand, agreed three
ways and shown by `osoe-stat`. Not built: the link's measurement. **Three
questions** in `docs/decisions/2026-10-06-tree-shape.md`: the fit (253 of the
Pi's 402 lines within the noise, against the brief's "every line"; the model
chooses within the noise of the fastest measured shape); where the link is
measured (the brief's place comes before the leads' mesh exists); and the
defaults' memory (a ceiling of 8 and a capacity of 4 make a PE's region 2.6
times the tag's). Part 2 waits on the three. The branch is rebased onto `main`
at `ca04a58`, the review's fixes beneath it (`da0a95d`): both builds' debug
suites pass, 773 and 392 tests, and its two models in release.

## Step 3: Rabenseifner's reduction

**Part 1 on the branch `rabenseifner`, on `tree-shape`, not merged**: the
arithmetic in the model and in C, checked for 2 to 70 leads; the protocol -
rows, grants, flow within a step - modelled, and holding on two to five leads
with six breakages failing; and Rabenseifner's cost in the chooser. **Two
questions** in `docs/decisions/2026-10-06-rabenseifner.md`: the fold carries
about 3.5 vectors on a link for a node count that is not a power of two, where
the brief has under two; and flow within a step, for which a design is proposed
and modelled. Also recorded there: the brief's pin of 64 KiB to the tree
contradicts its own arithmetic, and where leads have both a parent and children
the two link models disagree enough to flip the choice on 7 to 13 nodes. Rebased
onto `tree-shape` (`1929634`): both builds' debug suites pass, 774 and 393
tests, and its two models in release.

## What the author is asked, in one place

1. Step 1: Part 1's choice 2 (the io_uring check below the seam); and the
   review's one question, the version `main` calls itself between tags - 0.1.0,
   the tag's - for which 0.1.90 is recommended (readiness Q5-6).
2. Step 2: the fit's bar, the measurement's place, the defaults.
3. Step 3: the fold, and the design for flow within a step.

## Branches and worktrees left

`tree-shape` and `rabenseifner`, each in `build/scratch/wt/<name>`; nothing
else: the review's two worktrees and the branch `step1-review-fixes` are
removed, merged. Not removed, for the author: one directory in the `osoe-load`
VM, `/tmp/notetest-build-0732` (280 KiB), from a check of the build note's
escaping, whose removal the session's permissions refused - harmless.
