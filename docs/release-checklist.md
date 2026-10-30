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
  tree to be tagged (`ctest --preset models` is about an hour one at a time; the `release`
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

## For the 0.2 tag

Written 2026-10-10, from the items above that every tag needs, the after-release
list's one that names "the tag", and the author's answers of the v0.2 arc. Open
until the tree to be tagged has them; the author tags.

- [x] **Part 4 on a link above a gigabit**: 0.2 waits for the hardware (the
  author, `docs/decisions/2026-10-10.md` item 3, where `2026-10-09.md` item 3
  had let it ship without); `docs/fast-link.md` is its runbook. Done: three
  sessions on eight `c4-standard-16` nodes, 2026-10-12, 10-20 and 10-28
  (`docs/fast-link-results.md`), H1 to H19 with H25 and H26 closed.
- [x] **The review's findings settled on the candidate.** Round eight, at step
  4's merge point, said *release after named changes*, and they were made
  (`docs/decisions/2026-10-10-step4-merge-point.md`); every change since is read
  adversarially before it merges. The ninth, before Part 4, said the same,
  and its changes were made and read (`docs/decisions/2026-10-11.md`); a tenth
  runs before the tag, once every open item is cleared
  (`docs/prompts/task-v0.2-to-the-tag.md`) - begun 2026-10-30 on `5cefc7fb`,
  P1's merge, the last change of code an open item asked for. Done 2026-10-30:
  round ten, *tag after the named changes*, all three made the same day,
  nothing blocking (`docs/reviews/archive/v0.2-before-tag-review.md`).
- [x] **The four presets, `models`, `tier3` and the suite under load** on the tree
  to be tagged, at `OSOE_PROGRESS_HELPERS=0` set outright and at two and three
  where the merge point ran them, and with the setting unset, the default
  `auto` since 2026-10-28 - ThreadSanitizer's as `ctest` judges every test there
  since the ninth review's C9-1, its reports judged by `tsan.reports` at each
  count, none included, and no gate run apart; the six `SHMEM_THREAD_MULTIPLE` models with `-DOSOE_MULTIPLE_MODELS=ON`,
  last run at 0.1.0's tag; the single-node build in its presets and on Rocky
  Linux 9. Done 2026-10-30 (T1 to T3, `docs/decisions/2026-10-30.md`), on
  `0edd95a9` but the six `MULTIPLE` models, on `518f5a46`, its code but the
  version: nine builds without a warning; `release` and `debug` 1,214 of 1,214,
  `asan` 1,174 and `tsan` 841 at each of the four helper settings,
  `tsan.reports` passing at each; the 47 models the build registers, on the
  Mac; tier 3 138 of 138; the suite beside eight busy loops 1,214 of 1,214; the
  six `MULTIPLE` models 6 of 6; the single-node build whole in its presets, and
  on Rocky Linux 9.8 whole but four starts its 3.6 GiB VM's `/dev/shm` refuses
  by name, which pass with the check reporting instead.
- [x] **The SOS test suite on the tree to be tagged**, at four logical nodes of
  two PEs at `OSOE_PROGRESS_HELPERS=0` set outright and at two, and its team
  programs at 70 PEs, as at step
  4's merge point (`docs/decisions/2026-10-10-step4-merge-point.md`), which ran
  them before its review's fixes and every change since; and the README's
  sentence on Sandia OpenSHMEM's suite "on the tree tagged 0.1.0" made the
  tree tagged 0.2's (the ninth review's readiness Q5-4). Done 2026-10-30 (T4):
  161 of 165 at both counts, the four SOS's own; the team programs at 70 PEs
  20 of 20 at each; the README's sentence the tag's checks'.
- [x] **`shmem_global_exit` under Slurm by hand, and the floor test's pins**, as
  for 0.1.0 (above): the sixteen cases in the development VM's Slurm, and
  `OSOE_TEST_FLOOR_PINS=1` on the machines the pins were set on. Done
  2026-10-30 (T5, T6): sixteen of sixteen, on `518f5a46`, the code but the
  version; the pins held in the VM, three runs and tier 3's three floor tests,
  and on the Pi on the tree to be tagged, its round trips 1.09 to 1.10 times
  an io_uring echo's.
- [x] **The x86-64 item at two and three helpers too** (portability P3, under
  "After the release"), and the README's sentence on what "the tagged tree"
  did on x86-64 made 0.2's (readiness Q5-4). Until an x86-64 kernel runs a
  timing test of the helpers' wakes, what stands for x86's order on them is
  `lead_wake_model.py`, the owners' wake, and `helper_request_model.py`, the
  helpers' requests, which run under x86-TSO with breakages that must fail
  (portability P3's third fix, raised again by the ninth review's P9-5). Done
  2026-10-12 (T7): natively at two and three helpers on the cluster's first
  session, and the README's sentence with it (`docs/decisions/2026-10-12.md`).
- [ ] **An hour of `mix` on the Pi cluster, `failures=0`**, on the tree to be
  tagged, and the runbook end to end with its floor: no 0.2 tree has had either.
- [x] **The README's commands on the Pi cluster, and its quick start on Raspberry
  Pi OS**, as for 0.1.0. Done 2026-10-30 on the tree to be tagged (T11, T12):
  the quick start on the head node, a Pi 5, in 119 s with no warning, both
  rings and the probe; the installed `oshcc`'s ring under `srun`; the probe on
  every node; `osoe-pacer --quick`; the batch script with its secret; and
  `osoe-stat --summary` on every node of a running job through `srun --overlap`.
- [ ] **The release note**: `docs/release-notes/0.2.0.md`, drafted 2026-10-10 and
  fact-checked against its sources, made true of the tree tagged - its numbers
  marked "at the tag" filled in from the runs above, its provisional checks
  replaced by the tag's, anything merged after `5748c97` added, and its line on
  0.1.0's `osoe-pacer mix` hang kept (`docs/decisions/2026-10-09.md` item 4,
  the author: "fine").
- [x] **The version**: `CMakeLists.txt`'s `0.1.0` to `0.2.0` in the commit the tag
  names, with the README's places that give the current version - its Status,
  the version strings and the library's file name, the repository table's row,
  `SHMEM_VERSION`'s row - and the README's Status linking the new note. Done
  2026-10-30 (X2), before the tag's last runs, so that they run on the version
  the tag names; every test of a version takes it from the project's.
- [x] **The soname**: `libosoe.so.0`, as 0.1.0's (the author,
  `docs/decisions/2026-10-10.md` item 4) - the installed headers and the
  exported names are 0.1.0's, so a program built against 0.1.0 links and runs
  against 0.2: the ninth review's investigators built the README's ring against
  a 0.1.0 install and ran it once 0.2 was installed over it, on one node and
  two, with the right lines. The library's file is `libosoe.so.0.2.0`.
- [x] **The upgrade run again on the tree tagged**: a program built against the
  0.1.0 install, run against the tagged library installed over it (readiness
  Q5-3, Q5-4). Done 2026-10-30 (T8): the README's ring built against 0.1.0's
  install printed its four lines on one node and on two against
  `libosoe.so.0.2.0` installed over it, not rebuilt.
- [x] **If Part 4 turns the zero-copy send on by default**, its locked-memory
  questions are the tag's, since every user with a low `ulimit -l` then meets
  them: whether a connection that met the limit lends again after a while,
  where today it copies for the rest of the job (readiness Q5-4; `shmem_init`
  raises the soft limit to the hard one since 2026-10-11). Not so: Part 4's H8
  left it off, the author's A3 (`docs/decisions/2026-10-20.md`), so the
  questions stay after the release.

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
  - ~~`osoe-stat --summary`'s walk of a data collective: the generic phase
    publishing its state as it waits, as the progress thread does a barrier's
    (readiness Q5-1; the README says what the view walks today)~~ - done
    2026-10-27 (D1, node layout 42: a lead's waits name the node whose word
    ends them, but where one count of several leads' says how many);
  - io_uring's own device polling (`IORING_REGISTER_NAPI`, Linux 6.9 and later) as an opt-in
    setting, timed on a cluster: a site's busy polling reached nothing of osoe's receives
    (H27; the author, 2026-10-20, N3);
  - ~~an `osoe-pacer` exercise of collectives on per-thread teams under
    `SHMEM_THREAD_MULTIPLE`, so that the stress tester can see a lane shared by
    threads (correctness H1; `shmem.lanes.threads` sees it today)~~ - done
    2026-10-20 (T25: `threads`' fifth phase);
  - ~~step E's two models under the checker's weaker orders, not sequential
    consistency only~~ - done 2026-10-24 (T29: `lane` and `scan_pipeline` under
    x86-TSO); ~~the PID-namespace message when pids are shifted rather
    than repeated~~ - done 2026-10-27 (D26: the creator's namespace beside its
    pid); an x86-64 tsan build's heap placement (portability Q5);
  - a real x86-64 kernel (portability's third step for the x86-64 item).
- **The 1.6 session routines' semantics, should they ever be more than
  no-ops** - what design §10's "thorough 1.6 spec read" had left when the author
  closed it (`docs/decisions/2026-10-04.md` item 11).
- ~~**The global exit's single job-wide carrier**, modelled first, which closes
  what the README's second limit leaves open (round four, conformance F1)~~ -
  done 2026-10-30 (P1: modelled 2026-10-29, built at the author's word, the
  lowest-ranked of the nodes' carriers the job's).
- ~~**A hand-on wake for a reopened connection buffer** - a writer that reserves
  and leaves room wakes the next, a woken writer that finds none passes the
  wake on - `wait_word_model.py` first (scale S1)~~ - done 2026-10-29 (P2: the
  room count the default, `OSOE_TURNOVER_WAKE`, the hand-on a setting).
- ~~**The pipeline's window on links faster than gigabit**: measure, then the
  default (scale S2)~~ - done 2026-10-28 (P37: timed at the third cluster
  session, the default 8).
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
  item 4; T17): one-node variants of `shmem.leftovers.*`,
  `shmem.dead-pe.lead-in-init` and `shmem.job.0*`, and of the pacer's planted
  defects, so that the single-node build runs them and something in it shows
  the stress tester's oracles can fail (the step's review, readiness Q5-7).
  Built: `shmem.leftovers.reclaim.one-node` and `.live.one-node`, which plant
  the control segment alone - a job of one node makes no other object, so
  `start-leftovers.sh` and `restart-leftovers.sh` have no form there - and every
  planted defect but `mailbox-round-trip`, whose round trips run across nodes
  alone, and the relay's three (2026-10-24, T26), which need three nodes, on
  `0,0,0,0` (`tools.pacer.plant.*.0-0-0-0`, and `start_pes`'s). Left:
  `shmem.dead-pe.lead-in-init`, whose window, the job's rendezvous held open by
  a node that starts late, a job of one node has not - struck, or built with a
  shim that holds some other step of init open, as `slow-create.c` holds a
  creation, the author's choice; and `shmem.job.0*`, struck, since
  `shmem_test_job` compiles the internode backend's files
  (`docs/decisions/2026-10-06-single-node-build.md`, item 4 of its adversarial
  read).
- **What v0.2 step 2 left to after its merge** (its adversarial read, findings
  10 to 12, `docs/reviews/archive/v0.2-step2-adversarial-read.md`): multi-node
  tests on the measured link take shapes that vary from start to start, so pin
  `OSOE_LINK_*` in them or print the link on failure; a divergence across a
  crossover ends by the deadlock detector where one tree let a header name it -
  the call's fan-out could travel in the grant or the lane word; the link is
  measured on two and three nodes, where no fan-out differs.
- **What v0.2 step 3 left to after its merge** (its adversarial read, findings
  2 and 7, `docs/reviews/archive/v0.2-step3-adversarial-read.md`, and its
  record, `docs/decisions/2026-10-07-circulant-data-path.md`): ~~on two nodes the
  cost model's choice between the tree and Träff's follows how a size divides
  into 16 KiB pieces, a tree's cost charging a partial last piece whole - take
  the tree on two nodes, or charge its last piece by its bytes (step 2's fit)~~ -
  done 2026-10-29 (P4: the last piece charged its bytes, two nodes of the Pi's
  link taking the tree at every size, `docs/decisions/2026-10-29-two-node-choice.md`);
  Träff's at the default ceiling's last row (257 to 512 nodes) only through a
  lower ceiling's tests; the cost model's Träff's optimistic against the Pi from
  256 KiB, and `osoe-bench`'s back-to-back 64 KiB reduction no faster for it -
  both worth a look when the progress helpers change what a node carries both
  ways at once (looked at on 2026-10-12, H21 in `docs/pi-cluster-results.md`:
  the optimism stands at every helper count, and the 64 KiB was the binding).
- ~~**A lead that dies between making its segment and finishing it** (correctness
  F2)~~ - done 2026-10-24 (P11, in a start with a launch number); the relayed-goodbye test's sensitivity (F3); `env.probe`'s reopen check
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
  - ~~the release hold of a team's local release (design §3.8) has no test and no
    model: the correctness read's probe - a node stopped while 32 MiB streams
    to it - shows it engaging, and its removal ending the job (G1)~~ - done
    2026-10-25 (T16: `team_block_model.py`'s control queue, and
    `shmem.team-release.held.0-1-2` through `OSOE_TEST_HOLD_RELEASE`);
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
  - nothing asserts that the shared 64-bit atomics are lock-free, and ~~step D's
    four models run under sequential consistency only~~ (portability Q4) - done
    2026-10-24 (T29: the four under x86-TSO as well);
  - `posted_bitmap_model.py`'s ORDER check is reached by no listed breakage,
    and `take_arrival`'s comparison with the consumed word is redundant
    (correctness G3);
  - round two's C5, `iput` with a stride, dropped from the README and this list
    (conformance M2);
  - ~~a start-up check that a node's PEs share one PID namespace (portability Q2,
    the refutation's sketch): each PE's `/proc/self/ns/pid` device and inode in
    its slot, compared by the lead at the join and refused by name - a layout
    change - which would turn a missed death and a false one into a refusal;
    and the deadlock detector reads node-mates' `/proc/<pid>/task`, which across
    namespaces names another process~~ - done 2026-10-27 (D26, node layout 42:
    compared by every PE at the join, a node-mate judging its lead's pid only
    from inside the lead's namespace until then).
- ~~**A team of one PE claims no pool block**, maps no name and publishes its
  epoch word without a block: a `split_2d` with `xrange` 1 makes one per PE, so
  on a node of 256 PEs four of them empty the default pool (step D's second
  part, its first adversarial read, item 5; documented in the README's
  limits)~~ - done 2026-10-27 (P7, node layout 42).
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
- **From the review of v0.2 step 4's merge point**
  (`docs/reviews/archive/v0.2-step4-review.md`), each rated after the release
  or minor, none to be made before the tag, and two with a half built already
  (2026-10-10):
  - the ThreadSanitizer gate at a helper count tells the library's own
    collective words in the heap mapping from a program's heap, passes a
    report there only against a short list with a reason each, and requires a
    helper's frame in every report it passes (correctness Q2-2). *Done
    2026-10-11*, with the ninth review's C9-1 (`cmake/thread-sanitizer-gate.py`,
    `docs/decisions/2026-10-11.md`);
  - ~~a white-box test of the deadlock detector's helper half - an answer naming
    the request posted last, the helpers' applied counts in the activity, the
    pass after the request - each removal failing it (correctness Q2-3)~~ - done
    2026-10-24 (T15: `shmem.deadlock.helper-stale`, `-busy` and `-slow`);
  - the PEs' waits guarded against spinning, as the helpers' and, since
    2026-10-10, the agent's idle waits are by `shmem.helper-wake`'s two runs
    (scale S1; the step 1 review's Q3-1, half closed;
    `docs/decisions/2026-10-10-helper-wake.md`);
  - ~~Träff's rounds take a lane's rows, which the fan-out ceiling sizes, so past
    512 nodes at the default the cost model's best reduction is out of reach - by
    its own numbers 3.2 to 4.3 times faster at 1 MiB (scale S2): rows sized
    apart from the ceiling, before hundreds of nodes~~ - done 2026-10-25 (P3: a
    lane's rows the more of the ceiling's + 1 and Träff's rounds, to 65,536 nodes);
  - the receive pool's re-arms grow with the peers sending into a node at once
    (scale S3): a fan-in at many peers on the fast link, with
    `transport.am.receive_enobufs` beside the rate - Part 4's;
  - the mesh's accept loop rebuilding its notice of the missing nodes before
    every accept, O(n²) on the highest lead, and the ring's own ceiling of 8,192
    nodes refused as an io_uring error rather than by name (scale S7);
  - with the zero-copy send's default, Part 4 decides what its refusal for
    want of locked memory keeps (portability P1, built 2026-10-10:
    `docs/decisions/2026-10-10-zero-copy-locked-memory.md`): whether a
    connection that met the limit lends again after a while, where now it
    copies for the rest of the job (whether `shmem_init` raises the soft
    locked-memory limit to the hard one, as it does for open files, the author
    decided on 2026-10-11, for the rings on Linux 6.14 and later: it does,
    zero copy on or off);
  - ~~the backend's own threads named to the deadlock detector through
    `internode-backend.h`, not AM's owner records (readiness Q5-8)~~ - done
    2026-10-20 (P28: `transport.h`'s `backend_thread`, which AM answers from its
    owner records), and each owner's last pass shown with its age in
    `osoe-stat` (Q5-9);
  - a lead with helpers writing a death twice, its agent's line and a helper's,
    and "this PE was not waiting in the library" said of a PE in
    `shmem_finalize` (readiness Q5-3; the README quotes the lines as they are) -
    and a non-lead at no helpers wrote "the job is aborting on behalf of PE 0"
    twice once in about 25 failing runs, the line having two writers, `abort.c`
    and `fatal.c`, and no guard of once a process (the ninth review's readiness
    Q5-4);
  - ~~Träff's on teams that are not the world, typed reductions in place at its
    sizes, the whole-job `_to_all` at them, members' bits after a rounding sum,
    nodes at different helper counts~~, and SOS at three helpers, each right by
    the review's probes and none in the suite (conformance C3) - done 2026-10-25
    (T20) but SOS at three helpers, a run of the tag's (T4);
  - the x86-64 item at the next tag run at two and three helpers too
    (portability P3);
