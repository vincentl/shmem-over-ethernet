# Adversarial review: step 5 slice 7 (partial active sets) and the deprecated routines' profiling names

Tree under review: `slice7-review` worktree at eabf27c, `git diff 96b2e0f...eabf27c`.
The review was read-only. The executable evidence is in the scratchpad: `model.py`, an
interleaving model of `active-set.c`.

## Verdict

**No blocker.** The protocol does what the brief claims. Every signal is a count that its
receiver takes back, so pSync is restored and can be reused. That holds even for
back-to-back calls of *different* collectives on one pSync with no barrier between them,
which is more than the spec asks. The tree arithmetic is right for every set size. No
buffer is written before its owner enters or read after its owner leaves, alltoall's
exception aside, which the spec permits.

These need fixing:

- **Tree critical paths are O(log² n).** Every down phase sends to the smallest subtree
  first, and the reduction's up phase serialises its gets. See S1.
- **Stale public header.** `shmem.h` still tells users that pSync is never read or
  written. See S2.
- **Weak test.** The test cannot reach the cases it claims to cover. See S3.

The profiling change is correct.

## Evidence

**Interleaving model** (`scratchpad/model.py`). Each member is a generator that mirrors
`active-set.c` line by line: barrier/sync, broadcast from any root, fcollect, collect with
0–2 elements per member, reduce and alltoall.

How the model treats the network:

- A non-fetching AMO or a put is *in flight* until the scheduler delivers it.
- Delivery between one (src, dst) pair is unordered unless a `fence`, a `quiet` or
  put-with-signal orders it.
- Blocking gets read at one instant.

Each program is 2–8 calls, back to back on **one pSync with no barrier between them**.
Every call has its own dest buffer. The model asserts:

- every result, by value;
- that no barrier, sync or alltoall member leaves before all members have entered;
- that every remote write to call *c*'s buffer at *j* lands after *j* entered *c*
  (alltoall excepted);
- that every remote read of it happens before *j* returned;
- that no hang occurs;
- that all 256 pSync words are `SHMEM_SYNC_VALUE` at the end.

**Result:** 20,000 random trials with set sizes 1–23 had 0 failures.

**The model can fail.** Six mutations were each detected:

| Mutation | Failures |
|---|---|
| no fence in collect | 260/800 |
| no take-back | KeyError and ownership violations |
| offsets read before the release | 300/800 |
| put-signal unordered | 578/800 |
| alltoall's barrier without quiet | 30/800 |
| dissemination partner wrong | 75/800 |

**Tree arithmetic** was checked exhaustively for sizes 1–1024. For every `v`:

- children are `v+1, v+2, v+4, …`;
- `parent(child) == v`;
- the children's subtrees tile `[v+1, v+subtree_size(v))` in rank order, and for the
  root `[1, size)`;
- no node has more than 31 children, which is the size of `offsets[32]`/`lengths[32]`.

Result: 0 violations.

**Slots.** The highest slot used is 70. Every `SHMEM_*_SYNC_SIZE` is 256
(`shmem.h:46-52`). Dissemination uses at most 31 rounds, below the 64 reserved.
`SHMEM_SYNC_VALUE` is −1, and `await` compares signed (`sgn = true`), so its target is
`count − 1`. That is correct.

## Findings

### S1 — should-fix: broadcast, fcollect, collect and reduce are O(log² n) on the critical path, not O(log n)

**Where:**
- `active-set.c:186-187` (`down`: `for k = 1; …; k <<= 1`, smallest subtree first)
- `active-set.c:212-216` and `:305-316` (up phases: wait for *all* children, then fetch
  from each in turn)

**The problem.** A binomial broadcast is log n only when each parent sends to its
*largest* subtree (`v + span/2`) first. Here the child that owns half the set is served
last, so it starts its own subtree after the root has sent log n messages. Each send
costs one message time: bandwidth-bound for large messages, and `putmem_signal` returns
only on local completion. On that assumption, the time to the last member is:

| n | as written | largest-first |
|---|---|---|
| 64 | 21 | 6 |
| 256 | 36 | 8 |
| 1024 | 55 | 10 |
| 4096 | 78 | 12 |

The reduction's up phase has the same shape. `await(count = children)` holds the parent
until its largest child arrives, and only then does it fetch all log n children's M bytes
in turn. So `T(span) = T(span/2) + log2(span)·M`, which is L(L+1)/2·M: 21·M at 64
members.

fcollect and collect forward the *whole* n·M result down the same smallest-first tree.
Each level costs up to L·n·M, and the total is L²/2·n·M.

The design text says "log2 of the set's size deep" (design row at line 5955). That is
true of depth, but not of time. The project goal is logarithmic internode time.

**Fix:**
- In `down` (and so in `gather_then_spread`), iterate `k` from `span/2` down to 1.
  This is a one-line change and makes broadcast, the reduce result and the whole-array
  spread L·M.
- For the reduction's up phase, either:
  - give each child index its own arrival word (slots 71–101 fit easily in 256), and
    combine each child as it arrives, smallest first, because the small ones finish
    first; or
  - issue `getmem_nbi` for every child's chunk and combine after one quiet.
- A tree with no node awareness still spends log2(N·P) internode hops rather than
  log2(N) on an interleaved map. That is acceptable for deprecated routines, but worth
  one sentence in §7.4.

### S2 — should-fix: the public header and several comments now say the opposite of what the code does

**Where:**
- `shmem/include/shmem.h:33-40`, the user-facing header. It says the routines "run over
  the whole job only … any other ends the job by name" and that "pSync and pWrk are
  neither read nor written". Both are now false: a partial set writes pSync.
- More copies of "over the whole job only" in `shmem.h`: `:655`, `:765`, `:827`, `:866`.
- The same staleness in:
  - `alltoall.c:7`, `:52`
  - `broadcast.c:8`
  - `collect.c:8`, `:52`
  - `reductions.c:12`, `:89`. Line 12 also says "Complexity: O(1) here; the reduction's
    own in collectives.c", which is no longer the whole story.
  - `shmem/tests/CMakeLists.txt:1257`
- In the design notes, §7.1 step 5 (line 5856) lists slices 1–6 and says "the only active
  set they accept". §7.5 (line 5991) says "over the whole job".

**Why it matters.** A user reading `shmem.h` would conclude that pSync need not be
initialised for these routines. On a partial set, an uninitialised pSync (for example
zeros instead of −1) makes every `await` succeed one signal early: a wrong answer, not a
hang.

**Fix:** update these texts. `shmem.h` matters most: it should say that pSync must be
`SHMEM_SYNC_VALUE`-initialised, as the spec requires.

### S3 — should-fix: the test cannot reach several of the cases it names

**Where:** `shmem/tests/test-active-set-partial.c` and its registration at
`shmem/tests/CMakeLists.txt:1276-1278`.

1. **No back-to-back reuse is exercised.** The header says (lines 11-12) "three rounds
   back to back on the same pSync, with no barrier between". The brief asks for the same
   pSync "reused in back-to-back calls with no barrier between". But every collective in
   `run()` is followed by `shmem_sync(…, sync)` (lines 84, 94, 105, 116, 128, 139, 153,
   163). The only consecutive calls with nothing between them are barrier and sync. The
   design's claim that pSync "can be reused at once" across tree collectives is never
   tested. The model above supports the claim, but CI cannot.
   - **Fix:** add a run of broadcast (varying root) → fcollect → collect → reduce →
     broadcast with no syncs, each call with its own dest.
2. **Sizes are too small for the interesting trees.** Runs are L1, L2, L4 and the maps
   `0,0,1,1` and `0,1,2,0,1,0`, so no set is larger than 5.
   - No interior node has two or more children; the smallest such case is `v=4` in a
     set of 7.
   - No tree is 3 deep (8 or more).
   - `stride_log` is never above 1.
   - A mutation that breaks only a deeper case would pass. Examples: `span_of` for
     `v != 0`, a grandchild's range in `gather_then_spread`, or the collect offsets two
     levels down.
   - **Fix:**
     - Add one-node runs at 9 and 13 PEs, with `SHMEM_SYMMETRIC_SIZE=16m` as the
       7-PE tests use.
     - Add a set with `logPE_stride` 2.
     - Add a 9–12-PE map across three nodes.
3. **The zero-length paths are never run.** Each member contributes `rank % 3 + 1 ≥ 1`
   elements. So `if (child_bytes[n])` at `active-set.c:214`, a zero total, and `nelems == 0`
   for broadcast, fcollect and reduce never run. The model found no fault with zero-length
   contributions, but CI should run them.
4. **Validation paths are untested.** No test covers a caller outside its set, a stride
   that pushes the last member past the job, or `PE_root` outside the set.
5. **The test's own pSync reuse is beyond the spec.** Each collective is closed by a
   `shmem_sync` on the *same* pSync. The spec allows reuse only after "all previous
   collective routines using the pSync array have completed on all participating PEs".
   So the test is osoe-specific and could not double as a portability check against SOS.
   That is acceptable, but say so in the header.

### N1 — nit: validation refuses a legal set of one with a large stride

**Where:** `active-set.c:133`. `logPE_stride > 30` is fatal even when `PE_size == 1`.

**Breaking input:** `(PE_start 3, logPE_stride 31, PE_size 1)` in a job of 4. The spec
only needs `0 ≤ PE_start + (PE_size−1)·2^stride < npes`, and this set meets it. The
whole-job test at line 130 already allows any stride when `PE_size == 1`.

**Fix:** when `PE_size == 1`, treat the stride as 0 before the range check.

### N2 — nit: pSync misuse is reported under the wrong routine name

**Where:** `active-set.c:101, 111, 119, 254-268, 275-276`. A non-symmetric pSync, or a
NULL one, ends the job naming `shmem_long_atomic_add` or `shmem_putmem_signal` rather
than `shmem_broadcast64`. The failure is correct but the diagnostic is not.

**Fix:** check `osoe_symmetric_word_locate(routine, pSync, …)` once at entry to each
partial-set routine.

### N3 — nit: the brief does not match what was built

**Where:** `docs/prompts/done/phase1-step5-slice7-prompt.md`.

- **Slot map.** Decision 2 lists 66 total, 67 offset, 68/69 second pass. The code uses
  66 total, 67 offset, 68 grand, 69/70 second pass.
- **Alltoall entry barrier.** Decision 5 says alltoall does "a barrier (every `dest`
  ready)" first. The code has no entry barrier. The code is right: the spec's
  precondition says "The dest data object on all PEs in the active set is ready…"
  *before any PE calls* (§9.10 alltoall). The decisions record (item 13) should still say
  the brief was departed from.
- **Test file name.** The brief names the test `test-active-set.c`; the file is
  `test-active-set-partial.c`.

### N4 — nit: CheckProfiling's empty-check guard can no longer fire

**Where:** `cmake/CheckProfiling.cmake:73`. `checked` is increased 7 times without
condition. So `if(checked EQUAL 0)` at line 89 cannot fire. It is still not vacuous: if
nm parsing yields nothing, the deprecated loop reports 21 missing symbols and fails.

Two smaller points:

- The success message at line 92 counts the 7 as "each a strong pshmem_ definition with
  a weak shmem_ alias", which they are not.
- The line-79 exemption `NOT CMAKE_MATCH_1 IN_LIST deprecated` is right and narrow: it
  only exempts `pshmem_start_pes` and the like.

**Fix:** count the deprecated names separately and keep the guard on the pairs.

### N5 — nit: CLAUDE.md naming

**Test file-scope statics.** `test-active-set-partial.c:52-57` has `psync`, `pwrk`,
`isource`, `idest`, `dsource`, `ddest`. They outlive a screen and cross functions. Better:
`psync_arrays`, `work_array`, `int_source`/`int_dest` and so on. `pWrk`/`pSync` are the
spec's spelling at the API, but the lower-case contractions are ours.

**`active-set.c`.** `set_pe`, `span_of`, `absolute`, `up`, `down` and `await` are words,
and `v`/`k`/`n` are short-scope locals: fine. One naming issue there:
`osoe_active_set_is_whole_job` hides that it also validates, calls fatal, and fills `s`.
Something like `osoe_active_set_resolve` returning bool would say more.

### N6 — nit: `pshmem__my_pe` and `pshmem__num_pes` are reserved names in C++

**Where:** `pshmem.h:154-155`. C++ reserves any identifier that contains `__`; C only
reserves a leading `__`. This is harmless in practice and follows from the literal
reading of §10, which the author chose. It should be noted in the header, and C++ tools
should use `p_my_pe`.

### N7 — nit: some size arithmetic is unchecked

- `active-set.c:235`: `size * bytes` for fcollect's whole result.
- `active-set.c:341-344`: alltoall's `rank * nelems * stride * width`.

`osoe_rma_bytes` checks only the per-member size. An absurd `nelems` wraps silently here
where the team path would refuse it by name.

Also, `span_of` (`active-set.c:74-75`) overflows `int` for a set larger than 2^30. That
is unreachable at the project's 10⁶ target.

### N8 — suspicion: the new map tests may time out under ThreadSanitizer

**Where:** `tests/CMakeLists.txt:1277-1278`. The new map tests are registered under TSan.
The sibling `active-set` map tests at 1261-1264 are deliberately excluded under TSan.
Six PEs, a 20,000-long reduction three times per set, and every-root broadcasts could
approach the 60 s launcher limit under TSan.

**Status:** not run here; the shared VM was left alone. Check the tsan preset's time.

## By the requested order of weight

1. **Wrong results or hangs.** None found.
   - Tree arithmetic: exhaustive to 1024.
   - Counting argument: this is the hardest case. A signal from a member's call N+1
     implies it finished call N. For tree slots, that means every up-phase wait of N had
     already been consumed, and no down signal of N+1 can exist until all members have
     entered N+1. For dissemination slots, the sender for each round is unique, and an
     early signal from call N+1 replacing a late one from call N still implies the same
     arrival. The model confirmed this with deliveries between calls deliberately
     reordered.
   - Value words:
     - `SLOT_TOTAL` is read by the parent before it releases the child, and the child
       resets it only after the release.
     - `OFFSET`/`GRAND` are written by `long_p` + `fence` + add, and read after the
       wait.
     - A member cannot reach call N+1's up phase before resetting them in N.
   - Ordering: put-signal carries `OSOE_TRANSPORT_FENCED` (`signal.c:9-14`), and the
     intranode rmw is seq_cst (`rmw.h`), so `shmem_sync` without a quiet still publishes
     prior stores.
   - Buffers and roots:
     - collect offsets are in rank order;
     - in-place reductions work: `memmove` is skipped and the parent reads the child's
       dest;
     - the broadcast root's dest is untouched and its source is reusable, since
       `putmem_signal` is locally blocking.
   - Alltoall without an entry barrier is spec-conformant.
2. **pSync restoration and slot bounds.** pSync is restored on return at each member.
   Slots stay at 70 or below, and rounds at 31 or below.
3. **Whole-job path and validation.** The whole-job condition (`active-set.c:130-132`) is
   byte-for-byte the old one, and that path does not touch pSync. Validation computes in
   int64, so strides do not wrap. The one exception is N1.
4. **Profiling names.**
   - Binding is correct: `p<name>` is a strong `T`, `pshmem_<name>` is a strong `T` alias
     of it, and `<name>` is a weak `W` alias.
   - No library code calls the old names, so interposing them captures only
     application calls.
   - CheckExports' allowlist covers the new `p` names; `pshmem_*` names match the
     pattern.
   - CheckProfiling is not vacuous (N4).
   - Sandia's `pstart_pes`/`p_my_pe`/`pshmalloc` spelling matches my recollection. I
     could not verify it: no SOS source is in `corpus/` or `build/scratch/sos`.
   - `test-profile.c` cannot see binding in a shared build, as the CMake comment says.
     CheckProfiling is what guards it.
5. **Tests.** See S3.
6. **Performance and naming.** See S1, N5 and N6.
