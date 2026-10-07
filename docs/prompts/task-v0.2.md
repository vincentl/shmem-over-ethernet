# Task: osoe v0.2 - the desktop, the link's shape, Rabenseifner, helpers

**Status: written on 2026-10-05, the day the 0.1.0 release point was
reached and the talk given, from the author's decisions of that evening
(`docs/decisions/2026-10-05-v0.2.md`). It is the arc of v0.2: four steps,
each with a brief of its own for a session that starts cold, in this order,
and a list of items gated on hardware the project does not have. Each step's
session reads this file first, then its own brief.**

## The project in a paragraph

osoe is OpenSHMEM 1.6 over Ethernet with io_uring: PEs are processes, a node's
PEs share a control segment in `/dev/shm`, one progress thread a node drives
TCP between nodes, collectives are two-level (intranode counters, an internode
tree, large ones in pieces through lanes), and every lock-free protocol has a
model. 0.1.0 is tagged on `53b8f0f`: phase 1 (design §7.1) whole, six
adversarial reviews, the Pi cluster runbook, LGPL, `cmake --install`, a README
true of the tree. The talk's "What comes next" slide named three performance
items; its audience asked for one thing above the rest - the library and
`osoe-launch` on one desktop, for developing at small PE counts - and
suggested one thing that does not work (`process_vm_readv` for statics,
`2026-10-05-v0.2.md` item 9). v0.2 is the audience's item first, then the
two performance items the Pi can measure, then the one it cannot.

Read first, in this order: `CLAUDE.md`; `docs/README.md`;
`docs/decisions/README.md` (the rules in force, one line each);
`docs/decisions/2026-10-05-v0.2.md`; then each brief as you reach it.

## The order of work

1. **`v0.2-step1-single-node-prompt.md`: the single-node build.** An opt-in,
   `OSOE_SINGLE_NODE`, default off: no liburing, no io_uring, the classic
   futex, a job of more than one node refused by name, for Rocky Linux 9 and
   every desktop distribution - the full build unchanged. Three parts: the
   internode backend behind one file boundary (a refactor of the full build,
   no behaviour change), the option with its refusing backend and the
   classic futex, the README with two requirement lists and a desktop
   section. Tested in a Rocky 9 aarch64 VM. About a week. **The arc's first
   review point** is its merge point: what it produces is what desktop users
   install.
2. **`v0.2-step2-tree-shape-prompt.md`: the tree's shape from the link.**
   The root measures its link at start-up - the hop, the message cost, the
   bandwidth - and every node computes, from the same three numbers, the
   fan-out and window that make each large reduction or broadcast cheapest;
   `OSOE_COLLECTIVE_FANOUT` becomes the ceiling the layout is sized for. The
   cost model is fitted to the Pi's table of 2026-09-26 before it chooses
   anything. About a week, with Pi measurements.
3. **`v0.2-step3-rabenseifner-prompt.md`: Rabenseifner's reduction.**
   Recursive-halving reduce-scatter and recursive-doubling allgather over the
   node leads, through the lanes and grants, with the fold for node counts
   that are not powers of two; one more shape in step 2's chooser, taken when
   the link says so. About a week, with Pi measurements.
4. **`v0.2-step4-progress-helpers-prompt.md`: progress helpers**, built from
   `docs/design/progress-helpers-proposal.md`, whose §8 the author answered on
   2026-10-07. The lead's agent keeps the control plane on a control wire of its
   own to every peer; helper threads own rings and disjoint sets of the data
   wires and move the bytes; nothing between threads is a queue; zero helpers
   is today's thread. Part 0 - the profile and the one-thread io_uring ladder
   - can run before §8 is answered. Correctness on oversubscribed logical
   nodes; performance when a fast link exists. About two weeks. **The arc's
   second review point** is its merge point.

Each step merges into `main` at green checkpoints, the README true of the
tree at every commit; each step's brief moves to `done/` when its merge point
is passed, and `docs/prompts/README.md`'s row says what happened. Steps 2 and
3 depend on nothing in step 1; step 3 depends on step 2's chooser; step 4
depends on nothing but is last because its gain cannot be measured here.

## Gated on hardware

These are in v0.2 as options, not in the order above, because the author
cannot promise the machines now (`2026-10-05-v0.2.md` item 5). Each is
promoted into the order the day its machine exists:

- **The first native x86-64 run**: the four presets, `tier3`, `models`, the
  SOS suite at 4 × 2 and the pacer's smoke tests on any x86-64 Debian 13 or
  Fedora host, and the single-node build on x86-64 Rocky 9. No code is
  expected to change; the README's "no x86-64 kernel has run it" changes.
  Half a day of machine time. On the release checklist's after-release list
  already ("a real x86-64 kernel", "the x86-64 network path").
- **The pipeline's window on a link faster than gigabit**: measure, then the
  default (the checklist's scale S2). Step 2's chooser takes the window from
  the link, so this becomes a check of the chooser's choice against a
  measurement.
- **Step 4's Part 4**: the put stream and the mailbox rate at 0 to 4 helpers
  on a pair with a fast link, against `echo-uring`'s bare stream; the
  ladder's rungs there; the default of the zero-copy send, which step 4
  builds behind a setting off by default (`2026-10-07.md` item 7).
- **Many PEs a node**: 16 to 64 PEs a node on an x86-64 pair, where the
  cost per message of one thread serving many PEs - never measured - shows.

Cloud x86-64 pairs, if the author gets time on them, serve the first and
third; development stays aarch64 until then.

## Not in v0.2

- `process_vm_readv` and `process_vm_writev` for statics: two sentences in
  the README's statics limits saying why not (`2026-10-05-v0.2.md` item 9),
  written by whichever step touches the README first - step 1.
- RoCE, Soft-RoCE: the stance of 2026-09-26 stands.
- The rest of the release checklist's after-release list: it stays the list
  for v0.3, except what a step takes naturally (step 1 takes Q6's futex
  message; the gated items take the x86-64 ones).
- The mesh schedule proposal: it waits for a large system; step 4 states the
  doubled chain.

## How to spend a session

- **Subagents for everything mechanical** (CLAUDE.md): searches and sweeps,
  preset runs that report only what failed, bulk edits, checklist passes, the
  README's sentence-by-sentence check. Brief each as a peer; its report is
  evidence, not a verdict. Keep in the session what is load-bearing: the seam
  (step 1), the cost model and the chooser (step 2), the halving's protocol
  and model (step 3), the owner structure and the control wire (step 4), the
  atomics rules of §3.16, and every judgement a brief says to stop on. Long
  VM runs in the foreground of a call under ten minutes or in the background
  from the session, never backgrounded by a subagent.
- **An adversarial read before every merge**, by a fresh agent given the
  diff, the property the brief names, and nothing of your conclusions. **The
  full review** - `docs/reviews/adversarial-review-prompt.md`, its
  deliverables named `v0.2-stepN-*` - at the two review points, after step 1
  and after step 4. Fix what it finds before going on; a refuted finding is
  refuted in writing, in the record.
- **Models first** where a brief says so; a model's broken variants must fail
  before its passing means anything. **A speed claim is simulated and timed
  before it merges**; where no machine can time it, the README says so.
- **Decisions by performance and elegance**, not by the code or the tests
  they cost (`2026-10-05-v0.2.md` item 7).
- **Record as you go**: a dated file in `docs/decisions/` for each part, the
  index updated in the same commit, the briefs' rows filled in, each brief
  moved to `done/` at its merge point.
- **Stop and ask** only on what a brief lists, or on a decision that changes
  the design notes; carry on with everything that does not depend on the
  answer.

The VM `osoe` is shared, locked for every run (`exec 9>/tmp/osoe-vm.lock;
flock -w 10800 9`) and killed by PID, with no `git` inside; step 1 adds
`osoe-rocky`. The Pi cluster is reached through the Mac, holds a tree in
`~/osoe` and nothing else of ours, and needs an ssh session held open on each
node while jobs start; Pi comparisons alternate trees on the day and pool or
bind their small-collective runs (`docs/decisions/README.md`, "License and
release"). The host disk is near full: remove each part's worktree once it
has merged. There is a remote now, or there is about to be: the author
pushes; a session does not.
