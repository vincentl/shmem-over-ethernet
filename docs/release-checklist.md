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
- [x] **The SOS test suite clean** but for its own extensions (design §8.6; the
  runbook's §8 names them) - now including runs past 61 PEs once the teams are
  rebuilt: `shmem_team_reuse_teams` and `shmem_team_max` at 70 PEs, and
  `reduce_active_set`, `broadcast_active_set` and `repeated_barriers` at 70
  PEs with no iteration skipped, and `collect_active_set` at 32, the most it
  runs on (`docs/prompts/done/phase1-step7-slice3-prompt.md`, acceptance; met
  on step D's tree at 128 teams, `docs/decisions/2026-10-03-team-allocation.md`,
  "Acceptance", and at step E's merge point at the default of 256,
  `docs/decisions/2026-10-04-step-e-merge-point.md`; to be run again on the
  tree to be tagged). Met on the tagged tree, 2026-10-05: 161 of 165 at 4 × 2, the acceptance at 70 PEs and `collect_active_set` at 32 (`docs/decisions/2026-10-05-release-point.md`).
- [x] **The Pi runbook green end to end** on the cluster as rebuilt on
  2026-09-28 - the tree copied and built per `docs/pi-cluster.md` §2 - with the
  floor ratios, `osoe-bench` and the collective sweeps recorded in
  `docs/pi-cluster-results.md`. Green end to end on the tagged tree, 2026-10-05: the floor, `osoe-bench`, the corpus and every pacer run in `docs/pi-cluster-results.md`, "The release (2026-10-05)" (`docs/decisions/2026-10-05-release-point.md`).
- [x] **A license** (GNU LGPL, version 3 or any later version), **`cmake
  --install`**, and **a README enough for a stranger** to build and run a
  program on a Slurm cluster like the author's - read once more as the
  stranger on the tagged tree on 2026-10-05, by a fresh agent: enough, its
  fixes made (`docs/decisions/2026-10-05-release-point.md`).
- [x] **A version tag and a release note** listing the known limits: whatever
  the last Pi session measured short of the floor, and the README's Limits.
  The version is 0.1.0 (`docs/decisions/2026-10-02.md` item 7, D8). The
  author tags. **The commit the tag names says so**: the README's opening said
  "osoe has not made a release yet: there is no version tag" (round six's
  readiness Q5-5), and the README is true at every commit; on the release
  commit it says this is 0.1.0, the first release. The release note is written,
  `docs/release-notes/0.1.0.md`; the release point verified the tree
  (`docs/decisions/2026-10-05-release-point.md`), and what the author asked for
  the morning after went in before the tag, documents only
  (`docs/decisions/2026-10-05-before-the-tag.md`). Done 2026-10-05: the author
  tagged `53b8f0f` as `v0.1.0`, an annotated tag, "osoe 0.1.0".

## Before every release tag (the decisions index)

- [x] **An adversarial review of the release candidate, its findings settled.**
  Round three (2026-09-28, `docs/reviews/archive/phase1-release-review.md`)
  said *release after named changes*: the release fixes and the team redesign.
  Round four, at the fixes' merge point (2026-10-03,
  `docs/reviews/archive/phase1-release-fixes-review.md`), said the same, its
  named changes merged on 2026-10-04 (`docs/decisions/2026-10-04-review-fixes.md`).
  The full review ran again after the teams' step D - at its merge point,
  `docs/reviews/archive/phase1-step7-slice3-review.md` - and after step E, on
  the release candidate: round six, `docs/reviews/archive/phase1-step7-slice4-review.md`,
  *release, after the named changes*, every one made
  (`docs/decisions/2026-10-05-step-e-review-fixes.md`).
- [x] **An hour of `mix` on the Pi cluster, `failures=0`**, on the tree to be
  tagged. Evidence so far: two hours on the round-three candidate, `b0e0b53`,
  `failures=0`. Done on the tagged tree, 2026-10-05: `failures=0` in 3,607 s, and an hour of `threads` in a debug build too (`docs/decisions/2026-10-05-release-point.md`).

## Added by the release fixes

- [x] **The four presets, `models`, `tier3` and the suite under load** on the
  tree to be tagged (`ctest --preset models` is about half an hour; the `release`
  preset leaves the models out), and the six `SHMEM_THREAD_MULTIPLE` models
  once, configured with `-DOSOE_MULTIPLE_MODELS=ON`: the library provides
  `MULTIPLE`, and no preset runs them. Done for the tagged tree, 2026-10-05: the presets, tier 3 and the models on its code, the suite under eight busy loops 718 of 718, the six MULTIPLE models 6 of 6 (`docs/decisions/2026-10-05-release-point.md`).
- [x] **The README's commands on the Pi cluster as written**: the ring with
  `srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4`, the batch script with its
  secret, `env.probe` on every node, and `osoe-stat` on a running job's
  nodes. Done 2026-10-05: the ring, the batch script with its secret, the probe on every node, and `osoe-stat --summary` on every node of a running job through `srun --overlap`, which the README now prints (`docs/decisions/2026-10-05-release-point.md`).
- [x] **The README's quick start on Raspberry Pi OS**, which it names and has
  run only on Debian (round six's readiness Q5-5): on one Pi node, an ssh
  session held open (`RemoveIPC`, the runbook), a fresh copy of the tagged
  tree, the quick start's commands as printed - both rings print the README's
  four lines and exit 0, and the probe passes. Done 2026-10-05 on the cluster's head node, Raspberry Pi OS Trixie on a Pi 5: built in 72 s with no warning, both rings, the probe (`docs/decisions/2026-10-05-release-point.md`).
- [x] **`shmem_global_exit` under Slurm, by hand**, since no ctest runs under
  Slurm: `srun -K1 --cpu-bind=none -N1 -n4` with the caller's `atexit` handler
  taking 0 ms, 200 ms, 1 s and 5 s, the caller at PE 0 and at PE 3, and a
  status of 0 - every handler finishes and `srun` returns the caller's status. Done 2026-10-05 in the development VM's Slurm, all sixteen cases - the caller at PE 0 and PE 3, handlers of 0 ms to 5 s, statuses 0 and 7 (`docs/decisions/2026-10-05-release-point.md`).
- [x] **The floor test's pins** with `OSOE_TEST_FLOOR_PINS=1`
  (`-DOSOE_TEST_PINS=ON`) on the machines they were set on. Done 2026-10-05: the release preset with the pins on, on the development VM where they were set, as part of the tagged code's verification; on the Pi every ratio well inside them (`docs/decisions/2026-10-05-release-point.md`).
- [x] **x86-64 on the tagged tree**: it builds and passes its one-node tests
  (the `osoe-x86` VM, kept until the tag for this, at the author's word of
  2026-10-02), so that the README's sentence on architectures is true of it -
  every test Rosetta can run, 369 of 716 at round six, the rest refused io_uring
  or seccomp by the emulator. And, since no lanes test runs on one node, the
  aarch64 build made to order memory as x86-64 does
  (`docs/reviews/archive/phase1-step7-slice4-portability.md`, its appendix): its
  suite, the lanes tests repeated, and `mix` across logical nodes, about ten
  minutes - the one run that puts x86-64's memory order on step E (round six's
  portability Q1). Done 2026-10-05: 369 of 718 under Rosetta, every failure the emulator's; the x86-ordered aarch64 build 718 of 718, the lanes tests 1,250 times and `mix`, `failures=0` (`docs/decisions/2026-10-05-release-point.md`).

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
blocks the tag. Round three's own after-release list is
`docs/reviews/archive/phase1-release-review.md`'s, with its readiness report's
three diagnosis items (`phase1-release-readiness.md`, Q5's list): of those, what
round six's readiness review found still as it was - "on behalf of PE N"
naming the PE that noticed rather than the cause, and `osoe-stat` showing a
stopped node as a waiting one - and the rest of that list stand as round three
rated them (round six's readiness Q5-5).

- **From round six** (`docs/reviews/archive/phase1-step7-slice4-review.md`):
  - `osoe-stat --summary`'s walk of a data collective: the generic phase
    publishing its state as it waits, as the progress thread does a barrier's
    (readiness Q5-1; the README says what the view walks today);
  - an `osoe-pacer` exercise of collectives on per-thread teams under
    `SHMEM_THREAD_MULTIPLE`, so that the stress tester can see a lane shared by
    threads (correctness H1; `shmem.lanes.threads` sees it today);
  - step E's two models under the checker's weaker orders, not sequential
    consistency only; the PID-namespace message when pids are shifted rather
    than repeated; an x86-64 tsan build's heap placement (portability Q5);
  - a real x86-64 kernel (portability's third step for the x86-64 item).
- **The 1.6 session routines' semantics, should they ever be more than
  no-ops** - what design §10's "thorough 1.6 spec read" had left when the author
  closed it (`docs/decisions/2026-10-04.md` item 11).
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
  `OSOE_` names, silent today (Q5-6); the messages that could say more (Q5-6).
  The check above the seam that belonged below it (Q5-7) is done: v0.2 step 1's
  Part 1 moved it into the transport's settings check.
- **What v0.2 step 1 left to a later step** (its record's adversarial read,
  item 4): one-node variants of `shmem.leftovers.*`, `shmem.dead-pe.lead-in-init`
  and `shmem.job.0*`, and of the pacer's planted defects, whose
  `PacerPlant.cmake` launches `0,0,1,1` - so the single-node build runs none of
  the 77, and nothing in it shows the stress tester's oracles can fail (the
  step's review, readiness Q5-7).
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
- **From the review of v0.2 step 1's merge point**
  (`docs/reviews/archive/v0.2-step1-review.md`), each rated after the release:
  - a test that nothing in the library spins (scale Q3-1), best before v0.2
    step 4, whose helpers rewrite the progress threads' waits: more PEs than
    the machine has cores, PE 0 asleep outside the library for a second or
    two, and the job failing when the others and their agents used more than a
    few percent of the machine - in both builds, not labelled `internode`. A
    wait that spins 200 µs before it sleeps passes both suites today, the floor
    pins among them, and makes a 32-PE barrier on four cores 24 to 35 times
    slower;
  - the seam for a backend not of AM's shape (readiness Q5-8): design §4.10's
    note to say what `internode-backend.h` is - AM's own division, which an
    RDMA backend does not keep - and which of the runtime's functions a backend
    may call: the AM backend calls 38 of the rest of `libosoe` and 21 of
    `base/`;
  - a test that fails without the one-node quiet's fence (correctness K2) -
    all of quiet in the single-node build, and on x86-64 the only thing that
    orders a store before a later load there: on aarch64 the counters' `ldar`
    and `stlr` hide its absence, and the investigator's store-buffering litmus
    through `shmem_quiet` found the forbidden outcome 109 times in 500,000
    rounds without it only with the library built `-march=armv8.3-a`, whose
    acquire loads are `ldapr`. The litmus as a tier-2 test of two PEs on one
    node, built so where `ldapr` is emitted and the CPU has it, and on x86-64
    when a machine exists;
  - for whoever runs the x86-64 items next: the futex2 shim copied into the
    VM's own file system - read from the host's mount, Rosetta's translation
    daemon stalled the full x86-64 suite after 244 of its tests (portability
    Q9).
- **The mesh's start-up schedule** (`docs/design/mesh-schedule-proposal.md`),
  once a system large enough to measure it is at hand: the author postponed it
  on 2026-10-05 (`docs/decisions/2026-10-05-before-the-tag.md` item 2), and the
  README's limits say the start-up is a chain a larger system may need changed.
