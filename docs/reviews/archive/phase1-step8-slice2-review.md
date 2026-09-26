# Adversarial review: phase 1 step 8 slice 2 (THREAD_MULTIPLE, interim per-context mutex)

Tree: `build/scratch/wt/slice2` at `602c79d` (diff `main...602c79d`, 37 files). Read-only review;
nothing was built or run. Every claim below was checked against the code; where a claim is an
argument rather than a reproduced failure it says so.

## Verdict

**Proceed after specific changes.** The concurrent protocols themselves hold up: the extent claim,
the release (clear, then wake), the LANDED -> COPYING copy-out claim, the record publication order,
the reply slot's arm/await/complete, and the lock order are sound, and I found no deadlock and no
path that keeps a mutex or an extent claim. Three things stand in the way of merging. First, the
default `OSOE_MAX_THREADS=8` counts the main thread and the agent, so the SOS programs the brief
names as the acceptance set (8 worker threads each) end fatally. Second, `make_room`'s waiter
checks with acquire loads inside a registered-waiter protocol, which breaks §3.16 and
`wait-word.h`'s own contract. Third, no test reaches the slice's cross-thread machinery: tsan runs
only the intranode path, and the map tests never fill the zone. That third point is how the
unreleased-mutex bug fixed in 602c79d went unseen until a hang turned it up.

## Findings

### Blockers

**B1. The default thread table cannot hold the acceptance set's SOS programs.**
*Location:* `shmem/src/metrics.c:395` (`OSOE_DEFAULT_MAX_THREADS 8`), `:495` (the fatal);
`shmem/src/agent.c:49` (the agent claims a block); design §4.11 "How many".
*Scenario:* the brief's acceptance set includes "the SOS `mt_` programs".
`corpus/tests-sos/test/unit/mt_a2a.c`, `mt_contention.c`, `threading.c` and
`shmemx/gettid_register.c` each run 8 threads that call the library at the same time. Those 8
threads, plus the agent, plus the main thread (test-multiple.c's own header says "8 holds the main
thread, the agent and one wave"), come to 10 live claimants against a table of 8. The ninth claim
reaches `osoe_fatal("more threads of this PE are in the library than OSOE_MAX_THREADS=8 ...")`.
The same arithmetic ends any SERIALIZED program with a pool of 7 or more threads that take turns
calling in. Such a program ran before this slice, because a full table used to fall back to the
orphan block. The design's own default of "the node's online CPU count" does not fix this either:
it would be 4 on the VM and on a Pi, and it does not count the agent.
*Evidence:* observed in source (the counts); not run. Confirm with one command:
`osoe-launch -n 2 -- mt_a2a` at the default settings.
*Fix:* give the agent a block outside the `T` the application gets, for example a reserved index
`T`, so that `OSOE_MAX_THREADS` means application threads. Then raise the default to something the
known programs fit, such as `max(64, online CPUs)`. Both the thread records and the counter blocks
are `local PEs x T`, not job-sized. If the default stays small, the SOS sweep must set
`OSOE_MAX_THREADS`, and the runbook and §4.11 must say so.
*Confidence:* high.

### Should-fix

**S1. `make_room`'s check is an acquire load in a two-sided wait (§3.16).**
*Location:* `transport-am.c:660` (`claim_in_word`'s first load of the bitmap word, `acquire`),
`:737` (`copy_out_any`'s state load, `acquire`), `progress-host.c:153` (`landed` stores LANDED with
`release`). The waiter registers on `freed` (`osoe_wait_word_enter`, `:759`), and the movers wake
only when the waiter count is nonzero (`release_run`, `landed`).
*Scenario:* `base/wait-word.h` states that "the value's store and the waiter's check of it are
seq_cst". Here the waiter's check is `acquire`, and on Armv8.3+ GCC emits that as `ldapr`. The
sequence that loses the wake:
1. The waiter's `ldapr` of `bits` is satisfied before its registration (the release half of
   `LDADDAL`) becomes visible.
2. `release_run`'s `fetch_and` clears the bits, and its load of `waiters` reads 0, so it does not
   bump.
3. The waiter sees a full word and the old `seen`, and it sleeps.

This is the same store-buffer shape that `osoe_transport_load`'s comment records as measured on the
A76. The sweep bounds the sleep at 100 ms, so it is a stall and not a hang. `make_room` has no
`count_if_lost`, though, so `invariant.missed_wake` never records it. `landed`'s release store of
LANDED, which `copy_out_any` checks, is the same case on the mover's side. On x86-64 the locked
registration keeps it from happening.
*Evidence:* argued from the code and §3.16; not reproduced. `landing_extent_model.py` does not
cover this variant: it sleeps on the bitmap word itself, wakes unconditionally, and runs SC only
("runs are SC"). The implementation instead sleeps on a separate `freed` word with a waiter count.
*Fix:* `seq_cst` on the first load in `claim_in_word` and on the state load in `copy_out_any`, and
`seq_cst` on `landed`'s LANDED store. Add `count_if_lost` after a sweep-woken success. Extend the
model to the `freed` word with its waiter count, or note in §4.11 that the design moved off the
modelled shape.
*Confidence:* high that it violates the rule; medium that it shows up in practice.

**S2. The tests would not catch a breakage of the slice's own concurrent paths.**
*Location:* `shmem/tests/CMakeLists.txt` around line 1627 and `shmem/tests/test-multiple.c`.
- Under tsan only `osoe_tier2_test(multiple)` runs. That is the single-node `L<n>` counts, where
  every operation takes the intranode path. tsan therefore never sees the context mutex, the
  extents, the reply slots, `copy_out`, `pending` or `copy_destination`. The map tests are excluded
  under tsan (the reason is at CMakeLists.txt:426-431, 632-637). The unreleased-mutex bug fixed in
  602c79d was found by a map test hanging, not by a race tool.
- In the map tests (`OSOE_GET_LANDING_BYTES=8192`, so 8 extents), each worker has at most one piece
  in flight: a 64-byte `get_nbi` quieted at once, or an 8-byte blocking get. With 4 workers that is
  at most 4 extents out of 8, so the zone never fills. That leaves several things unexercised by
  any test: `make_room`'s wait, `copy_out_any`, and the LANDED -> COPYING race between two threads
  (the one `copy_out_claim_model.py` exists for). `am.landing_full` is not even asserted.
  `test-context.c` fills the zone, but with a single thread.
- The brief's test list has more gaps: nothing issues nonblocking gets on the shared default
  context (`complete_gets` under the mutex, with pieces copied out by other threads); no test takes
  locks from several threads; and no test calls into the library from a thread-exit path (see S3).

*Fix:* add a mode that runs several `get_nbi` per thread on `SHMEM_CTX_DEFAULT` before one quiet,
with a 4-extent zone, and asserts that `am.landing_full` and `am.gets_completed` rose. Add a
lock-contention mode. Add a debug check that no context mutex is held when an API routine returns
(a thread-local depth counter checked at the end of `test-multiple`), which would have caught
602c79d's bug on any map. For tsan, either a one-process harness that drives `claim_run`,
`copy_out` and `complete_gets` from threads with a fake lead (as `test-progress.c` does for the
engine), or one map test under tsan with a suppression limited to the lead's landing writes.
*Confidence:* high.

**S3. A thread that calls in after its block was given back keeps using that block.**
*Location:* `metrics.c:451-458` (`release_block`) and the fast path of `osoe_metrics_block`
(`:470`).
*Scenario:* `release_block` clears `THREAD_CLAIMED` but leaves `my_block` and `my_session` cached.
POSIX runs key destructors in no specified order. If the application registers its own key, for
example one whose destructor destroys a per-thread private context at exit, and that destructor
runs after osoe's, then its `shmem_ctx_destroy` goes through `osoe_count` into a block that another
thread may already have claimed. A blocking get or a fetching AMO in the same destructor would arm
another thread's reply slot. Two threads then share one counter block and one thread record, which
breaks the single-writer rule and the slot's one-waiter rule. (glibc runs C++ `thread_local`
destructors before key destructors, so those are safe. Other keys are not.)
*Evidence:* the code path is observed in source; the trigger depends on the application.
*Fix:* in `release_block`, also set `my_block = NULL` and `my_session = 0`. A later call then
claims again and re-sets the key, and POSIX reruns destructors up to
`PTHREAD_DESTRUCTOR_ITERATIONS`.
*Confidence:* medium.

**S4. A thread exiting while the main thread finalizes can write into an unmapped segment.**
*Location:* `metrics.c:451-458`; `node.c:1141` (`osoe_metrics_fini`, which sets `pool = NULL`) and
the `munmap(n->hdr, ...)` about 25 lines later.
*Scenario:* `pool` is a plain global, and `pool_session` is bumped at init but not at fini. A
worker thread exiting while the main thread runs `shmem_finalize` (legal, since the worker is not
inside a call) can pass the `pool` and session check, get descheduled, and then store into a block
after the control segment is unmapped: SIGSEGV. It is also a data race that tsan would report.
*Fix:* take one small mutex in `release_block` and around `osoe_metrics_init`/`_fini`, or bump
`pool_session` in fini and hold the mutex across the check and the store.
*Confidence:* medium; the window is narrow.

**S5. `record_mutex` is taken on every barrier and collective at every thread level.**
*Location:* `metrics.c:563-615`; callers at `barrier.c:133,147,156,164,182` and
`collectives.c:167,352,425,592`.
*Scenario:* the comment says the mutex is "taken only on the way into and out of a wait that
sleeps, never on a path that does not". That is false: `team_sync`, `osoe_barrier_all` and every
data collective call `osoe_blocked_enter` unconditionally. So every barrier now takes two mutex
round trips, even under SINGLE. That contradicts brief decision 1 ("Below MULTIPLE nothing takes a
lock it did not take before") on a timed floor path, and the cost is unmeasured (per the project's
rule that performance claims need timing). Separately, the single-owner scheme shows only the
first waiting thread. If a thread sits in a long wait, as `sit` does in the exhaust test, a hung
barrier on another thread never appears in `osoe-stat`. The brief asked for "osoe-stat shows each
thread's wait". `tools/stat/segment.c`'s `osoe_segment_watch` likewise shows only the first
record with a watch.
*Fix:* a compare-and-swap on an owner word instead of the mutex, or better, put the "blocked in"
record in the thread record, which is what the brief says. Correct the comment. Time the barrier
floor before and after.
*Confidence:* high on the facts; the performance cost is unmeasured.

**S6. The design notes and the brief are not reconciled with what was built.**
The diff touches no file under `docs/design`. Four places are now wrong:
- §3.14 still says exhaustion is "visible rather than fatal".
- §4.6 "As built" still describes `OSOE_GETS_IN_FLIGHT` records, the blocking slot and the bump
  arena.
- §4.11 gives the default `T` as "online CPU count" (the code uses 8). It says a claimer sleeps
  "on the word with the value it read" (the code sleeps on `freed`). It says "`PRIVATE` contexts
  cache the creator's block", which was not done; harmless, but the text is wrong.
- Brief decision 1 says `shmem_init` "still chooses the most, now MULTIPLE". `init.c` chooses
  SERIALIZED (`OSOE_INIT_LEVEL`). The choice is defensible, since spec §9.2.2 lets the
  implementation choose, but it is a decision and belongs in the decisions log.

*Fix:* update §3.14, §4.6 and §4.11, and record the `shmem_init` level choice.

### Nits

- **N1.** `tools/stat/segment.c:674` uses `__atomic_load_n`, which §3.16 forbids; the rest of the
  file uses `atomic_load_explicit` on `_Atomic` casts.
- **N2.** `threads_concurrent` (`transport-am.c:182`) is written in `osoe_transport_threads` and in
  fini but never read.
- **N3.** `token_sequence` is `_Thread_local` (`transport-am.c:793`). Two consequences:
  - A thread that reuses a released record starts its sequence at 1 again, so its early tokens
    repeat those of the thread that last held the index.
  - Two threads on one shared context mint nonblocking tokens with the same owner and sequence,
    differing only in extent.

  Neither is a correctness bug for this PE's own operations: blocking replies are matched exactly,
  and `complete_gets` counts bytes. It does weaken §3.4's duplicate-reply check back to "after a
  thread exit" rather than "after 2^32". Keep the counter in the thread record, or in the context
  block for nonblocking tokens.
- **N4.** `complete_gets` rescans all of `pending` on every wake and never compacts it
  (`transport-am.c:1007`). `pending` grows without bound between quiets, even though `make_room`
  has long since copied most pieces out, so the cost is O(pending x wakes). Drop entries whose
  record no longer holds their token.
- **N5.** `lock.c`: `refuse_held` and `remember` are separate critical sections. Two threads of one
  PE acquiring the same lock (which spec §9.13 puts on the user, and SOS's `mt_lock.c` wraps in its
  own guard) get a misleading "a lock must be zero on every PE before its first use" fatal, after
  the second thread's `node := 0` has overwritten the first thread's queue node. Mark the key as
  pending under `held_mutex` before touching the queue, and name the real cause.
  `osoe_lock_warn_held` reads `held_count` without the mutex.
- **N6.** `context.c`: `osoe_context_team_destroyed` and `osoe_context_finalize` scan `pool` without
  `pool_mutex` while another thread's `create` may be writing an element. This is a formal data
  race.
- **N7.** `OSOE_GETS_IN_FLIGHT` is now silently ignored, and an `OSOE_GET_LANDING_BYTES` that is not
  a multiple of 1 KiB is silently truncated. Say so once, or refuse the value.

## Checked and found sound

- **The extent claim and release.** `claim_in_word` claims by CAS over one word, so a run never
  crosses words. `release_run` clears the bits before waking, which is the model's `wake_first`
  ordering. A record is set back to FREE before its bits are released, and the next claimer's CAS
  reads the `fetch_and`'s value, so its `issue_record` happens after that FREE.
- **Copy-out claim ABA.** Between `complete_gets`' token check and `copy_out`'s CAS, the record can
  be freed, reissued and landed as another piece. The CAS then copies that other piece on its
  owner's behalf: it reads the record's token and length after winning, uses its
  `copy_destination` and credits its block. Completion is by bytes, so this is harmless.
- **`copy_destination` is a plain write, read on another thread.** It is ordered by a chain:
  - issuer release store of ISSUED -> lead acquire load in `landing_request`;
  - lead release store of LANDED -> copier's CAS.

  The chain holds across processes.
- **Reply slot.**
  - `arm` stores `ready = 0` and then `armed_token`, both with release.
  - The lead's acquire of `armed_token` puts its `ready = 1` after that 0 in modification order.
  - The waiter loads `wake` before `ready`, and the lead's bump is a seq_cst RMW followed by an
    unconditional `futex_wake`. That is §3.16's own-predicate shape, correctly ordered.
  - Duplicates are refused: `LANDED_BLOCKING` or `LANDED` fails `landing_request`'s ISSUED check.
- **Deadlock.**
  - Every path calls `make_room` before `take_credit`.
  - Nonblocking credits come back at landing, and blocking ones once the thread has read its reply.
  - A piece's extents are claimed only after its context's mutex is taken, and its request is sent
    in the same call.
  - The lead never waits on an application thread.

  I found no cycle through the context mutex, the extents or the credits. A blocked thread holding
  the default context's mutex delays other threads but always completes, which satisfies spec
  §9.2 item 3.
- **Mutex release and lock order.**
  - After 602c79d, all six context entry points (`put`, `get`, `rmw`, `fence`, `wait`,
    `wait_gets`) pair `context_enter` and `context_leave` with no return in between.
  - The allocator's, the held list's and `record_mutex` are leaves.
  - `pool_mutex` is never nested with a context mutex: `destroy` takes them one after the other.
  - No path takes two context mutexes.
- **Watch.** The range is stored and then the bit is set, both seq_cst. The writer loads the bitmap
  and then each range, both seq_cst, and wakes every overlapping record. Unwatch clears the bit and
  then the range. This matches `watch_list_model.py` (`bitfirst`, `plainbit`, `firstmatch`).
- **Nothing grows with the job's size.**
  - Thread records are `local PEs x T x 128 B`, with `T <= 1024`.
  - `watch_active` is a fixed 16 words.
  - The zone is fixed by `OSOE_GET_LANDING_BYTES`.
  - The context block gained one word.
  - collect's lengths are O(members) in private memory and on the tree, which is C4 as the author
    accepted it.
  - The internal region's spare words sit at +64..+88 of a 128-byte line and so do not overlap the
    next team's line.
- **Collectives' scratch.** Each member writes its length word before bumping `arrived`, and the
  lead reads the lengths before releasing `released`. The lead's total word is rewritten only after
  every member has arrived at the following gather. The split's number word is used by one
  collective on the parent at a time. All of this is safe under concurrent collectives on
  different teams (per-team `internode-generic` state and per-team words).
- **The rmw fix in 602c79d is correct.** The wave barrier added to the test closes the test's own
  race.

## Not verified

- B1 was worked out by arithmetic and not run; confirm it with `mt_a2a`.
- S1's lost wake was not reproduced. `fence-litmus` in the VM, with an `LDADDAL` then `LDAPR`
  variant, would settle it.
