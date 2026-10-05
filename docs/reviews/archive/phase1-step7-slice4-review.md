# Round six: synthesis - osoe after the team redesign's step E

Review of `main` at `375973a`, run 2026-10-04 and 05 against
`docs/reviews/adversarial-review-prompt.md`, at the last merge point of the
first release's task (`docs/prompts/task-first-release.md`): the tree step E of
the team redesign left (`docs/decisions/2026-10-04-team-lanes.md`,
`2026-10-04-team-scans.md`, `2026-10-04-step-e-merge-point.md`), the release
candidate. Five investigators, one a question, each holding round five's
findings as claims to check; run two at a time, as the plan's usage allowed -
correctness and scale first, then conformance, readiness and portability. The
one finding rated *fix before release* was given to the refutation the
procedure asks for by the lead itself, in the source and against the
investigator's measurement; the lead also made the Pi runs the scale
investigator asked for. Then this synthesis, by the lead. Two of the findings
went to the author, who decided them the same night (`docs/decisions/2026-10-04.md`
items 10 and 11).

The five deliverables: `phase1-step7-slice4-conformance.md`,
`phase1-step7-slice4-correctness.md`, `phase1-step7-slice4-scale-and-performance.md`,
`phase1-step7-slice4-portability.md` and `phase1-step7-slice4-readiness.md`.

## Verdict

**Release, after the named changes below - every one of them made in the fixes
that follow this review (`docs/decisions/2026-10-05-step-e-review-fixes.md`).
Nothing blocks.** Each of the five says, in its own area, that the tree can be
tagged 0.1.0: four as it stood, readiness after one sentence. Step E does what
its records say. Every write it added into a lane, a small slot, a lane word or
a grant line is consumed before its owner returns; the departure from the
proposal the author accepted, a parent reading a child's lane word with its
first piece, is right, and the proposal's rule fails in the model as recorded;
no two live threads can share a lane. 514,710 collective calls, their expected
values taken from the specification alone, came back right, on seven team
shapes, either side of the small-collective line and of every piece edge. The
README's scaling section is true of the code to the byte, and the grant's cost
holds as it states it - a bound, once a collective - over 27,000 entry patterns
of the timing model and in the Pi's probes. An aarch64 build made to order
memory as x86-64 does passed the whole suite and the lanes tests 25 times each;
the README's quick start, run as printed, built in 19.7 s and ran both rings.

What the review found is of three kinds: one sentence of the README that sent a
user to a view that cannot show what they were told it would (`osoe-stat
--summary` and the data collectives); a cost older than step E that a power of
two pays, which the author chose to fix now; and the tests, models and words
that step E's own records had left thinner than the code - the scans' lane per
thread untested, their lane protocol model-checked only outside the tree, the
lanes kept from ThreadSanitizer for a reason that did not apply, sentences
grown stale.

## What must precede release, ranked

Nothing was rated *blocks release*. The changes the lead and the author took
before the tag:

1. **`osoe-stat --summary` follows barriers and syncs only** (readiness Q5-1,
   *fix before release*; *measured*, and in source by the lead's refutation).
   The README sent a hang to it - "each team's collective in progress, to walk a
   hang down" - but only `progress.c` writes the tree state's epochs it reads
   (`stat.c`'s `read_tree_state`), and the data collectives' generic phase runs
   on the calling thread and writes none: a team hung in a reduction, broadcast,
   gather or scan reads "nothing open" on every node, its last column naming a
   node-mate of the culprit. Older than step E; every round had tested the view
   on barriers. *Fixed* as the investigator proposed: the README, `--help` and
   design §3.14 say what the view walks and where a data collective's hang
   shows (the PE lines); the view itself is on the checklist's after-release
   list.
2. **A large reduction's or broadcast's piece held 16,368 bytes** (scale U1,
   *after release*, the author's word "fix U1 now"; *measured*). The 16-byte
   header sat inside the lane's 16 KiB slot, so every power-of-two size from 16
   KiB went in one piece more than its multiple of 16 KiB - 32 KiB in three
   pieces against a window of two, the third waiting a round trip. In the
   development VM 32,768 bytes took 1.17 to 2.39 times 32,736; on the Pi's
   gigabit, 0.5 to 11% (the lead's `piece-edge`). Older than step E; the scans
   had been given the same fix in its second part. *Fixed*: a slot is the header
   and 16 KiB beside it, 16,400 bytes; measured before and after, the VM's 32
   KiB broadcast 0.54 of its old time and reduction 0.67, 64 KiB 0.75; the Pi's
   power-of-two sizes 0.95 to 0.98 at 4 × 4.
3. **Design §10's "thorough 1.6 spec read" was still listed as blocking**
   (conformance M2). Closed by the author: four rounds' conformance reviews
   read the C interface against the specification; what it had left, the
   session routines' semantics should they ever be more than no-ops, is on the
   after-release list.

## After release, and minor

Every one below was fixed with this review unless it says otherwise.

- **Correctness** (*measured*): **H1** a lane per thread was untested for the
  scans - a one-line regression sending every scan of a PE through one lane
  passed all 115 lane, scan, pipeline and team tests and returned wrong prefix
  sums with status 0 - and `osoe-pacer` cannot see a lane shared by threads at
  all: the investigator's stress is now `shmem.lanes.threads`, which fails under
  that regression and the reductions' alike; the pacer's exercise of
  collectives on per-thread teams is after the release. **H2** the scans' lane
  protocol was model-checked only by the second part's adversarial model,
  outside the tree: it is `lane_scan_model.py` now, with `scan_one_lane` among
  its breakages. **H3** `lane_model.py`'s small path's DOWN check was reached by
  no listed breakage: `node_small_down` reaches it. **H4** "the grant counter is
  the one guard": the reader's clear is a second, which turns a stale count into
  a named end. **H5** the lanes tests were kept from ThreadSanitizer for a
  reason their one-PE-a-node maps do not have: they run under it now, but the
  deadlock detector's case and the two-a-node map. **H6** a stale comment.
- **Scale** (*measured*): **U2** the Pi's "nothing moved on the reductions" was
  measured only after barriers, which release the tree's root a hop before its
  children; the records credited the intranode phase. The lead's
  `reduce-skew` on the Pi measured the grant where it shows - the root node's
  lead late +278 µs at 4 × 4, a middle lead +216 at a fan-out of 2, a non-lead
  or a leaf nothing much, nobody late nothing - and the records say so. **U3**
  the README's job object said nothing of plane or arbitrary distributions,
  under which the rank map grows with the PEs.
- **Conformance**: **M1** a large scan's members are compared only when its
  team spans nodes. **M3** the README's SOS sentence cites the tree of 28
  September: it changes when the checklist's SOS item is ticked on the tagged
  tree. **M4** no suite test ran large scans or broadcasts from two threads at
  once: `shmem.lanes.threads` does.
- **Readiness** (*measured*): **Q5-2** the class-split refusal now says what is
  wrong; **Q5-3** the `/dev/shm` refusal says what a heap object holds and which
  settings size it; **Q5-4** `SHMEM_INFO`'s small-collective row names the
  scans; **Q5-5** three items the checklist lacked - the tagged commit's README
  ("there is no version tag"), the quick start on a Pi node, round three's
  after-release list; **Q5-6** the history section's qualifiers, the evidence
  count, the decisions file's commit count and its piece rule as built;
  **Q5-7** what the other nodes print when a job ends. **Q5-8**, the seam,
  holds: after release.
- **Portability**: **Q1** the README's x86-64 passages describe this tree now,
  and the checklist's x86-64 item adds the x86-ordered aarch64 build, the one
  run that puts x86-64's memory order on step E; **Q2** the containers' counts;
  **Q3** the quick start's `/dev/shm` on a small machine; **Q4** a put's wake is
  best effort - the README no longer offers `shmem_p` as a sure wake. **Q5**,
  notes: the PID-namespace message when pids do not repeat exactly, step E's two
  models run under sequential consistency only, an x86-64 tsan build's heap
  placement - after release.

## What is sound

- **Conformance** (*measured*): the specification-derived matrix of 514,710
  calls on fourteen configurations from 1 to 18 PEs, three one-line breaks of
  the code each failing it in exactly its class; four threads a PE making
  collectives on four teams at once in waves, under ThreadSanitizer too; round
  five's F1 lock still needed - four races a run without it; 253 teams a PE on
  four maps; the merge point's SOS sweep and acceptance meeting the release
  point's items 2 and 3.
- **Correctness** (*measured* and by argument): E5 for every write step E
  added; the E4 departure; lanes claimed by compare-and-swap, a PE as many as
  its thread records; design §3.16 in step E's code and library-wide; the whole
  debug suite under eight busy loops, step E's 165 tests ten times each, a stress
  of 9,000 rounds over five maps, under both sanitizers, with the deadlock
  detector at one second and no false end.
- **Scale and performance** (*measured*, *observed in source*): every figure
  of the README's scaling section recomputed from `internal-region.h`; nothing
  step E added grows with the job's PE count; the grant's bound over 27,000
  entry patterns; at 1,024 numbers and a 128-byte line, 200 large, granted
  splits right; the merge point's barrier move the cluster's two speed modes,
  not step E.
- **Portability** (*measured*): no architecture-specific layout or word in
  step E; 3,595 exports alike on both targets; the x86-ordered build validated
  by a store-buffering litmus test before it was trusted; an LL/SC build
  716 of 716; `lane_model.py` under the checker's x86-TSO mode, all 30 cases
  keeping their verdicts; no order-of-evaluation hazard in 205 translation units.
- **Readiness** (*measured*): 58 jobs, most made to fail - mismatches between
  nodes named within 75 ms, a never-entered large collective proved a deadlock in
  seconds, a killed PE ending the job in 0.17 s, a node stopped ten seconds
  waited for and the sum right; `OSOE_SMALL_COLLECTIVE_BYTES` refused by name
  for every bad value; 253 splits then a refusal; the quick start as printed;
  the history's counts; every new file's licence line.

## Refuted, or corrected

Nothing rated *fix before release* was refuted: Q5-1 held in the source - the
summary's reader asks only for the fields the progress thread writes - and in the
investigator's runs. Corrected, as the lead checked them:

- **U1's size**: the VM's 2.4 times is the latency-bound case; on the Pi's
  gigabit the extra piece cost 0.5 to 11% (the lead's `piece-edge`), since a
  piece's time on the wire hides most of the round trip. After the fix, the Pi
  at one PE a node read 3% slower at 64 KiB on both sides of the edge, a size
  whose piece count the fix does not change among them - noise, at p of about
  0.4 over six runs a side.
- **H3's variant**: of the investigator's four `node_small_down` cases, three
  hold with one thread a node - sharing the small release slot is safe while a
  node's small reductions take turns - and only the two-thread case fails, at
  63 states; the model keeps one of each.
- **Portability Q2**: the 64 MiB `/dev/shm` count, about 560, is an estimate
  from the suite's size, not a run.

## Sources

The five deliverables; the lead's notes, kept in the session's scratch tree
(`build/scratch/review-e/lead-notes.md`), with the investigators' scripts and
probes; the lead's Pi runs under
`docs/design/evidence/pi-cluster/2026-10-04-step-e/review/` (`reduce-skew`,
`piece-edge`, and U1's before and after, on the Pi and in the VM); the fixes
and their verification, `docs/decisions/2026-10-05-step-e-review-fixes.md`.
