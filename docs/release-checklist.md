# Release checklist

**Written 2026-10-02, and accepted by the author on 2026-10-04** ("Skimmed it.
Accepted."). Moved into the tree from the author's status page at the author's word ("yes, move checklist";
`docs/decisions/2026-10-02.md` item 5), by the release-fixes brief
(`docs/prompts/done/task-release-fixes.md`, Part 5 item 3). Its items are the
page's own eight, design §1's release point, the decisions index's lines on a
release, and what the release fixes added. A ticked item says where it was
done; an open one, what is left.

## The release point (design §1)

- [x] **OpenSHMEM 1.5 complete**: contexts and `SHMEM_THREAD_MULTIPLE`, phase 1
  step 8 in three slices.
- [x] **OpenSHMEM 1.6 complete**: the scans (phase 1 step 10), the deprecated
  routines' profiling names (both spellings) and active sets smaller than the
  job (step 5 slice 7) are built, as design §7.4 and §7.5 record.
- [ ] **The SOS test suite clean** but for its own extensions (design §8.6; the
  runbook's §8 names them) - now including runs past 61 PEs once the teams are
  rebuilt: `shmem_team_reuse_teams` and `shmem_team_max` at 70 PEs, and
  `reduce_active_set`, `broadcast_active_set` and `repeated_barriers` at 70
  PEs with no iteration skipped, and `collect_active_set` at 32, the most it
  runs on (`docs/prompts/done/phase1-step7-slice3-prompt.md`, acceptance; met
  on step D's tree at 128 teams, `docs/decisions/2026-10-03-team-allocation.md`,
  "Acceptance", and at step E's merge point at the default of 256,
  `docs/decisions/2026-10-04-step-e-merge-point.md`; to be run again on the
  tree to be tagged).
- [ ] **The Pi runbook green end to end** on the cluster as rebuilt on
  2026-09-28 - the tree copied and built per `docs/pi-cluster.md` §2 - with the
  floor ratios, `osoe-bench` and the collective sweeps recorded in
  `docs/pi-cluster-results.md`.
- [x] **A license** (GNU LGPL, version 3 or any later version), **`cmake
  --install`**, and **a README enough for a stranger** to build and run a
  program on a Slurm cluster like the author's - to be read once more as the
  stranger on the tagged tree (the release fixes' Part 4).
- [ ] **A version tag and a release note** listing the known limits: whatever
  the last Pi session measured short of the floor, and the README's Limits.
  The version is 0.1.0 (`docs/decisions/2026-10-02.md` item 7, D8). The
  author tags.

## Before every release tag (the decisions index)

- [ ] **An adversarial review of the release candidate, its findings settled.**
  Round three (2026-09-28, `docs/reviews/archive/phase1-release-review.md`)
  said *release after named changes*: the release fixes and the team redesign.
  Round four, at the fixes' merge point (2026-10-03,
  `docs/reviews/archive/phase1-release-fixes-review.md`), said the same, its
  named changes merged on 2026-10-04 (`docs/decisions/2026-10-04-review-fixes.md`).
  The full review runs again after the teams' step D - at its merge point,
  `docs/reviews/archive/phase1-step7-slice3-review.md` - and after step E
  (`docs/prompts/task-first-release.md`).
- [ ] **An hour of `mix` on the Pi cluster, `failures=0`**, on the tree to be
  tagged. Evidence so far: two hours on the round-three candidate, `b0e0b53`,
  `failures=0`.

## Added by the release fixes

- [ ] **The four presets, `models`, `tier3` and the suite under load** on the
  tree to be tagged (`ctest --preset models` is about half an hour; the `release`
  preset leaves the models out), and the six `SHMEM_THREAD_MULTIPLE` models
  once, configured with `-DOSOE_MULTIPLE_MODELS=ON`: the library provides
  `MULTIPLE`, and no preset runs them.
- [ ] **The README's commands on the Pi cluster as written**: the ring with
  `srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4`, the batch script with its
  secret, `env.probe` on every node, and `osoe-stat` on a running job's
  nodes.
- [ ] **`shmem_global_exit` under Slurm, by hand**, since no ctest runs under
  Slurm: `srun -K1 --cpu-bind=none -N1 -n4` with the caller's `atexit` handler
  taking 0 ms, 200 ms, 1 s and 5 s, the caller at PE 0 and at PE 3, and a
  status of 0 - every handler finishes and `srun` returns the caller's status.
- [ ] **The floor test's pins** with `OSOE_TEST_FLOOR_PINS=1`
  (`-DOSOE_TEST_PINS=ON`) on the machines they were set on.
- [ ] **x86-64 on the tagged tree**: it builds and passes its one-node tests
  (the `osoe-x86` VM, kept until the tag for this, at the author's word of
  2026-10-02), so that the README's sentence on architectures is true of it.

## Added by the round-four review

- [x] **Which history to publish** (the review's readiness Q5-3): 512 of the
  526 commits at the review predate the scrub of host-specific material from
  the tree, and the history still carries what the tree no longer does. The
  author decided on 2026-10-04: the whole history, as it is, as the record of
  the AI-assisted development (`docs/decisions/2026-10-04.md` item 2, with the
  scan behind it). The commits keep the author's address, and the public
  repository is named `shmem-over-ethernet` (the same record, item 4).

## After the release

What the reviews rated *after release*, kept here so that none is lost; none
blocks the tag.

- **The global exit's single job-wide carrier**, modelled first, which closes
  what the README's second limit leaves open (round four, conformance F1).
- **A hand-on wake for a reopened connection buffer** - a writer that reserves
  and leaves room wakes the next, a woken writer that finds none passes the
  wake on - `wait_word_model.py` first (scale S1).
- **The pipeline's window on links faster than gigabit**: measure, then the
  default (scale S2).
- **The x86-64 network path**, on any x86-64 Debian 13 host (portability Q3).
- **`osoe-stat --gc` reading a segment into private memory**, its status
  counting only what it failed to do (Q4); a Clang user's first command (Q5);
  a sandbox's refusal of futex2 named as one (Q6); the 33rd start within a
  minute (Q7); the `EPERM` hint naming the sysctl only when it is set (Q1).
- **`osoe-stat` past 64 segments on one machine** (readiness Q5-5); unknown
  `OSOE_` names, silent today (Q5-6); the messages that could say more (Q5-6);
  the check above the seam that belongs below it (Q5-7).
- **A lead that dies between making its segment and finishing it** (correctness
  F2); the relayed-goodbye test's sensitivity (F3); `env.probe`'s reopen check
  racing its child (F4); F5's small items.
- **Example 48's plain profiling names in C11** (conformance M1); a fractional
  `SHMEM_SYMMETRIC_SIZE` usable whole (M3); where the cache routines are
  declared (M5).
- **A test past 16 logical nodes** of the global exit and a proved deadlock
  (scale, minor 1).
- **Six of round four's minors** that this list had dropped (the review of step
  D's merge point, readiness): `OSOE_JOB_SECRET` not marked *checked* in the
  README's table; `SHMEM_INFO`'s subset; `oshcc -O2 -show`, the wrapper taking
  `-show` only as its first argument; `--static`; `osoe-launch` never printing
  the job id; `transport.h:536-539`'s sentence.
- **From the review of step D's merge point**
  (`docs/reviews/archive/phase1-step7-slice3-review.md`), each rated after the
  release or minor:
  - the release hold of a team's local release (design §3.8) has no test and no
    model: the correctness read's probe - a node stopped while 32 MiB streams
    to it - shows it engaging, and its removal ending the job (G1);
  - a PE's descriptor generation for a team number is 32 bits: taking one number
    2^32 - 1 times ends the job at the next split - hours on one PE, days on
    several (conformance F2, scale T2); 64 bits, or a generation that wraps;
  - `osoe-stat`'s counters view keeps 64 PEs a node (readiness), and a split
    waiting on another thread's pending number shows as `(+0xN)` there, wanting
    a blocked-record kind of its own;
  - a split called with other arguments on the PEs of one node is silent, and
    across nodes a proved deadlock: two more words in its reduction, a hash of
    the arguments and its complement, would fail it alike on every PE;
  - a busy round runs again at once, spinning a one-node parent's PEs (scale
    minor 4); 257 leads claiming at once start on one bitmap word; the
    engine's team array is zeroed twice (`progress.c:1141`), more than half of
    a block's 570 bytes on the lead;
  - nothing asserts that the shared 64-bit atomics are lock-free, and step D's
    four models run under sequential consistency only (portability Q4);
  - `posted_bitmap_model.py`'s ORDER check is reached by no listed breakage,
    and `take_arrival`'s comparison with the consumed word is redundant
    (correctness G3);
  - round two's C5, `iput` with a stride, dropped from the README and this list
    (conformance M2);
  - a start-up check that a node's PEs share one PID namespace (portability Q2,
    the refutation's sketch): each PE's `/proc/self/ns/pid` device and inode in
    its slot, compared by the lead at the join and refused by name - a layout
    change - which would turn a missed death and a false one into a refusal;
    and the deadlock detector reads node-mates' `/proc/<pid>/task`, which across
    namespaces names another process.
- **A team of one PE claims no pool block**, maps no name and publishes its
  epoch word without a block: a `split_2d` with `xrange` 1 makes one per PE, so
  on a node of 256 PEs four of them empty the default pool (step D's second
  part, its first adversarial read, item 5; documented in the README's
  limits).
