# Evidence and test audit

Agent `evidence-audit`, Question 2 (final third), phase 1 step 2 slice 1, against
`main` at `7ff9343` (slice merge `d542c63`). This is the **bounded** version of the
job: the lead cut the scope after a usage-limit stop. What was dropped is listed
under "Not investigated".

Supporting files are in `docs/reviews/evidence/phase1-step2-slice1/evidence-audit/`:

- `debug-clean-run.log` - one clean `cmake --workflow --preset debug` of a private copy.
- `model-reruns/*.out` - every recorded model, re-run.
- `model-variants/liveness-and-variants.py` and `.out` - extra model checks written for this audit.
- `mutations/*.diff`, `*.log`, `run-mutation.sh` - each mutation and what the suite said.

All builds and mutations were made in `build/scratch/review-evidence-audit/tree`;
the tracked tree was not touched (`git status` shows only new files under `docs/reviews/`).
Every timing below is *measured under contention* (shared 4-core VM).

Claim labels: *documented* / *observed in source* / *measured* / *recalled*.

---

## 1. Models

### 1.1 Reproduction (*measured*)

All ten recorded runs were re-executed on the host (python3) from their evidence
directories and diffed against the committed `.out` files.

| Recorded file | Result |
|---|---|
| `am-backend/{wire,credit,arrival}_model.out` | byte-identical |
| `blocked-record/blocked_record_model.out` | byte-identical |
| `multiple/selftest.out` | byte-identical |
| `watch-options/coverage_model.out` | byte-identical |
| `multiple/dirty_set_model.out` (254 s), `multiple/get_completion_model.out` (981 s), `multiple/watch_list_model.out`, `watch/range_watch_model.out` | differ **only** in wall-clock seconds and the 15-second progress lines; after stripping those, identical - every verdict, state count, depth and counterexample trace |

So there is no nondeterminism in what matters: state counts are reproducible to the
unit (for example 18,902,872 states for `get_completion` "2 issuers x 2 gets, 2
quiets"). A recorded file is not literally `diff`-clean because it embeds timing;
that is cosmetic, but it means "diff against the record" needs a filter, and none
is provided (minor, see F8).

**Drift:** none. Every `engine.py` and every model under `docs/design/evidence/*/`
is byte-identical to `tools/modelcheck/` (`coverage_model.py` exists only under
`watch-options/`, by design).

**ctest:** `tools/modelcheck/CMakeLists.txt` registers only `model.wire`,
`model.credit`, `model.arrival` (tier1, TIMEOUT 300). `blocked_record`,
`dirty_set`, `get_completion`, `watch_list`, `range_watch`, `coverage` and -
notably - `selftest.py` are **not** run by any preset. The CMake comment says this
is deliberate for "designs already settled"; but `blocked_record` and
`range_watch` model code that *is* implemented (`shmem/src/watch.c`, the blocked-in
record) and both run in under a second. See F6.

### 1.2 Can each check fail?

For each model: every `check` instruction / terminal verdict, and the recorded
variant that reaches it (*measured* from the `.out` files, confirmed by re-run).

| Model | Check | Reached by |
|---|---|---|
| wire | OVERWRITE (`wrote byte over a full ring`) | `nospace` |
| wire | STREAM (`byte read as`) | `consumed_early` |
| wire | LOST WAKE | `wake_after`, `nowake` |
| credit | `credits went to -1` | `race_take` |
| credit | DESCRIPTORS | `on_apply`, `none` |
| credit | LANDING | `on_drain nbi`, `on_apply nbi` |
| arrival | POSTED/CONSUMED one-word | `consume_reread` |
| arrival | `order()` "repeated" branch | `noconsume` |
| arrival | `order()` **"lost" branch** | **none recorded; and I could not reach it** (below) |
| arrival | STUCK | `wake_after`, `nowake` |
| blocked_record | torn read | `nogen`, `bumpafter`, `hoist`, `noverify` |
| dirty_set | `quiet returned with prior put(s)` | `unlocked`, `naive`, `bitfirst`, `plainmax` |
| get_completion | `quiet returned with prior get(s)` | `snapshot`, `noretag`, `nolock` |
| watch_list / range_watch / coverage | LOST WAKE | `firstmatch`, `checkfirst`, `notifyfirst`, `plainbit`, `plainput`, `plainblk`, `endpoints`, `satzero`, `sigfirst`, ... |

Every check has a variant that fires it except one half of one: the `'lost'` arm
of `arrival_model.order()` (`tools/modelcheck/arrival_model.py:134-138`). I removed
the POSTED check from `consume_reread` (which otherwise fires first) to see whether
the lost-arrival then reaches `order()`; it does not - the run ends `STUCK` instead,
because a lost arrival is never forwarded at all, so `order()` is never evaluated on
it (`model-variants/liveness-and-variants.out`, section 4). The arm is message text
for a state that cannot occur in this model, not a vacuous safety net: the defect
is still caught, by STUCK. Minor (F8).

### 1.3 The engine

*Observed in source* (`tools/modelcheck/engine.py`):

- **State hashing** is 96-bit BLAKE2b hash compaction, with the miss bound printed
  per run (worst recorded: 2e-15). `exact=True` exists. `selftest.py` compares
  reduced and unreduced runs (gpre/gpost folding, symmetry) on 12 cases and the
  recorded result reproduces. Sound.
- **Limits are not silent.** TIME/STATE/DEPTH LIMIT are verdicts, `report()`
  returns False for an incomplete run, and the three ctest-registered models
  `sys.exit(1)` on it. The other models print `SOME RESULT UNEXPECTED OR
  INCOMPLETE` but **always exit 0** (no `sys.exit`) - harmless today because ctest
  does not run them, a trap the day someone registers one (F6).
- **Liveness is only checked at states with no successor** (`engine.py:456-468`).
  LOST WAKE and STUCK are deadlock detection. A state graph in which one thread
  sleeps or spins forever while another can always step has no such state and is
  reported `ok`. Demonstrated: a wire reader that never publishes `consumed`
  (re-delivers byte 0 forever) is reported **`ok`, 343 states**, while an AG-EF
  check (can every reachable state still reach a good terminal?) finds **343 of 343
  states trapped** (`liveness-and-variants.out`, section 2). See F5.
  I then ran that stronger check over every `design` case of wire, credit,
  arrival and range_watch (32 cases): **0 trapped states in all of them**. So the
  blind spot is real in the engine but hides nothing in these four models today.
  Not run on dirty_set / get_completion / watch_list (state counts too large for
  the Python graph builder inside this budget).
- **selftest scope.** `selftest.py` covers only the three `multiple/` models. It
  does not exercise `tso=True`, `nd`, `await` or `andn`, and it cannot notice the
  liveness blind spot because both of its runs share it.

### 1.4 One claim in a model that does not hold as written (*measured*)

`arrival_model.py:216-220` says of the TSO rows: "Were the bump a plain store
too, these rows would fail where the SC rows pass." I built that variant (wake
bump as plain load + plain store). It **passes** under SC and TSO at
members=2/3, epochs=2/3 (`liveness-and-variants.out`, section 3). Reason (*observed
in source*): `fwake` is in `SERIAL`, so the engine forces the arriver's store buffer
to drain before the futex_wake, which is also what a real syscall does. The design
is not wrong - a locked add is still right because two arrivers of *different
teams* share the lead's wake word, which this model does not contain - but the
sentence claims evidence the model does not give. F7.

---

## 2. Tests

### 2.1 "131 tests, four presets" (*measured*)

- Private clean `debug` workflow: **131/131 passed**, 16.35 s real
  (`debug-clean-run.log`).
- Configure-only count in the private copy: debug 131, release 131, asan 131,
  **tsan 126**. The five missing under tsan are exactly
  `shmem.internode.{0-1-2, 0-0-1-1-2-2, 0-0-0-1-2-2, 0-1-2-0-1-0}` and
  `shmem.differential` (`shmem/tests/CMakeLists.txt:271,313`), as the what-changed
  note says. Nothing is excluded under asan.
- "131 tests" is therefore true of three presets, not four. I did not run
  release/asan/tsan to green (shared VM); "four presets green" is *documented*,
  not re-measured.
- Every one of the 131 has both `TIMEOUT` and `LABELS` (checked mechanically from
  `ctest --show-only=json-v1`).
- Every `test-*.c` under `shmem/tests`, `base/tests` and `tools/stat` is registered.

### 2.2 Can a failing PE still produce a passing test?

*Observed in source*, `tools/launch/launch.c:285-360` and `shmem/tests/check.h`:

- `CHECK` does not abort; it counts, and `main` returns `CHECK_DONE()`. All test
  programs return it except four that are intentionally output-matched
  (`test-bad-put`, `test-bad-team`, `test-info`, `test-differential`).
- `osoe-launch` fails the job if **any** rank exits nonzero, dies on a signal, the
  timeout expires, or shared memory is left behind. It reaps with `waitpid(-1)`
  and maps pid to rank; I found no path where a bad status is dropped.
- A PE that exits 0 early without its checks would leave its peers in a collective
  and the job would time out; no test has a rank-conditional early `return 0`.
- `arrivals.h`: the zero-entry guard is present and the per-round rows are
  present. I did not find a third vacuity. One residual weakness, *observed in
  source*: `ARRIVALS_STAGGER_MS * me` is the only thing spreading arrivals, so PE 0
  never waits and with `n == 1` the check is trivially true; that is inherent.
  Mutation a/a2 below never reached the `arrivals.h` assertion at all, because the
  library's own epoch-divergence `osoe_fatal` fires first - so the helper's
  ability to see an early release in the *multi-node* tests remains
  **undemonstrated by this audit** (the commit history says it was demonstrated
  once; *documented*).
- **Weakened regex (F4).** 18 tests pass on `PASS_REGULAR_EXPRESSION`, which makes
  ctest ignore the exit code. For the five `shmem.watchdog.*` tests the expression
  `"has died; this PE would wait for it forever"` contains a semicolon, so CMake
  stores it as a **two-element list** - `'has died'` and `' this PE would wait
  for it forever'` - and either alone passes (*measured*: the JSON test model
  shows the two elements). Any output containing "has died" passes the test.

### 2.3 Mutation table (*measured*; private copy, `debug` preset, one run each unless stated)

| # | Mutation (diff in `mutations/`) | Tests run | Result |
|---|---|---|---|
| a | `progress.c:220,666` release when `arrivals >= 1` | barrier, multinode, internode, differential (22) | **Caught**, 15/22 fail - every multi-node test. Detector is the library's own `osoe_fatal` ("release ... arrived for epoch 1 out of order"), not a test assertion. Single-node tests unaffected, correctly. |
| a2 | same, but release **exactly once**, on the first arrival, and keep counting | same | **Caught**, same 15. Detector again the library ("team 0 has arrivals for epochs 1 and 2 at once"). `arrivals.h` never got to speak. |
| b | `signal.c:98` drop `OSOE_TRANSPORT_FENCED` from put_signal's rmw | signal, internode, differential, multinode (16) | **NOT caught: 16/16 pass.** See F1. |
| c | `transport-am.c:602` `osoe_transport_wait` does not wait for the acknowledgement | quiet, internode, differential, multinode, rma, signal (22) | **Caught, but by timing**: `test-internode.c:149` fails on 3 of the 4 multi-node maps, plus `shmem.differential`. `shmem.internode.0-1-2-0-1-0` **passed** with quiet a no-op. `shmem.quiet.*` cannot see it (single node). See F3. |
| d | unmutated; `shmem_test_internode` under `OSOE_AM_CREDITS=1` and `=2`, 4 multi-node maps x 3 repetitions | 24 runs | All 24 pass. The reply pool never empties, so the drop path at `progress.c:64-67` is **never executed by any test**. |
| d2 | `progress.c:64` drop the first reply as if the pool were empty | `shmem.internode.0-1-2` | **Caught only by the launcher's 60 s timeout.** No library diagnostic at all: three ranks SIGKILLed, nothing names the drop. See F2. |
| e | `watch.c:95` notify without the `watch_seq` bump (wake still sent) | wait, wait-many, signal, stat, watchdog, internode, differential, multinode (35) | **Caught weakly**: 2/35 (`shmem.signal.L4`, `shmem.internode.0-0-0-0`), both through `invariant.missed_wake != 0`. All six `shmem.wait.*` / `shmem.wait-many.*` tests **passed**. The window (between the waiter's load of `watch_seq` and its `futex_wait`) is hit by chance; which tests fail will vary by run. |
| e2 | `watch.c:95-96` no bump and no wake | wait, wait-many, signal, internode, differential, surface (18) | **Caught robustly**: 10/18, by `test-wait.c:61` (elapsed-time bound) and `missed_wake` of 13-57. |
| f | `connection.c:149` `osoe_connection_drain_reserve` always returns false | connection, progress, internode, differential, multinode, barrier-am (22) | **NOT caught: 22/22 pass.** See F3b. |

Score: 9 mutants, 5 caught outright (a, a2, c, d2-by-timeout, e2), 1 caught by
chance (e), 2 not caught (b, f), 1 path shown unreached (d).

What this says, calibrated: the barrier mechanism and quiet's acknowledgement wait
are genuinely protected, and the protection for the barrier is the best kind - the
library's own invariant fires before any test has to. The `missed_wake` invariant
counter read through `osoe-stat`'s reader (`invariants.h`) is the most valuable
single check in the suite: it is what caught e and e2, and it catches things no
test author listed. The two uncaught mutants are both code with **no consumer**,
which is a different defect from a vacuous test and is reported as such.

---

## 3. Findings

### F1. `OSOE_TRANSPORT_FENCED` is carried and never read; no test can see it removed
- **Location:** `shmem/src/transport.h:118,180`; `shmem/src/signal.c:98`; `shmem/src/am.h:110`; `shmem/src/transport-local.c:35`; absent from `transport-am.c` and `progress.c`.
- **Failure scenario:** Today none - both backends apply one context's operations to one PE in issue order (intranode: calling thread; AM: one in-order stream per node pair), so the flag is redundant and mutant b is an *equivalent mutant*. The scenario is step 3+/RoCE: `transport.h:180` makes FENCED the contract by which put_signal's put-before-signal ordering is requested. A backend that reorders (an RDMA write and an atomic on different QPs, or a future multi-rail AM path) will consult a flag that nothing has ever honoured and **no test will fail when it is ignored**, because nothing in the suite distinguishes "ordered by flag" from "ordered by accident".
- **Evidence:** *measured* - mutant b, 16/16 pass (`mutations/b-*.diff`, `.log`). *Observed in source* - `grep -n FENCED shmem/src` finds one producer, zero consumers; `progress.c:157-161` reads `m.flags` only for `OSOE_TRANSPORT_FETCH`.
- **Severity:** clarification needed (design-doc: say the flag is currently vacuous and that put-before-signal is tested only as an outcome of stream order). Becomes *design change needed* the moment a backend can reorder.
- **Confidence:** high that the flag is unread and untested; medium on the forward risk. A test backend (or fault-injection knob) that delays un-fenced operations would settle it and make the flag testable.

### F2. A get/AMO reply that finds the pool empty is dropped silently; the job hangs with no diagnostic, and no test reaches the path
- **Location:** `shmem/src/progress.c:61-68` (`enqueue`); counter declared `progress.h:113`; contradicts `shmem/src/reply.h:40-41` and the comment at `progress.c:75-82`.
- **Failure scenario:** any break of §3.8's distributed credit invariant (a peer's accounting bug, or the pool being shared with something the credit does not count) makes `osoe_reply_take` return NONE; `enqueue` increments `reply_reservation_failures` and returns. The request is gone. The origin PE blocks in `shmem_get`/fetching AMO forever. `reply_reservation_failures` is a private struct field: it appears exactly twice in the tree (declaration and increment), is not in the metric table, and `osoe-stat` cannot see it.
- **Evidence:** *measured* - mutant d2 (drop exactly one reply): `shmem.internode.0-1-2` runs 60.01 s and dies by `osoe-launch: timeout after 60 s`, with no `osoe:` line from the library (`mutations/d2-*.log`). *Measured* - 24 unmutated runs at `OSOE_AM_CREDITS=1` and `2` all pass, so the path is never executed by the suite (`mutations/d-*.log`). *Documented* - `reply.h:40-41`: "Overflow is therefore a hard assertion naming the peer, not a silent drop". *Observed in source* - the code does the opposite.
- **Severity:** design change needed. Either make it the hard assertion the header promises (as `enqueue_collective` already is), or export the counter as an invariant metric so `invariants.h` fails every tier-2 test that trips it.
- **Confidence:** high. This confirms what sibling agents reported; I did not independently establish whether the invariant *can* break in the current code, only that if it does nothing notices.

### F3. Quiet's completion guarantee is asserted only through a race, and one registered map passes with quiet disabled
- **Location:** `shmem/tests/test-internode.c:120-152`; `shmem/tests/CMakeLists.txt:274` (map `0,1,2,0,1,0`); `shmem/tests/test-quiet.c` (single node only).
- **Failure scenario:** with `osoe_transport_wait` returning without waiting (mutant c), the test's `shmem_quiet(); shmem_barrier_all();` still usually sees the data, because the barrier's own internode round trip gives the agent time to apply the puts. 3 of 4 multi-node maps happened to lose that race and failed; `shmem.internode.0-1-2-0-1-0` won it and passed. On a faster or idler machine more would pass. A regression that makes quiet return early *sometimes* would be caught less often still.
- **Evidence:** *measured*, one run (`mutations/c-*.log`). Not repeated 3 times (budget) - so the per-map catch rate is unknown; that at least one map can pass is established.
- **Severity:** clarification needed / test gap. A deterministic version: hold the target's agent (a test hook or a wire that withholds bytes until told) and assert that quiet has *not* returned, then release it. The wire's split injection is the natural place.
- **Confidence:** medium-high.

### F3b. `osoe_connection_drain_reserve` has no production caller, and its only test cannot observe it
- **Location:** `shmem/src/connection.c:149-176`; `shmem/tests/test-connection.c:225-240, 295-312`.
- **Failure scenario:** the test puts one `REPLY_ID` message into a DRAINING buffer, marks it FREE, and a few lines later `osoe_connection_open` (`connection.c:100-104`) resets `head` and `filled` to the header size - discarding the reply before any seal. `ONCE` asserts `ngot >= TOTAL` where `TOTAL = WRITERS * PER_WRITER` excludes replies, and ORDER/STALESEQ iterate only over what was delivered. So zero replies delivered satisfies every check. The comment at `test-connection.c:211-213` ("so the guard's path runs constantly rather than never") is true of the *call* and false of any *effect*.
- **Evidence:** *measured* - mutant f (function body replaced by `return false`): 22/22 pass including `shmem.connection`, `shmem.progress` and every multi-node test (`mutations/f-*`). *Observed in source* - `grep -rn drain_reserve shmem/` finds the definition, the declaration and the test only.
- **Severity:** clarification needed (dead code presented as tested; §3.8's DRAINING guard is claimed as exercised). If step 3 intends to use it, the open-after-drain overwrite is a real defect waiting for its first caller.
- **Confidence:** high. Confirms the sibling report.

### F4. Watchdog tests' pass expression is split by its semicolon
- **Location:** `shmem/tests/CMakeLists.txt` - `PASS_REGULAR_EXPRESSION "has died; this PE would wait for it forever"` (four `shmem.watchdog.*.L*` tests and `shmem.watchdog.default`).
- **Failure scenario:** CMake treats the value as a list; the test passes if the output matches `has died` **or** ` this PE would wait for it forever`. Because these tests ignore exit status by construction, any output containing "has died" passes them - for example a different fatal that mentions a dead PE without the watchdog having done its job.
- **Evidence:** *measured* - `ctest --show-only=json-v1` reports `['has died', ' this PE would wait for it forever']` for all five.
- **Severity:** minor. Fix: replace `;` in the expression with `.` or `\\;`.
- **Confidence:** high.

### F5. The model engine detects deadlock, not livelock or starvation
- **Location:** `tools/modelcheck/engine.py:456-468`; README's and docstring's claim that reductions keep "every violation, lost wake and stuck state".
- **Failure scenario:** a protocol in which the waiter is asleep forever while some other thread can always take a step (a sweeper, a retry loop, a progress thread that polls) has no successor-free state; the engine reports `ok`. §3.10's sweep and the progress loop are exactly such always-enabled threads, and `arrival_model.py` says it leaves the sweep out for a related reason. A future model that includes one silently loses LOST WAKE detection.
- **Evidence:** *measured* - `wire/noadvance` is reported `ok` by `explore()` and 343/343 states trapped by an AG-EF reachability check (`model-variants/liveness-and-variants.out`). Counter-evidence, also *measured*: the same check finds 0 trapped states in all 32 `design` cases of wire, credit, arrival and range_watch, so no current verdict is wrong because of this.
- **Severity:** clarification needed (document the limit; ~40 lines add the stronger check for small cases - the script here is that code).
- **Confidence:** high.

### F6. Most models, and the engine's self-test, are not run by ctest; the unregistered ones cannot fail a process
- **Location:** `tools/modelcheck/CMakeLists.txt:12`; `blocked_record_model.py`, `dirty_set_model.py`, `get_completion_model.py`, `watch_list_model.py`, `range_watch_model.py` (no `sys.exit`).
- **Failure scenario:** an edit to `engine.py` made for the three registered models changes a verdict in `range_watch` or `blocked_record`, both of which model shipped code. Nothing runs them; if something did, they exit 0 regardless.
- **Evidence:** *observed in source*; *measured* - `blocked_record`, `range_watch`, `coverage` each run in <= 1 s, `selftest` in 26 s, `watch_list` in 30 s.
- **Severity:** minor. Register the fast ones and `selftest.py`; add `sys.exit(0 if good else 1)`.
- **Confidence:** high.

### F7. `arrival_model.py` claims its TSO rows discriminate a plain wake bump; they do not
- **Location:** `tools/modelcheck/arrival_model.py:75-79, 216-220`.
- **Failure scenario / evidence:** *measured* - the plain-store bump variant passes SC and TSO in four configurations (section 1.4). The TSO rows therefore demonstrate nothing the SC rows do not, and the sentence "these rows would fail" is unsupported.
- **Severity:** minor (a wrong sentence in evidence; the design choice of a locked add is still correct for the multi-writer wake word, which is outside this model).
- **Confidence:** high for the configurations tried.

### F8. Small items
- Recorded `.out` files embed wall-clock and progress lines, so a plain `diff` reports a mismatch for four of ten files that are in fact identical. Minor.
- `arrival_model.order()`'s "lost" arm is unreachable in any variant I could construct; the defect it names surfaces as STUCK. Minor.
- "131 tests, four presets": tsan has 126. Minor wording.

---

## 4. What is sound, and why I believe it

- **The recorded model evidence is honest.** Ten of ten runs reproduce to the exact state count; copies have not drifted; every safety check but half of one has a breaking variant that reaches it; `selftest` shows the reductions change no verdict.
- **The launcher propagates failure correctly** - nonzero exit, signal death, timeout and leaked shared memory all fail the test.
- **Registration hygiene is complete**: 131/131 with timeout and label, no orphan test sources, tsan exclusions exactly as documented and justified in place.
- **The barrier's all-nodes rule is protected twice** - by tests on three-node maps, and before that by the library's own epoch-divergence fatal, which fired in both barrier mutants within 10-30 ms.
- **`invariants.h` + `invariant.missed_wake` works** and is independent of the library's headers; it caught a race-window mutant that the dedicated wait tests missed.
- **Total loss of notify is caught robustly** (e2: 10/18, by both a timing bound and the invariant).

## 5. Not investigated (cut by the bounded scope)

- The coverage build (`gcov`/`llvm-cov`) and per-file line coverage; coverage by inspection of error paths, wraparound and full-buffer paths.
- Mutations I had planned beyond the six: credit off-by-one (`reply.c`), reservation overlap (`connection.c` CAS), wire `consumed`-before-copy in the C code (the model covers it; the C test was said to see it "about half of runs"), seal/STALESEQ, malformed-segment field checks one by one, exactly-once rmw under concurrency across nodes, fence ordering as distinct from FENCED, the `test-differential.c` / `cmake/Differential.cmake` claim (it did fail under mutant c and a/a2, which shows the diff is live, but I did not read `Differential.cmake` for a way to pass vacuously - for example all transcripts equally empty).
- Whether `arrivals.h` can see an early release in a multi-node test once the library's own fatal is out of the way (needs a mutant that also suppresses `progress.c:215-217`).
- Repeating mutants c and e three or more times to give catch *rates*; each was run once.
- release / asan / tsan workflows to green in the private copy.
- AG-EF liveness on the three large `multiple/` models.
- `base/tests/*` and `tools/stat/test-segment.c` assertions (read only for registration).
- Load-induced flakiness (assigned to another agent).

## 6. Lessons for a successor API

- `put_with_signal` forces an ordering contract (`FENCED`) through the transport seam for one routine. In both existing backends it is free and therefore untested. A successor API that made "ordered after my previous operation to this PE" a property of the *context* rather than a per-call flag would have one ordering rule to test instead of a flag with no observable behaviour.
- `shmem_quiet`'s guarantee ("remote writes are visible") is unobservable from inside a conforming program except by racing, which is why F3 is hard to test deterministically. A completion *handle* or counter the program can read would make the guarantee assertable.
