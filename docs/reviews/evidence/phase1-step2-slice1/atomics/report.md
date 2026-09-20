# Atomics sweep

## Atomics sweep (Question 2, first third): §3.16 against the tree

Agent `atomics`. Scope: every file in `shmem/src/`, `base/` and `tools/stat/` at `main` (`7ff9343`). Claims are labelled as follows:
- *documented*: from the design doc or code comments.
- *observed in source*: read in the code.
- *measured*: run in the `osoe` VM (aarch64, GCC 14.2). The VM is shared and busy, so every number is measured under contention.
- *recalled*: from memory, not verified.

Evidence files, in `docs/reviews/evidence/phase1-step2-slice1/atomics/`:

| File | What it is |
|---|---|
| `store-buffer-litmus.c`, `.out` | Dekker/store-buffer litmus, five variants, 1M rounds each |
| `shared-layout.c`, `shared-layout-aarch64.out`, `shared-layout-x86-64.out` | `offsetof`, `sizeof` and cache line of every header-declared shared struct. aarch64 by GCC in the VM; x86-64 by Apple clang `-arch x86_64` under Rosetta, which follows the same SysV layout rules for these types but is clang, not GCC |
| `drain-reserve-accounting.c`, `.out` | `osoe_connection_drain_reserve` bracketed by `release` and `open` |

### 1. Mechanical result of the sweep

All *observed in source*. Each grep was first checked against something known to be there: the `_explicit` pattern finds 162 sites.

- Non-`_explicit` atomic calls (`atomic_load(`, `atomic_store(`, `atomic_fetch_*(`, `atomic_exchange(`, `atomic_compare_exchange_*(`, `atomic_flag_*`, `atomic_init`): none.
- `memory_order_relaxed`, `_consume`, `_acq_rel`: none in `shmem/`, `base/`, `tools/`.
- `volatile`: none. `__atomic_*`, `__sync_*`, asm, pause or yield intrinsics: none.
- ThreadSanitizer suppressions, `no_sanitize`, `__has_feature`, annotations: none.
  - The only tsan accommodation is not registering the multi-node tests under the tsan preset (`shmem/tests/CMakeLists.txt:256-313`).
  - That is stated openly, and it is the evidence gap the what-changed note already owns. It is not a papered-over race.
- Operator forms (`++`, `=`, `+=`) on `_Atomic` objects: none found. Every `_Atomic` declaration (list in §2) is touched only through `atomic_*_explicit`.
- Every CAS and `fetch_*` names `seq_cst`. Every load names `acquire`, except the two `seq_cst` failure orders in `rmw.h:40` and `metrics.c:277`, which are stricter than the rule and harmless.

The letter of §3.16 is kept everywhere. The findings below concern its application: which rule a given word falls under, and the promises adjacent to it.

### 2. Inventory of shared words

P means the word crosses processes. T means it is shared only between threads of one process. "pub-by X" means a plain (non-atomic) field whose visibility is carried by the atomic X stored after it.

#### Control segment — `shmem/src/node.h`

| Word | Writer(s) | Reader(s) | Scope | Type | Orders at each site | Shape / verdict |
|---|---|---|---|---|---|---|
| `hdr.ready` | lead (`node.c:441`) | non-leads (`node.c:333`), osoe-stat | P | `_Atomic u32`, futex | store `release`; load `acquire`; `futex_wait(…,0)` on the word itself | word-is-predicate futex; sound; order inconsistent with wire/job/area (F4) |
| `hdr.joined`, `mapped`, `finalized` | every local PE (`node.c:452,469,504`) | every PE `wait_count` (`node.c:88`) | P | futex u32 | `fetch_add seq_cst`; load `acquire`; wait on loaded value | sound |
| `hdr.failed` | failing PE (`node.c:64`) | `check_failed` | P | u32 | store `release`; load `acquire` | polled; wake can be missed by design (*documented* `node.c:79`), bounded by sweep |
| `hdr.aborting/abort_pe/abort_status` | declaring PE (`node.c:487-489`) | `watchdog.c:67-68` at sweeps | P | u32/i32 | three `release` stores, flag last; `acquire` loads, flag first | single-writer publication, polled; sound. Two PEs declaring at once can interleave `abort_pe` and `status`, which is cosmetic |
| `hdr.{world,shared}_{arrived,released,copied}` (`base/epoch.h`) | any member / lead agent | members `wait_eq` | P | futex u32, own line | `fetch_add seq_cst`; load `acquire` before compare; wait on loaded value | sound (S2) |
| `hdr.arrival[team].epoch` | the one member whose bump hit the target (`progress-host.c:172`) | lead agent (`progress-host.c:198`) | P | u32, own line | store `release`, then `fetch_add seq_cst` on `agent_wake`; agent loads `agent_wake` first | predicate-then-bump; sound (S1) |
| `slot.phase` | owner | siblings, watchdog, osoe-stat | P | u32 | store `release`; load `acquire` | publishes the plain fields below |
| `slot.rank,pid,tid,symmetric_size,heap_bytes,heap_name` | owner, before `phase=JOINED` (`node.c:445-450`) | siblings after `joined==L`; watchdog; osoe-stat | P | plain | pub-by `phase`/`joined` | acceptable: written once before a locked add, never again |
| `slot.agent_wake` | any local PE (`progress-host.c:147,177`), stopper (`agent.c:116`) | that PE's agent (`agent.c:53,73`) | P | futex u32 | `fetch_add seq_cst`; load `acquire` before the predicate | sound |
| `slot.watch_rng` | owner (`watch.c:61,78`) | every notifier (`watch.c:89`) | P | u64 | store `seq_cst`; load `acquire` | two-sided with the ivar store; `seq_cst` as §3.16 requires |
| `slot.watch_seq` | notifiers (`watch.c:96`) | owner (`watch.c:66`, futex) | P | futex u32 | `fetch_add seq_cst`; load `acquire` before ivar | sound |
| `slot.blocked_gen` | owner (`metrics.c:312,324`) | osoe-stat (`segment.c:280,288`) | P | u64 | two `fetch_add seq_cst` | seqlock generation |
| `slot.blocked_detail`, `blocked_since_ns` | owner | osoe-stat | P | u64 | `release` inside the odd window | covered by generation |
| `slot.blocked_routine[32]` | owner, `memcpy`/`memset` | osoe-stat `memcpy` | P | plain chars | none | formally a data race; covered by the generation check; acceptable for a diagnostic (F7) |
| `slot.sweeps` | any thread of owner (`watchdog.c:62`) | osoe-stat | P | u64 | `fetch_add seq_cst` | sound |
| `slot.world_epoch`, `shared_epoch` | owner (`barrier.c:74`) | osoe-stat | P | u32 | `release` | single-writer |
| `slot.blocks_exhausted` | any thread of owner (`metrics.c:292`) | osoe-stat | P | u32 | `fetch_add seq_cst` | sound |
| `slot.heap_in_use/high_water/largest_free` | owner (`malloc.c:259-262`) | osoe-stat | P | u64 | load `acquire` + store `release` | single-writer under SERIALIZED; stale reads fine |
| `landing.wake` | lead agent (`progress.c:104,194`) | owner (`transport-am.c:340,345`) | P | futex u32 | `fetch_add seq_cst`; load `acquire` first | sound |
| `landing.ready` | owner arms 0 (`transport-am.c:358`), agent sets 1 (`progress.c:103`) | owner | P | u32 | both `seq_cst` | predicate; stricter than needed |
| `landing.length`, `value`, `token` | lead agent (`progress.c:189,190,258,259`) | owner after `ready` | P | u32/u64 | `release`; `acquire` | pub-by `ready` |
| `landing.data[]` | lead agent `memcpy` | owner `memcpy` after `ready` | P | plain bytes | pub-by `ready` | acceptable |
| `landing.bytes_issued/bytes_landed/counter_wake` | nobody yet | nobody | — | — | — | declared for nonblocking gets; unused |
| directory, `job/step/node/session/world_size/heap_base`, metric table | lead before `ready` | everyone after `ready`, osoe-stat | P | plain | pub-by `ready` | acceptable; osoe-stat may read before `ready` and see zeros, which its magic check rejects |
| counter blocks `c[i]` (`base/counters.h`) | owning thread only | osoe-stat (`segment.c:220`) | P | `_Atomic u64` | load `acquire` + store `release`; claim is CAS `seq_cst` on `c[THREAD_CLAIMED]` | single-writer; a moment stale, never torn (8-aligned, checked by the reader at `segment.c:217`). Sound |

#### Connection area — `connection.h`, `reply.h`, `connection-area.c`

| Word | Writer(s) | Reader(s) | Scope | Orders | Verdict |
|---|---|---|---|---|---|
| `buffer[k].head` | any local PE: CAS `seq_cst`/`acquire` (`connection.c:64`). Lead agent: `fetch_or seq_cst` seal (`:116`), `release` stores in open/release/drain (`:103,145,166`) | both | P | as listed | sound: the agent's plain stores happen only while the seal bit excludes every writer CAS. ABA checked: `head` encodes the whole reservable state (offset plus seal), so a CAS that succeeds against a recycled buffer reserves legitimately in the current generation and reads `seq` afterwards. Benign |
| `buffer[k].filled` | writers `fetch_add seq_cst` (`:85`); agent `release` stores (`:100,146`) and `fetch_add` (`:168`) | agent (`:123`) | P | | sound for open/seal/release; wrong for drain (known; F7 adds the test-vacuity detail) |
| `buffer[k].seq` | agent `release` before clearing the seal (`:102`) | writers after a successful CAS (`:77`), agent (`progress.c:440`) | P | | pub-by `head`; sound |
| `ack_requested` | any PE store `seq_cst` (`:178`) | agent `exchange seq_cst` (`:183`) | P | | sound: an exchange cannot drop a request |
| `acked` | lead agent `release` (`:188`) | any PE `acquire` (`connection.h:287`) | P | | single-writer, monotone, polled |
| `credit.available` | any PE: CAS loop (`reply.c:107-115`), `fetch_add` (`:120`) | any | P | `seq_cst` | counter CAS: ABA-immune, because the value is the state |
| area header `ready` | lead `seq_cst` (`connection-area.c:280`) | openers | P | | word-is-predicate futex |
| area header plain fields | lead before `ready` | openers | P | pub-by `ready` | acceptable |

#### Wire — `wire.c` (private structs)

| Word | Writer | Reader | Orders | Verdict |
|---|---|---|---|---|
| `direction.produced` | sending agent, `store seq_cst` (`:374`) | both | load `acquire` | SPSC; stronger than needed |
| `direction.consumed` | receiving agent, `store seq_cst` (`:401`) | both | load `acquire` | same. Nobody waits on `consumed` (a full ring is polled), so no two-sided shape exists here today. Step 3 must not add a futex wait for room without revisiting this |
| `direction.wake` | sender `fetch_add seq_cst` after `produced` (`:375`) | receiver (`:407,412`) | | predicate-then-bump; sound. Nothing in `shmem/src` calls `osoe_wire_wait`; the agent polls every 250 µs (*documented*, §3.10 note) |
| `header.ready` | creator `seq_cst` | opener | | sound |
| `header.leaving` | each side `fetch_add seq_cst` (`:300`) | each side (`:318,322`) | | sound: load, compare, wait on loaded value |
| `header.capacity`, `magic` | creator before `ready` | opener | plain, pub-by `ready` | acceptable |

#### Job segment — `job.c`

| Word | Verdict |
|---|---|
| `ready`, `failed` (`seq_cst` stores), `joined`, `left` (`fetch_add seq_cst`) | sound |
| `node_of[world_size]`: a plain `uint32_t` array, written concurrently by every lead for its own ranks, read by every lead for the overlap check (`job.c:207-213`), published by `joined` | the final read is sound (after `joined == node_count`). The duplicate-claim check is a plain-load and plain-store race between processes: F6 |

#### Process-private, thread-shared

| Word | Verdict |
|---|---|
| `agent.c` `stop_requested` (`release` store, then `fetch_add` on `agent_wake`), `agent_ready` (`seq_cst`, private futex) | sound; S1 |
| `init.c` `initialized`, `thread_level` | `release`/`acquire`, writers under the `lifecycle` mutex; fine |
| `base/futex.c` `timeouts` | `fetch_add seq_cst`; fine |
| `base/order.c` `serial` (`_Thread_local`) | the serializing store; see F5 |
| `transport-am.c` `next_token`, `default_context`; `watch.c` `the_watch` | plain, application-thread only under SERIALIZED; §4.11 owns the rest |
| `progress-host.c` `consumed[]`, all of `struct osoe_progress` | agent thread only while it runs; main thread only after `pthread_join` (`session_end` order, `progress-host.c:277`). Sound |

### 3. What is sound, and the argument that convinced me

**S1. Predicate-then-bump is not a two-sided (Dekker) protocol, and the tree treats it correctly.**
- The shape is `store(predicate, release)`, then `fetch_add(seq, seq_cst)`, then `futex_wake`. The waiter does `load(seq)`, then `load(predicate)`, then `futex_wait(seq, loaded)`.
- It appears in the agent stop (`agent.c:34-40`), the arrival handoff (`progress-host.c:172-178`), the landing slot (`progress.c:100-105`) and the wire (`wire.c:374-376`).
- It is sound under C11's own model, not only on TSO. The `fetch_add`'s release half forbids the earlier predicate store from being ordered after it.
  - A waiter whose `seq` load sees the bump therefore sees the predicate.
  - A waiter that does not see the bump holds a stale `seq`, which the kernel's compare rejects.
- Every site loads the sequence word before the predicate (*observed in source* at each site listed).
- I found no site with the loads reversed, and no futex wait whose expected value is loaded after the check.

**S2. Epoch waits** (`base/epoch.c:25-37`).
- The wait is load, compare, then wait on the loaded value, and the word is its own futex.
- The bump is a locked add, and exactly one bumper observes equality. That is what makes the arrival handoff single-writer.
- Sound at this level. The bounded-skew argument belongs to the protocol agent.

**S3. The two genuinely two-sided protocols use `seq_cst` on both stores.**
- The two-sided store is the watch insert (`watch.c:61`). Its counterpart is an AMO; in `rmw.h` every op is a locked RMW or a `seq_cst` store.
- *Measured* on aarch64 (`store-buffer-litmus.out`, 1M rounds each; the control was run four times):

  | Variant | Both-zero outcomes |
  |---|---|
  | `plain` (volatile `str`/`ldr`), the control | 68, 182, 91, 203 |
  | `seqcst` store + `acquire` load, which is what the tree does | 0 |
  | `release` store + `acquire` load | 0 |
  | `amo`: `seq_cst` store vs `fetch_add` | 0 |
  | `put`: `seq_cst` store vs plain store | 58 |

- The control proves the harness can see store buffering on this core, so the zeros mean something.
- GCC 14 emits `stlr` for both store orders and `ldar` for the `acquire` loads (objdump of the litmus binary). The design records the `stlr` half but not the `ldar` half, and the `put` row suggests the pair is what closes the window.
- The `put` row makes §4.10's "best effort" visible.
  - On aarch64 too, a put's `memcpy` against a waiter loses wakes at about 6e-5 per forced collision.
  - That is the accepted case. It is correctly routed to `transport.watch.late_wake` for 2-byte waits and bounded by the sweep (`wait.c:106-111`).
- That x86-64 behaves as the design says (`mov` versus `xchg`) is *documented* by the design's own disassembly. I could not run x86 GCC here.

**S4. No unaligned 64-bit atomic exists in any shared struct** (*measured*, `shared-layout-*.out`).
- Every `_Atomic uint64_t` sits at an offset ≡ 0 mod 8 on both targets.
- Sections of the control segment start on `align_up(OSOE_CACHELINE)` inside a page-aligned mapping (`node.c:216-236`).
- Landing slots stride by an aligned size.
- osoe-stat refuses an unaligned load (`segment.c:217`).
- The `_Alignas` claims match the layout. Each of these owns a cache line: each epoch counter, each handoff word, `head`, `filled`, `seq`, `ack_requested`, `acked`, `credit`, and `produced`, `consumed`, `wake`.
- The false-sharing case I looked for is avoided as `node.h:62-66` says. A notifier's `watch_seq` bump lands on slot line 1, not on line 0, which the owner's agent uses.

**S5. No shared struct carries a pointer, `size_t` or `long`** (*observed in source*).
- Checked: `node.h`, `connection.h`, `osoe_credit`, the `metrics.h` descriptor, and the private headers in `wire.c`, `job.c` and `connection-area.c`.
- All handles with pointers (`osoe_node`, `osoe_connection`, `osoe_wire`) are process-private.
- `OSOE_CACHELINE` differs by architecture (128 versus 64), so struct sizes differ between aarch64 and x86-64.
- Every shared object is node-local and osoe-stat reads offsets from the directory, so no reader depends on the compile-time layout.

**S6. The progress thread never spins on `filled`.** `progress.c:493-505` returns and revisits, as §3.5 demands. No application-side wait spins: each either futex-waits or calls `nanosleep`.

### 4. Findings

#### F1 — The AM backend's blocking calls never sweep: no abort check, no liveness check, no `sweeps` heartbeat
- **Location:**
  - `transport-am.c:175-193` (`reserve_waiting`)
  - `transport-am.c:305-321` (`take_credit`)
  - `transport-am.c:335-351` (`await_reply`)
  - `transport-am.c:601-610` (`osoe_transport_wait`)
  - contract at `transport.h:48-55`; hook at `base/sweep.h` and `watchdog.c:55`
- **Failure scenario:**
  - Node A's PE 3 is in `shmem_quiet`, a get, or a fetching AMO toward node B.
  - A sibling PE on node A dies, and another sibling's watchdog stores `aborting`.
  - Every PE blocked in `wait_until` or a barrier exits at its next 100 ms sweep.
  - PE 3 does not. Its loop never calls `osoe_sweep_expired`, so it never reads `aborting`, never runs `slot_dead`, and never bumps `pe.sweeps`.
  - It sits for `OSOE_BOOTSTRAP_TIMEOUT` (60 s default, `bootstrap.h:39`), then dies with "node N has not acknowledged", which names the wrong cause.
  - `await_reply` computes a sweep deadline and then discards `osoe_futex_wait`'s `ETIMEDOUT`.
- **Second consequence:**
  - `osoe-stat` reads "sweeps not rising" as stalled rather than merely blocked (`node.h:95`). A PE healthily waiting in quiet is therefore indistinguishable from a wedged one.
  - Quiet, get and AMO publish no "blocked in" record at all. `osoe_blocked_enter` is called only from `barrier.c` and `wait.c`; `barrier_all`'s internal quiet is covered, a bare `shmem_quiet` is not.
- **Evidence:** *observed in source*. `transport.h:53-55` still says "nothing exercises this yet"; the slice made that false.
- **Severity:** design change needed. The fix is small: call `osoe_sweep_expired` on the sweep schedule in all four loops, and publish the blocked record.
- **Confidence:** high. A test that kills a sibling while a PE is in an internode quiet would make this executable; I did not run one because of the tightened budget.

#### F2 — A job-lifetime wait is bounded by the bootstrap timeout, and expiry is fatal
- **Location:** the same four loops; `osoe.bootstrap.timeout_ns`.
- **Failure scenario:**
  - 256 PEs on a node each put 64 MB to one slow peer and call `shmem_quiet`.
  - A Pi on 1 GbE moves about 110 MB/s, so 256 × 64 MB takes about 150 s.
  - Once the peer needs more than 60 s to apply and acknowledge, every PE aborts a healthy job.
  - The spec gives `shmem_quiet` no timeout.
  - `OSOE_BOOTSTRAP_TIMEOUT` is documented (`init.c:77`) as "seconds to wait for the other local PEs", so nobody tuning a long transfer would find it.
- **Evidence:** *observed in source*; arithmetic above.
- **Severity:** design change needed.
- **Confidence:** high that the code does this. Medium that step 3 will not replace these loops wholesale: the comments say the sleep becomes a wait, not that the deadline goes.

#### F3 — Sleep-poll loops re-wake the lead every ~20 µs per waiting PE
- **Location:** `transport-am.c:183,311,603`. Each iteration calls `osoe_progress_host_wake_lead()`, which is one locked add on `slots[0].agent_wake` plus a `futex_wake` syscall, then `nanosleep(20 µs)`.
- **Failure scenario:**
  - At 256 PEs per node, all in the quiet of a `barrier_all`, the lead's `agent_wake` line takes up to 256 locked adds and 256 syscalls per period. The period is nominally 20 µs and longer in practice with timer slack.
  - The agent's `futex_wait` returns `EAGAIN` every time, so the agent never sleeps and `agent.wakeups` counts noise.
  - This is the one place §3.11's "no spinning" is bent. It is not a spin, but it is a poll where the designed answer is a futex word; `acked` would need a 32-bit companion, as §4.9 says of 64-bit counters.
- **Evidence:** *observed in source*; *documented* as a step-2 stand-in (`transport-am.c:168-173`). Unmeasured.
- **Severity:** minor now. It becomes a performance problem at the stated scale if it survives step 3.
- **Confidence:** medium. For the performance agent.

#### F4 — The same futex shape is classified two ways, and §3.16 does not say which is right
- **Location:**
  - `node.c:441` stores `hdr.ready` with `release`.
  - `wire.c:238`, `job.c:161`, `connection-area.c:280` and `agent.c:47` store the identical word-is-its-own-predicate futex with `seq_cst`.
  - `wire.c:233-235` says why: "this is the publishing half of a two-sided protocol (§3.16)".
  - Likewise `node.c:64` (`failed`, `release`) against `job.c:68` (`failed`, `seq_cst`).
- **Scenario:** I cannot construct a failure.
  - The waker's `futex_wake` takes the kernel's bucket lock, which is a locked operation after the store.
  - The waiter reads the word under that lock, so the kernel supplies the second half of the protocol.
  - By the wire's own reading of §3.16, `node.c` is a violation. By `node.c`'s reading, the wire's comment is wrong.
  - The next contributor will copy one of them.
- **Severity:** clarification needed. §3.16 should name this third shape ("a futex word that is its own predicate") and fix one order for it.
- **Confidence:** high on the inconsistency; high that neither loses a wake.

#### F5 — §3.16 mis-states ISO C, and the serializing store rests on a compiler courtesy
- **Location:** design §3.16, "Only the four ISO orders … `acq_rel` and `consume` are extensions"; `base/order.c:7-12`.
- **The six orders.** *Recalled*, not verified against the standard text here: C11 §7.17.3 defines all six enumerators (`relaxed, consume, acquire, release, acq_rel, seq_cst`), and none is a GCC extension. The rule to use four is fine; the reason given for it is not.
- **The serializing store.**
  - `osoe_serializing_store` is a `seq_cst` store to a `static _Thread_local` that nothing ever loads.
  - C11 gives a store to an unobserved object no obligation to drain anything. It works on x86-64 because GCC emits `xchg`.
  - On aarch64 it is an `stlr` to a thread-private line.
  - The litmus gives no reason to think that serializes anything: it tested `stlr` and `ldar` on the same pair of words, not against an unrelated one.
  - On aarch64, `fence`'s ordering of the caller's plain stores therefore rests on something else, namely the locked ops and syscalls that follow. §4.6 should say what.
- **Severity:** clarification needed.
- **Confidence:** medium. The six-orders point becomes high if my recollection of §7.17.3 is confirmed. The aarch64 point would be settled by a litmus with a thread-local `stlr` between a plain store and the peer's load; I did not run one.

#### F6 — The job map's duplicate-rank check is a cross-process plain-memory race
- **Location:** `job.c:205-214`.
- **Scenario:**
  - Two leads whose `OSOE_GTIDS` both name rank 7 run the loop at the same time.
  - Both load `node_of[7] == UNSET`, and both store.
  - No error is raised, the map keeps the later store, and the losing node still believes rank 7 is local.
  - The check exists for exactly this launcher bug and misses it whenever the leads are simultaneous, which at start-up they are.
- **Fix shape:** after `joined == node_count`, each lead re-reads its own ranks and requires `node_of[r] == b->node`; or claim each rank with a CAS.
- **Severity:** minor (misconfiguration detection only). **Confidence:** high.

#### F7 — Notes below the threshold of a finding
- **Bare spin at finalize.**
  - `flush_before_close` (`progress-host.c:284-290`) spins with no sleep, no futex and no relax hint for as long as a peer takes to read its wire, up to the bootstrap timeout.
  - It is the only true busy loop in `shmem/src`, and it runs once per job on the lead.
  - Under oversubscription it takes a core the peer's agent may need; §3.11 forbids exactly this. Minor.
- **Unverified barrier claim.**
  - `metrics.c:301-305` says two `seq_cst` RMWs are "full barriers on both targets".
  - On aarch64 that is an unverified claim about LSE or outline atomics against plain stores to other addresses (`blocked_routine`'s `memcpy`).
  - The worst case is a torn routine name accepted by osoe-stat, once, in a diagnostic. That is not worth a barrier, but the comment should be softened.
- **`drain_reserve` (known).** What I can add is that `test-connection.c` cannot see it.
  - The driver drains one reply and marks the buffer FREE; `osoe_connection_open` then resets `head` to 32 over it.
  - `drain-reserve-accounting.out` shows:
    - `head=96 filled=64 sendable=0`
    - then "first writer reservation lands at offset 32: ON TOP OF THE DRAINED REPLY"
  - The test's ONCE check is `ngot >= TOTAL` (`test-connection.c:295`) and never counts `REPLY_ID` messages.
  - The header comment's claim that the guard's path "runs constantly rather than never" is therefore true of the call and false of any assertion on its result.
- **AM-layer counters not exported.**
  - The progress engine's §3.14 counters (`segments_sent`, queue depths, pool high-water; `progress.h:109-114`) are private to the lead and absent from the metric table.
  - osoe-stat therefore cannot see the AM layer at all. For the diagnosability agent.

### 5. Not investigated

- x86-64 GCC codegen for `release` versus `seq_cst` stores. No x86 GCC was available, so the design's disassembly claim stands as *documented*.
- Cortex-A72 and A76 behaviour of `stlr` and `ldar`. The litmus ran on Apple cores under a hypervisor and should be re-run on a Pi before §3.16's aarch64 claim is relied on.
- Whether outline atomics (no LSE) change the `amo` row.
- These files were grepped for atomics and shared-struct field access, not read line by line:
  - `progress.c` beyond its atomic sites and `flush`/`drain`, including the parser
  - `am.c`
  - `allocator.c`
  - `malloc.c` internals
  - `symmetric.c`
  - `team.c`
- `tools/stat/stat.c` beyond its use of `segment.c`'s loaders.
- A kill-a-sibling-during-quiet test for F1, and a long-quiet test for F2.
- The reservation, credit, arrival and wire protocols as state machines. Those belong to the protocol agent, and nothing above contradicts their models.

### 6. Lessons for a successor API

- **Waitable objects.**
  - `shmem_wait_until` on an arbitrary symmetric word forces a two-sided protocol against every write path, including `memcpy` puts that cannot take part in one.
  - The spec then patches it (1.5 change #17: "update with an AMO"), and the implementation needs two counters to say which missed wakes are bugs.
  - A successor should make the waitable object a distinct type whose only writers are notifying operations.
- **Completion without a timeout.**
  - `shmem_quiet` has no timeout and no error return, so an implementation must choose between hanging and aborting on a slow peer (F2).
  - A completion call that can report "not yet" would let the library keep its liveness machinery without inventing a deadline the specification never gave it.
