# 2026-10-05: the release point

The last stretch of `docs/prompts/task-first-release.md`: the release point's
conditions (design §1) and everything on `docs/release-checklist.md`, checked
on the tree to be tagged - `main` at `beae139`, round six's review and its
fixes on step E's merge point - by Claude, through the night of 2026-10-04 to
05 at the author's word: "work as long as you can in the next session window
to close as many release items you can independently. Then tomorrow I can do
my part." The author's part: the tag, and publishing the repository.

The tree checked is `beae139`, its archive extracted for every run; where a run
used `81ba75a`, the verified tree of round six's fixes, the code is the same -
`beae139` differs from it only in text. This record's own commit, the release
commit, changes text alone: the README's opening for the release, the release
note, the checklist's ticks, this record and the Pi's results.

## In the development virtual machines

- **The four presets, tier 3 and the models** (round six's fixes' verification
  on `81ba75a`, `docs/decisions/2026-10-05-step-e-review-fixes.md`): debug 718 of
  718, release 718 of 718 with the floor pins on, asan 715 of 715, tsan 490 of
  490; tier 3 113 of 113; the models 30 of 30.
- **The six `SHMEM_THREAD_MULTIPLE` models**, configured with
  `-DOSOE_MULTIPLE_MODELS=ON`: 6 of 6 in 22 minutes, `model.get_completion` the
  longest at 786 s.
- **The suite under eight busy loops** (`osoe-load`): 718 of 718.
- **The SOS suite at 4 × 2**: 161 programs clean; the two C11 programs over
  `short *`, SOS's extension, do not build; `shmem_team_b2b_collectives` and
  `shmemalign` fail as the runbook says. **Past 61 PEs**, at the default of 256
  team numbers, 70 PEs on one node and on ten logical nodes of seven:
  `shmem_team_reuse_teams`, `shmem_team_max`, `shmem_team_split_2d`,
  `reduce_active_set`, `broadcast_active_set` and `repeated_barriers` exit 0 with
  no split failing and nothing skipped; `collect_active_set`, which refuses past
  32 PEs by its own limit, exits 0 with its 32 active sets at 32 PEs on one node
  and on four; the per-node split of `SHMEM_TEAM_SHARED` valid on every node at
  61, 62 and 64.
- **x86-64** (`osoe-x86`): built under Rosetta with no warning, 369 of the
  suite's 718 tests passing and every failure the emulator's - io_uring refused
  (326), a ring opened (11), seccomp refused (9), the static link's nested
  aarch64 archive, `ldd` on another architecture, the differential runner's
  io_uring; and the aarch64 build made to order memory as x86-64 does (round
  six's portability review): 718 of 718, the lanes, scan and pipeline tests and
  their kin 1,250 times with no failure, and five minutes of `mix` at 16 PEs on
  four logical nodes, `failures=0`.
- **The stranger's path**, from a clean copy in `osoe`: the README's quick start
  as printed - the build in 19 s with no warning, the ring's four lines on one
  node and on two logical nodes, the probe; `cmake --workflow --preset release`,
  718 of 718 in 536 s; `cmake --install`, which put `oshcc`, `osoe-pacer` and
  `osoe-stat` in the prefix; the ring built by the installed `oshcc` and run by
  `srun`.
- **`shmem_global_exit` under Slurm, by hand** (`srun -K1 --cpu-bind=none -N1
  -n4`): the caller at PE 0 and at PE 3, its `atexit` handler taking 0 ms, 200
  ms, 1 s and 5 s, a status of 0 and of 7 - in all sixteen, `srun` returned the
  caller's status and the handler finished, the job's end its handler's time and
  54 to 176 ms more.
- **A stranger's read of the README** on the tagged tree, by a fresh agent: the
  README is enough for its reader; its fixes are in this commit - the opening for
  the release with x86-64's standing in it, the steps on a Slurm cluster in
  order, `osoe-stat` on every node of a Slurm job, what a report of a problem
  holds, the models' count and command, and figures grown stale. Left for after
  the release: the containers' paragraphs belong under running, not building.

## On the Raspberry Pi cluster

The runbook end to end (`docs/pi-cluster.md` §1 to §8), the tree copied to
`~/osoe` and built fresh per §2, in four parts from the Mac and the rest run
detached on the head node, so that no connection from the Mac need stay open
for hours (`docs/pi-cluster-results.md`, "The release (2026-10-05)"; the raw
outputs and scripts under `docs/design/evidence/pi-cluster/2026-10-05-release/`):

- the release build with no warning; the probe ok on every node; tier 1 86 of
  86 and tier 2, on a node, 626 of 626; the smoke test on block and cyclic
  maps;
- **the floor**: the fastest round trips 1.090 to 1.099 times an io_uring
  echo's, the stream 0.999 of its rate - every pin held with room;
- `osoe-bench` at four shapes, its lines step D's merge point's, the 64 KiB
  reduce and broadcast 0.6 and 2% faster with a 16 KiB piece;
- the endings: a global exit's status from every task, a dead PE named;
- the corpus as the runbook expects: NPB IS, PRK, ISx, 64 of the 72 examples
  and the other eight as documented, OSU's nineteen programs, ssca1's three
  alignments, the SOS suite 161 of 165;
- the README's commands: the quick start on the head node, Raspberry Pi OS
  Trixie on a Pi 5, in 72 s; the installed `oshcc`'s ring under `srun`; the
  batch script with its secret; `osoe-stat --summary` on every node of a running
  job;
- every `osoe-pacer` run of §6, `failures=0`: the default set (ten exercises
  now, 3,014 s, where the runbook still said five minutes each for two; it
  says so now), the three
  sorts, mailbox, teams, atomics, rma, locks, memory, collectives, waits,
  deprecated, `start_pes`, the teams' exhaustion and the counted runs;
- **an hour of `mix`**, the checklist's: `failures=0` in 3,607 s;
- **an hour of `threads`** in a debug build with `OSOE_CHECK_QUIET`,
  `OSOE_TEST_PERTURB` and an overfilled landing zone: `failures=0` in 3,600 s, and no `OSOE_CHECK_QUIET:` line.

## What is left for the author

- **The tag**, on the release commit, and **the repository published** - the
  whole history, as decided (`2026-10-04.md` item 2).
- Optionally, **`RemoveIPC=no`** on the Pi's nodes, which needs sudo.
