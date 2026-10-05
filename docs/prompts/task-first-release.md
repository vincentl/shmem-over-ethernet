# Task: take osoe to its first release

**Status: written on 2026-10-02 for one session that carries the project from
the round-three review to the first tag. It is the session's opening prompt;
the work itself is in the briefs it names, in the order it gives.**

## The project in a paragraph

osoe is OpenSHMEM 1.6 over Ethernet with io_uring: PEs are processes, a node's
PEs share a control segment in `/dev/shm`, one progress thread a node drives
TCP between nodes, collectives are two-level (intranode counters, an
internode tree), and every lock-free protocol has a model. Phase 1 (design
§7.1) is built: steps 0 to 10, the stress tester `osoe-pacer`, the Pi cluster
runbook, LGPL, `cmake --install`, the README. Three adversarial reviews have
run; the third, on 2026-09-28 (`docs/reviews/archive/phase1-release-review.md`),
says *release after named changes*. The plan that validated its sixteen
findings is `docs/prompts/done/phase1-release-fixes-plan.md`; the author's
decisions on it are `docs/decisions/2026-10-02.md`. What stands between the
tree and the tag is three things: the release fixes, the team redesign, and
the release point's conditions (design §1, "Release point").

Read first, in this order: `CLAUDE.md`; `docs/README.md`;
`docs/decisions/README.md` (the rules in force, one line each);
`docs/decisions/2026-10-02.md`; then each brief as you reach it.

## The order of work

1. **`docs/prompts/done/task-release-fixes.md`**, Parts 0 to 6. Part 0 first and at
   once: it commits the review, the plan, the proposal, the briefs and the
   day's record, all untracked today. Then the small fixes, global exit,
   start-up, the README's own items, the scrub, the whole-tree verification.
   About a week.
2. **`docs/prompts/done/phase1-step7-slice3-prompt.md`**: step D of the team
   redesign (`docs/design/team-resources-proposal.md` §3), two parts and a
   merge point. About two weeks.
3. **Step E** (the proposal §4): write its brief first, from the proposal and
   D's record, in the form of D's brief (written on 2026-10-04 as
   `docs/prompts/done/phase1-step7-slice4-prompt.md`) - it is the proposal's build plan,
   not a new design, so build from it without waiting, and put any point
   where it departs from the proposal to the author before building that
   point. About two weeks.
4. **The release point's conditions** (the plan §5, step 8): the SOS sweep
   past 61 PEs, the Pi runbook end to end on the rebuilt cluster, an hour of
   `mix`, the release note of known limits, and everything on
   `docs/release-checklist.md`. Prepare the tag; the author tags.

Merge into `main` at green checkpoints; the README is true of the tree at
every commit, temporary sentences marked so (`2026-10-02.md` items 4 and 6).
No push: there is no remote.

## How to spend the session

- **Subagents for everything mechanical** (CLAUDE.md, "Delegate
  straightforward work"): searches and sweeps, preset runs that report only
  what failed, bulk edits, checklist passes, the README's sentence-by-sentence
  check, the scrub. Brief each one as a peer - the goal, the files, what to
  report - and treat its report as evidence, not a verdict. Keep in this
  session what is load-bearing: protocol and data-structure design, the
  atomics rules of §3.16, the models, the judgement calls a brief says to
  stop on. Long VM runs go in the foreground of a call under ten minutes or
  in the background from this session, never backgrounded by a subagent.
- **An adversarial read before every merge**, by a fresh agent given the
  diff, the property it must keep (the brief says which) and nothing of your
  conclusions, asked to find the path that breaks it. **The full review** -
  `docs/reviews/adversarial-review-prompt.md`, its deliverables named for the
  work reviewed - at the three merge points: after the release fixes, after
  D, after E. Fix what it finds before going on; a finding you refute is
  refuted in writing, in the record.
- **Models first** where a brief says so; a model's broken variants must
  fail before its passing means anything.
- **Record as you go**: a dated file in `docs/decisions/` for each part, the
  index updated in the same commit, the briefs' rows in
  `docs/prompts/README.md` filled in, each brief moved to `done/` when its
  merge point is passed.
- **Stop and ask** only on what a brief lists, or on a decision that changes
  the design notes; carry on with everything that does not depend on the
  answer.

The VM `osoe` is shared, locked for every run and killed by PID; the Pi is
reached through the Mac and holds no tree until you copy one; the host disk
is near full - the briefs say the rest.
