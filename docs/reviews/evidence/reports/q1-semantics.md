# Q1 — SHMEM semantics review of `docs/design/shmem-ethernet-design.md`

Reviewer: SHMEM semantics. Scope: design §4 and §7 against OpenSHMEM 1.6. I read the spec text end to end for every phase-1 routine, plus §3, §4, §7, §8, §9.2–§9.5, §9.8, §9.11, §9.12 and Annex B/G (`scratchpad/spec16.txt`).
Design references "§N.M"; spec references "§9.x.y" (or "spec §3" etc.).

Artifacts (all outside the repo):

| Path | What |
|---|---|
| `scratchpad/exp/semantics/watch_model.py` | Exhaustive interleaving model of the §4.9 waiter/waker protocol (SC and x86-TSO memory models, futex semantics, slot lifecycle) |
| `scratchpad/exp/semantics/watch_model.out` | Its output (reproduced below) |
| `scratchpad/exp/semantics/mo.c` | GCC 14.2 aarch64 codegen check for C11 `memory_order_*` arguments |
| `scratchpad/exp/semantics/cma.c` | Sibling-process `process_vm_writev` test under Yama `ptrace_scope` 0 and 1 |

Evidence labels: *documented* (spec § quoted), *observed* (experiment or source in this repo or VM), *recalled* (from memory, not verified here).

---

## Summary by severity

| # | Severity | Finding | Confidence |
|---|---|---|---|
| B1 | **blocking** | Get replies and fetching-AMO results cannot be written into the origin's *private* `dest` by a progress thread in another process. The design never says which process hosts the progress thread | high |
| D1 | design change | Symmetric heaps are **per PE** (spec §3, §8, §9.2). §4.1 describes one shared segment and never lays out L heaps, their VAs, or node memory (L × `SHMEM_SYMMETRIC_SIZE`) | high |
| D2 | design change | Watch-table key cannot be a raw address. The same symmetric object has a different VA in the writer's process → lost wake (model V6) | high |
| D3 | design change | Slot lifecycle (per-slot counter, removal, probing) is unspecified. The obvious implementations lose wakes (model V8, V9). Correct variants exist (V8fix, V9fix) | high |
| D4 | design change | The protocol is a Dekker pattern. It is correct under sequential consistency and x86-TSO only if *both* the waiter's insert-count increment and the writer's ivar store are serializing (V4/V4b/V4c vs V5). §4.9's memory-order rule names only half of the four accesses | high (model) / medium (ARM hardware semantics) |
| D5 | design change | Table 14 and the `SHMEM_CTX_NOSTORE` text require `quiet`/`fence` (and so `barrier_all`) to cover **memory stores**. §4.8 says the opposite and misapplies spec §3.2 item 4 | high on text, medium on the intended meaning of "memory store" |
| D6 | design change | Completion data structures: 64-bit counters cannot be futexed, `landed == issued` can starve, `ndirty = 0` races with concurrent issue, blocking get/fetch AMO have no per-op completion, and the thread level provided is never stated | high |
| C1 | clarification | Rejecting statics: detection is easy, but the conformance cost is understated. Almost every spec example uses `static`, and no wrapper is scheduled in any phase | high |
| C2 | clarification | Allocator edge semantics: barrier placement, size 0, failure, `free(NULL)`, calloc overflow, cross-PE agreement on `SHMEM_SYMMETRIC_SIZE`. The debug checksum cannot ride a dataless barrier. Phase for calloc/realloc/align is unstated | high |
| C3 | clarification | `shmem_pe_quiet` signature in §4.6 is wrong (`size_t npes`; context variant is `shmem_ctx_pe_quiet`). PE numbering is team-relative | high |
| C4 | clarification | The phase-1 context/team stubs have exact required behaviours. `SHMEM_TEAM_SHARED` must exist, and `shmemx.h` must exist (none in the repo) | high |
| C5 | minor | "Returning NULL unconditionally is conforming" for `shmem_ptr` conflicts with the `SHMEM_TEAM_SHARED` definition | medium |
| C6 | minor | "Stronger than spec" atomicity is overclaimed for multi-byte puts and mixed-width AMOs. The phase-1 AMO set (uint64 only) cannot exercise the 32-bit wait path the seq word exists for | high |
| C7 | minor | §4.3 says Yama `ptrace_scope` "defaults to 1". Observed 0 on the Debian 13 VM, so this is distribution-dependent | high (observed) |
| C8 | minor | The 32-bit `seq` can wrap (ABA). Not reachable in practice | high |

---

## B1 — Get replies land in private memory the progress thread cannot reach (blocking)

**Location.** §4.6 "`bytes_landed` — progress thread adds as chunks land"; §4.7 "Write replies **directly into the destination**"; §3.12 "a small fixed pool per node" (host process never named); §3.8 "AMO results" descriptors.

**Spec.** §9.6.1.5 and §9.6.2.2: `dest` is the "Local address of the data object to be updated". It can be any private memory: stack, `malloc`, a static. Spec §3: "Private data objects are stored in the local memory of each PE and can only be accessed by the PE itself". Fetching AMOs return the value to the calling PE (§9.7: "return after the data has been fetched from the target PE and delivered to the calling PE").

**Failure scenario.** PE 3 on node A calls `shmem_getmem(buf_on_stack, src, 1<<20, pe_on_node_B)`. Node A's progress thread receives the reply chunks. That thread lives in *some* process (a PE, or a helper). Unless it is PE 3's own process, it has no mapping of PE 3's stack. The design says replies are written "directly into the destination" and that the progress thread bumps `bytes_landed`. Neither is possible without:

- `process_vm_writev` (CMA): a syscall per chunk, dependent on ptrace permission; or
- a shared bounce region that PE 3 copies from itself.

**Evidence (observed).** `cma.c` in the VM: sibling A writing sibling B's static buffer.

```
ptrace_scope=0:  A process_vm_writev -> 13 (ok)            B sees: written-by-A
ptrace_scope=1:  A process_vm_writev -> -1 (Operation not permitted)   B sees: untouched
```

So the "direct" path works in the dev VM (Debian default 0) and fails wherever Yama is set to 1, which §4.3 itself says is the default. That is exactly the "passes in dev, fails on target" pattern.

**Consequences for other sections.**

- §3.8's memory argument ("~48 bytes per outstanding get … roughly 12 MB") holds on the *target*. The *origin* still needs staging for every outstanding `_nbi` get's bytes until the origin copies them. `_nbi` completion is defined at `quiet` (§9.6.2.2: "The operation is considered complete after a subsequent call to shmem_quiet"), so the copy may legally be deferred to `quiet`. The staging is then bounded only by the §4.6 blocking resource cap, which the design already permits.
- `bytes_landed` must mean "copied by the origin", or be split into received and copied.
- Fetching-AMO results need a per-operation result slot in *shared* memory that the origin PE reads.

**Recommendation.**

- State where progress threads run: a helper process, or threads inside one PE. Also state what happens when that PE finalizes or dies.
- Give each PE a shared reply region. The origin thread copies into `dest`: a blocking get while it waits, an `_nbi` get at `quiet`.
- Treat CMA as rejected, for the same reason §4.3 gives.

**Severity** blocking. **Confidence** high.

---

## D1 — Per-PE symmetric heaps are not laid out (design change)

**Spec.**
- spec §3, Figure 1: each PE has its own "Symmetric Heap"; "The Symmetric Heap may reside in different memory regions on different PEs."
- §9.2 item 2: "Each PE has a single symmetric data segment and symmetric heap".
- §8, `SHMEM_SYMMETRIC_SIZE`: "Specifies the size (in bytes) of the symmetric heap memory **per PE**".
- §9.3 Note to Implementers: "must avoid … O(N) memory translation tables … may re-map the allocated block of memory based on agreed virtual address".

**Design.** §4.1: "Shared `memfd`/hugetlbfs segment, mapped by every local PE. Intranode put/get collapses to `memcpy`". §4.2: "(region id, offset)". §4.4: "Fixed pool at init, sized by `SHMEM_SYMMETRIC_SIZE`". Nothing says the segment holds L distinct heaps.

**What must be true, and is not written.**

1. **Node memory.** The node segment holds L heaps of S bytes each: 256 × S at 256 PEs. For hugetlbfs, a shared mapping reserves huge pages at `mmap` unless `MAP_NORESERVE` is used (*recalled*, kernel `hugetlbpage.rst`), so the pool must be pre-sized for L × S.
2. **VA layout.** For `shmem_malloc` to return the same pointer on every PE (the §4.2 "identical-VA" goal), each process must map *its own* heap at a fixed VA X. The other L−1 heaps must then sit at VAs that differ per process (for example X + (k+1)·S in some per-process order). So "identical VA" holds only for the own heap. A given PE b's heap has a different VA in each other process. This matters for:
   - `shmem_ptr` arithmetic (needs a per-target base, still O(1));
   - the progress thread (sees every heap at its host process's VAs);
   - the watch-table key (D2).
3. **VA budget.** Each process maps L × S. That is fine with 48-bit VA. On a 39-bit-VA kernel (512 GiB user) it caps L × S (*recalled* that some Raspberry Pi arm64 kernel configs use 39-bit VA; unverified).
4. **Region id.** On the wire, `(region id, offset)` addresses the *target PE's* heap. The PE is already in the message header, so this is fine, provided "region id" never means "the node segment".
5. **`SHMEM_SYMMETRIC_SIZE` must agree across PEs** (see C2).

**Does offset addressing work for intranode puts between PEs?** Yes, if the local translation is `base_of(target_pe) + offset`, with `base_of` a per-process table of L entries (O(local PEs), not O(N) — compatible with the §9.3 note). The design's sentence "intranode access and `shmem_ptr` are raw pointer arithmetic" is true only with that per-target base.

**Severity** design change needed. **Confidence** high.

---

## Watch table (§4.9): spec check and interleaving model

### Spec facts (all documented, and consistent with the design)

- §9.11.1: `void shmem_wait_until(TYPE *ivar, int cmp, TYPE cmp_value);` with TYPE from Table 6. That table covers int, long, long long, and unsigned int/long/long long, int32/int64, uint32/uint64, size_t, ptrdiff_t, so both 32- and 64-bit types. Deprecated short/unsigned short add 16-bit.
- §9.11.8: `int shmem_test(TYPE *ivar, int cmp, TYPE cmp_value);` returns 1 or 0.
- Table 13: `SHMEM_CMP_EQ/NE/GT/GE/LT/LE`.
- §9.11.1: "The ivar object at the calling PE may be updated by an AMO performed by a thread located within the calling PE or within another PE", and "must ensure that shmem_wait and shmem_wait_until do not return before the update … is fully complete".
- Annex G.2 #17: ivar "must be updated using OpenSHMEM AMOs". §9.8.1 extends atomicity to "any point-to-point synchronization routine that accesses the signal data object".
- Example 27 (§9.8.3) waits with `shmem_wait_until(&sig_addr, SHMEM_CMP_EQ, 1)` on a word updated by `shmem_put_signal`. **Signal operations must therefore also run the watch notify.** The design's "operations that write the heap" covers this, but should name signal ops explicitly.
- G.1 #19: `shmem_test_all` on an empty set returns 1. Confirmed.
- The design's 32-bit-futex argument is sound, and 16-bit ivars make it stronger.

### Model

`watch_model.py` explores every interleaving (DFS over explicit states) of threads built from the design's pseudocode:

- **Waiter:** `watch_insert` = probe from slot 0, CAS key EMPTY→addr or join an existing key, `slotcnt++`, `count++`. Then loop: `load seq; load ivar; if pred: remove; futex_wait(seq)`.
- **Waker:** store ivar (or atomic add). Then `load count; if 0 done; probe for key; seq++; futex_wake(all)`.
- **Futex:** compares *memory* atomically; wake-all on the slot's seq word; no timeout, so a missed wake shows as a terminal state with a sleeper.
- **SC:** every store visible at once.
- **TSO:** per-thread FIFO store buffer with nondeterministic flush. Serializing operations (lock-prefixed atomics, CAS, futex syscalls) need an empty own buffer and write memory directly.

Deliberate reversals (V1–V3) are included to show the model can find lost wakes.

Output (`watch_model.out`, 4.1 s):

```
== 1 waiter / 1 writer: design protocol and deliberate reversals
V0 design order                                                SC  states=    204 terminals=    4  -> no lost wake
V1 check-then-insert (insert after first predicate check)      SC  states=    128 terminals=    4  -> LOST WAKE
V2 writer bumps seq before storing ivar                        SC  states=    158 terminals=    5  -> LOST WAKE
V3 waiter loads ivar before seq                                SC  states=    224 terminals=    5  -> LOST WAKE
== memory model (Dekker pattern: waiter count++ / load ivar  vs  writer store ivar / load count)
V4 TSO, count++ and ivar store both non-serializing            TSO states=    336 terminals=    5  -> LOST WAKE
V4b TSO, count++ serializing, ivar store plain                 TSO states=    288 terminals=    5  -> LOST WAKE
V4c TSO, count++ plain, ivar store serializing                 TSO states=    240 terminals=    5  -> LOST WAKE
V5 TSO, both serializing (lock-prefixed atomics)               TSO states=    204 terminals=    4  -> no lost wake
== keys
V6 writer keys by its own VA of the slot (differs from waiter's) SC  states=    215 terminals=    4  -> LOST WAKE
== two writers (progress thread + local PE), atomic add, wait for ivar==2
V7 two atomic adders, design                                   SC  states=   6102 terminals=   30  -> no lost wake
== concurrent waiters on one address / slot reuse
V8 naive refcount: remover clears key while joiner increments  SC  states=  57544 terminals=  422  -> LOST WAKE
V8fix refcount CAS with -1 teardown + key recheck              SC  states=  87906 terminals=  264  -> no lost wake
== linear-probe hole: slot 0 (key Y) freed while key A lives in slot 1
V9 hole -> second A slot claimed at 0; writer stops at first match SC  states=  54310 terminals=   66  -> LOST WAKE
V9fix writer bumps every matching slot                         SC  states=  71520 terminals=   40  -> no lost wake
```

(The full counterexample traces are in the `.out` file.)

### Sound (from the model)

- **The ordering rules are correct and each is necessary.** Load seq before ivar, store ivar before bumping seq, insert before the first check. V0 and V7 (two concurrent atomic adders, standing in for progress thread plus local PE, or two progress threads on different connections) find no lost wake. V1, V2 and V3 each lose one.
- **The race named in the task cannot happen under SC.** The waker reads "empty" before the insert is visible, and the waiter checks the predicate before the store. The waker orders store < read-count; the waiter orders insert < read-ivar. "Read-count before insert" plus "read-ivar before store" gives store < read-count < insert < read-ivar < store, a cycle. V0 confirms this exhaustively.

### D2 — Key must be (PE, region, offset), not address (design change)

§4.9 says "is anyone waiting on this address" and "`watch_find(node->watch, addr)`". Per D1, the waiter's ivar is at VA X+o in its own process. A local PE c doing an AMO on that PE's copy writes through *its* mapping at Y_c+o. The progress thread writes at its host's VA.

**V6** (writer key ≠ waiter key): `T1:load ('key',0)->1000 … T1:load ('key',1)->0 ; T0:futex_wait SLEEP` — lost wake.

The reverse also fails. If the key is the offset only, PE a waiting on offset o and PE b waiting on offset o (each on its own heap copy) share a slot, and every AMO on either copy wakes both. That is not lost, but it is the §3.11 herd again.

**Fix.** Key = (target PE, region, offset). Both waiter and every writer compute it from the translation layer, never from a raw pointer.

**Confidence** high.

### D3 — Slot lifecycle loses wakes unless specified (design change)

§4.9 says "a small per-slot counter for concurrent waiters on one address", "fixed open-addressed table". Neither removal nor probing is specified.

- **V8, naive refcount.** Waiter 1 decrements to 0 and clears the key while waiter 2, having read key==A, increments the count. Waiter 2 sleeps on a slot whose key is EMPTY, so the writer's find misses. Trace ends: `T0:store ('key',0)=0 ; T2:load ('key',0)->0` with T1 asleep.
- **V9, deletion to EMPTY in linear probing.** A's slot sits past a colliding key Y. Freeing Y's slot to EMPTY makes a probe that stops at EMPTY miss A. Such a find/insert can also create a second A slot, and a writer that stops at the first match then misses the other.
- **V8fix (no lost wake):** join by CAS on the count, refusing negative values, then re-check the key and undo if it changed; teardown CAS count 0→−1, then clear the key, then reset the count.
- **V9fix (no lost wake):** the writer bumps every slot matching the key, or equivalently tombstones plus a claimant that scans the whole chain.

**Sizing.** "Sized from local PE count" is not enough:
- §9.11.2–9.11.7 (`wait_until_all/any/some[_vector]`) wait on `nelems` addresses at once. A single futex word per wait then needs one shared seq across all the waiter's slots, or `futex_waitv`.
- Under `SHMEM_THREAD_MULTIPLE`, each thread of a PE may wait.

A full table needs a defined fallback, for example a node-wide overflow seq with wake-all. The "Pass a safety-sweep deadline" rule turns any residual bug into latency, and should stay.

### D4 — Memory model: the protocol is a Dekker pattern (design change)

- **Waiter:** store `count` (insert) → load `ivar`.
- **Waker:** store `ivar` → load `count`.

Under x86-TSO, the waker's buffered ivar store and the waiter's buffered count increment can both be invisible to the other's load. V4, V4b and V4c each lose a wake when either store is non-serializing. V5 (both serializing) is correct. The kernel faces the same shape in its futex hash-bucket `waiters` count, and inserts `smp_mb()` there (*recalled*, `kernel/futex/` comments; not re-read here).

§4.9 says "release on the waker's stores, acquire on the waiter's loads". The Dekker pattern needs:
- release semantics on the **waiter's count store** and on the waker's ivar store;
- acquire semantics on the **waker's count load** and on the waiter's ivar load.

The design names only two of the four.

**Observed (GCC 14.2, aarch64, `mo.c`).**
- `atomic_load_explicit` / `atomic_store_explicit` emit `ldar` / `stlr` for **both** `memory_order_acquire` and `memory_order_seq_cst`. The argument is ignored for plain loads and stores.
- `atomic_fetch_add_explicit` emits `ldaddl` for `memory_order_release` and `ldaddal` for `seq_cst` (`-march=armv8.1-a`), or `__aarch64_ldadd4_rel` vs `__aarch64_ldadd4_acq_rel` (outline atomics).

So the "release" count increment the design would write compiles to a *weaker* instruction than the default. Whether ARM's `ldar`/`stlr`/`ldaddl` semantics make a release store visible to a later acquire load on another core — the property the Dekker pattern needs — was not verified here (*recalled*: the instruction names come from ARM's weak-memory-order annotations; formal cross-core guarantees unverified).

**Recommendation.**
- Use `memory_order_seq_cst` (the default) for all four accesses.
- Make `watch_insert`'s count increment an atomic `fetch_add`, and every ivar-modifying operation (all AMOs and signal ops are CPU atomics already) a locked atomic.
- Record V5's precondition as a §9 invariant.

**Severity** design change (small). **Confidence** high for the model, medium for the hardware semantics.

### Other §4.9 checks

- **Ivars of different widths.** A waiter on `uint64_t` at o and a `uint32` AMO at o+4 key differently and miss each other. That is undefined behaviour by spec §3.2 item 2 ("using different datatypes"), so it is sound by spec. A debug assert costs nothing.
- **Remote AMOs from several nodes via several progress threads** is V7: sound.
- **C8, 32-bit seq wrap (ABA).** A waiter loads q, is descheduled, then exactly 2³² bumps return seq to q, and `futex_wait` sleeps. Unreachable in practice, and the safety deadline covers it. Minor.

---

## D5 — `quiet`/`fence` must cover memory stores; §4.8 says they do not (design change)

**Spec (documented).**
- Table 14 (§9.12) lists "Memory Store" with X under both **Fence** and **Quiet**. Footnote 9: "Ordering and/or delivery of memory store operations are ensured only on contexts created with certain options."
- §9.5, `SHMEM_CTX_NOSTORE`: "Quiet and fence operations performed on the given context are **not required** to enforce completion and ordering of memory store operations performed by the application." So on contexts *without* it, including the default context ("No options are enabled on the default context"), they are required.
- §9.10.1: `shmem_barrier_all` "ensures completion of all previously issued memory stores and remote memory updates".
- §9.12.1 and §9.12.2 Notes: "can enforce memory store ordering only for the calling thread".
- §9.12.3: `shmem_pe_quiet` "ensures completion of memory store, …".

**Design.** §4.8: "Accesses through the returned pointer are ordinary loads and stores, not OpenSHMEM operations. **`fence` and `quiet` do not order them.**" It cites §3.2 item 4 as making this the application's bug.

§3.2 item 4 concerns *concurrent* AMO and load/store access. It says nothing about a completed `shmem_ptr` store followed by `barrier_all` and a load on another PE. That sequence is spec Example 9 (§9.1.9), which the spec presents as correct.

**Failure scenario.** PE 0 stores through `shmem_ptr(dest, 1)` and calls `shmem_barrier_all()`. PE 1 returns from its barrier and reads `dest`. If PE 0's store is still buffered (TSO or ARM weak ordering), PE 1 reads a stale value. By Table 14 that is the implementation's fault.

**Fix.** Cheap: `quiet`, `fence`, `pe_quiet` and the barrier perform one serializing operation in the calling thread — a locked atomic on a private word, or the futex/`io_uring_enter` syscall they usually make anyway, made unconditional. The spec limits this to the calling thread, so no cross-thread `membarrier` is needed. Correct §4.8's statement and citation.

The same applies to "Blocking get" having no X in Table 14: sound, it matches §4.6 "Gets do not participate in fence".

**Severity** design change (small). **Confidence** high on the text. Medium on what the spec authors meant by "memory store".

---

## D6 — Completion data structures (§4.6) (design change)

Spec basis: §9.12.2 "ensures completion of all operations … issued by the calling PE **on the given context**"; §9.12.1 "to the same PE on the same context"; `SHMEM_CTX_INVALID` → "no operation is performed" (both).

1. **Blocking on 64-bit counters.** `wait_until(pe->bytes_landed == pe->bytes_issued)` cannot be a `futex_wait` on either counter (§4.9: 64-bit futexes return `EINVAL`). It needs a 32-bit wake word bumped after `bytes_landed`, with the same load-word-first / store-counter-first order as §4.9. Unspecified.
2. **Starvation.** Waiting for *equality* of a moving pair can fail forever while another thread of the PE keeps issuing `_nbi` gets (THREAD_MULTIPLE). §9.12.2 Notes: "A call to shmem_quiet by a thread completes the operations posted **prior to** calling shmem_quiet". Snapshot `issued` at entry and wait for `landed >= snapshot`; `landed` is monotonic.
3. **`pe->ndirty = 0` race.** Thread T1 is in `quiet`, past the scan. T2 issues a put to node n, adding n and raising `high_seg[n]`. T1 then clears the set. T2's later `quiet` never waits for that put and returns before it completes, which is non-conforming for T2. Fix options: per-context or per-thread dirty sets, or clear only entries whose `high_seg` still equals the value waited for (CAS).
4. **Blocking get and blocking fetching AMO.** Neither may wait on the per-PE counters. A blocking `shmem_getmem` would then wait for every unrelated outstanding `_nbi` get of the PE: conforming but unbounded extra latency, and under (2) possibly forever. Both need per-operation completion (a token or slot, which B1 needs anyway). The design is silent.
5. **Per-PE vs per-context.** Phase 1 has only `SHMEM_CTX_DEFAULT`, so over-quieting across contexts is conforming (`quiet` may complete more than required). But §4.5/§4.6 advertise "a per-context dirty set" while the code sketch is per PE. Pick one.
6. **Thread level.** The design never states what `shmem_init_thread` returns in `provided`. If phase 1 provides `SHMEM_THREAD_SINGLE` or `FUNNELED`, items 2–3 are latent, not bugs. State it (§9.2.1).
7. **`fence` sealing is not required for ordering.** §9.12.1 only guarantees order. The single ordered stream (§3.1) already delivers in order, so `fence` need not seal at all. Sealing is a latency choice that costs aggregation, which §4.6 already notes. Sound but optional.

**Severity** design change. **Confidence** high.

---

## C1 — Statics in phase 1 (clarification)

**Spec.**
- spec §3: "In OpenSHMEM the following kinds of data objects are symmetric: • Global and static C and C++ variables. These data objects must not be defined in a dynamic shared object (DSO)."
- Annex B: "The OpenSHMEM Specification does not specify how OpenSHMEM programs are compiled, linked, and run". `oshcc` is described only as the *Reference Implementation's* wrapper (informative).

**Detection.** The library does not need to tell a static from a bad pointer. The origin translates its local pointer to `(region, offset)`. Anything outside its own heap range `[X, X+S)` is "not a phase-1 symmetric address", and that one range check handles statics, stack and wild pointers alike. It can be made friendlier by testing the main executable's writable `PT_LOAD` via `dl_iterate_phdr` to print "static symmetric objects unsupported". `shmem_addr_accessible` (§9.1.8) must then return 0 for statics, and `shmem_ptr` may return NULL. Both are self-consistent. Detection is sound.

**Conformance cost is understated.** Static symmetric variables appear in:
- §3.2 Examples 1–4;
- §9.1.1 Ex. 5, §9.1.4 Ex. 7, §9.1.9 Ex. 9;
- §9.6.1.1 Ex. 17, §9.7.1.3 Ex. 21, §9.7.1.7 Ex. 25;
- §9.8.3 Ex. 27, the canonical `put_signal` plus `wait_until` example;
- §9.10.1 Ex. 29, §9.12.1 Ex. 45, §9.12.2 Ex. 46.

Phase 1 therefore cannot run the spec's own examples for most of its routines. "`shmem_malloc` covers most real codes" is unsupported here.

**Wrapper.** §4.3 relies on "the wrapper" for `-fno-pie`/`-no-pie` (phases 1.5 and 2). No wrapper appears in §7 (not phase 1, not deferred), and it adds a build artifact §1 does not budget for.

**Recommendation.** Keep phase 1's rejection, but:
- (a) state it as a known non-conformance in §7;
- (b) define the error behaviour (abort with message vs `shmem_addr_accessible == 0`);
- (c) add the wrapper to a phase;
- (d) plan to port the spec examples to `shmem_malloc` for tier 1.

**Severity** clarification needed. **Confidence** high.

---

## C2 — Allocator semantics (§4.4) (clarification)

Spec facts (documented):

| Routine | Size 0 / NULL | Barrier | Failure |
|---|---|---|---|
| `shmem_malloc` §9.3.1 | size 0 → "performs no action and returns a null pointer" | "semantically equivalent to shmem_barrier_all **on exit**" otherwise | returns null pointer (barrier still performed; text makes no exception) |
| `shmem_free` §9.3.2 | NULL → "no action" | "calls a barrier **on entry**" | — |
| `shmem_realloc` §9.3.3 | NULL → acts as malloc; size 0 → frees | "may call barriers on both entry and exit" | "block … unchanged and a null pointer is returned"; "all PEs return a null pointer" |
| `shmem_align` §9.3.4 | size 0 → null, no barrier | exit | alignment must be a power-of-two multiple of `sizeof(void*)`, else UB |
| `shmem_calloc` §9.3.6 | count or size 0 → null, "without performing a barrier" | exit | null on failure; zeroed |
| §9.3 intro | — | "The implicit barriers performed by these routines quiet the default context" | — |

Findings:

1. **Determinism argument is sound** given identical sizes and identical heap capacity. Collectives on one team cannot run concurrently within a PE (§9.4 Thread Safety: "A team may not be used concurrently by multiple threads in the same PE for collective operations"), so the absence of an allocator lock is also sound.
2. **Heap capacity must be agreed.** `SHMEM_SYMMETRIC_SIZE` is read from each PE's environment (§8). If two PEs see different values (a per-rank wrapper script, or a heterogeneous `srun --export`), `shmem_malloc` succeeds on some PEs and returns NULL on others. Offsets stay identical, but symmetry of *existence* breaks. Exchange S at rendezvous (§6.2) and abort on mismatch, or use the agreed minimum. Also specify the parser: float plus one suffix k/m/g/t (binary), trailing characters ignored ("20kk" = 20 KiB), ceiling of the product, invalid → `shmem_init_thread` nonzero or termination. Tier-1 unit test.
3. **Debug checksum "at the barrier the call already performs".** The §5.3 barrier is a dataless counter (`fetch_add` on `arrived`, an arrival AM carrying only the epoch), so there is nothing to compare. Size-0 calls perform no barrier at all. And because every world-team blocking collective shares the same epoch counters, `shmem_malloc` on one PE silently matches `shmem_barrier_all` on another. The checksum must travel in the arrival AM or intranode slot. The size-0 / no-barrier asymmetry means a PE calling `malloc(0)` while others call `malloc(n)` (undefined behaviour) hangs instead of being diagnosed — acceptable, but say so.
4. **`free` quiets on entry.** The barrier on entry quiets the default context before the block is released, so in-flight default-context puts to the block land first. That is correct and should be written down.
5. **`free` on other contexts.** Operations pending on other contexts are the user's responsibility (§9.3.2). Nothing to do in phase 1.
6. **Phase of calloc/realloc/align is unstated.** §7 lists only malloc and free; "Deferred (mechanical)" does not mention the others. §9.8.3 Ex. 27 and §9.11.8 Ex. 42 use `shmem_calloc`. calloc must check `count × size` overflow deterministically.
7. **`shmem_malloc_with_hints` as alias.** Conforming ("If the implementation cannot optimize, the behavior is same as shmem_malloc"). Realloc's "if hints cannot be preserved … null" is trivially satisfied.

**Severity** clarification. **Confidence** high.

---

## C3 — `shmem_pe_quiet` (clarification)

**Spec §9.12.3.**

```c
void shmem_pe_quiet(const int *target_pes, size_t npes);
void shmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes);
```

**Design §4.6.** `void shmem_pe_quiet(shmem_ctx_t ctx, const int *target_pes, int npes)`. Wrong name for the context variant and wrong type for `npes`. §5 of the spec requires `shmem.h` to "contain only the interfaces and constant names defined in this specification".

**Semantics (documented).**
- `npes == 0` → returns immediately, `target_pes` ignored.
- The completion set includes memory stores (D5), blocking put, AMO, put-with-signal, and nonblocking put, put-with-signal and get.
- Visibility is as for `shmem_quiet`.

**Semantics (not stated in §9.12.3).**
- Validity of `target_pes` entries. §9.5 says point-to-point routines on a context use "team-relative PE numbering", and an invalid PE number is undefined behaviour. Treat entries as team-relative to `ctx`'s team and assert in debug builds.
- Behaviour for `SHMEM_CTX_INVALID`. The spec specifies no-op only for `fence`/`quiet`; do the same here.

**Design claims that hold.**
- Waiting on the full per-PE get counter over-waits but conforms.
- Mapping PEs to node dirty entries over-waits (it waits for other local PEs' traffic on the same connection) but conforms.
- The spec Note "might be equivalent to shmem_quiet" is quoted correctly.

---

## C4 — Contexts and teams plumbed, only defaults implemented (clarification)

What phase-1 stubs must do (documented):

| Call | Required behaviour |
|---|---|
| `shmem_ctx_create` §9.5.1 | Failure is **conforming**: "a nonzero value is returned and ctx is set to SHMEM_CTX_INVALID. An unsuccessful context creation call is not treated as an error". A phase-1 stub that always fails is legal |
| `shmem_team_create_ctx` §9.5.2 | `SHMEM_TEAM_INVALID` → nonzero plus `SHMEM_CTX_INVALID`; may fail anyway |
| `shmem_ctx_destroy` §9.5.3 | `SHMEM_CTX_INVALID` → no-op; otherwise implicit quiet |
| `shmem_ctx_get_team` §9.5.4 | default context → `SHMEM_TEAM_WORLD`, return 0; `SHMEM_CTX_INVALID` → `SHMEM_TEAM_INVALID` plus nonzero |
| `shmem_[ctx_]fence/quiet` §9.12 | `SHMEM_CTX_INVALID` → no-op |
| `shmem_team_my_pe/n_pes` §9.4.1–2 | `SHMEM_TEAM_INVALID` → −1 |
| `shmem_team_get_config` §9.4.4 | INVALID → no-op, nonzero; `config_mask == 0` → no action (config may be NULL); otherwise 0 and *current* values |
| `shmem_team_translate_pe` §9.4.5 | any INVALID or non-member → −1 |
| `shmem_team_split_strided/2d` §9.4.6 | if unsupported: `new_team = SHMEM_TEAM_INVALID` and nonzero "on all PEs in the parent team". This is still a collective and must be consistent |
| `shmem_team_destroy` §9.4.8 | INVALID → no-op; predefined → undefined behaviour |
| `shmem_ctx_session_start/stop` §9.9 | INVALID → no action |
| `shmem_team_ptr` §9.1.10 | WORLD → as `shmem_ptr`; INVALID → NULL |

Also:
- **`SHMEM_TEAM_SHARED`** (spec §7) is a predefined handle and must exist and be usable (`shmem_team_n_pes`, `translate_pe`, `team_ptr`). Its definition is "the team of all PEs that would mutually return a non-null address from … shmem_ptr … for all symmetric heap objects". That makes it the §8.1 logical node, and the design already has the locality function. §4.1 treats it as optional ("if … ever implemented"); it is not optional.
- **`shmemx.h`**: spec §5, "The shmemx.h header file must exist, even if no extensions are provided." Observed: `git ls-files` has only `shmem/include/shmem.h` (35 lines) and no `shmemx.h`. §10 records only `shmem.h`.
- §4.5 "A context is a sequence-number space" is incomplete. A context also fixes the team, and so the PE numbering of every point-to-point call (§9.5). That does not change phase 1.

---

## §4.8 `shmem_ptr` / `shmem_team_ptr` (mostly sound)

**Documented.**
- §9.1.9 `void *shmem_ptr(const void *dest, int pe);` — "on the specified PE in the world team"; NULL "Otherwise"; "When calling shmem_ptr, dest is the address of the referenced symmetric data object on the calling PE."
- §9.1.10 `void *shmem_team_ptr(shmem_team_t team, const void *dest, int pe);` — `pe` is "the PE number in the provided team"; WORLD is identical to `shmem_ptr`; INVALID → NULL; "otherwise invalid" → undefined behaviour.
- spec §3.1: passing the result where a symmetric address is required is undefined behaviour.
- spec §4.2: passing both `a` from `shmem_ptr` and the symmetric address of A to one call targeting PE i is undefined behaviour (overlap).

**Sound.** The design's signatures, the "team-scoped, not context-scoped" correction, the NULL-across-logical-nodes rule, and the "not OpenSHMEM operations" characterisation for *atomicity* all match the text.

**Issues.**
- **D5:** the "`fence`/`quiet` do not order them" sentence contradicts Table 14.
- **Out-of-range `pe`.** §9.1.9 does not define it (the §9.5 undefined-behaviour rule is phrased for context operations). A bounds check returning NULL is cheap and safe.
- **C5.** "Returning `NULL` unconditionally is conforming" holds for `shmem_ptr` in isolation. But `SHMEM_TEAM_SHARED` is *defined* as the PEs for which `shmem_ptr` is non-null for all heap objects, and "All valid teams have at least one member" (§9.4). Unconditional NULL, even for `pe == my_pe`, leaves `SHMEM_TEAM_SHARED` with no consistent membership. Minor, medium confidence.
- **Team-relative `pe` in `team_ptr`.** Needs `translate_pe` to world before the locality function. Trivial, but the §4.8 sentence "it is free" should mention it.

---

## AMOs and signals (§4.1, §3.8, §7)

**Documented.**
- §9.7.1.3 compare-swap and §9.7.1.7 fetch-add return the prior value "in one atomic operation"; Table 6 types.
- §9.7: fetching routines "return after the data has been fetched … and delivered to the calling PE"; non-fetching ones "may return before the operation executes on the target PE".
- §9.8.5 `shmem_signal_add` and §9.8.7 `shmem_signal_set` (1.6, G.1 #4), both `(uint64_t *sig_addr, uint64_t signal, int pe)` plus `shmem_ctx_` variants.
- §9.8.1: signal object is `uint64_t`; atomic with respect to same-operator put-with-signal, signal-add ↔ `SHMEM_SIGNAL_ADD`, signal-set ↔ `SHMEM_SIGNAL_SET`, signal-fetch, and point-to-point synchronization.
- §9.8.3: the signal update "indicates the delivery of its corresponding dest data words".

**Sound.**
- Applying fetching AMOs at handler time and carrying the value (§3.8 table) is required. Deferring would reorder stream-ordered operations.
- Signal-add/set as handler-time, AMO-shaped, non-fetching operations fit §9.8.
- Invariant 5 ("signals land after every payload byte") satisfies §9.8.3 internode. Intranode, `memcpy` then the atomic signal update gives the same guarantee.
- A progress-thread CPU atomic and a local PE CPU atomic on the same shared page are genuinely exclusive for same-width operations. The §3.2 quote (atomicity domains; `SHMEM_TEAM_SHARED` as a separate domain, footnote 4) is accurate.

**C6, overclaim (minor).** §4.1: "The shared mapping gives real CPU-level atomicity across that whole set". "That whole set" includes AMOs concurrent with ordinary stores. A multi-byte put (`memcpy`) overlapping an AMO word is not atomic with a CPU CAS; the AMO can observe a torn word. Mixed-width AMOs (§3.2 item 2) are also not mutually atomic at CPU level: `add32` on the low half does not carry into the high half. The claim holds only for same-width word stores. Say so, so nobody relies on it.

**Phase-1 AMO coverage (minor).** Only `shmem_uint64_atomic_fetch_add` and `compare_swap`, plus the uint64 signal ops. The 32-bit wait path the §4.9 seq word exists for, and every Table 6 32-bit type, cannot be driven by a phase-1 AMO. Add one 32-bit AMO (e.g. `shmem_int_atomic_add`) to phase 1 or to its tests.

---

## C7 — Yama default (minor)

§4.3 says Yama `ptrace_scope` "defaults to 1". Observed `cat /proc/sys/kernel/yama/ptrace_scope` → `0` on the Debian 13 VM. The value is distribution-dependent (*recalled*: Ubuntu sets 1). The Debian uapi `linux/prctl.h` defines `PR_SET_PTRACER 0x59616d61` / `PR_SET_PTRACER_ANY` (observed). Whether the running kernel honours that prctl was not tested. It only matters for phase 1.5, and for B1 if CMA were chosen.

---

## Claims verified as sound (with evidence)

| Design claim | Spec evidence |
|---|---|
| §4.5 allocation is world-collective in 1.6 | §9.3.1–9.3.6 each: "a collective operation on the world team"; G.1 #10 |
| §4.2 offset wire format, O(1) translation | §9.3 Note to Implementers: avoid "O(N) memory translation tables" and allow re-mapping at an agreed VA |
| §4.6 gets not in fence | Table 14: Blocking Get blank, Nonblocking Get Quiet only; §9.12.1 "does not guarantee order of delivery of nonblocking Get" |
| §4.6 fence is per context | §9.12.1 "to the same PE on the same context"; G.1 #18 |
| §4.6 `_nbi` completion defined only at quiet | §9.6.2.1/§9.6.2.2 "considered complete after a subsequent call to shmem_quiet" |
| §4.6 blocking cap on outstanding `_nbi` is legal | No spec bound on outstanding operations and no failure mode defined; blocking is not prohibited |
| §5.6 `barrier_all` ≡ ctx_quiet(default) + team_sync(world) | §9.10.1 Notes, verbatim |
| §4.8 `shmem_team_ptr` is team-scoped; WORLD/INVALID behaviour | §9.1.10, verbatim |
| §4.9 `shmem_test` needs no table; test_all on empty returns 1 | §9.11.8; G.1 #19 |
| §4.9 ivar updated only by AMOs; wait/test exclusive with same-type AMOs | G.2 #17; spec §3.2 para 1 "Exclusivity is also guaranteed when the target PE performs a wait or test operation" |
| §4.9 waiter/waker ordering rules | Model V0/V7 (no lost wake), V1–V3 (each reversal loses a wake) |
| §4.4 no cross-process allocator lock | §9.4 Thread Safety forbids concurrent collectives on one team within a PE; §9.2 item 5 |
| §6.4 repeated init/finalize | §9.1.1, §9.1.4 ("All other calls to shmem_finalize perform an operation semantically equivalent to shmem_barrier_all"), §9.1.5 — note: non-last finalize **must still barrier**, which §6.4 omits |
| §7 session routines as no-ops | §9.9 "hints"; INVALID ctx → no action |

The last row contains a small omission. §6.4 says a non-last `shmem_finalize` just decrements. §9.1.4 requires "an operation semantically equivalent to shmem_barrier_all" (minor, high confidence).
