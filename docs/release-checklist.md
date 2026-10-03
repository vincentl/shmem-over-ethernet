# Release checklist

**Draft, written 2026-10-02 - the author has not read it yet.** Moved into the
tree from the author's status page at the author's word ("yes, move checklist";
`docs/decisions/2026-10-02.md` item 5), by the release-fixes brief
(`docs/prompts/task-release-fixes.md`, Part 5 item 3). Its items are the
page's own eight, design §1's release point, the decisions index's lines on a
release, and what the release fixes added. A ticked item says where it was
done; an open one, what is left.

## The release point (design §1)

- [x] **OpenSHMEM 1.5 complete**: contexts and `SHMEM_THREAD_MULTIPLE`, phase 1
  step 8 in three slices.
- [x] **OpenSHMEM 1.6 complete**, but for the known non-conformance design
  §7.4 records: the scans, the deprecated routines' profiling names, and
  partial active sets.
- [ ] **The SOS test suite clean** but for its own extensions (design §8.6; the
  runbook's §8 names them) - now including runs past 61 PEs once the teams are
  rebuilt: `shmem_team_reuse_teams` and `shmem_team_max` at 70 PEs, and
  `reduce_active_set`, `broadcast_active_set`, `collect_active_set` and
  `repeated_barriers` at 70 PEs with no iteration skipped
  (`docs/prompts/phase1-step7-slice3-prompt.md`, acceptance).
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
  The full review runs again after the fixes, after the teams' step D and
  after step E (`docs/prompts/task-first-release.md`).
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
