# Implement phase 1, step 2, second slice: what the second review found

You are continuing **§7.1 step 2** of **osoe** (OpenSHMEM Over Ethernet). Slice 1
built the in-process AM backend and merged as `d542c63`. A second adversarial
review then ran against it. This slice fixes what that review found *below and
beside* the data path — the acknowledgement protocol, the silent failure paths,
timeouts, cleanup — and makes each defect visible to the test suite before
fixing it. Slice 3 (`phase1-step2-slice3-prompt.md`) then does the internode
seam, the tree, and the scaling work; **do not start on it here**.

Read `docs/prompts/README.md` for what "phase", "step" and "slice" mean. Machine
setup is unchanged: see "Setting up on a new machine" in
`phase1-step2-slice1-prompt.md`.

## Read first

1. `CLAUDE.md` — naming, delegation, mechanical-rename rules.
2. `docs/reviews/archive/phase1-step2-slice1-review.md` — the synthesis. Finding IDs
   below (B1, D1…D15) are its IDs.
3. `docs/reviews/archive/phase1-step2-slice1-correctness.md` — C1–C9, with the evidence.
4. The evidence you will reuse, under
   `docs/reviews/evidence/phase1-step2-slice1/`: `protocols/` (`ack-echo.*`,
   `naive-echo-fix.patch`, `quiet-stress.*`, `reservation_model.py`),
   `atomics/drain-reserve-accounting.c`, `evidence-audit/model-variants/` (the
   reachability check) and `mutations/`, `diagnosability/programs/`
   (`lost-wake.patch`, `hang.c`), `portability/forged-get-reply.patch`,
   `performance-scale/limit-*.c`, `omissions/run-signal-experiments.sh`.
5. Design §3.4, §3.5, §3.8, §3.10, §3.15, §3.16, §4.6, §6.2, §6.4.

**`docs/reviews/` is a committed record: read it, never edit it.**

## Who does what

You orchestrate, and you do the load-bearing work yourself: the models, the
protocol changes, and every design-doc change. Delegate to Sonnet subagents (Agent
tool, `model: "sonnet"`) what CLAUDE.md lists: builds and preset runs in the VM,
load loops, greps, the mechanical edits of part D, checklist sweeps. **Run at most
two subagents at a time** — the account's usage window is small, and eleven at
once exhausted it during the review. A subagent's report is evidence, not a
verdict.

## State

`main`, clean tree, **131 tests**, four presets green. The review and these briefs
are committed; nothing in `shmem/`, `base/` or `tools/` has changed since
`d542c63`.

## Decisions already made — do not reopen

The author decided these on 2026-09-20. Implement them; if one proves wrong, that
is a *surprise* (see Working method) — stop and say so.

1. **No fatal time bound anywhere in the library.** A wait may give up only for
   lack of *progress*: the thing it watches has not advanced for
   `OSOE_NO_PROGRESS_TIMEOUT` seconds. **Default 0 = never.** This applies to
   init waits too. `OSOE_BOOTSTRAP_TIMEOUT` is retired. We cannot protect against
   every failure on a large machine, and we will not fail early under contention.
   *(Wording trap: §3.10's "futex deadlines are absolute" means an absolute clock
   instant for one sweep-length sleep. That stays. What goes is any deadline whose
   expiry is fatal regardless of progress.)* Harness timeouts — ctest,
   `osoe-launch -t` — are not library code and stay.
2. **The ack request carries a sequence number** ("acknowledge through S").
3. **The DRAINING guard is needed, as a pre-open drain** (part B2).
4. **Every shared-memory object is unlinked as early as possible — the control
   segment included — and the library installs no signal handlers.** This
   *reverses* §10's recorded decision that the control segment keeps its name until
   finalize. The reasoning, to be written into §10 and §3.14: what that decision
   wanted was uncooperative diagnosis of a *live* job from outside, and the name was
   only the rendezvous; `/proc` is a rendezvous that needs no name. A name that
   survives a crash is at once the leak and the post-mortem record — the same bytes
   — so only one can be the default, and the default is that **no death of any
   process can strand an object**. A site may configure Slurm to clean `/dev/shm`
   between jobs on exclusive nodes; the library must not depend on it.
   `OSOE_KEEP_CONTROL_SEGMENT=1` opts back into the named segment for post-mortem
   work.
5. The internode seam and the tree are slice 3.

## Order of work

Four parts, in order. **Part A exists so that parts B and C can be seen to work:
every defect is reproduced, and every new check is shown to fail, before its fix
is written.** That is also the refutation pass the review did not get — if a
reproduction fails, the finding may be wrong, and that is a surprise.

---

### Part A — make the defects visible

**A1. Livelock in the model engine.** `tools/modelcheck/engine.py:456-468` detects
deadlock only (evidence-audit F5). Add the reachability check the audit wrote
(`evidence-audit/model-variants/`): a state is *trapped* if no state satisfying
the model's `done` predicate is reachable from it. Report trapped states as a
failure. *Criterion:* a wire reader that never advances, which today reports
`ok`, fails; all existing `design` cases still pass; `selftest.py` covers the new
check with one case that must fail.

**A2. A model of the acknowledgement protocol** — `tools/modelcheck/ack_model.py`,
recorded under `docs/design/evidence/am-backend/`. Two leads, one writer per
side, two or three segments, header-only segments allowed, the flag consumed at
*promotion* as the C does it (`progress.c:492-502`), TSO and SC.
- Property **TERMINATES**: every `quiet` returns (no trapped state with a quiet
  outstanding).
- Property **QUIESCES**: in any state where no writer wants an ack beyond what
  its peer has acknowledged and nothing is unsent, no lead has a send step
  enabled.
- *Criterion:* variant `current` (one `owe_ack` bool) **fails QUIESCES**; variant
  `naive` (`naive-echo-fix.patch`) **fails TERMINATES**; variant `design` (B1
  below) passes both. If `current` or `naive` passes, stop: that is a surprise.

**A3. A missed-wake counter for epoch waits** (D8). In `osoe_epoch_wait_eq`
(`base/epoch.c:25-37`), when a sweep expires and the condition is *already true*,
count `invariant.missed_wake` exactly as `wait.c:107` does. §3.14 already says
"or any other wait". *Criterion:* apply `diagnosability/programs/lost-wake.patch`
(removes `osoe_epoch_wake_all` at `progress-host.c:115`) in a scratch copy: today
all 22 barrier/internode tests pass; after A3 they must fail through
`invariants.h`. Record the before/after in the commit message.

**A4. Node-scope AM metrics** (D10, first half). Export through the §3.14 metric
table what `progress.h:106-114` keeps private: segments sent, header-only
segments, reply-reservation failures, control- and bulk-queue high-water, pool
high-water. Node scope only; a per-connection scope is slice 3. They are what
lets a test *see* B1 and B3. Expect a control-segment layout bump; §3.14's
append-only rule and `OSOE_SEGMENT_HAS` apply.

**A5. Reproductions as tests, failing first.** Each lands as a registered test
that fails on the current tree, in the same commit as — or the commit before —
its fix. If you land it before, register it `DISABLED` with the reason, and
enable it with the fix.
- *Ack echo:* one put, one quiet, idle 500 ms, then assert header-only segments
  sent ≤ a small constant (from A4's counter). Basis: `protocols/ack-echo.c`.
- *Reply pool:* `OSOE_AM_CREDITS=2`, enough concurrent gets to empty the pool.
  Basis: `performance-scale/limit-pool-exhaustion.c`.
- *Reply starvation* (B2): many writers, a tiny connection buffer
  (`OSOE_AM_*` sizing variables), one remote getter; assert the get completes
  while the writers are still running. The review could **not** provoke this with
  two writers on four cores, and argued it from the source — so this reproduction
  matters. If you cannot make it fail with up to 16 writers and the smallest legal
  buffer, stop and report before writing B2.
- *Unswept waits* (D2): a PE in an internode `shmem_quiet` toward a SIGSTOPped
  node, a sibling declares abort; assert the waiter exits within a few sweep
  intervals and that `osoe-stat` shows it `blocked in: shmem_quiet`.
- *Forged lengths* (D4): extend the malformed-segment tests with a get reply whose
  `chunk_offset` exceeds the landing slot, a message `len` overrunning its
  segment, `message_count > 0` with `total_len == header`. Basis:
  `portability/forged-get-reply.patch`.

**A6. Test hygiene.**
- `test-wait.c`: a second `shmem_barrier_all` after the already-satisfied checks
  (`:39-43`) and before the ring (C9). Add `CHECK_FATAL` to `check.h` — prints like
  `CHECK`, then exits nonzero — and use it wherever a failed check is followed by a
  call that would then block forever. Sweep the suite for that shape (Sonnet).
- `shmem.watchdog.*`: the `;` in `PASS_REGULAR_EXPRESSION` splits the pattern
  (evidence-audit F4).
- Register **all** models and `selftest.py` with ctest, each exiting nonzero on an
  unexpected verdict (F6). Remove wall-clock timings from recorded `.out` files so
  a plain `diff` is the reproduction check (F8). Fix or delete the claim at
  `arrival_model.py:216-220` (F7), and the unreachable "lost" arm.
- `OSOE_SEGMENT_HAS`: add the older-shorter-directory case the premise audit
  built (`premises/segment-has-mutation.log`; one constant at
  `test-segment.c:154`), so reverting `segment.c:126` fails a test.
- `test-connection.c:295` must count `REPLY_ID` messages (needed by B2).

---

### Part B — the protocols

**B1. Acknowledgement: "acknowledge through S".** (Review B1; correctness C1+C2.
Fix both together — C1's echo is what keeps C2 from hanging `quiet`.)

Pinned design:
- *Writer side.* `osoe_connection_request_ack` takes the segment sequence the
  caller needs covered — `transport-am.c` already tracks `high_segment[slot]` —
  and publishes it into a shared per-connection word `ack_wanted_through` as a
  **monotone maximum**: CAS loop, `seq_cst`, skip if already ≥. Sequence
  wraparound: state and test the comparison you choose.
- *Lead side.* Split the bool. `owe_ack` = "the peer asked; my next segment
  pays it" — set at `progress.c:283`, cleared whenever *any* segment is finalized,
  since every segment carries the cumulative `ack = applied`. A segment carries
  `ACK_REQUESTED` iff its own `seq ≤ ack_wanted_through` and `seq > acked`.
  **A header-only segment sealed only to pay a debt never carries the flag.**
- *Late request.* If `ack_wanted_through > acked` and no unfinalized segment with
  `seq ≥ ack_wanted_through` remains to carry the flag, seal one header-only
  segment *with* the flag. Its acknowledgement is cumulative and covers S. It must
  not be re-sent every pass: remember the highest sequence sent carrying the flag.
- `osoe_progress_forget_acks` and its comment (`progress.h:154-158`) go; teardown
  must no longer need it. `osoe_progress_idle` then needs no exception for acks.
- The wait loop in `osoe_transport_wait` does not change shape here (B4 does that).

*Criteria:* A2's `design` variant passes; the A5 ack-echo test passes; the
injection from the protocols report (`quiet-stress.sh` with the two 3 ms sleeps)
completes 20 of 20; `fence` no longer re-flushes peers that have acknowledged
(algorithmic-scaling 10); mutation — make the debt-paying segment carry the flag
again — fails the echo test. Update §3.4 and §4.6.

**B2. The pre-open drain.** (Correctness C7; protocols P4.) Today `drain()`
competes with application writers through `osoe_connection_reserve`, so under
saturating puts replies — get replies, AMO replies, *and barrier arrivals and
releases* — can wait indefinitely. §3.8's DRAINING state already exists
implicitly: between `osoe_connection_release` and `osoe_connection_open` a buffer
keeps its seal bit and is private to the progress thread.

Pinned design:
- In `flush`, immediately before `osoe_connection_open(conn, k, …)`
  (`progress.c:531`), drain pending replies into the free buffer `k` through
  `osoe_connection_drain_reserve`: control queue first, then bulk with the existing
  one-chunk-per-descriptor rotation, **capped at half the buffer's capacity**
  (§3.8's cap, review D8 of round one).
- Fix `osoe_connection_drain_reserve`'s accounting (`connection.c:149-174`;
  `atomics/drain-reserve-accounting.out` shows `head=96 filled=64`): while the
  buffer is private, plain `release` stores suffice, and `filled` must count the
  same bytes `head` does.
- `osoe_connection_open` takes the pre-filled extent and publishes `filled`, `seq`,
  then `head` at that extent — not reset to the header size — preserving its
  existing store order (seal bit cleared last).
- The opportunistic `drain()` into an open buffer stays: it is what keeps reply
  latency low when there is room.
- Correct `connection.h`'s header comment: what it says about DRAINING must be true
  of the loop, not only of a test.

*Criteria:* extend `protocols/reservation_model.py` — promote it into
`tools/modelcheck/` — with the pre-open drain and a broken variant (open resets
`head` over drained bytes) that fails; the model's ORDER and ONCE checks must each
be shown able to fail as the *first* violation in some variant (they never were:
protocols report); the A5 starvation test passes; `test-connection.c` counts
replies and fails under mutation (`drain_reserve` body → `return false`).

**B3. The reply pool.** (D1.) Size it by §3.8's own formula — per connection
`C + degree × max_teams + slack`, from the node-wide pool — with `degree` a named
constant that slice 3's tree will set; today it is 1. An empty pool in `enqueue`
(`progress.c:61-72`) becomes what `reply.h:38-42` promises: `osoe_fatal` naming the
peer node, the queue depths, and the kind of request. The comment "leaves the
request" is false — the parser has consumed it — so fix the comment, not the
parser. `enqueue_collective`'s message must stop blaming the credit.
*Criteria:* the A5 pool test dies with the named fatal on both nodes' stderr
rather than a 60 s timeout; with default sizing, a test holding `C` gets
outstanding per connection plus a barrier completes.

**B4. Sweeps, the blocked record, and the no-progress rule.** (D2, D3.)
- All four AM loops — `transport-am.c:175-193` (`reserve_waiting`), `:305-321`
  (`take_credit`), `:335-351` (`await_reply`), `:601-610` (`osoe_transport_wait`) —
  call `osoe_sweep_expired` on the §4.10 schedule (abort word, lead liveness,
  `pe.sweeps`), and bracket the wait with `osoe_blocked_enter`/`leave` naming the
  routine and, in `detail`, the **peer node**. `await_reply` must stop discarding
  `ETIMEDOUT`.
- One helper in `base/` expresses the rule: given a *progress word* and the
  configured interval, report "stalled" only if the word has not changed for that
  long. Default interval 0 → never. Progress words: a connection's `acked` or
  bytes consumed from its wire for quiet/reservation/credit; the landing slot's
  token/ready for a get; `joined`/`mapped`/`ready` for init and rendezvous.
- A stall is an `osoe_fatal` that names what was watched, its value, and for how
  long. Remove every use of `bootstrap.timeout_ns` as a fatal bound — grep for all
  of them, including `flush_before_close` (`progress-host.c:284`), which is also a
  bare spin: make it sleep and sweep.
- Retire `OSOE_BOOTSTRAP_TIMEOUT`: environment table, `SHMEM_INFO` text, §6.2,
  §6.4, tests that set it. Tests that need a job to die set
  `OSOE_NO_PROGRESS_TIMEOUT` explicitly.
- `blocked.detail` is untyped and prints as `(+0x3)` (diagnosability D-7): give
  the record a small detail-kind so `osoe-stat` prints "node 3" or "epoch 17".
- The 20 µs re-wake polling in these loops is a step-2 stand-in and **stays** until
  step 3; say so where it lives, with a pointer to atomics F3.

*Criteria:* the A5 unswept-wait test passes; a quiet toward a node SIGSTOPped for
longer than any old timeout completes when the node is continued, with
`OSOE_NO_PROGRESS_TIMEOUT` unset; with it set to 2, the same test aborts naming the
stalled connection; `transport.h:48-55`'s "nothing exercises this yet" is removed.

**B5. Receive-side validation and the trust boundary.** (D4.) Validate, and
`osoe_fatal` naming the peer node on failure: a get reply's token, `chunk_offset +
chunk ≤ total ≤` the landing slot's capacity (`progress.c:44-50`, `:173-190`); a
message `len` against the bytes left in its segment *before* applying it
(`am.c:161-167`); `message_count` against `total_len` (`am.c:138`); `rmw.width` ∈
{4, 8}, a known `op`, offset aligned to width. Then **state the trust boundary**
in §3.4 and §6.2, which today say nothing: peers are the same job and the same
build, assumed buggy but not hostile; the network is trusted at the level Slurm's
own traffic is; validation exists to turn a peer's bug into a named abort on the
receiving node rather than corruption. Leave authentication of the step-3
handshake as an open §10 item with portability F2's suggestion.
*Criterion:* the A5 forged-length tests die with the named fatal and nonzero exit.

**B6. Small correctness items.** The root's own arrival makes the same epoch check
its handler makes (`progress.c:662-664` vs `:213-217`; P6). The job map's
duplicate-rank check claims each rank by CAS (`job.c:205-214`; atomics F6).

---

### Part C — how a job dies, and what it leaves behind

**C1. `osoe_fatal` must not run a collective.** (D9.) `fatal.c:43` calls `exit()`,
which runs `start_pes`'s `atexit` handler — `shmem_finalize`, a barrier — and the
job hangs. Flush stderr, publish the abort word as now, and leave through a path
that runs no `atexit` handlers. *Criterion:*
`conformance/fatal-under-start-pes.*` exits promptly, 10 of 10.

**C2. `exit()` without `shmem_finalize`.** (D14.) First read what the 1.6
specification says about implicit finalization at exit — quote it in the commit.
Whatever it requires, **an exit path must never silently complete a collective
the application did not call**: today the dead PE's handler makes
`shmem_barrier_all` *return* on survivors that never matched it. If the
specification requires an implicit finalize, it must be the real collective and
the hang belongs to the application; if it does not, declare a job abort naming
the PE. Remote nodes cannot see a local abort word until step 4 — say so in
§3.15; do not build step 4 here. If the specification's text leaves no good option,
stop: that is a surprise.

**C3. Unlink early — all six kinds.** Every object is opened only during init. Each
creator unlinks its object as soon as everyone who must open it has: heaps and the
control segment at `mapped == local_count`; the connection area likewise; the job
segment at `joined == node_count`; the wire needs an explicit both-sides-opened
handshake word (it has `leaving` for close, nothing for open). Store ordering per
§3.16; model the wire handshake if it is more than a counter. A re-init after
finalize is a fresh session with fresh names, as now. `osoe-launch`'s leak check
and the tests that rely on it must still mean something: after init completes,
**nothing** named `osoe.*` for this job exists, and the launcher should be able to
assert that *during* the run, not only after it.
*Criterion:* `omissions/run-signal-experiments.sh` — SIGTERM, and SIGKILL, to every
PE after init — leaves nothing. The remaining window is init itself; C4 covers it.

**C3a. `osoe-stat` finds jobs through `/proc`.** Every PE keeps its control-segment
file descriptor open for the life of the session (one fd per PE; say so in §3.14's
cost rules). `osoe-stat` discovers segments by scanning `/proc/[0-9]*/maps` for
`/dev/shm/osoe.<job>.… (deleted)` entries, finds the matching entry in
`/proc/<pid>/fd/`, and opens *that path* read-only; any live PE of the node serves,
so a dead lead does not blind it. *Measured* in the development VM on 2026-09-20
(kernel 6.12, same uid, default `/proc` mount options): a process knowing only
`/proc` discovered an unlinked segment, opened it this way and read live updates.
Not measured: `hidepid=` mounts, a Pi kernel, another uid (expected: root only).
So: add the check to `tools/probe` (create, map, unlink, then open through
`/proc/self/fd` from a child), and make `osoe-stat`'s failure message say which of
"no such job", "not your job" and "/proc is restricted" it hit. Do **not** use
`/proc/<pid>/map_files/`: it needs a capability ordinary users lack (*recalled* —
verify before writing it into the doc). This route also works where Slurm gives a
job a private `/dev/shm`, where a name-based reader outside the job sees nothing;
say so in §3.14. `shmem/tests/invariants.h`, `test-stat.c` and `tools/stat`'s own
tests read segments by name today: move them to the new route, keeping
`tools/stat/test-segment.c`'s foreign-segment test on a plain file, which is what
it is really about. `osoe-stat`'s reader still shares no headers with the library.

**C3b. The opt-in.** `OSOE_KEEP_CONTROL_SEGMENT=1` leaves the control segment named
until finalize, for post-mortem reading; `osoe-stat` accepts a path as well as a
job. Only in this mode can anything be stranded, and only this mode needs
`osoe-stat --gc`, which must then cover it (D11; `stat.c:623-668`) and must refuse
to remove a segment that a live process still maps.

**C4. Init is the remaining window: reclaim, do not trip.** A job killed *during*
init can still leave named objects, and the next job with the same id then dies
with `File exists`. At init, an existing object with this job's name is reclaimed
if its recorded owner pid is dead **and** `fstat` says it is ours; otherwise the
fatal names the owner uid and pid (D15). Never open an object whose `st_uid` is not
ours: today only the size is checked (`node.c:304`, `wire.c:198`, `job.c:121`,
`connection-area.c:209`). `shm_unlink` returning `ENOENT` at finalize is not fatal
(portability F7) — and after C3 it is the normal case.

**C5. The launcher.** `osoe-launch` forwards SIGTERM/SIGINT to its children, sets
`PR_SET_PDEATHSIG` in each, and unlinks whatever of the job's objects still has a
name on the way out — which after C3 means a job that died during init
(`launch.c:259-341`; omissions 6).

**C6. `/dev/shm` capacity.** Before creating objects, compare the request with
`fstatvfs` free space and fail with a named fatal saying how much was wanted and
how much exists. Do **not** `posix_fallocate`: it defeats demand paging and a 4 GiB
Pi cannot afford it. Note in §6.2 that the check is advisory (another job can take
the space later) and that the SIGBUS that follows lands on the *lead* applying
someone else's put (omissions 5). Mark sibling heaps and the connection area
`MADV_DONTDUMP` (omissions 4).

---

### Part D — mechanical, mostly for Sonnet

Each is small; batch them sensibly. Conformance F-numbers are from
`phase1-step2-slice1-conformance.md`, portability's from `…-portability.md`.
- `SMA_SYMMETRIC_SIZE` through `spec_env()` (F3); reject `SHMEM_SYMMETRIC_SIZE=0`,
  accept the floating-point forms §8 allows such as `1e3` (F9, omissions 11).
- `shmem_pcontrol` declared in `shmem.h` (F7); `<mpp/shmemx.h>` (F8).
- Calls before init or after the last finalize: a named fatal, not a segfault (F5).
- `shmem_test_any`/`wait_until_any` rotate their starting index (F4; §9.11.3).
- The static `libosoe.a` contains `osoe_base` (portability F10). Feature-test
  macros so the tree builds with `-std=c11`, not only gnu11 (F12).
- The probe's checks run at init as §1 says, and refuse with a message
  (portability F3); `tools/probe` tests what the design uses — a shared futex word
  woken across processes in a named shm object (F4).
- Fatal messages that omit the peer (`transport-am.c:189, 317, 348`;
  diagnosability D-6).
- `osoe-stat` bounds `block_count` and `local_count` against the segment's size
  before looping (D-7).
- **Design-doc corrections**, each in the commit that makes it true or, if already
  false, in one commit here: §1 "cores are over-provisioned" vs §3.11/§3.12; §3.16
  — ISO C defines six memory orders, the project uses four by rule; name the third
  futex shape (a word that is its own predicate) and fix one order for it
  (`node.c:441,64` vs `wire.c:238` etc.; atomics F4); "an unaligned 64-bit atomic
  faults on aarch64" — measured not to on Linux, the rule stands, the reason does
  not (portability F13); §4.6 — what carries fence's store-visibility on aarch64,
  where the serializing store is an `stlr` to a word nothing reads (atomics F5;
  **run the three-thread litmus first** and let it decide the text); the header of
  the design doc still says "No implementation".
- **Not here:** the deprecated 1.4 AMO names (F6), generic `shmem_put`/`shmem_g`,
  force-remote's semantics, anything about teams, `<mpp/…>` beyond the one header.

## Out of scope

Sockets and io_uring (step 3). Liveness across nodes, `shmem_global_exit` (step 4).
The internode seam, the tree, work discovery, `local_of_rank`, chained wakes,
per-connection metrics, force-remote (slice 3). New collectives, teams, contexts.

## Rules

The Rules and Testing sections of `phase1-step2-slice1-prompt.md` apply unchanged,
with these amendments:

- The "futex deadlines are absolute" rule is about clock instants. Decision 1 above
  governs whether a wait may ever be fatal.
- **Model before code** for B1, B2 and C3's handshake. A model in which nothing
  can fail proves nothing: each new property needs a variant that violates it
  *first*, and A1's trapped-state check is part of every new model.
- **Every fix lands with the mutation that proves a test sees it**, named in the
  commit message with the before/after.
- **Run the load loop before you call a part done**: every test you added or
  changed, 30× in isolation under 4× CPU oversubscription in the VM
  (`load-survey.md` has the exact method). A test that asserts something the code
  does not promise is the defect to expect; fix the promise or the test, whichever
  is wrong, and say which.
- A control-segment layout bump happens at most once per part; say in the commit
  what `osoe-stat` from before the bump does with the new segment.

## Working method

- State a short plan, then proceed. **Do not stop for approval. Stop only on a
  surprise**, and then say what you expected, what happened, and the options:
  - a reproduction in part A does not reproduce (the finding may be wrong);
  - a model contradicts a pinned design above, or a pinned design cannot meet its
    criterion;
  - a fix needs a wire-format or control-segment change this brief does not
    anticipate, or would change §4.10's contract;
  - the specification's text blocks C2;
  - the `/proc` route of C3a fails anywhere it is tried — a preset, a sanitizer,
    the 16 KiB-page VM — since decision 4 rests on it;
  - a rule in the Rules section blocks progress.
- Branch `phase1-step2-slice2`. Small commits, each building warning-free and
  passing all four presets in the VM, each message saying what was verified and
  how. The design doc changes in the same commit as the code that makes it true.
- Give the user short progress notes at the end of each part.
- When done: summarize commits, what was verified and how, design-doc changes,
  anything deferred, anything the review got wrong, and open questions. Record the
  outcome in `docs/prompts/README.md`. **Ask before merging into `main`.**
