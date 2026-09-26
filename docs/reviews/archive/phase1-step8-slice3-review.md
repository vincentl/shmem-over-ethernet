# Adversarial review: phase 1 step 8 slice 3 (lock-free issue on shared contexts)

Tree: `build/scratch/wt/slice3` at 3353ddb; diff `bc5fbb6...3353ddb`. Read-only review; no build or
mutation was run (the `mutslice3` worktree exists, clean at 3353ddb, so I left mutation runs to the
main session). Every claim below is from reading the code, marked *observed in source*, or from
reasoning, marked *by reasoning*. File:line refers to 3353ddb.

## Verdict

The get protocol (generation, two pairs, the pair in token bit 47, retag on a moved generation)
matches `get_completion_model.py` step by step. The dirty set (CAS raise, then the bit only when it is
clear; clear, re-check and re-set under the mutex) matches `dirty_set_model.py`. The piggyback on
`full` matches `pe_quiet_model.py`, with one exception: the models use unbounded integers, and the
code compares 32-bit words as a wrapping difference that `full` can fall more than 2^31 behind. That
failure is **absorbing**: once it starts, every wait on the context returns at once for the rest of
the run. This is the one finding I would hold the merge for. Second, the put re-check loop can starve
a quiet while another thread keeps putting (the design has the same loop). Third, the tests cannot
see four of the six mutations the brief says the slice must catch, because a read-back over the same
ordered connection hides an early quiet. §3.16 is clean. The node-segment block stays one cache line.

## 1. A quiet returning before a prior operation is complete

### B1 (blocker): the piggyback test wraps, and once it wraps it stays wrong

`shmem/src/transport-am.c:1599-1606`, `:1630`; `node.h` (`generation`, `full` are `uint32_t`); design
§4.11 item 2 (line 3770, "compared as a wrapping difference").

The test `(int32_t)(full - e) > 0` is only sound while `generation - full < 2^31`. Nothing keeps it
there. `full` moves only on a *full* quiet that does not piggyback. `generation` also moves on every
`pe_quiet` and on every gets-only wait (`osoe_transport_wait_gets`, i.e. every `iget`/`ibget` on a
shared context, `rma-typed.c:120,191`), and none of those store `full`.

Interleaving (a single thread is enough):
1. `full = F`. The program runs 2^31 + 1 `shmem_pe_quiet` or `shmem_iget` calls on the default
   context under MULTIPLE, with no `shmem_quiet`, `barrier_all` or `clear_lock` in between. Each call
   bumps the generation, so `generation = F + 2^31 + 1`.
2. Some thread puts to node N, then calls `shmem_quiet`. It loads `e = F + 2^31 + 1`. Then
   `full - e = 0x7fffffff` as a 32-bit value, which is positive, so the quiet piggybacks and returns
   while the put is still in flight.
3. The piggyback path neither bumps the generation nor stores `full`. So from then on **every** wait
   on the context, of every kind, piggybacks. No quiet on that context ever completes anything again.

Checked numerically: with `full = 0`, `e = 2^31` bumps and `e = 2^31 + 1` piggybacks.

Reachability: an `iget` between node-mates still takes this path (`osoe_transport_wait_gets` does not
check locality). Each one costs a mutex and a 256-record scan, around 1 µs, so 2^31 of them take
roughly 30 to 60 minutes. That is a long-running threaded job whose only synchronization is
`iget`/`pe_quiet` plus `wait_until`/AMOs. Rare, but the result is silent and permanent.

The models cannot see this: `pe_quiet_model.py` compares `f > e` on unbounded integers.

Fix, either of:
- (a) Make `generation` and `full` 64-bit. The pair is still `g & 1`, and only bit 47 travels in the
  token. The block would then be 72 bytes and the layout 25. The `reserved` word plus some repacking
  may keep it within one line.
- (b) Keep 32 bits, and have a non-full wait that finds `(uint32_t)(g + 1 - full) >= 2^30` upgrade
  itself to a full quiet: wait for every node's bits and store `full`. That costs one full quiet per
  2^30 non-full waits.

Either way, add a model case, or a unit test that starts `generation` near the boundary through a
debug knob, and correct the design's sentence at line 3770.

### Verified sound (no finding)

- **Lost gets across the pair switch.** Code order at `:1185-1197`: the issuer loads the generation,
  adds, reloads, and on a change subtracts, wakes and retries. The waiter stores `g + 1`, then loads
  `issued[g&1]` inside `complete_pair`. Every access is `seq_cst` or an RMW, so the Dekker holds. The
  token's pair is taken from the confirmed `g`, and `copy_out` credits `bytes_copied[pair]` (`:761`).
  This is the model's design case. The model's `order` variant covers the code loading `issued`
  before `copied`.
- **`copied > issued` fatal (`:1115`).** It cannot fire spuriously. While the mutex is held, only
  transient adds reach the closed pair, and those are subtracted. A confirmed add to the closed pair
  precedes the bump in S.
- **The scan missing a landed piece (`complete_pair`, `:1103-1134`).**
  - `seen` is loaded before the scan.
  - A landing stores `LANDED` `seq_cst`, then bumps `counter_wake` while the waiter is registered
    (`progress-host.c:156-159`), so a piece that lands behind the scan wakes the sleep.
  - ABA on a record is harmless: `copy_out` re-reads the token after its CAS claim, and a claimed
    piece of another context is an early completion, credited to that context's pair.
  - A piece copied by another thread (`make_room`) credits the pair and wakes the waiter.
- **Piggyback coverage.** Only a full quiet stores `full`. `pe_quiet` and gets-only waits piggyback
  on it but never store it, which is correct. Happens-before from the full quiet's copy-outs to the
  piggybacker runs through the fetch_add on `copied`, then the unlock, then the lock.
- **Serial and shared mixing.** `osoe_transport_threads` (`:211`, called at `init.c:278`) runs after
  `osoe_transport_init` and before teams, allocator, agent or any issue. Both dirty sets are allocated
  whatever the mode (`:150-157`), so the late flip cannot find a NULL. Shared mode never touches
  `pending`, `order` or `touched`, and serial mode never touches `shared_high`, `dirty_bits` or pair 1.
  A block reused across modes starts with both pairs equal, because destroy's wait closes the only
  pair that can hold gets.

## 2. Hangs and starvation

### S1 (should-fix): the put re-check loop waits for puts issued after the quiet began, without bound

`transport-am.c:1560-1570`; design §4.11 item 2 (line 3763, "set the bit again and repeat").

The loop is: `through = high`; ask; `wait_acknowledged(through)`; clear the bit; `acknowledged()`;
otherwise re-set the bit and **loop and wait again**.

Interleaving: thread P streams puts to node N and never quiets (a producer, U6-style). Thread Q calls
`shmem_quiet`.
1. Q loads `through = s` and waits one round trip for the acknowledgement of s.
2. Meanwhile P's puts raise `shared_high` to s+1, the open segment.
3. Q clears the bit, re-checks, finds `acked = s < s+1`, re-sets and waits for s+1. P is now in s+2.

Q returns only when a whole round trip passes with no put by any thread to N. The stall watch never
fires, because `acked` keeps advancing. Q holds the context mutex throughout, so every other thread's
quiet, pe_quiet and iget on the context queues behind it. The design claims "never waits for a get
begun after its bump" for gets; it makes no such claim for puts, and this loop is why.

Fix (by reasoning; run it as a model variant before adopting): **re-set and stop, do not wait
again**. Every put that returned before the call raised `high` before the call, so it is at or below
`through`, and the wait for `through` covers it. The re-check exists only so that a put which found
the bit set and skipped its `fetch_or` is not lost. Re-setting the bit hands that put to the next
wait. Add the variant (`reset_exit`, expected to pass) to `dirty_set_model.py`, then change the design
text and the code together.

### Nit: waiters queued on the mutex are invisible

A thread blocked in `pthread_mutex_lock` (`context_enter`, `:197`) publishes nothing and sweeps
nothing. Only the holder shows in `osoe-stat`. A shared quiet shows no "waiting for the context's
mutex" anywhere. Also suspected (not measured): glibc mutexes are not fair, so a thread quieting in a
tight loop can keep another waiter off the mutex for a long time. Piggyback bounds the work, not the
wait.

### Checked, no hang found

- **A holder waiting on something only a queued waiter could do.** Extents are claimed before the
  add (`make_room` at `:1176` precedes the add loop), so no issuer holds an uncounted zone claim while
  counted.
- **An issuer counted in the closed pair but not yet sent.** It waits only on `take_credit`, which is
  returned by the lead at landing, and on `reserve_waiting`, which is freed by the lead. The holder
  wakes the lead (`:1124`).
- **Transient excess in `issued`.** It is always subtracted, then `counter_wake` is woken (`:1194`).
- **An ack that was never asked for.** The wait is for `through`, and the ask is for a reload that is
  at least `through` (`ask_acknowledgement`, `:1336-1350`). The first build's hang is fixed as stated.
- `osoe_blocked_enter`'s `record_mutex` is held only briefly, so there is no lock-order problem
  between it and the context mutex.

## 3. §3.16

**No violation found.** Every new access names an order, and none is `relaxed`.
- The two Dekker pairs are `seq_cst` or RMW on both sides:
  - generation store against issued RMW, and issued load against generation load;
  - `mark`'s CAS then bit load, against the quiet's `fetch_and` then its `shared_high` load in
    `acknowledged()`.
- `generation` and `full` are written only under the mutex. `seq_cst` is stronger than `full` needs,
  which is acceptable.

Two nits:
- `__builtin_ctzll` (`:1367`, `:1554`) is a compiler intrinsic. §3.16 bans intrinsics for *atomics*
  only, and `watch.c:117` already uses it. Fine, but `stdc_trailing_zeros` (C23) or a helper would
  keep the "no intrinsics" wording literally true.
- `copy_destination[extent]` is a plain store read by another thread (`:1200`). Its happens-before
  runs through the lead's acquire of ISSUED and its `seq_cst` store of LANDED, possibly in another
  process. This predates the slice (slice 2) and is not new.

## 4. Growth with job size; per-context memory

- **Node-segment block:** 4 + 4 + 16 + 8 + 8 + 16 + 4 + 4 = 64 bytes, one cache line, independent of
  the job. No stop condition. Fix B1 option (a) grows it past one line unless repacked.
- **Nit, process memory:** `dirty_set_allocate` (`:150-157`) allocates both the serial set
  (13 B per peer) and the shared set (8 B per peer plus bits) for **every** context whatever its mode,
  about 1.6 times the virtual memory of slice 1. That contradicts the brief's "O(nodes) process memory
  per context is slice 1's, unchanged". The cause is the default context's late mode flip. Allocate
  by mode instead: pass the level into `osoe_transport_init`, or allocate the shared set in
  `osoe_transport_threads`.
- **Nit, per-quiet cost:**
  - `wait_dirty_shared` scans ceil(nodes/64) words per quiet. The serial path scans the communication
    degree. This is 64 loads at 4k nodes, so acceptable, but it is O(nodes), not O(degree), against
    the author's "logarithmic internode" axis.
  - The pe_quiet `wanted` calloc of `peer_count()` bools per call is O(nodes) memory and zeroing on
    every call. This predates the slice (serial path).
- **Nit, the scan:** `complete_pair` rescans all 256 records on every wake, and runs even when nothing
  was issued, costing 256 `seq_cst` loads per quiet and per `iget`. Loading `issued[pair]` and
  `copied[pair]` first, and scanning only when they differ, is sound: equality is the exit test anyway.

## 5. Tests against the brief's mutations

The new check (put own words to `right` on the default context, quiet or pe_quiet, then a blocking
get of them back from `right`) **cannot see an early quiet**. The get travels the same connection
behind the put, and the peer's agent applies in stream order, so the read-back returns the put's
value whether or not the quiet waited. The `locked_count` put and the lock release also go to the
same node in order.

| Brief's mutation | Caught by the tests? |
|---|---|
| Issuer never re-loads `gen` (`noretag`) | **Only by luck.** A bump must land in the issuer's few-nanosecond window between the generation load and the add, and the piece must still be in flight at the *next* quiet, since the scan copies every landed piece whatever its pair. The "nonblocking get on the default context" value check would then fail. Not reliable. |
| Quiet clears the bits without the re-check (`naive`) | **No.** The lost put is hidden by the same-connection read-back. |
| Bit set before `high_segment` is raised (`bitfirst`) | **No**, same reason. |
| Raise by load and store (`plainmax`) | **No**, same reason. |
| Copier credits the pair the token does not name | **Yes.** The quiet that closes pair 1 hangs until the ctest timeout, or `copied > issued` is fatal. |
| A `pe_quiet` storing `full` | **No.** On map 0,1 there is one peer node, so `pe_quiet(right)` is a full quiet. On 3-node maps the ordering argument above still hides it. |

The brief makes "a mutation above escapes" a stop condition, so four escapes should stop the merge
until the tests change. Suggested oracles:

- **A debug-build oracle for the put mutations.** At `wait_shared` entry, after loading `e`, snapshot
  `shared_high[]`. It is O(nodes), which is fine in debug. At return, piggybacked or not, fatal
  unless `acked(peer) >= snapshot[slot]` for every slot the wait covers. Every prior put raised `high`
  before the call, so this is exactly the spec's property. It turns `naive`, `bitfirst`, `plainmax`
  and `pe_full` into deterministic failures whenever they lose a put.
- **A third-party end-to-end check.** A puts to node N1 and quiets, then signals a PE on node N2,
  which gets the word from N1 over its own connection. A 3-node map is needed.
- **For `noretag`:** a debug knob that yields or sleeps between the issuer's generation load and its
  add, so that straddling quiets become routine.
- **Missing coverage:**
  - `iget` on a shared context: `osoe_transport_wait_gets` now takes the new `only_gets` path, and
    nothing tests it concurrently.
  - A *created* shareable context (`shmem_ctx_create(0)` under MULTIPLE): only the default one is
    exercised, including destroy's implicit shared wait.
- **The lock pin (`test-multiple.c:302-318`)** does what it says. It counts `context_enter` only, and
  it is skipped when `right` is a node-mate. That is fine, since the 0,1 map runs it.

## 6. The timing claim

The raw numbers support the arithmetic (checked against
`docs/design/evidence/pi-cluster/2026-09-26-late/threads-bench/{before,after}.txt`, added in 4dd698e):

| | Four threads against one thread, three runs | Mean |
|---|---|---|
| Before | 0.975, 0.860, 0.845 | 0.89 |
| After | 1.36, 1.53, 1.40 | 1.43 |

By the brief's test ("4 threads beat 1 by more than they did under the mutex"), the claim holds on
the Pis. Caveats the notes should state:

- **Evidence missing at the reviewed commit (should-fix, already fixed after it).** At 3353ddb the
  design notes cite `docs/pi-cluster-results.md`, which then held no threads numbers. They arrived in
  4dd698e, after the reviewed commit.
- **Two threads do not scale** (should-fix, wording). After the slice, two threads ran at 1.05, 0.945
  and 0.954 of one thread. §4.11 reports only the four-thread point and "one thread is unchanged". Say
  that two threads are flat.
- **Attribution (should-fix, wording).** The bench quiets every 32 pairs. The gain combines lock-free
  issue with quiets now collapsing by piggyback, where under slice 2 each quiet stalled all issue for
  a round trip and nothing collapsed. The bench cannot separate the two, yet the notes attribute the
  gain to "issue". Piggybacked waits are counted under `transport.wait_nothing_outstanding`, so their
  share is not visible either. Suggestion: a piggyback counter, and a Pi run with issue only.
- **VM issue-only figure unverified (nit).** "Issue alone, without quiets, rose 10-25%" comes from a
  bench variant that is not in the tree, since `QUIET_EVERY` is fixed at 32. The WIP commit (02018ae)
  records 1.72 to 1.89 M, which is 10%. The 25% has no record I could find.
- **Oversubscription applies to the Pi too (nit).** The Pi run is `-c4` with four application threads
  plus the PE's agent thread, which is also oversubscribed, only less so than the VM. The explanation
  "the VM was oversubscribed" is plausible but was not tested, for example by running the VM at two
  threads with more cores or by pinning.

## Smaller items (nits)

- **Brief step 1, "`osoe-stat` reads both pairs", was not done.** `tools/` never reads
  `struct osoe_context_block`, before or after the slice. Either do it or drop it from the brief's
  record. The same applies to "`bytes_landed` stays ... for `osoe-stat`".
- **Stale comments:**
  - `node.h:760-770` still describes a single `bytes_issued` and `OSOE_LANDING_CONTEXT_SHIFT`, which
    does not exist (it is `OSOE_LANDING_OWNER_SHIFT`).
  - `transport.h:516,533` still say the wait is `bytes_landed == bytes_issued`.
- **Wrong counter on piggyback.** A piggybacked wait counts
  `OSOE_C_TRANSPORT_WAIT_NOTHING_OUTSTANDING` (`:1604`), which conflates "nothing to do" with
  "someone else did it". A separate counter would also serve the timing attribution above.
- **Naming.** The brief says `high_segment` "becomes atomic". The code instead adds a parallel
  `shared_high`. `shared_highest_segment` would say what it is under CLAUDE.md's naming rule;
  `shared_high` leaves out the noun.
- **Tokens.** Token sequences are now 31 bits. Only duplicate detection depends on them, together
  with the exact record token, so this is harmless.
