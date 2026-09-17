# Q2 adversary: attacks on the proposed OpenSHMEM core (`q2-architect.md`)

Date: 2026-09-16. Target: `scratchpad/out/q2-architect.md` (ARCH §x). The evidence tags match ARCH §0 (OFI, UCX, P4, GEX/GPI/LCI, MPI, NV, VEN, FUT, USE, SEM). `SPEC Lnnnn` is a line in `scratchpad/spec16.txt`, which I re-read for every spec claim below.

Labels:
- *documented / observed in source*: the claim is taken from a spec line or from an evidence report that cites a primary source.
- *observed in source (new)*: I fetched the source myself in this pass. The URL is given.
- *recalled*: from memory, not verified.
- *arith*: my own arithmetic.

Ratings:
- **fatal**: a conforming program can deadlock or get wrong results, or the design goal fails outright.
- **significant**: a HOT path or a scale target misses the claimed cost, or the semantics are underspecified enough that two conforming cores would behave differently.
- **minor**: a cold path, wording, or an easy fix.

---

## 0. Scorecard

| # | Attack | Class | Rating |
|---|---|---|---|
| A1 | `wait_until` targets outside `OC_SEG_NOTIFY` (statics, plain `shmem_malloc`) always poll, including the spec's own examples and PRK | (a) | significant |
| A2 | A notify counter that counts only NIC rmw loses wakeups: local-thread AMOs, on-node or CPU atomic paths, put-updated ivars | (a)(d) | **fatal** (cheap fix) |
| A3 | Memory-store ordering (Table 14) and `SHMEM_CTX_NOSTORE` are not modelled, and the lazy fence cannot order a later load/store | (a) | significant |
| A4 | A blocking get or fetching rmw on a shared context uses a class-wide counter wait: wrong or starving under THREAD_MULTIPLE | (a)(d) | significant |
| A5 | Put-with-signal on multi-rail or multipath: puts may be striped across rails, the rmw is pinned to one NIC, and `FI_FENCE` covers one endpoint pair | (a)(b) | significant |
| A6 | Segment 0 (data/BSS) cannot be both counter-free (put target) and NOTIFY (signal target); shared-library statics break the offset rule | (a) | significant |
| A7 | "Registered once" reserved segments: unknown per-hint partition, `realloc` cannot move between segments, pinning without ODP, peer-mapping VA × PPN | (a)(c) | significant |
| A8 | On UCX/mlx5, AMOs on the bulk heap (where every real app keeps them) are not atomic-registered, so they take the slow path | (b) | significant |
| A9 | AMO and reduction types: `long double` is correctly excluded, but deprecated `short` waits and float/long double/complex reductions via "RMW SUM" are not covered | (a) | minor |
| A10 | Context options: `NOLOCK` conflates SERIALIZED and PRIVATE; no thread level at `oc_dom_open`; SOS blocks on target counters only in THREAD_SINGLE | (a) | minor |
| A11 | Nonblocking gets into private memory imply per-op state, despite "no per-op state" | (a) | minor |
| A12 | `global_exit`, multiple init/finalize, `pe_accessible` under MPMD | (a) | minor |
| B1 | cxi H = N overclaimed: SOS on cxi falls back to polling | (b) | significant |
| B2 | cxi G/J: `cntr_cnt = 16` per domain against 3 counters per context plus notify counters | (b)(c) | significant |
| B3 | cxi I: AMOs use connection-based reliability state per peer (unquantified); cell should be `?` | (b)(c) | significant |
| B4 | Portals: F = E contradicts §1.3; I/K rows mix BXI with the reference implementation (eager `PtlSetMap`, `conn_t`, progress thread) | (b)(d) | significant |
| B5 | IBGDA DC: C for I/J hides DCI renegotiation under all-to-all traffic; CST disabled on multi-NIC | (b) | minor |
| B6 | uTofu D = N rests on a search summary and recollection | (b) | minor |
| B7 | GASNet-EX C = C although offset addressing is unimplemented | (b) | minor |
| B8 | CXI `FI_FENCE` is an endpoint capability that disables PCIe RO for *all* RMA, not a per-op cost | (b) | minor |
| C1 | "Touched peers" = N for the HOT all-to-all apps (ISx, Graph500, alltoall), so laziness saves nothing and the budget must be per node | (c) | significant |
| C2 | Lazy `rt->lookup` storm: N² lookups job-wide; lookup is impossible from device-initiated paths | (c) | significant |
| C3 | Visibility at `oc_ctx_wait` needs a per-(ctx, peer) dirty set plus a flush per dirty peer (the Open MPI O(N) bitmap) | (c)(d) | significant |
| C4 | Sparse fence-epoch tables per context: "4 B" ignores hash overhead; × contexts × PPN | (c) | minor→significant |
| C5 | Registration × NICs × segments × PPN against CXI optimized-MR limits | (c) | minor |
| D1 | `done` is defined as "visible", but visibility is established only inside `wait` | (d) | significant |
| D2 | The fence gate is "completed at the initiator", but the spec says "delivered"; on UCX, put completion is local only | (d) | significant |
| D3 | Failure semantics: a failed op behind which a fenced signal is queued; at-most-once for *failed* ops; `OC_EIO` from other threads' ops | (d) | significant |
| D4 | Atomicity scope contradictions (job-wide vs TEAM_SHARED CPU path vs "one path per segment") | (d) | minor |
| D5 | Small inconsistencies: entry-point count, a `target_cpu` bit outside the enum, notify semantics that vary by substrate | (d) | minor |

---

## (a) OpenSHMEM features the core cannot express, or expresses only slowly

### A1. `wait_until` on ivars that are not in the signal segment — **significant**

**Claim attacked.** ARCH §2.7 binds notify counters only to `OC_SEG_NOTIFY` segments ("never set on the bulk heap"). §1.3 claims that "the fast path of every HOT usage pattern maps to a native operation" on Portals, CXI and UEC.

**Evidence**
- *Documented.* The spec requires only that `ivar` is a "symmetric address of a remotely accessible data object" (SPEC L7701). Symmetric objects may be "in the symmetric heap or in the global/static memory section" (SPEC L653–654).
- *Documented.* The spec's own put-with-signal example waits on `static uint64_t sig_addr` (SPEC ~L5590–5605, Example 27). The Notes say sig_addr "could be a global/static C variable" (SPEC L5560–5562). Example 27 is therefore a segment-0 signal.
- *Observed in source (new).* In PRK Stencil, the HOT per-iteration wait target `iterflag` comes from `prk_shmem_align(...)` (bulk heap, no hints): https://github.com/ParRes/Kernels/blob/25051e46/SHMEM/Stencil/stencil.c L153, with the wait at L477.
- *Observed in source (new).* PRK Synch_p2p flags come from `prk_shmem_align` (p2p.c L176–178) and are waited on per message (L262, L277, L292): https://github.com/ParRes/Kernels/blob/25051e46/SHMEM/Synch_p2p/p2p.c
- *Documented.* `malloc_with_hints` has no users (USE §1). ARCH itself concedes this in Q10.

**Consequence.** Every existing CPU `wait_until` user, and the spec example, takes the §7 "spin/backoff" path, which "burns a core per waiting PE", on *every* network. The §1.3 "every HOT pattern native" claim is false for PRK and for any port of the NVSHMEM codes that uses static or unhinted signal buffers.

**Smallest revision**
- Allow `OC_SEG_NOTIFY` on segments 0 and 1 as an explicit, costed choice. Set it by default where binding costs nothing: Portals CT on the LE (P4 §5), uGNI `POST_CQWRITE`, and UET where the MR is not `IDEMPOTENT_SAFE`.
- Keep it off only where it demonstrably kills the fast path (CXI optimized MRs, OFI §2).
- Downgrade the §1.3 claim to "HOT patterns whose wait targets are allocated with `SHMEM_MALLOC_SIGNAL_REMOTE`".

### A2. The rmw-only notify counter loses wakeups — **fatal as specified (cheap fix)**

**Claim attacked.** ARCH §2.7: "The counter counts `rmw` operations applied to segment `seg` ... Why rmw-only is enough: since 1.5, wait/test ivars must be updated using AMOs." The layer-above protocol blocks in `oc_notify_wait` until the count increases.

There are three ways the counter never moves while the ivar does.

1. **Local-thread AMOs.**
   - *Documented.* "The ivar object at the calling PE may be updated by an AMO performed by a thread located within the calling PE" (SPEC L7710–7711). Signal wait says the same (SPEC L8866–8867).
   - An rmw from thread B to `my_pe` is a *self* rmw. ARCH §2.4 lets on-node rmw use CPU atomics whenever `OC_CAP_RMW_CPU_COHERENT` holds (only the non-coherent case must loop back through the NIC). It also lets `SHMEM_TEAM_SHARED` contexts use CPU atomics.
   - A CPU-executed rmw does not bump a NIC target counter. Thread A, blocked in `oc_notify_wait(timeout = -1)`, never wakes: a deadlock in a conforming program.
2. **On-node load/store transports bypass the NIC counter.**
   - *Observed in source (via OFI §5).* SOS forces hard polling whenever CMA or XPMEM on-node transports are built in (SOS `configure.ac` L535–541). SOS on cxi also falls back to polling (OFI §8, wait row).
   - SOS evidently met this exact lost-wakeup problem and avoided it by never blocking. ARCH's "native" notify for on-node peers has no equivalent guard.
3. **Put-updated ivars.**
   - *Observed in source (new).* PRK Synch_p2p updates the flags that the peer waits on with `shmem_int_p` (a put), not an AMO (p2p.c L268, L281, L298).
   - This is non-conforming under the 1.5 clarification (SPEC L10471–10472), but the study targets **1.4** too. In 1.4 the clarification did not exist (*recalled*; the Annex G "Clarified" wording suggests intent, but the 1.4 text was not re-read).
   - Deprecated-but-required `shmem_short_wait` / `short` `wait_until` types (SPEC L7680–7695) have **no** AMO of that type (Tables 6–8, SPEC L4241–4290). So a program using them cannot comply with "updated using AMOs" at all.
   - SOS's target counter counts *all* incoming writes (OFI §5; P4 §5 `CT_COMM`), so these programs work on SOS. They would hang on a core that counts only rmw on substrates that don't over-count.

**Smallest revision**
- Specify that *every* rmw the core executes on an `OC_SEG_NOTIFY` segment, by *any* path (NIC, loopback, CPU atomic, AM emulation, self), bumps that segment's notify count and wakes local waiters (host futex plus NIC counter).
- Require `oc_notify_wait` callers in the layer above to use a finite timeout with a re-check, which turns any residual miss into latency instead of a hang.
- Optionally let `OC_SEG_NOTIFY` count puts too (a capability bit), for 1.4 and `short` compatibility.

### A3. Memory-store ordering and `SHMEM_CTX_NOSTORE` — **significant**

**Evidence**
- *Documented.* Table 14 lists **Memory Store** as affected by both fence and quiet (SPEC L8893–8896). Footnote 9 says store ordering is "ensured only on contexts created with certain options".
- *Documented.* `SHMEM_CTX_NOSTORE` means "Quiet and fence operations performed on the given context are not required to enforce completion and ordering of memory store operations" (SPEC L2967–2972).
- *Documented.* Fence "can enforce memory store ordering only for the calling thread" (SPEC L8948–8951).
- The ARCH mapping table (§4a) mentions only `PRIVATE/SERIALIZED` → `NOLOCK`. `NOSTORE` is unmapped.

**Why the lazy fence breaks.**
- `oc_ctx_fence` is "O(1), no traffic: the next op per peer gets `OC_F_FENCE`" (ARCH §2.5).
- Consider: a put to an on-node peer via NIC loopback or a CMA path; then `shmem_fence()`; then a *store* through `shmem_ptr` to the same object.
- The store is not a core op, so nothing carries the fence flag, and the earlier put can land after the store.
- *Observed in source (via UCX §8).* OSSS-UCX implements on-node fence with compiler memory barriers (`__sync_synchronize`, `_mm_mfence`, `memfence.h`); Open MPI uses `opal_atomic_wmb()`. Existing implementations do treat stores as fenced.

**Smallest revision**
- Add a "stores" mode, e.g. `oc_ctx_fence(ctx, OC_FENCE_STORES)`. It drains in-flight ops to ranks whose `oc_seg_peer_ptr` is non-NULL, then issues a CPU store barrier.
- Map `SHMEM_CTX_NOSTORE` to the current lazy fence and quiet. The default context has no options (SPEC L2957), so it pays the store-aware form. That cost is on-node only and zero if no on-node op is in flight.

### A4. Blocking get and fetching rmw on shared contexts — **significant**

**Claim attacked.** ARCH §2.3: a blocking get means "initiate, then wait on this operation. With counters only, that means wait on `OC_OPC_GET`". §2.6: `oc_ctx_wait` returns when `done + failed ≥ issued` "as sampled at entry. Operations issued concurrently by other threads may extend the wait". Q12 admits only "may over-wait".

**Attack**
- Ops complete out of order: gets have no ordering (ARCH §2.3; SPEC L8933–8935). So `done ≥ issued_at_entry` does **not** imply that *this* get's data has arrived.
- A correct counter-only implementation must therefore wait for `done + failed ≥ issued` *current*.
- Under THREAD_MULTIPLE on the default context (no options, so shareable, SPEC L2957), with other threads issuing gets continuously, that condition may never hold. The result is **starvation, not over-waiting**.
- *Observed in source (via OFI §5).* SOS in THREAD_MULTIPLE with context locks forces a poll limit of −1 ("spin-poll forever"). This is the practical symptom.
- Threaded Graph500 and GUPs use contexts (USE, ARCH §4b), but any library that shares the default context (Kokkos, hclib) hits this.

**Smallest revision.** No API change is needed. State that the blocking `oc_get`, a fetching `oc_rmw` without `OC_F_ASYNC`, and `oc_get_word` complete **per operation** internally. Precedents:
- the IBGDA ibuf slot plus CST (NV §4);
- DMAPP syncid (VEN §A1).

Counters remain the only completion mechanism for `OC_F_ASYNC`.

### A5. Put-with-signal atomicity on multi-rail and multipath — **significant**

**Claim attacked.** ARCH §2.5: put-with-signal "needs no compound primitive: `oc_put(...)` followed by `oc_rmw(..., OC_F_FENCE)`". §2.2: "the core may spray packets, fragment, stripe across rails". §2.4: "All `rmw` to the same `(rank, seg, off)` must go through a single NIC and queue."

**Attack**
- *Documented.* The spec requires that "the completion of signal update ... indicates the delivery of its corresponding dest data words" (SPEC L5540–5543).
- *Documented.* `FI_FENCE` defers the fenced op "until all previous operations targeting the same **peer endpoint** have completed" (SEM §5.2, quoting `fi_msg.3.md` L329–340).
- If the core stripes the put's fragments over rail B while the signal rmw is pinned (for atomicity) to rail A, the hardware fence on A never sees B's fragments. The signal can become visible before the data.
- *Observed in source (via NV §4, §8).* NVSHMEM needed software address hashing for AMOs on multi-NIC and **disables CST skipping on multi-NIC** because skipping "would be unsafe" (`ibgda_device.cuh` L2124–2240; `ibgda.cpp` L4491).
- The same hazard exists in software gating if "earlier ops to that rank" are tracked per queue.

**Smallest revision.** Define `OC_F_FENCE` scope as (ctx, rank) **across all rails**. Require the core either to:
- place every op issued to rank r since the last fence onto the rail that will carry the fenced op (rail affinity per (ctx, rank) within a fence window), or
- software-drain the other rails before posting the fenced op.

Report which one applies with a capability bit. Also require ops held in software queues (`OC_F_TRY` backpressure, bounce buffers) to be posted before a fenced op, because a NIC fence sees only posted ops.

### A6. Symmetric statics as segment 0 — **significant**

**Claim attacked.** ARCH §2.1: "segment 0 = data/BSS", and "offset of a global within one executable image is the same across processes even under ASLR (*recalled*)".

**Attack 1: attribute conflict**
- Statics are simultaneously put targets (SPEC Example 45: `static long dest[10]`, L8960–8975) and signal or wait targets (SPEC Example 27: `static uint64_t sig_addr`).
- Segment attributes are per segment, and binding a counter to a CXI MR "disables optimized RMA for every operation on that MR" (OFI §2: `cxip_dom.c` L387–390; `fi_cxi.7` §Optimized RMA).
- Segment 0 therefore either slows all static puts or polls on all static waits (A1).

**Attack 2: images**
- Offsets are invariant only within one ELF image. Statics in shared libraries (a Python extension module hosting pplx or Triton-style code, a C++ library with `static` members, `dlopen`ed plugins) sit at an ASLR-randomised base independent of the executable's `.data` (*recalled*: standard ELF/ASLR loader behaviour).
- The spec says "Each PE has a single symmetric data segment" (SPEC L1885) without restricting it to the main executable. It also notes that statics are symmetric only "between processes running from the same executable file" (SPEC L1630–1633, MPMD).
- A single `(seg 0, off)` cannot address library statics, and `shmem_pe_accessible` under MPMD needs per-peer image identity. ARCH §4b lists `addr/pe_accessible` as a local "registry lookup".

**Smallest revision**
- Allow a small set of *image segments* (one per loaded image that has symmetric statics, identified by build-id), plus an optional NOTIFY sub-range, e.g. a dedicated linker section `.shmem_signal`.
- `pe_accessible` compares build-ids through the runtime at first use.
- If this is considered too much, state explicitly that shared-library statics are unsupported.

### A7. "Registered once" reserved segments vs `malloc`/`realloc`/hints — **significant**

**Attacks**
1. **Unknown partition.**
   - `SHMEM_SYMMETRIC_SIZE` gives one heap size (SPEC ~L1992–1993). ARCH splits it into bulk, RMW, signal and device reservations fixed at init.
   - An app that places 90 % of its heap under `ATOMICS_REMOTE` (the spec even recommends that when "the majority of operations ... are atomic", SPEC L2216–2220) fails, even though the total fits.
2. **No migration.**
   - "`shmem_realloc` preserves allocation hints" (SPEC L2098). Growth must stay inside the same reservation; ARCH §7 says growth beyond the reservation is unsupported.
   - Reserving large VA to avoid this runs into (3) and (4).
3. **Pinning.**
   - Registering an untouched reserved VA range requires ODP or ATS on most NICs. Otherwise the whole reservation is pinned and committed at init, on every PE.
   - CXI documents network page faults as a significant performance hit (OFI §9; ARCH Q15).
   - Matrix column C = N for cxi, Portals and UEC is conditional on this and should say so.
4. **Peer mapping VA.**
   - `oc_seg_peer_ptr` requires mapping every on-node peer's segments (TEAM_SHARED requires non-NULL `shmem_ptr` for *all* heap objects, SPEC L1167–1173).
   - With PPN = 128 and a 1 TiB total reservation per process, that is 128 TiB of VA per process (*arith*). This is the entire 47-bit x86-64 user VA (*recalled*: 128 TiB with 4-level paging).
   - A modest 64 GiB reservation gives 8 TiB (*arith*), which is fine. The reservation size is therefore bounded by VA × PPN, not only by memory.

**Smallest revision**
- Make one heap reservation whose hint *sub-ranges* are carved at allocation time, with growth from opposite ends or a buddy scheme over one registration.
- Express segment attributes as per-range attributes where the substrate allows it (Portals: several LEs over one range; libfabric: several MRs over sub-ranges).
- Otherwise document a fixed split controlled by environment variables.
- State the VA bound `PPN × reservation ≤ user VA` and pin behaviour per substrate in the capabilities.

### A8. AMOs on the bulk heap on UCX/mlx5 — **significant** (feasibility)

**Evidence**
- *Observed in source (via SEM §4.4).* UCX `NEWS` L649 reports "remote access error when rcache memory is not registered with atomic access".
- ARCH §2.1 keeps atomic registration (`OC_SEG_RMW`) off the bulk heap. ARCH §2.4 says rmw on non-RMW segments "may have to use a slower path".
- Every HOT AMO user allocates without hints: Kokkos, bale, Graph500, ISx (USE §1–2; hints have no users).

On UCX, HOT AMOs would therefore fall to `amo_sw` (target CPU plus progress thread, UCX §8.4), while matrix column D says N.

**Smallest revision**
- Split the attribute. Registering atomic access does **not** by itself disable fast RDMA on mlx5 (*recalled*; verify). Register the bulk heap `OC_SEG_RMW` wherever it costs puts nothing, and reserve separate segments only for counter binding (`NOTIFY`).
- Change the UCX cell D to "N if the heap is atomic-registered, else E".

### A9. AMO widths and types — **minor**

- **`long double`.** It is correctly outside the AMO tables: Tables 6–8 contain no `long double`, `char` or `short` (SPEC L4241–4290). No AMO needs 16 bytes, so ARCH's w ∈ {4, 8} is sufficient **for AMOs**.
  - `long double` appears only in RMA types (SPEC L3485) and reductions (Tables 10–11, SPEC L7150–7212).
- **Deprecated `short` / `unsigned short` `wait_until` / `wait`.** These are still required (SPEC L7680–7695) and need a 2-byte coherent local compare. `oc_seg_sync` plus a local load covers it, but see A2(3) for the notify problem.
- **Reductions.** ARCH §4b/§7 "1-element reductions: RMW SUM into root" works only for integer SUM, and bitwise AND/OR/XOR where `rmw_ops` allows. It cannot express PROD, MIN or MAX (not core ops; Q11), float/double/long double SUM (integer add ≠ IEEE add), or complex SUM/PROD.
  - SOS Portals aborts at startup if the atomic size is below `sizeof(long double _Complex)`, precisely because it uses atomics for reductions (P4 §8b.2).
  - Revision: state that the RMW-SUM reduction path is integer-only.
- **`OC_RMW_READ` "may be implemented as fetching ADD 0".**
  - On CXI and UET that is a remote *write* for counting purposes. It bumps `FI_RMA_EVENT` or notify counters (spurious wakeups) and is non-idempotent under UET rules (FUT §A.5).
  - Revision: prefer a native READ, or plain get plus sync where the substrate offers only datatype-granular atomicity, as SOS Portals already does (P4 §8a: `atomic_fetch` = `PtlGet`).

### A10. Context options and thread levels — **minor**

- **`OC_CTX_NOLOCK` conflates two options.**
  - `SHMEM_CTX_SERIALIZED` still hands a context between threads (SPEC L2961–2963).
  - `SHMEM_CTX_PRIVATE` pins it to its creating thread (L2964).
  - PRIVATE additionally permits non-atomic counter updates and thread-local doorbells. Cray exposed thread registration for this reason (VEN §A8.4). Revision: add a separate `OC_CTX_THREAD_PRIVATE`.
- **`oc_dom_open` has no thread-level argument.**
  - Under SINGLE or FUNNELED, the core can skip atomic increments of `issued` (ARCH §2.6 "incremented before posting") and use `FI_THREAD_DOMAIN` (OFI §6).
  - Revision: pass the thread level.
- **Unexplained divergence from SOS.** Both SOS transports block on target counters **only in THREAD_SINGLE** and poll otherwise (P4 §5: `shmem_synchronization.h` L180–198; OFI §5). ARCH offers native `oc_notify_wait` at every thread level without addressing why SOS refused. That reason is not stated in the reports: **unverified**, investigate before claiming N.

### A11. Nonblocking gets into private memory — **minor**

- `get_nbi` `dst` is arbitrary private memory, delivered by quiet (SEM §4.2).
- With a bounce copy, or dynamic registration per op, the core must keep a per-op record (bounce → dst, or an MR handle) until the counter covers it.
- ARCH claims "no per-op handles", but internally the state is O(outstanding nbi gets) (SOS: long frags and bounce free lists, P4 §8b.1, OFI §8).
- Revision: state the bound and that `oc_ctx_wait(GET)` performs the copy-out.

### A12. `global_exit`, multiple init/finalize, and `pe_quiet` — **minor**

- **`global_exit`** "results in the immediate notification to all PEs to terminate" (SPEC L1505–1507). Victims may be blocked in `oc_ctx_wait` or `oc_notify_wait` with an infinite timeout, or be GPU kernels busy-polling (NV §9). `rt->abort` alone cannot unblock the NIC wait objects. Revision: add `oc_dom_interrupt` or require waits to be signal-interruptible.
- **Multiple init/finalize** (SPEC L1257–1260; 1.6 Annex G item 2).
  - Reference counting belongs above the core.
  - A real finalize followed by re-init re-registers segments with the same client keys. CXI keys 0–99 must be closed first (OFI §2), and lazy address caches refill (C2).
  - Revision: document whether `oc_dom_close` / `oc_dom_open` must be restartable, and whether the address cache may survive.
- **`pe_quiet` → `quiet`** is allowed (SPEC L9114–9115). No attack.
- **Team-relative addressing** via affine world translation is sound (SEM §4.9). The only cost is a PPN-sized table for TEAM_SHARED-derived teams. No attack.

---

## (b) Feasibility-matrix overclaims

### B1. cxi H (target notify) = N — **significant**

- ARCH cites OFI §5 and §9. OFI §8 (wait row) says SOS blocking wait requires "THREAD_SINGLE, no hard polling, **no manual progress**", and that "cxi (manual progress) ... fall[s] back to polling".
- cxi is `FI_PROGRESS_MANUAL` only (OFI §5), and its wait objects have "a documented race and a recommendation for short epoll timeouts" (OFI §5).
- No shipping OpenSHMEM blocks on a cxi target counter.
- Revision: H = "C/? (counter exists; blocking wait unproven under manual progress; kills optimized RMA on that MR)".

### B2. cxi G = N and J = C — the counter budget — **significant**

- *Observed in source (via OFI §2).* The cxi domain attributes are `mr_cnt = 100`, **`cntr_cnt = 16`**, `ep_cnt = 128` (`cxip_info.c` L30–60). ARCH §3.2 quotes the ep and cq budgets but not `cntr_cnt`.
- ARCH gives each context 3 counter classes (PUT/GET/RMW) plus one counter per notify segment.
- If `cntr_cnt` is a hard per-domain limit: (16 − notify counters) / 3 ≈ **4–5 contexts per process** (*arith*). SOS, with 2 counters per context, gets ≈ 7.
- Whether 16 is enforced or advisory is **unverified**.
- Revision:
  - Allow `OC_OPC_GET | OC_OPC_RMW` to share a counter, as SOS does (OFI §8).
  - Add a `max_cntr` limit to `oc_caps_t`.
  - Mark J = "C, ≤ min(128 EP, 32 CQ, cntr_cnt/k)".

### B3. cxi I = C (~90 B/peer) — AMO reliability state — **significant**

- *Documented (via OFI §3).* "Cassini implements a connection-based reliability model to support reliable execution of AMOs" (`fi_cxi.7` §Unreliable AMOs). The size is **unquantified**, and "unreliable AMOs are not ordered".
- ARCH requires exactly-once rmw, so it must use the reliable, connection-based AMOs.
- The HOT AMO apps address **every** peer: ISx `fadd` per target PE per iteration (USE §2, item 6); Graph500 CSWAP per edge. Per-peer AMO connection state therefore scales with N, not with a sparse "touched" set.
- Revision: I = "? for rmw", and open Q "CXI AMO connection state per peer".

### B4. Portals row — **significant**

- **F = E contradicts §1.3.** ARCH §1.3 says the fast path of every HOT pattern is native on Portals. Portals F = E (fence = quiet unless TDO + small; P4 §8b.3), and fence plus put-with-signal are HOT (PRK, Graph500 and GUPs quiet or fence after every op, USE §2.1). On SOS Portals, put-with-signal is put + quiet + AMO (ARCH §2.5 itself). Revision: remove Portals from the §1.3 sentence or qualify it with "BXI with TDO".
- **I = N "8 B logical map".** `PtlSetMap` takes the *full* N-entry `ptl_process_t[]` map eagerly: 8 MB at 10^6, plus O(N) KVS gets in SOS (P4 §3). Only `PTL_IGNORED` (the runtime owns the map) avoids it. The reference implementation eagerly allocates a `conn_t` of a few hundred bytes (estimate) *per rank* (P4 §3: `ptl_ni.c` L178–211), i.e. "hundreds of MB" at 10^6. ARCH §3.4's "lookup is a function and nothing is cached" is not what the API requires.
- **K = N.** The reference implementation needs a progress thread per process and uses "too many cores" (P4 §5, §9). That is E for the reference implementation. BXI hardware details are `?` (ARCH itself).
- Revision: split the row into "Portals 4 spec/BXI (?)" and "P4 reference implementation (E for I, K)".

### B5. IBGDA DC: I and J = C — **minor**

- *Documented (via NV §3.2).* "Each time the DCI switches to a different DCT, the connection must be renegotiated".
- All-to-all AMO or put traffic through few DCIs thrashes. The DCT map-by fix brings back O(N) QPs (NV §9).
- CST is disabled on multi-NIC (NV §8), making quiet costlier there.
- Revision: annotate C with "latency E under all-to-all".

### B6. uTofu D = N — **minor**

- The ARMW op list (CSWAP, ADD, …) is *recalled* (VEN §B1). SEM §4.4 has only a search summary; "the PDF itself was not read".
- ARCH's own two-source rule is not met.
- Revision: D = "N? (4/8-B ARMW documented; op list unverified)".

### B7. GASNet-EX C = C — **minor**

- ARCH says "offset addressing UNIMPLEMENTED" (GEX §A.2), and ibv keeps rkey plus base tables per peer.
- Unimplemented is not "cheaply emulated". Revision: C = E.

### B8. cxi F = N "per-op flag" — **minor**

- *Observed in source (via SEM §5.2).* "Requesting FI_FENCE prevents the use PCIe RO for RMA" (`cxip_info.c` L1987–1992). This is a capability requested at endpoint or domain setup, so **all** RMA on that endpoint loses relaxed ordering, not just fenced ops.
- Graph500 and PRK fence after every op, and `FI_FENCE` also defers every op posted after the fenced one (SEM §5.2). The per-op flag therefore serialises per peer.
- Revision: add a `oc_dom_open` flag `OC_DOM_WANT_FENCE`. The matrix then says "N, costs PCIe RO for all RMA when enabled".

---

## (c) Scale explosions

### C1. "≤ 64 B per *touched* peer, per process" — **significant**

- **Touched = N for HOT patterns.** ISx per-iteration `fadd` plus put to every PE; Graph500 CSWAP to arbitrary owners; PRK transpose alltoall; strip-mined alltoall and collect inside the layer above (ARCH §7 "O(N²) data for alltoall"). After the first alltoall or barrier-heavy phase, every PE has touched every PE. Laziness then buys nothing at steady state.
- **Per node, not per process** (*arith*, from ARCH's own table):
  - 76 B × 10^6 peers × 128 PPN = **9.1 GiB per node**;
  - cxi at ~90 B (OFI §3) → **10.7 GiB per node**, before AMO connection state (B3) and fence epochs (C4).
- **Revision**
  - Make the address cache and key tables **node-shared** (shared-memory, read-only after fill). Portals `PTL_IGNORED` and UET FA tables are already node-scoped (P4 §3; FUT §A.3).
  - Report `bytes_per_peer_per_node` in capabilities.
  - Rewrite the budget in §3.3 per node.

### C2. Lazy `rt->lookup` storms — **significant**

- One `lookup` per (process, first-touched peer) means N² = **10^10** lookups job-wide at 10^5 and 10^12 at 10^6 (*arith*), concentrated in the first collective or alltoall.
- If `lookup` is a PMIx direct-modex fetch, each miss is an RPC to the peer node's server (*recalled*). That is a thundering herd against the runtime at exactly the moment startup is "done".
- Alternatively, the runtime already holds all N entries per node, and laziness only moves an O(N) table from the core into PMIx.
- **Device paths cannot call `lookup`.** A GPU kernel issuing its first put to a new rank (DeepEP, pplx dispatch to all experts, USE §2) needs the address resolved in advance (NV §6–7; ARCH Q13). Device-initiated capability therefore implies eager addressing for the device-visible rank set.
- **Revision**
  - Constrain `lookup` to be O(1) and local (algorithmic, or a node-shared table).
  - Add a batch `lookup_range(r0, n)` for predictable eager fills (collective setup, device paths).
  - Declare `OC_CAP_DEVICE_INITIATED` to require eager addresses for the ranks involved.

### C3. Visibility at `oc_ctx_wait` needs O(touched peers) flushes — **significant**

- ARCH §2.6 moves the visibility flush (CXI `FI_DELIVERY_COMPLETE`-style flushing read, UCX `ucp_ep_flush_nbx`, IBGDA DUMP CST) from each op into `oc_ctx_wait`.
- To flush at wait time, the core must know **which peers** have ops that are transmit-complete but not yet visible, and flush each one.
- *Observed in source (via UCX §8.5).* Open MPI keeps exactly this: an O(N)-bit bitmap plus an int array of N **per context**, and issues a flush or dummy get to every dirty PE at each fence or quiet.
- After an alltoall, one `quiet` means 10^5 flush RTTs.
- Graph500 and GUPs call quiet after *every* op (USE §2.1), so they pay one flush per op anyway. The HOT case gains nothing, and the idle "two loads and a compare" claim applies only when nothing was issued.
- Whether CXI transmit-complete already implies visibility is open (ARCH Q9). If it does, this attack collapses for CXI but stands for UCX and IBGDA.
- **Revision**
  - Add per-(ctx, peer) dirty-tracking cost to §3.2 and §3.3.
  - Allow `OC_CAP_TXCOMP_VISIBLE` to skip it.
  - Let the layer above choose per op between "flush-on-op" and "flush-at-wait" (a `OC_F_VISIBLE` flag), because quiet-after-every-op users want the former.

### C4. Fence-epoch tables — **minor → significant**

- ARCH puts this at "≈ 4 B × touched peers per context" (sparse). A sparse hash entry (key + epoch + overhead) is realistically 16–24 B (*recalled*: typical open-addressing or uthash overhead; the cxi AV's `UT_hash_handle` alone is ~56 B per OFI §3).
- *Arith.* 32 contexts × 10^6 touched × 16 B = 512 MB per process, i.e. **65 GiB per node** at 128 PPN.
- A dense 4 B × N array per context is 4 MB at 10^6, cheaper than sparse once touch is high (C1). It also costs an extra table load per op on the put fast path.
- **Revision.** Adopt Q3's alternative for high-touch contexts: a context-wide "fence all ops until the pre-fence `issued` count has drained" (one word), switching to a dense per-peer array only when `max_ctx` is small.

### C5. Registration × NICs × segments × PPN — **minor**

- ~8 segments × 4 NICs per node (GPU nodes) × 128 PPN = 4096 registrations per node (*arith*), each a separate MR per NIC domain.
- CXI's "64k standard MRs or 2k optimized MRs" capacity (OFI §2) is quoted without scope. If it is per NIC, 128 PPN × 8 optimized MRs = 1024, which is within 2k but leaves little headroom for device heaps.
- UET optimized headers index MRs with 12 bits (FUT §A.2).
- Revision: state `max_segs` as a per-node budget divided by PPN, like `max_ctx_hint`.

---

## (d) Internal inconsistencies and imprecise semantics

### D1. The meaning of `done` — **significant**

- §2.6 says "`done` counts operations that are **remotely complete and visible**".
- The same section says visibility is guaranteed only by the flush inside `oc_ctx_wait`, which runs "if a substrate's completion event can precede target placement".
- On CXI (transmit-complete), UCX (local completion, UCX §1: "completion of the operation on the remote address requires ... flush") and IBGDA, `done` as read by `oc_ctx_count` is therefore **not** visible.
- Revision:
  - Define `done` as "the substrate's completion".
  - Define "visible" as a property that only `oc_ctx_wait` returning establishes.
  - Make `oc_ctx_count` consumers (the idle check in quiet) test `issued == done && !dirty`.

### D2. Fence gate: "completed at the initiator" vs "delivered" — **significant**

- ARCH §2.5: the fenced op is applied "only after every operation previously issued ... has *completed at the initiator*".
- The spec: ops before the fence "are guaranteed to be **delivered** before any subsequent operations" (SPEC L8931–8933).
- On UCX, put completion means only "local buffer can be reused" (UCX §1), so gating on it orders nothing. That is why `ucp_worker_fence` exists and why Open MPI added per-peer dummy gets and flushes (UCX §8.5).
- On CXI, `FI_FENCE` works only because requesting it disables PCIe relaxed ordering (B8), i.e. transmit-complete is made to imply placement.
- Revision: define the gate as "placed at the target" and let each substrate map that to its own mechanism.

### D3. Failure semantics — **significant**

- **Signal after failed data.** A put fails (counted in `failed`, detected only at the next wait). A fenced signal rmw queued behind it (`FI_FENCE` defers "until ... completed", and an error is a completion) is then applied. The receiver's `wait_until` wakes and reads data that never arrived. The sender aborts later, but the receiver has already acted. Revision: `OC_F_FENCE` must suppress (fail) the fenced op if any gated predecessor failed.
- **Duplicates of failed ops.** "At-most-once applied after completion" (§2.2) says nothing about ops counted **failed**. A failed put may still be applied later (for example a stale retransmit), and a failed rmw may or may not have executed, which is the replay hazard in VEN §A4. Revision: failed ⇒ "outcome unknown; the core guarantees no application after `oc_ctx_wait` returns `OC_EIO`", or else abort synchronously.
- **Cross-thread `OC_EIO`.** `oc_ctx_wait` "returns `OC_EIO` if `failed` advanced", including failures of *other* threads' ops on a shared context. Combined with "failed > 0 ⇒ abort" (§2.2), a thread doing an unrelated quiet aborts the job. That is acceptable only if it is stated. The Q14 error-record queue is the real fix.

### D4. Atomicity scope — **minor**

- §2.4 requires exclusivity "through any context of any domain in the job" (stronger than the spec).
- The same section lets `SHMEM_TEAM_SHARED` contexts use CPU atomics (a separate atomicity domain per SPEC L684–694), and requires "one execution path per segment: all-NIC or all-CPU".
- These three statements cannot all hold for one segment reached through both a WORLD context and a SHARED-derived context.
- The spec makes only *concurrent* cross-domain access undefined (SPEC L690–691). *Sequential* mixing is legal and requires `oc_seg_sync` before a CPU atomic when the NIC is non-coherent. ARCH mentions sync only before loads.
- Revision: state "per segment, per atomicity domain", and require sync before CPU rmw on non-coherent substrates.

### D5. Small inconsistencies — **minor**

- **Entry-point count.** §1.3 says 22 entry points. The §1.2 sketch has 20 `oc_*` functions plus 3 runtime callbacks.
- **`caps.target_cpu`.** It is "a bitmask over `OC_OPC_*`, plus a reliability bit folded into it" (§3.5), but no bit is defined beyond `OC_OPC_ALL = 7`.
- **Notify over-counting.** §2.7 lets a substrate over-count puts (Portals `CT_COMM`). Programs that update ivars with puts (A2(3)) then work on Portals and hang elsewhere: a portability hazard. Specify one behaviour or add a capability bit.
- **`oc_get_word`.** It has no flags argument, so there is no `OC_F_TRY` and no async form. That is inconsistent with `oc_put_word`, and it blocks lazy-lookup backpressure (C2).
- **§2.2 blocking put.** "Zero-copy, with the core waiting for its own local completion": on CXI, local completion is a target ACK, so a large blocking put costs a round trip. That contradicts the "blocking put is locally complete on return, no ack" rationale in the same section, unless bounce-copying is mandated.

---

## Smallest overall revision set (priority order)

1. **A2.** Every rmw path bumps notify and wakes local waiters. Waits in the layer above use a finite timeout with a re-check. (Fixes the fatal issue.)
2. **D2 + D1 + C3.** Fence gate = "placed at the target". `done` ≠ visible. Visibility is established by `oc_ctx_wait` with per-peer dirty-tracking cost declared, or skipped via `OC_CAP_TXCOMP_VISIBLE`.
3. **A5 + D3.** Fence scope covers all rails of (ctx, rank). A fenced op fails if a gated predecessor failed.
4. **A4.** Blocking gets and fetching rmw complete per operation internally.
5. **A1 + A6 + A8 + A7.** Attributes per range, not per whole segment. NOTIFY allowed on data/BSS and heap where cheap. Atomic registration on the bulk heap where free. One reservation with sub-ranges. Image segments for shared-library statics.
6. **A3.** Store-aware fence for non-NOSTORE contexts.
7. **B1–B4, B6–B8.** Correct the matrix cells (cxi H, G/J `cntr_cnt`, I AMO state; Portals F/I/K split; uTofu D; GASNet C; CXI fence as a domain capability). Remove Portals from the §1.3 "every HOT pattern native" claim.
8. **C1, C2, C4.** Per-node, node-shared address state. Local O(1) lookup with a batch form. Context-wide fence epoch for high-touch contexts.
