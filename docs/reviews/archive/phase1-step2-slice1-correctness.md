# Correctness: from the atomics up to the barrier

Question 2 of the second adversarial review, against merge `d542c63`. Assembled by
the review lead from three investigations, each of which holds the detail, the
evidence and the experiment sources:

| Part | Report | Evidence directory |
|---|---|---|
| §3.16 atomics sweep | `evidence/phase1-step2-slice1/atomics/report.md` | `…/atomics/` |
| Protocols, by model and by reading | `evidence/phase1-step2-slice1/protocols/report.md` | `…/protocols/` |
| Audit of the models and the tests | `evidence/phase1-step2-slice1/reports/evidence-audit.md` | `…/evidence-audit/` |

Two further investigations contributed correctness findings and are cited where
used: diagnosability (`phase1-step2-slice1-diagnosability-and-deferrals.md`) and
performance-and-scale (`phase1-step2-slice1-performance-and-scale.md`).

Labels are the review's: *documented*, *observed in source*, *measured*,
*recalled*. Everything measured ran in private copies on the shared 4-core VM and
is **measured under contention**. "Lead-verified" means the review lead re-read
the cited lines after the agent reported; it does not mean re-measured.

**Not done in this round** (usage limits): a refutation pass by fresh agents.
(The CPU-load flake survey was run last; see C9.) In its place, every finding
rated *design change needed* or worse below was either found independently by two
or more agents, or lead-verified in source, and says which.

## Verdict

The concurrency at the bottom is in better shape than the prompt feared, and the
layer above it is in worse shape than the test suite shows.

**The atomics and the wait/wake protocols are sound.** All 162 atomic accesses
name an order; there is no `relaxed`, no `volatile`, no intrinsic, no tsan
suppression; every futex wait loads its sequence word before its predicate; the
reservation and credit CAS loops have no harmful ABA; no 64-bit atomic is
misaligned on either architecture. A new model of §3.5's reservation protocol
written *from the C* passes with two and three writers under SC and TSO, and all
seven of its broken variants fail.

**The acknowledgement protocol is wrong twice, and the two defects hide each
other.** This is the most important correctness result, because the obvious fix
to the visible defect turns a CPU-burning nuisance into a hang.

**Four of the tree's promises have no test that can see them broken**: FENCED
ordering, the DRAINING guard, a lost barrier wake, and the reply pool's failure
path.

## Findings

Ranked. IDs are this document's; the source report's ID follows in brackets.

### C1. Every acknowledgement requests an acknowledgement — *blocking for step 3*

- **Location:** `shmem/src/progress.c:283-284`, `:442`, `:447`, `:467-468`, `:515`.
- **Scenario:** one bool, `owe_ack`, means both "I owe the peer an ack" and "I
  want one". A's flagged segment makes B owe; B's header-only reply is sealed
  while `owe_ack` is true, so it carries `ACK_REQUESTED`; A now owes; and so on
  until `osoe_progress_forget_acks` at finalize.
- **Evidence:** *measured* by two agents independently. One put and one quiet
  followed by 2 s of application sleep: ~5,000 segments per node, all but ~5
  header-only, versus 4–5 with no put; a third node that received no put stayed
  at 5. *Lead-verified* in source.
- **Consequence:** the lead never reaches §3.10's idle; on TCP it is ~5,000 tiny
  packets per second per quiesced pair, forever; `acks_only` and `segments_sent`
  stop meaning anything; it defeats any future lazy-connection scheme.
- **Severity:** design change needed now, blocking before a socket exists.
  **Confidence:** high. [protocols P1; algorithmic-scaling 1]
- **Do not fix alone — see C2.**

### C2. The ack request rides whichever segment is finalized next; only C1's echo keeps `quiet` live

- **Location:** `progress.c:467-468` (request taken at the top of `flush`),
  `:492-502` (the *previously* sealed buffer is finalized and consumes the flag),
  `:512-523`; `transport-am.c:592-613` (`flush_dirty` runs once; the wait loop
  re-wakes the lead but never re-requests).
- **Scenario:** put #2 lands in segment S while S−1 is sealed. The request is
  attached to S−1. If the peer replies before applying S, the reply acknowledges
  S−1 and nothing ever asks for S. Today the peer's reply itself carries the flag
  (C1), so another round covers S. The echo is load-bearing by accident.
- **Evidence:** *measured*. With the naive C1 fix (`naive-echo-fix.patch`) and two
  3 ms sleeps injected in the lead's thread — timing only, no logic — 4 of 5 runs
  of a put/put/quiet loop aborted with "node 1 has not acknowledged this PE's
  writes". Unmodified tree: 0 failures in 6 runs.
- **Severity:** design change needed, jointly with C1. **Confidence:**
  medium-high; an ack-protocol model would raise it (specified in the protocols
  report).
- **Fix shape:** the request carries a sequence number — "acknowledge through S" —
  and the lead flags every segment up to S; an ack-only segment never requests.

### C3. A get or AMO request that finds the reply pool empty is silently dropped

- **Location:** `progress.c:61-72`; pool sized at `progress-host.c:233` as
  `peers × C` where §3.8 asks `C + degree×max_teams + slack`; `reply.h:38-42`
  promises a hard assertion naming the peer.
- **Scenario:** the parser has consumed the request; `enqueue` counts a failure
  and returns. The origin waits `OSOE_BOOTSTRAP_TIMEOUT` and aborts with "no get
  reply", on the wrong node; its credit never returns; the counter is exported
  nowhere.
- **Evidence:** *measured* — reproduced 3 of 3 with `OSOE_AM_CREDITS=2` by the
  performance agent; a mutant that drops one reply is caught only by the
  launcher's 60 s timeout. Found independently by three agents. *Lead-verified*.
- **Reachability at defaults:** argued, not measured. The protocols agent holds it
  unreachable under SERIALIZED with only a world barrier; the performance agent
  holds ≥64 local PEs with gets outstanding toward one peer sufficient. It becomes
  plainly reachable with team barriers, nonblocking gets, or `THREAD_MULTIPLE`.
- **Severity:** design change needed (the performance report rates it blocking).
  **Confidence:** high on the mechanism.
  [protocols P3; performance 1; evidence-audit F2]

### C4. The AM backend's blocking calls never sweep

- **Location:** `transport-am.c:175-193`, `:305-321`, `:335-351`, `:601-610`,
  against the contract at `transport.h:48-55`.
- **Scenario:** a PE in an internode quiet, get, fetching AMO or credit wait never
  reads the abort word, never runs the liveness check, never bumps `pe.sweeps`
  and publishes no "blocked in" record. A sibling dies; every PE in a barrier or
  `wait_until` exits within 100 ms; this one sits for 60 s and then blames the
  wrong node.
- **Evidence:** *observed in source* (atomics), *measured* (diagnosability: PEs
  sat beside a dead lead for 12 s showing `blocked in: -`, `sweeps 0`).
  *Lead-verified*: `osoe_sweep_expired` and `osoe_blocked_enter` are called only
  from `barrier.c`, `wait.c`, `watchdog.c`, `metrics.c`.
- **Severity:** design change needed; small fix. **Confidence:** high.
  [atomics F1; diagnosability D-2]

### C5. Every data-path wait is bounded by the *bootstrap* timeout, and expiry is fatal

- **Location:** the same four loops; `osoe.bootstrap.timeout_ns`, 60 s default.
- **Scenario:** *arith* — 256 PEs × 64 MB to one peer over 1 GbE is ~150 s. A
  healthy `shmem_quiet` aborts the job. The spec gives quiet no timeout, and
  `OSOE_BOOTSTRAP_TIMEOUT` is documented as the wait for local PEs to appear.
  *Measured* (diagnosability): a lead SIGSTOPped for 20 s with the timeout at
  20 s killed every PE talking to its node.
- **Severity:** design change needed. **Confidence:** high on the code.
  [atomics F2; diagnosability D-7; performance 8]

### C6. A lost wake in the barrier release is invisible to everything

- **Location:** `progress-host.c:115`; `base/epoch.c:25-37`;
  `invariant.missed_wake` is counted only at `wait.c:107` although §3.14 says
  "or any other wait".
- **Evidence:** *measured*. With `osoe_epoch_wake_all` removed, 30 barriers took
  3.00 s instead of 0.015 s; all 22 barrier/internode/multinode/differential
  tests passed; every `invariant.*` counter stayed zero.
- **Severity:** design change needed — count a sweep expiry that finds the
  condition already true in `osoe_epoch_wait_eq`, as `wait.c` does.
  **Confidence:** high. [diagnosability D-1]

### C7. Promises with no test under them

| Promise | Mutation | Result |
|---|---|---|
| put-before-signal (`OSOE_TRANSPORT_FENCED`, `signal.c:98`) | flag removed | all 16 selected tests pass — the flag has one producer and **no consumer**; ordering is a by-product of stream order |
| §3.8 DRAINING guard (`connection.c:149-174`) | body → `return false` | all 22 selected tests pass — no production caller; the function mis-accounts `filled` and `osoe_connection_open` overwrites the drained reply (*measured*, `drain-reserve-accounting.out`); `test-connection.c:295` never counts `REPLY_ID` |
| quiet means applied (`osoe_transport_wait`) | returns without waiting | 3 of 4 internode maps and the differential fail; `shmem.internode.0-1-2-0-1-0` passes — asserted only through a race |
| watch wake (`watch_seq` bump) | bump removed | caught in 2 of 35 tests, by chance, via `invariant.missed_wake`; every `shmem.wait*` test passes |
| `shmem.watchdog.*` expected output | — | a `;` in `PASS_REGULAR_EXPRESSION` splits the pattern; "has died" alone passes |

**Severity:** clarification needed each; the DRAINING row is a design-doc claim
("the guard's path runs constantly") that is false of any assertion.
**Confidence:** high. [evidence-audit F1, F3, F3b, F4; atomics F7; protocols P4]

### C8. Smaller items

- **Receive side trusts three lengths** — a get reply's `chunk_offset`
  (`progress.c:50`), a message `len` against its segment (`am.c:161-167`),
  `message_count` against `total_len` (`am.c:138`). Minor on shared memory; a
  forged reply overwrote 64 bytes past a landing zone 3 of 3 with the test
  exiting 0 (portability F1). **A step-3 entry condition.**
- **The serializing store on aarch64** (`base/order.c`) is an `stlr` to a
  thread-local nothing reads. Nothing shows it serializes anything; fence's
  store-visibility rule on aarch64 rests on the locked operations that follow.
  §4.6 should say so. Unmeasured; a three-thread litmus would settle it.
  [atomics F5]
- **§3.16 says ISO C defines four memory orders**; *recalled*: C11 §7.17.3
  defines six. The rule is fine, the stated reason is not. [atomics F5]
- **One futex shape, two orders**: `hdr.ready`/`failed` stored `release` in
  `node.c:441,64`, the identical shape `seq_cst` in `wire.c`, `job.c`,
  `connection-area.c`, `agent.c`. Neither loses a wake (the kernel's bucket lock
  supplies the other half); §3.16 should name the shape. [atomics F4]
- **The job map's duplicate-rank check** is a cross-process plain load/store race
  (`job.c:205-214`) and misses simultaneous leads. [atomics F6]
- **The root's own arrival skips the epoch check** its handler makes
  (`progress.c:662-664` vs `:213-217`). [protocols P6]
- **`flush_before_close` is a bare spin** (`progress-host.c:284`). [atomics F7]
- **The model engine detects deadlock only**, not livelock; a reachability check
  added by the audit finds no trapped state in any current `design` case, so no
  verdict is wrong. Only 3 of 9 models are registered with ctest.
  `arrival_model.py:216-220` claims a variant fails that passes.
  [evidence-audit F5–F7]
- **Waiters re-wake the lead every ~20 µs each** (`transport-am.c:183,311,603`):
  at 256 PEs in a barrier's quiet that is 256 locked adds and 256 syscalls per
  period on one cache line. A step-2 stand-in that must not survive step 3.
  [atomics F3]

### C9. The load survey: one more timing-fragile test, of exactly the predicted shape

Run after the rest of this document was assembled; full method and logs in
`evidence/phase1-step2-slice1/reports/load-survey.md`. Release preset, private
copy, 16 spinners on 4 cores (load average 12–17), VM otherwise idle.

- **Full suite under load: 131/131 in 5 of 5 runs.** All 52 multi-process tests
  (every `internode`, `multinode`, `force-remote`, `differential`, `barrier*`,
  `wire*`, `progress`, `signal`, `quiet`, `landing`, `wait*`, `stat`, `watchdog`,
  `reply`) then ran 30× each in isolation under load: **51 had 0 failures.**
- **`shmem.wait.L2` failed 2 of 53 isolated invocations (~4 %)**, never in a
  full-suite run. *Measured.*
- **Diagnosis (lead, from source):** `test-wait.c:35-40` zeroes `flag`, calls
  `shmem_barrier_all`, then asserts `flag == 0`. But the ring that follows has each
  PE sleep `SLEEP_MS = 5` ms and then `atomic_add` its *neighbour's* flag
  (`:50-52`). A PE descheduled for more than 5 ms between leaving the barrier and
  line 39 sees 1. **The test asserts something the code does not promise** — that
  a PE reaches its first check within 5 ms of its neighbour leaving the barrier.
  Then, because `CHECK` records and continues (`check.h:9-14`), the test proceeds
  to `shmem_uint64_wait_until(flag, SHMEM_CMP_EQ, 0)` at `:41`, which can never
  become true, and hangs until the launcher's 60 s timeout — the prompt's
  "a test that keeps running after a failed check" pattern, here turning a
  failed assertion into a hang. The library is not at fault.
- **Fix shape:** a second barrier after the already-satisfied checks and before
  the ring; and consider making `CHECK` fatal in tests whose later steps block.
- **Also noted:** the failing run's launcher reported two objects "left behind"
  that were gone by the end of the survey; `/dev/shm` showed no net growth.
- **Severity:** minor (test defect). **Confidence:** high on the mechanism; the
  rate is from 53 invocations.

This is a reassuring result with one caveat. The three earlier flakes plus this
one make four found by this technique, and the survey covered one preset at one
load level on one host.

## What is sound, and what convinced us

- **§3.16's letter** — mechanical sweep, each grep first shown to find a known
  site (162 `_explicit` accesses).
- **Two-sided protocols** — *measured* on aarch64, 1M rounds: the tree's
  `stlr`+`ldar` pattern gave 0 both-zero outcomes where the plain-store control
  gave 68–203, so the harness can see the effect it reports absent. The put
  `memcpy` path lost 58, which is §4.10's documented best-effort case made
  visible. Re-run on a Cortex-A76 before relying on it.
- **Predicate-then-bump** (agent stop, arrival handoff, landing slot, wire) is
  sound under C11's model, not only TSO; every site loads the sequence word first.
- **§3.5 reservation** — new model from the C; ORDER and ONCE were never the
  *first* check to fail in any variant, so treat those two as unproven, not passing.
- **§5.4 equality waits, teardown ordering, wire close, agent stop** — sound by
  line-by-line reading; arguments in the protocols report. Not modelled.
- **Exactly-once rmw, ack-after-apply** — rest on a reliable ordered stream and
  `ack = applied` set only after a whole segment is applied.
- **All ten recorded model runs reproduce** to the state count; no drift between
  `docs/design/evidence/` and `tools/modelcheck/`; every model check but one
  unreachable arm has a variant that reaches it.
- **The launcher** fails a job on any nonzero exit, signal death, timeout or
  leaked shm; no third way for `arrivals.h` to be vacuous was found; 131/131 in a
  clean private run; every test has a timeout and a label.
- **Both barrier early-release mutants** are caught in milliseconds by the
  library's own epoch-divergence fatal. `invariants.h` reading
  `invariant.missed_wake` through the `osoe-stat` reader is the most valuable
  single check in the suite.

## Where the evidence is weakest

1. **No model of the acknowledgement protocol**, which is where the defects are.
   The protocols report specifies it: current tree should fail quiescence, the
   naive fix should fail termination, a sequence-carrying request should pass both.
2. **The load survey covered one preset, one load level, one host** (C9). Repeat
   under `debug` and `asan`, where timing differs most.
3. **Teardown, the multi-node barrier and the dirty set are unmodelled.**
4. **Catch rates from single runs** for the quiet and notify-bump mutants; six
   planned mutations not run (credit off-by-one, reservation overlap, STALESEQ,
   malformed-segment fields, cross-node exactly-once rmw, fence ordering).
5. **tsan cannot see the multi-node path**, as the what-changed note says; and the
   tsan preset will not run at all on a 47-bit-VA kernel such as the Pi 5's
   (portability F14).

## Lessons for a successor API

- **`shmem_quiet` has no handle**, so it must cover everything a context ever
  sent: the dirty set, the cumulative ack and the request-an-ack side channel
  where C1 and C2 live all follow from that. A completion object returned by a
  put or a batch carries its own sequence number by construction.
- **Completion has no "not yet" and no error**, so an implementation must choose
  between hanging and inventing a deadline the specification never gave it (C5).
- **`wait_until` on an arbitrary word** forces a two-sided protocol against every
  write path, including `memcpy` puts that cannot take part in one; the spec then
  patches it with "update with an AMO". Make the waitable object a distinct type
  whose only writers are notifying operations.
- **`shmem_finalize` is both a barrier and the teardown**, which is what makes
  "the root leaves first while still owing the release" possible.
