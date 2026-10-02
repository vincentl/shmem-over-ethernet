# Load survey: which tests fail when the machine is busy

Phase 1, step 2, slice 1 (the in-process AM backend). This is a mechanical
survey, not a diagnosis: for each failure the assertion or timeout is quoted
and the reporting PE is noted where the log makes that determinable, but root
causes in the source are left for a follow-up review.

## Method

All work ran in `<repo>/build/scratch/review-load/tree`,
a private rsync'd copy (`--exclude build --exclude corpus --exclude .git`), built
once and never rebuilt again for the rest of the survey:

```
mkdir -p <repo>/build/scratch/review-load
rsync -a --delete --exclude build --exclude corpus --exclude .git \
  <repo>/ <repo>/build/scratch/review-load/tree/
limactl shell osoe sh -c 'cd .../tree && cmake --preset release && cmake --build --preset release -j4'
```

Build directory: `tree/build/release`. No tracked file in the main tree was
touched; no build ran in `build/debug|release|asan|tsan` of the main tree.

1. **Baseline, no load:**
   `ctest --test-dir build/release -j2 --output-on-failure`
2. **Load:** in the VM, `for i in $(seq 16); do (while :; do :; done) & done`
   (16 spinners, 4x the VM's 4 cores), started detached and left running for
   the whole of steps 3-4; `uptime` checked before/during/after to confirm
   `load average` (1-minute figure) stayed above 12.
3. **Full suite x5 under load:**
   `ctest --test-dir build/release -j2 --timeout 120 --output-on-failure`,
   output of each run saved to
   `docs/reviews/evidence/phase1-step2-slice1/load-survey/run-{1..5}.log`.
4. **Isolated 30x repeats under load**, for every test that failed in step 3
   (none did) plus every test matching
   `internode|multinode|differential|barrier|wire|progress|quiet|signal|wait|stat|watchdog|landing|reply`
   (52 tests — the full match set, nothing skipped):
   `ctest --test-dir build/release -R '^<exact name>$' --repeat until-fail:30 --timeout 120`
   Per-test logs kept in the private tree at
   `build/scratch/review-load/isolated-logs/<test-name>.log` (not copied into
   the evidence tree; only the one failure's log is, per step 5).
5. For the one test that failed, captured failure output and classified the
   symptom (below); noted which node/PE reported it, or that the log does not
   say.
6. Killed the load (`pkill -f 'while :'` in the VM), confirmed `uptime` settled,
   and re-ran the full suite once clean.

## VM load during runs

`uptime` samples (1-minute load average; 4 real cores, 16 spinners running):

| When | load average (1 min) |
|---|---|
| Just after starting spinners | 14.43 |
| During full-suite run 1 | 15.59 |
| During full-suite run 5 | 17.35 |
| Just before starting the isolated-repeat loop | 16.75 |
| Midway through the isolated-repeat loop | 17.43 / 16.70 |
| Immediately after `pkill` | 14.94 (decaying) |
| ~20s after `pkill` | 11.70 (decaying) |
| Before the final clean run | 8.78 (decaying), spinners confirmed gone by `ps` |

Load stayed above 12 for the whole of steps 2-5.

## Results

**Baseline (no load):** 131/131 passed, total ctest wall time 9.66s.

**Full suite x5 under load:** 131/131 passed in every one of the 5 runs.
No test failed or timed out in the full-suite runs. Total ctest wall time per
run: 15.34s, 15.04s, 15.81s, 15.57s, 15.64s (vs. 9.66s unloaded — about
1.6x slower end-to-end, as expected with 2 workers on 4 heavily oversubscribed
cores).

**Isolated 30x-repeat runs:** of the 52 tests matching the multi-process
pattern, 51 passed all 30 repeats cleanly. One did not: `shmem.wait.L2`.

| test | failures / 30 (step 4) | additional reruns | symptom | reporting node/PE |
|---|---|---|---|---|
| `shmem.wait.L2` | 1 (repeat stopped at attempt 17 of 30, per `until-fail` semantics) | 0/30 on an immediate retry; 1 more failure at attempt 6 of a 200-attempt batch run to capture verbose output (2 failures / 53 total single-test invocations, ~3.8%) | timeout (60.02s, matching the launcher's internal per-job `-t 60`), preceded by two failed `CHECK`s | not determinable from the log — see below |

All other 51 tests: 0/30 failures. This includes every `internode.*`,
`multinode.*` (including `force-remote.*`), `differential`, `barrier-am.*`,
`barrier.L{1,2,4}`, `wire`, `wire-pair`, `progress`, `signal.L{1,2,4}`,
`quiet.L{1,2,4}`, `landing.L{1,2,4}`, `wait.L{1,4}`, `wait-many.L{1,2,4}`,
`stat.L{1,2,4}` / `stat.segment` / `stat.absent` / `stat.help`,
`watchdog.barrier.L{2,4}`, `watchdog.wait_until.L{2,4}`, `watchdog.off`,
`watchdog.default`, `model.wire`, and `reply`.

Nothing was skipped: all 52 tests matching the pattern were run the full
30 times (or more, for `shmem.wait.L2`, to capture output).

## The one failure: `shmem.wait.L2`

Captured with `--output-on-failure` on a reproduction run (the original step-4
run used the exact command the brief specifies, which does not pass
`--output-on-failure`, so its own failure produced only a timing line;
`shmem.wait.L2.fail.log` below is from a second, otherwise-identical
reproduction under the same load that also failed and was run with
`--output-on-failure` to get the assertion text):

```
Test #70: shmem.wait.L2 ....................***Failed   60.02 sec
/…/shmem/tests/test-wait.c:39: CHECK(shmem_uint64_test(flag, SHMEM_CMP_EQ, 0) == 1) failed
/…/shmem/tests/test-wait.c:40: CHECK(shmem_uint64_test(flag, SHMEM_CMP_NE, 0) == 0) failed
osoe-launch: timeout after 60 s; killing 2 process(es)
osoe-launch: rank 0 killed by signal 9
osoe-launch: rank 1 killed by signal 9
osoe-launch: left behind: /dev/shm/osoe.683763.0.job.0
osoe-launch: left behind: /dev/shm/osoe.683763.0.0.0
```

Full first-40-lines capture:
`docs/reviews/evidence/phase1-step2-slice1/load-survey/shmem.wait.L2.fail.log`

**Symptom:** assertion, then timeout. `test-wait.c:39-40` is the very first
pair of checks in the test, right after `shmem_init`/`*flag = 0`/
`shmem_barrier_all()` (see the file for the surrounding code) — it asserts
that a PE's own freshly-zeroed `flag` reads back as `0`. Both checks fail
together, which is consistent with a single PE's `flag` holding a nonzero
value at that point rather than two different PEs each hitting one check.
The process(es) then never reached the intended exit, and after 60s
`osoe-launch`'s own timeout killed both ranks.

**Reporting node/PE:** not determinable from the log. The `CHECK()` macro's
failure message (`check.h`) prints only the source file and line, not a PE
number, and `osoe-launch`'s per-rank kill messages ("rank 0 killed by signal
9", "rank 1 killed by signal 9") are just the timeout's cleanup, not an
indication of which rank hit the assertion. Given the brief's note that a
failed check on one node has previously corrupted its peer's stream, this
ambiguity itself is worth flagging: nothing in the current output would let a
future reader tell node 0's report from node 1's.

**`/dev/shm` note:** the failing run's own log claims two objects were "left
behind" (`osoe.683763.0.job.0`, `osoe.683763.0.0.0`). A check of `/dev/shm`
after the survey found no such files and no net growth (see below) — either
a later test's launcher cleaned up matching stale segments, or the objects
were removed by something else before the final count. The claimed leak did
not turn into a persistent one over the course of this survey, but the
launcher's own warning message says it happened, which is itself worth a
closer look outside this survey's scope.

## `/dev/shm` growth

- Before starting (pre-build, pre-load): `ls /dev/shm | wc -l` = 36
- After the full survey (load killed, suite re-verified green): 36

No net growth. (All 36 objects present throughout carry timestamps from
~13:4x on 2026-09-20, hours before this survey's ~17:2x-17:46 window — i.e.
they are the pre-existing stale objects mentioned in the brief, untouched by
this survey.)

## Final clean re-run

After killing the load and confirming `uptime` settled (8.78 and falling,
`ps` confirmed no spinner processes left):

`ctest --test-dir build/release -j2 --output-on-failure`: 131/131 passed,
9.30s — consistent with the baseline. The suite is green.

## What was skipped

Nothing. All 52 tests matching the required pattern were run 30 times (or
more) under load; the one failure was chased down with additional
reproduction runs rather than left unclassified.
