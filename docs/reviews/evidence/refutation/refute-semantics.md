# Refutation: SHMEM semantics findings D1–D6 (with O2, O3, O5)

Target: `docs/design/shmem-ethernet-design.md` (the design, cited as "doc Lnnn"). Spec: `scratchpad/spec16.txt` (cited as "spec Lnnn" plus §).
Evidence labels: **documented** (doc/spec text quoted), **observed** (run by me here), **recalled** (memory, not verified).

What I ran:
- Reran `exp/semantics/watch_model.py` pieces, plus new variants R-V8a/b/c, R-V9a/b and R-V5x (a python harness fed on stdin, not saved).
- Codegen in the Lima VM (GCC 14.2.0) for `memory_order_*` load, store, fetch_add and CAS on three targets:
  - aarch64 `-march=armv8.2-a` (LSE);
  - aarch64 `-march=armv8-a -mno-outline-atomics`;
  - x86-64 (`x86_64-linux-gnu-gcc`, disassembled with `x86_64-linux-gnu-objdump`).

## Verdict table

| ID | Finding | Verdict |
|---|---|---|
| D1 / O5 | Per-PE heaps are never laid out | **SURVIVES** (design change). The doc's own §4.4 text shows it assumes one heap shared by all local PEs |
| D2 | Watch key must be (PE, region, offset), not address | **SURVIVES** (design change, small; falls out of the D1 fix) |
| D3 | Slot lifecycle loses wakes | **SURVIVES-DOWNGRADED** → clarification. The probe-hole loss is real even with one waiter per key; the refcount half is moot under THREAD_SINGLE; a per-PE-indexed slot removes both |
| D4 | Dekker pattern; §4.9 names only half the memory-order constraints | **SURVIVES-DOWNGRADED** → clarification/minor. Real on x86 only for plain stores to an ivar (`signal_set`, `atomic_set`); aarch64 acquire/release codegen equals seq_cst |
| D5 / O3 | fence/quiet must cover memory stores; §4.8 says not | **SURVIVES** on text. No narrow definition of "memory store" exists. Practical severity is small (cheap fix), except the §8.1 force-remote ordering case |
| D6 / O2 | Completion counters and thread level | **SURVIVES-DOWNGRADED** → clarification. THREAD_SINGLE is legal (§9.2.1 Notes), so items 2/3/4 become latent. Item 1 (64-bit wait word) and item 6 (state the level) remain |

---

## D1 / O5 — symmetric heaps are per PE; §4.1 lays out one segment

**Claim.** The spec gives each PE its own symmetric heap. The design describes a single shared segment and never lays out L heaps, their per-process VAs, or node memory of L × `SHMEM_SYMMETRIC_SIZE`.

**Refutation attempt (b): does "(region id, offset)" already imply per-PE regions?**
- doc L648–649 (§4.2): "`(region id, offset)`, translated at both ends. This survives ASLR, PIE, heterogeneous nodes, and future region types." "Region" here reads as a *kind* of region (heap, statics, future types), not a per-PE instance. The PE is already carried in the header, so the wire format itself is fine.
- doc L622 (§4.1): "Shared `memfd`/hugetlbfs segment, mapped by every local PE." Singular.
- doc L641: "Reserve a fixed virtual range". doc L696 (§4.4): "Fixed pool at init, sized by `SHMEM_SYMMETRIC_SIZE`." Singular.
- **doc L712–713 (§4.4) is decisive against refutation:** "Inline headers mean every local PE writes identical bytes to the **same shared pages** — harmless but confusing". That is true only if all local PEs share *one* heap.
  - With per-PE heaps, each PE's allocator headers would sit in its own heap's pages.
  - With a single shared heap, a put from PE c to PE a at offset o would also change PE b's object at o. That is non-conforming.
- doc L729 (§4.5): "a single symmetric heap implicitly".
- doc L944–945 (§4.9): "another local PE storing through the shared heap".
- Invariant 7 (L1621) and §8.1 say nothing either way.

**Spec (documented).**
- `SHMEM_SYMMETRIC_SIZE` is the heap size "per PE" (spec §8, ~L1186).
- §9.2 item 2 (spec ~L1886): "Each PE has a single symmetric data segment and symmetric heap".
- Spec §3 (Figure 1) shows a heap per PE.

**Verdict: SURVIVES (design change needed).** This is not only unwritten detail: the doc's own §4.4 argument assumes one shared heap.

**Tightest statement.** The node segment must hold L disjoint per-PE heaps. Consequences:
- Node memory and hugetlb reservation of L × S.
- Each process maps its *own* heap at the agreed VA. Sibling heaps sit at per-process VAs, so intranode put and `shmem_ptr` compute `base[target_pe] + offset` (O(L) table, allowed by the §9.3 note).
- The progress thread writes at its host's VAs.
- VA budget per process is L × S.

**Fix direction.**
- §4.1: "segment = L heaps + node structures". Give the base-table rule and the sizing formula.
- Rewrite the §4.4 "same shared pages" sentence. Private metadata is still correct, for a different reason.
- §4.2: define region id as (region kind) scoped to the header's target PE.

## D2 — watch-table key must be (PE, region, offset)

**Claim.** The table is keyed by raw address. A writer in another process sees the same symmetric object at a different VA, so it misses the waiter's slot.

**Refutation attempt.**
- doc L987–988: "is anyone waiting on this address". doc L963/L974: `watch_insert(node->watch, ivar)` and `watch_find(node->watch, addr)`. Raw pointers in two different processes.
- The only way the address key is correct is the design's (wrong) single-shared-heap-at-identical-VA picture from D1. Even then it would be the wrong semantics: two PEs' ivars would alias.
- With per-PE heaps it is a lost wake. Model V6 reproduces this (the writer's key differs; the waiter sleeps). I reran the model harness and got the same result.
- **The safety sweep keeps it from hanging.** doc L1004–1008: "Pass a safety-sweep deadline … Count timeout-wakes". Every cross-process wake would degrade to the sweep latency (about 100 ms class), so it is a correctness-of-design bug, not a hang.

**Verdict: SURVIVES (design change, small).**

**Fix.** Key = (target PE, region kind, offset). Waiter and writers both derive it through the translation layer. Once the PE is in the key, a slot array indexed by local PE is the natural structure (see D3).

## D3 — slot lifecycle (refcount, open-addressing deletion) loses wakes

**Claim.** Unspecified removal and probing lose wakes (V8, V9).

**Refutation attempts.**
1. **Is the refcount race (V8) reachable?** A waiter always waits on an ivar "at the calling PE" (§9.11.1). With the D2 key, two waiters on one key must be two threads of the same PE.
   - Under SINGLE, FUNNELED or SERIALIZED, at most one thread of a PE is inside `shmem_wait_until`. Phase 1 has only `wait_until` and `test`, not the vector variants.
   - So **V8's same-key concurrency is unreachable if phase 1 provides ≤ SERIALIZED**. See D6 for why that is legal.
2. **Is the probe hole (V9) only a duplicate-key artifact?** No. The original V9 includes a second A-waiter, so I removed it (observed):
   ```
   R-V9a hole, ONE A waiter (no duplicate key), writer stops at EMPTY        SC -> LOST WAKE
   R-V9b same, writer scans past EMPTY                                       SC -> no lost wake
   R-V8a waiter(A) + waiter(Y, pred already true), naive remove, first-match SC -> LOST WAKE
         trace: Y inserts slot0; A reads key0==Y; Y clears key0; A probes to slot1;
                writer probes slot0 == EMPTY, stops; A sleeps on seq[1]
   R-V8b same, writer full scan                                              SC -> no lost wake
   R-V8c fixed remove + full scan                                            SC -> no lost wake
   ```
   The deletion-to-EMPTY hole loses a wake with **distinct keys, one waiter each**. So it survives any thread level whenever the table is open-addressed with delete-to-EMPTY and early-terminating probes.
3. **Does the doc handle it?** doc L990–991 only says "fixed open-addressed table of cache-line-padded slots, sized from local PE count". No deletion or probe rule. The safety sweep (L1004) turns a loss into latency, not a hang.

**Verdict: SURVIVES-DOWNGRADED → clarification needed.** The defect is real but has trivial, well-known fixes, and it becomes nearly vacuous once D2 is fixed.

**Tightest statement.** With open addressing, removing a slot to EMPTY while another key sits later in the same probe chain makes a writer that stops at EMPTY miss that key. This is a lost wake, bounded by the sweep.

**Fix direction (cheapest).** With key = (PE, …) and thread level ≤ SERIALIZED, use **one slot per local PE, indexed by local rank**:
- The waiter stores the offset (a serializing store; see D4), then loads seq, then loads ivar.
- The writer, which knows the target PE, loads `slot[pe].key` and compares.
- No probing, no refcount, no deletion hazard.
- MULTIPLE or vector waits later need per-thread slots or tombstones plus a full-chain scan (V9fix/V8fix).

## D4 — Dekker pattern; §4.9 names only half the memory-order constraints

**Claim.** The waiter does (store count, load ivar) and the writer does (store ivar, load count). Both stores must be visible to the other side's load. §4.9 names only waker stores and waiter loads.

**Doc text.** doc L1001–1002: "release on the waker's stores, acquire on the waiter's loads. On aarch64 these are `stlr`/`ldar`."
- Not covered: the **waiter's store** into the table (count or key, in `watch_insert`) and the **waker's load** of it (`watch_nonempty`/`watch_find`).
- doc L316–317 (§3.5) says omissions are visible "never on x86". That is wrong for this pattern: TSO store buffers break Dekker (model V4/V4b/V4c).

**Correcting the semantics reviewer (observed, GCC 14.2).** The claim "GCC ignores memory_order on plain loads/stores" is wrong as stated; relaxed differs, as the lead reviewer showed. My rerun:

| op | aarch64 LSE | aarch64 no-LSE, inline | x86-64 |
|---|---|---|---|
| load acquire | `ldar` | `ldar` | `mov` |
| load seq_cst | `ldar` | `ldar` | `mov` |
| store release | `stlr` | `stlr` | **`mov` (not serializing)** |
| store seq_cst | `stlr` | `stlr` | **`xchg` (serializing)** |
| fetch_add release | `ldaddl` | `ldxr`/`stlxr` | `lock xadd` |
| fetch_add seq_cst | `ldaddal` | `ldaxr`/`stlxr` | `lock xadd` |
| CAS acq_rel/acquire | `casal` | `ldaxr`/`stlxr` | `lock cmpxchg` |

**What remains on x86 (TSO).** Model: V4b and V4c lose a wake unless *both* Dekker stores are serializing. I reran R-V5x (lock-prefixed count++, plain ivar store): LOST WAKE.
- **Writer's ivar store.** Phase-1 `shmem_uint64_atomic_fetch_add` and `compare_swap` compile to `lock xadd` or `lock cmpxchg` whatever order is given. They are serializing, so fine.
- **But phase 1 also has `shmem_signal_set`** (doc L1399), and Example 27 waits on a signal word. A set is a *store*. Written as `atomic_store_explicit(…, memory_order_release)`, which is what §4.9 literally prescribes for "waker's stores", it compiles to plain `mov`. That is V4b, a lost wake. The same applies to a future `shmem_*_atomic_set` and to the non-atomic `put` data path if anyone wakes on it (ivars must be AMO-updated, G.2 #17).
- **Waiter's table store.** If written as `atomic_fetch_add` or CAS, it is `lock`-prefixed and fine. A plain `atomic_store(release)` of the key/offset (for example in the per-PE slot fix from D3) is `mov` and loses wakes.
- **Waker's seq bump** is `fetch_add` → `lock xadd`, fine. `futex_wake` is a syscall, which serializes.

**aarch64.** Acquire load and release store produce exactly the seq_cst instructions (`ldar`/`stlr`). `fetch_add(release)` differs from seq_cst only in the *load* half (`ldaddl` vs `ldaddal`; `ldxr` vs `ldaxr`). The store half is `stlxr` in both.
- So, *under the weak-ordering annotation model the design relies on*, acquire/release is sufficient on aarch64 **provided all four Dekker accesses carry them**. That means acquire on the waker's table load and release on the waiter's table store as well.
- Whether any shipping Arm core has behaviour weaker than TSO that these hints actually constrain was not verified (recalled only).

**Verdict: SURVIVES-DOWNGRADED → clarification / minor.** On aarch64 this is a missing sentence. On x86 it is a concrete but narrow bug: the `signal_set` path, and any plain-store table write, if implemented as the doc literally says.

**Tightest statement.** The §4.9 wake decision is a Dekker pair. Correctness needs:
- (a) the waiter's table store and the writer's ivar store both to be serializing on x86 (lock-prefixed, `xchg`, or a syscall);
- (b) release on both stores and acquire on both loads on aarch64.

"Release on waker stores" alone permits `mov` on x86 for store-shaped AMOs (`signal_set`).

**Fix.** State the four-access rule in §4.9 and §9. Use `memory_order_seq_cst` (or CAS/`fetch_add`) for the ivar store of set-shaped AMOs and for the waiter's table write. Correct the "never on x86" remark at L316–317.

## D5 / O3 — fence/quiet must cover "memory store"; §4.8 says they do not

**Claim.** Table 14 and `SHMEM_CTX_NOSTORE` require fence and quiet on the default context to order and complete memory stores. doc L924–927 says "`fence` and `quiet` do not order them. Completion routines do not apply."

**Search for a narrow definition of "memory store" (documented).** Every hit in the spec:
- spec L567–570 (overview §2 item 9): fence orders "Put, AMO, and **memory store** operations to symmetric data objects"; quiet ensures completion of "remote access operations and **stores** to symmetric data objects".
- spec L1658/L1728 (§9.1.9/§9.1.10, `shmem_ptr`): "ordinary loads and stores to dest may be performed". This is the only place the spec defines stores into another PE's object, and it calls them *ordinary* stores.
- spec L2966–2972 (`SHMEM_CTX_NOSTORE`): "not required to enforce completion and ordering of memory store operations **performed by the application** … the application must perform a synchronization operation on a context without the SHMEM_CTX_NOSTORE option". The same page (L2958) says "No options are enabled on the default context".
- spec L6190–6192 (§9.10.1): barrier_all "ensures completion of all previously issued memory stores and remote memory updates".
- spec L6371, L6501 (`shmem_sync`): "ensures completion and visibility of previously issued memory stores".
- spec L8885–8892 (Table 14): Memory Store has X in both columns. Footnote 9 limits this to "contexts created with certain options" and points to §9.5.1.
- spec L8956–8959 and L9028–9031 (fence/quiet Notes): "can enforce memory store ordering only for the calling thread … the update must be made visible to the calling thread according to the rules of the memory model".
- spec L9011–9012 (quiet): "All operations on symmetric data objects are guaranteed to be complete and visible to all PEs when shmem_quiet returns."
- Annex G.6 #2 (L10686): "completion semantics of memory load and store operations in context of shmem_barrier_all".

None restricts "memory store" to stores made through OpenSHMEM routines. "Performed by the application", "ordinary loads and stores", and the memory-model language all mean plain CPU stores. Stores through `shmem_ptr` are the only plain stores that can reach another PE's symmetric object. **No refuting text found.**

**Refutation attempt (d): is the failure reachable in normal mode?**
- Intranode put is a synchronous `memcpy` (doc L623), so put-then-store ordering holds without effort.
- For `barrier_all`, the calling thread's arrival is a `fetch_add` on the §5.3 counter (doc L1068–1070) → `lock xadd` (observed). That serializes and so drains that thread's TSO store buffer before any peer can be released. **In normal mode `barrier_all` is conforming by construction on x86**, and on aarch64 by the release annotation.
- `quiet`/`fence` with an empty dirty set do nothing, so nothing guarantees store visibility at return. Observing the violation needs another PE to learn "quiet returned" without a later serializing operation by the caller, which is a narrow window. Low practical impact.
- **§8.1 force-remote** (doc L1455–1457) is concrete:
  1. `putmem` to same-node PE 1 is routed over TCP and queued;
  2. `fence()` seals only (doc L812–815);
  3. store via `shmem_ptr(x, 1)`, which §8.1 lets return non-NULL within a logical node (L1450–1452);
  4. the put lands later and overwrites the newer store.

  This violates fence's "delivered before any subsequent operations" (spec L8932–8934), with memory store listed in Table 14. The test harness would report false mismatches, or mask real ones.

**Verdict: SURVIVES** on text (design change, small). The doc's statement and its §3.2-item-4 citation are wrong: item 4 (spec L698) concerns *concurrent* AMO and load/store access, not sequenced stores followed by fence/quiet. Practical severity is low in normal mode and a real bug in force-remote mode.

**Tightest statement.**
- On the default context, quiet, fence, pe_quiet and barrier must complete and order the *calling thread's* plain stores into symmetric objects, including via `shmem_ptr`.
- §4.8 must not say otherwise.
- Under force-remote, `shmem_ptr` stores can precede earlier-issued puts.

**Fix.**
- quiet, fence and pe_quiet end with one unconditional serializing operation on the calling thread (a locked op on a private word).
- `shmem_ptr` returns NULL whenever force-remote is on (or fence waits for acks on pointer-reachable peers).
- Rewrite doc L924–927 and L935–941: stores via `shmem_ptr` *are* covered for the calling thread. Only other threads' stores are the application's responsibility (spec L9028–9031).

## D6 / O2 — completion counters, thread level

**Claim.**
1. The 64-bit counters cannot be futexed.
2. `landed == issued` can starve.
3. `ndirty = 0` races under THREAD_MULTIPLE.
4. Blocking get and fetching AMO wait on per-PE counters.
5. Per-PE vs per-context state is ambiguous.
6. The thread level is never chosen.

**Spec: must an implementation support a given level? (documented)**
- §9.2.1 (spec L1926–1931): `provided` "returns the support level provided by the library". The allowed values are all four levels. No text requires `provided ≥ requested` or any minimum.
- §9.2.1 Notes (L1945–1947): "If the OpenSHMEM library is initialized by shmem_init, the library implementation can choose to support any one of the defined thread levels."
- §9.2.2 (L1972–1973) repeats this.
- So `shmem_init` does **not** imply SINGLE, but phase 1 **may legally provide SHMEM_THREAD_SINGLE** (or FUNNELED/SERIALIZED) from both routines.
- Grep of the doc for `THREAD_`: no hits. `shmem_init_thread` and `shmem_query_thread` are absent from §7 (L1385–1400). So the level is indeed unchosen (O2's core point stands).

**Item by item.**
1. **64-bit wait.** doc L807–808 `wait_until(...)` is pseudocode, and the doc already states the rule. Invariant 18 (L1646–1648) says futex words are 32-bit. Invariant 10 (L1627–1629) prescribes "64-bit `>=` predicates with a separate wake word". The pattern is known; it is just not applied at L808. `bytes_landed` is written by the progress thread and read by the PE whatever the thread level, so a 32-bit wake word with §4.9 ordering is needed. **Clarification.**
2. **Starvation.** Needs another thread of the same PE issuing concurrently. Impossible at ≤ SERIALIZED. **Latent.** Snapshot-and-`>=` is the right form for later.
3. **`ndirty = 0` race.** `dirty`, `ndirty` and `high_seg` are private to the PE and written only by PE threads. At ≤ SERIALIZED there is no concurrent writer. **Latent.**
   - Unlike these fields, the §3.5 reservation protocol (L281–317) is multi-writer *across PEs* at any thread level. It is already designed with atomics, so the thread-level downgrade does not reach it and does not reach D2–D4 (the waker/waiter are in different processes).
4. **Blocking get over-waits** on unrelated `_nbi` gets. At SINGLE this is bounded (no concurrent issuer), so it is conforming latency, not starvation. A per-op landing token is needed anyway for B1/O1 (private `dest`), which is out of my scope.
5. **Per-PE vs per-context.** Only `SHMEM_CTX_DEFAULT` exists in phase 1 (§7 L1402–1406), so over-quieting is conforming. Wording only.
6. **Thread level.** Survives as a required decision. Also §6.4 (L1367) says the refcount "must be updated atomically" and "per process, not per thread", which only matters above SINGLE.

**Verdict: SURVIVES-DOWNGRADED → clarification needed** (from design change).

**Tightest statement.**
- Phase 1 must state its provided thread level and add `shmem_init_thread`/`shmem_query_thread`. Choosing SINGLE (or SERIALIZED) is conforming and makes items 2–4 latent.
- Whatever the level, the PE's wait on `bytes_landed` (written cross-process by the progress thread) needs a 32-bit wake word under the §4.9 ordering. Invariant 10's recipe applies.
- Before any MULTIPLE work: snapshot-and-`>=` for quiet, per-thread or CAS-cleared dirty state, per-op completion for blocking get.

---

## Notes for the coordinator
- D1's strongest evidence is new: doc L712–713 ("same shared pages") shows the design assumes one shared heap. That is conformance-breaking, not merely unwritten.
- D3 and D2 have a joint fix: a per-local-PE slot keyed by offset. It needs the waiter's slot write to be serializing (D4).
- D4's live x86 case is `shmem_signal_set` (phase 1) written with `memory_order_release`.
- Scratch: I briefly created `out/tmpq2/refsem_mo.c` and deleted it. VM `/tmp/rs/` holds the codegen test.
