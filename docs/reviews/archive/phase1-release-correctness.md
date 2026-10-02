# Round three, question 2: is osoe correct?

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. **Status: partial.** The review
was stopped at the author's request before three investigators reported
(`models`, `newest-work`, `pacer`); their reports land in
`build/scratch/review-r3/reports/` when they finish, and this file says what
each was asked. What is here: the atomics audit (`atomics-audit`), the
adversarial read of the deadlock detector (`deadlock-adversary`), the lead's
own read of the detector and the silent-peer check, and the lead's baseline,
load and eager-detector runs in a fresh VM. Labels: *documented*, *observed
in source*, *measured*, *recalled*; "lead-verified" means the lead re-read or
re-ran it.

## Verdict so far

**Nothing found ends a healthy job, loses a wake or corrupts a defined
operation.** The atomics discipline of §3.16 holds across all 330 atomic
calls in the library: no forbidden form, every two-sided protocol at
`seq_cst` on both sides, the shipped aarch64 library free of `ldapr`, and the
project's own lost-wake litmus reproduced (*measured*). The deadlock detector,
the newest and most consequential piece, could not be made to end a live job
by any program inside what OpenSHMEM defines; its one false positive is the
signal-handler wake the README already lists as a limit. In a fresh VM the
four presets, the tier-3 preset (every model and the 103 shaped-link tests
included) and three idle repeats of the release suite passed whole but for a
machine-dependent performance pin; under eight busy loops two tests time out,
one of them a unit test that spins where the library sleeps; at the detector's
most eager setting the suite reproduces the record exactly. Two real defects,
neither affecting a program's result: a diagnostic record read torn (K1) and
the test that spins under load (K3).

## Findings, ranked

### K1. The "blocked in" record's routine name can be read torn (fix before release, cheap; diagnostic impact only; high; *measured* by `atomics-audit`, not yet independently reproduced)

- **Location**: `shmem/src/node.h:180` (`blocked_routine`, a plain `char[32]`
  inside the generation-word protocol), `metrics.c:575-578` (the writer fills
  it with `memcpy`/`memset`), `deadlock.c:308` and `tools/stat/segment.c:476`
  (the readers copy it with `memcpy` between two `seq_cst` loads of the
  generation).
- **Why**: an acquire load orders later accesses after it, not earlier ones,
  so the reader's plain loads of the fields may be satisfied after its second
  generation load; a seqlock reader needs an acquire fence before the second
  load or atomic loads of the fields. On a core without LSE the writer's
  first bump is `ldaxr/stlxr` with no `dmb`, so "a locked RMW is a full
  barrier on both targets" (`metrics.c:558-564`) is false there. The
  investigator's litmus, copying the protocol field for field, accepted torn
  records in 0.6-0.9% of accepted reads on the library's own flags and on an
  Armv8.3 build; with the routine stored as four atomic words (release store,
  acquire load) it accepted none in 26-32 million reads.
- **Effect**: `osoe-stat` can show a routine name from a different wait than
  the detail beside it, or one spliced from two; the deadlock report is
  practically safe, since the PEs it reads have been asleep for seconds.
- **Severity**: fix before release because it is one fence or four atomic
  words and a §3.16 violation the project would not want to ship; nothing a
  program computes depends on it.

### K2. The deadlock detector: sound inside OpenSHMEM's model; one stated limit confirmed; the second collection is witnessed only by the model (`deadlock-adversary`, lead-verified on the counted loops)

- **No false positive within the specification** (*measured*, all at
  `OSOE_DEADLOCK_SECONDS=1`, 4-8 PEs on one to three logical nodes): a
  wait-heavy ring with an increment always in flight (atomic increment, put
  and quiet, fetch-add, mixed); `SHMEM_THREAD_MULTIPLE` with 4 and 8 threads a
  PE and shared-context quiets; data collectives and locks over three nodes
  (8,000 rounds, 30 s); a helper thread making library calls; node-mate
  stores through `shmem_ptr`. None was declared. The investigator's own
  accidental real deadlock was declared in about 2 s with a correct report
  every time. The suite's 14 `shmem.deadlock.*` tests pass, and the whole
  release suite at one second reproduces the record: 597 of 598, the failure
  the real deadlock of `exit-without-finalize.start_pes`, which the detector
  ends first (*measured*, lead, `load/eager-release.log`).
- **Why it holds** (lead-verified in source): every counted loop - the epoch
  wait, the two region waits, the many-variable waits, the transport's seven -
  calls `osoe_wait_ends` before any visible action, including the three
  subtle ones (`reserve_waiting`'s nudge, `copy_out` inside a wait, the
  chained epoch wake); every data-collective condition callback only loads;
  posts precede waits in program order and the pass between the halves takes
  them; "acknowledged" means engine-applied (`progress.c:563, :848`), compared
  per node since that node's own first sample; `/proc` requires exact equality
  per PE and state `S` only; non-lead agents run no pass (`agent.c`), so no
  excluded thread does visible work.
- **The one false positive** (*measured*, `deadlock-adversary/programs/alarm-wake.c`):
  a PE waits in `shmem_int_wait_until` on a flag its own SIGALRM handler sets
  four seconds later (by a plain store or `shmem_int_atomic_set` to itself)
  while the others wait in a barrier; the job is ended "for 2 s" with a
  correct-looking report, though left alone it finishes in 4 s. Outside the
  specification's model (no routine is async-signal-safe; wait variables are
  updated by AMOs from *another* PE), and the README lists it: "A program
  woken only from outside the library - by a signal handler's store ... should
  turn detection off." Design §3.15's bullet "What it cannot see, and so never
  ends" is wrong for this case - the detector does end such a job;
  `deadlock.h:53-59` has it right. Fix the design text (minor).
- **Tests versus model** (*measured*): a mutant that declares after the first
  all-quiet collection passes all 14 tests (only `deadlock_model.py`'s
  `one_collection` breakage sees it); a mutant that never consults `/proc` is
  caught by four (`helper` ×2, `busy` ×2). Record the gap (after release).
- **False negatives, all after release**: a collection has no timeout, so a
  probe never answered leaves the root stuck in that collection for the job's
  life (`deadlock.c:619-620`); a real deadlock behind the shared-context quiet
  mutex, an uncounted futex sleep (`transport-am.c:212`), is never found and
  could be counted at no cost; an unreadable `/proc` makes the detector
  silently inert.

### K3. `shmem.reply` spins where the library sleeps and times out under load (after release; test robustness; high; *measured*, lead)

- Under eight busy loops on the fresh VM's four cores, `shmem.reply` (0.06 s
  idle) timed out at 60 s in the release run and again in the debug run, and
  once in five repeats; idle it passed 20 of 20. Its four `hammer` threads
  wait for a credit with `while (!osoe_credit_served(...)) sched_yield();`
  (`shmem/tests/test-reply.c:104-106`) where the library sleeps on the
  credit's word, so under oversubscription each handoff can cost a scheduling
  period; the ticket field is 21 bits (`reply.c:107`), so no wrap is involved.
  `shmem.strict-c11`, a nested build of 76 s idle, also passed its 180 s
  timeout under load. Nothing in the library was implicated: the newest tests
  (`deadlock|agent|scan|stopped|vanished|pacer|pipeline|allgather|thread|multiple|context|team|lock|signal|wait`,
  266 tests) repeated five times under the same load passed every time.

### K4. Minor, from the atomics audit

- `complete_gets` in serial mode checks a landed piece only by `copy_out`'s
  claim CAS, whose failure order is `acquire` (`transport-am.c:1175-1179`),
  where the two other sites load the state `seq_cst` first; harmless on the
  hardware (0 lost wakes in 2 million rounds), one load to add.
- `transport.h:459` still says `osoe_transport_load` is "one acquire load";
  the code is `seq_cst`, and its own comment says why acquire loses wakes on
  Armv8.3+ (also `internode-generic.c:560, :1333`). A second backend's author
  would copy the stale order.
- `reply.h:239` is the library's one `memory_order_relaxed`, and
  `connection.c:30-31` initialises wait words by struct assignment; both before
  the area's `ready` release, harmless.
- `osoe-stat` reads the segment's directory with plain loads and never checks
  `ready` (`segment.c:280-350`); bounded by the metric-table checksum.
- §3.16's tables still list the wire's `produced`/`consumed`, which no longer
  exist; `rmw.h` and `transport.h` say both paths run "the same instruction"
  where a remote non-fetching `set` is an exchange and the local one a store
  (both atomic on the same word).

## Runs (lead, fresh VM `osoe-load`: Debian 13 arm64, kernel 6.12, four cores)

| Run | Result |
|---|---|
| debug, asan, tsan whole | 598/598, 597/597, 387/387 |
| release whole, without the models | 597/598: `shmem.floor`'s stream pin (readiness R2) |
| tier-3 preset whole: release suite with every model and the 103 shaped-link tests | 725/725, 21 minutes |
| release ×3 idle (`--repeat until-fail:3`) | 1,794 runs, 0 failures |
| release under 8 busy loops | 596/598: `shmem.strict-c11` and `shmem.reply` timeouts (K3) |
| debug under 8 busy loops | 596/598, the same two |
| the newest 266 tests ×5 under load | 1,330 runs, 0 failures |
| release at `OSOE_DEADLOCK_SECONDS=1` | 597/598, the real deadlock of `exit-without-finalize.start_pes` ended first (as recorded) |

## Pending when the review was stopped

- **`models`** reported after the stop (`reports/models.md`; run on the Mac):
  every model, the self-test and the timing script give the verdict their
  cases expect; 225 of 226 recorded rows reproduce exactly, state counts
  included (the one gap is `get_completion`'s 18.9-million-state case, which
  needs more than one ten-minute call and reached 12.5 million states without
  a violation); no case ends on a limit; every `check` is reached in the
  passing cases and every one can fail, by a shipped breakage or a one-line
  mutation, so the defect the project hit twice before, a check no case
  reached, is absent today. `pipeline` and `scan_pipeline` match
  `internode-generic.c` on every rule; `lead_wake` and `wait_word` match their
  C. **One finding, fix before release (K6, README)**: `README.md:466`'s
  "every lock-free and futex protocol ... is checked exhaustively" overstates
  it - the active-set `pSync` protocol's only model was a random one, not in
  the tree, run against `eabf27c` before the fixes that changed the protocol,
  so today's `active-set.c` has no model; every model runs under SC or
  x86-TSO while the targets are aarch64 (the `ldapr` lost wake was found by
  measurement, not a model); the watch as built is modelled only by the
  opt-in `watch_list`. After release: `deadlock_model.py` has one
  single-threaded PE per node, a "waiting" flag set by fiat where the C reads
  `/proc`, no sweeps and atomic sample halves, so the detector's safety on real
  nodes rests on the argument and the tests (none found wrong); `abort_exit`'s
  `built` cases model a pidfd wake on the ring the build lacks (rerun with the
  build's sweeps they pass; `both` not run); `landing_extent`'s `release_early`
  fails as stuck rather than by its named property; `selftest.py` covers three
  models and turning the reductions off changes two others' verdicts through
  folded ghost steps; `abort`'s fatal case sits at 11.8 million states against
  a 12-million termination bound (the CMake comment's "5.7 million" is stale).
- **`newest-work`** reported after the stop (`reports/newest-work.md`): all
  three pieces sound. The scan's five rules sit in `internode-generic.c` where
  the model puts them; consecutive scans need no epoch (cumulative piece
  numbers and counters per team); two threads' teams share only a statistics
  word; the type list equals Table 10's 26 SUM types; `ctest -R scan` 12 of 12,
  and 1 MiB scans checked against a serial sum over D = 1, 2, 3, 8, fan-outs 2,
  4 and 32, five maps and `OSOE_HEAP_SHIFT` found 0 failures (48 ms at D = 1,
  25 at 2, 9 at 8); the two mutations the record names each fail 9 of 12
  tests. The agent's slice touches the agent thread only and is never a
  priority; the CPU union never narrows the agent; both tests assert the
  setting itself. The silent-peer check reads exactly `tcpi_unacked` (the
  gate) and `tcpi_last_ack_recv`; all nine tier-3 vanished-node tests pass.
  **One finding, fix before release (K5)**: no test holds the gate that
  protects a stopped node with a full receive buffer. The investigator put
  64 MiB toward a node stopped 15 s against a 4 s keepalive budget
  (`OSOE_KEEPALIVE=1,1,3`, map `0,0,1`): with the gate, `ss` showed zero-window
  probing, nothing unacknowledged, 6.7 s since the last ACK, and the quiet
  completed at 15.1 s with every byte correct; with the gate removed, 36 of 36
  related tests still pass while the experiment ends the job in 4 s.
  `shmem.stopped-node.keepalive` puts 8 bytes and never reaches a zero window.
  Add the experiment as a test. Minor: finalize's flush and close loops never
  run the check (a node vanishing after the final barrier costs about fifteen
  minutes); the README's "their total" for the keepalive budget is
  IDLE + INTERVAL × COUNT (120 s, not 76) and it never says what
  `OSOE_KEEPALIVE=0` costs; the per-team-number cost is 201.5 KiB, the scans'
  share 2.54 MiB at the defaults, and 5.2 GiB rather than 4.1 at the largest
  settings, where the `/dev/shm` refusal names only `SHMEM_SYMMETRIC_SIZE`;
  `sched_setattr` and `sched_setaffinity` failures are silent and a
  `SCHED_BATCH` job gets no slice.
- **`pacer`** reported after the stop (`reports/pacer.md`): of eight defects
  planted in the library, each in its own copy, `--quick` caught seven within
  its first rounds (1.2-4.7 s) and a 90-second `mix` the same seven (0.08 to
  86.7 s): an intranode put dropping its last byte, one internode put in a
  thousand landing at offset + 8, `fetch_add` returning the new value, MAX
  computed as MIN for the last element, a signal sent before its data, and a
  team broadcast leaving the root's `dest` alone. **The eighth, a
  `shmem_quiet` that skips the last connection's acknowledgement, passed
  `--quick` on four maps, 90 s of `mix` and the narrow ctest set**, because
  every exercise's next step travels on the same ordered stream as the data;
  only a reader on a third node could see stale bytes and no exercise has one
  (two unit tests that stop a node do catch it: `shmem.unswept-wait` and
  `shmem.stopped-node.no-progress`; the `OSOE_CHECK_QUIET` hook covers only
  the shared-context wait). The records list this blind spot; the README's
  presentation of `--quick` as the check that osoe works does not. After
  release: a three-node relay exercise; `mix`'s team periods never move data
  between node-mates on block maps (teams `{k, k+K}`), so the `shmem_ptr` check
  never ran in the clean 3-minute `mix` and the node-mate mutation was seen
  only in the world period at 86.7 s; four checks have no plant test, contrary
  to the rule (`rma.c:741`'s data check at a signal, `sort.c:639`, `sort.c:484`,
  `threads.c:324`), the plants test the comparator rather than each check's
  blind spot and all run on the one map `0,0,1,1`; `teams` never checks a
  broadcast root's `dest`; `--quick` and `mix` reduce only non-negative whole
  numbers. Clean runs: `--quick` 1.4 s and 2.5 s on two maps, a 3-minute `mix`
  on 8 PEs with `failures=0`; the counted build's 1,792 of 1,794 confirmed, the
  two never called `shmem_finalize` and `shmem_global_exit`. The oracles are
  closed forms from seeds, independent of arrival order, covering every byte
  of RMA with canaries and every PE's collective result, the root's included.

## Sources

`reports/atomics-audit.md` and `atomics-audit/` (the litmus programs and the
table of 330 calls), `reports/deadlock-adversary.md` and
`deadlock-adversary/` (programs, logs, two mutated trees), `load/*.log`,
`shmem/src/deadlock.c`, `base/sweep.h`, `base/epoch.c`, `region-wait.c`,
`transport-am.c`, `agent.c`, `wire.c`, `progress.c` (lead-verified), design
§3.10, §3.11, §3.15, §3.16, §4.6, §4.9, §4.11, `docs/decisions/2026-09-28-overnight.md`.
