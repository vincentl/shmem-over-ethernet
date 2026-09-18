# Implementing `transport.h` on RoCE

High-level plan and cost estimate for a RoCEv2 backend behind the §4.10 boundary.
Date: 2026-09-17.

Inputs: `shmem/src/transport.h`, `docs/reviews/core-api.md` (cited as CORE §x),
`docs/design/shmem-ethernet-design.md` (cited as §x).

**Status.** Analysis, not a decision. The decisions it argued for live in
`docs/design/shmem-ethernet-design.md` — §4.10 for the boundary's contract, §4.1
for the registration trap, §7.1 for what is scheduled, §10 for what is open and
what was rejected. Two sections have been corrected since it was written, both
marked in place: §3.1's heap mapping, which proposed a scheme that would have
destroyed §4.1's fixed heap base, and §8.4's remedy for collectives.

## How to read this document

The reference platform is RoCEv2 on ConnectX-6/7-class NICs at 200 GbE, verbs
plus mlx5 direct verbs / DevX where stock verbs is insufficient, 64–128 PPN.

This repository's evidence labels apply. Nearly every hardware fact below is
***recalled***: no RoCE hardware has been measured for this project and no
driver source was read while writing this. CORE §7.2 already holds four open
questions (C4, C12, C15, and the atomics reachability question) on exactly the
mlx5 behaviour this plan leans on, and they are not closed here. Arithmetic of
mine is marked *arith*. §10 lists what must be verified before any of the
numbers are quoted as results rather than as estimates.

---

## 1. Verdict on one page

**(a) Feature utilization.** The boundary reaches the RoCE features that carry
OpenSHMEM: RDMA write and read, inline sends, 8-byte atomics, same-QP ordering,
selective signalling, XRC or DC scalability, on-demand paging, relaxed ordering.
Three groups are out of reach, and only the last is the boundary's fault:

1. **Absent from RoCE, not from the boundary** — target-side counters, 4-byte
   and bitwise atomics in stock verbs, non-fetching atomics, symmetric rkeys.
2. **Out of phase scope** — GPU-initiated paths (`transport.h` is a host-side
   blocking C API; CORE §7.2 C8 says the same of the core).
3. **Blocked by where the boundary is drawn** — in-network collectives (SHARP),
   UD multicast broadcast, and triggered / cross-channel operation chaining.
   §4.10 keeps collectives above the boundary, so a backend cannot offer them.
   This is the largest single loss and it is a deliberate design choice, shared
   with CORE §1.1.

**(b) Performance.** Yes, on the data path. The boundary itself costs nothing
measurable: build-time binding with no function-pointer table (§4.10) means a
`static inline` fast path, and every RoCE-shaped decision — inline versus
bounce versus zero-copy, QP choice, batching, emulation — sits *below* the
boundary where the backend is free. Two properties of the API happen to fit
RoCE unusually well:

- **`OSOE_TP_FENCED` as a per-operation flag** rather than ordered-by-default
  delivery. On one RC/XRC QP per `(ctx, pe)`, RoCE places writes in issue order
  and orders a following atomic behind them, so the flag is free and
  `osoe_tp_fence` is an `mfence`. That makes `put_signal` one WQE cheaper than
  on CXI, which must enable a fence capability and lose PCIe relaxed ordering
  for all RMA on the endpoint (CORE §4.2). Where the fabric sprays, the flag is
  also the *only* thing that has to be paid for — the rest of the traffic keeps
  multipathing.
- **`(pe, region, offset)` addressing.** Verbs wants a remote virtual address
  and an rkey, which looks like a mismatch. §3 shows it collapses to 16 bytes
  *per node* — not per PE — because osoe already maps every local PE's heap into
  every local process.

The weak spots are AMO throughput, blocking waits, and collectives.

**(c) Left on the table.** Split by cause, with the estimates from §8:

| Cause | Worst item | Estimated cost |
|---|---|---|
| Boundary lacks a batching hint | doorbell per small put | **1.5–3×** small-put message rate |
| Boundary lacks `set_range` | every blocked wait polls | **a core per waiter**, or a shared core per node for +~3 µs wake |
| Collectives above the boundary | no SHARP barrier | **~4–6×** on barrier at 10^6 PEs |
| Phase 1 has no `get_nbi` | no read pipelining | **up to ~10×** on gather loops (recovered when §7.3 lands) |
| RoCE has no target counters | blocking wait is a spin | the same core; see above |
| RoCE atomics are not CPU-coherent | on-node AMO via NIC loopback | **~100×** on that operation, weighted by on-node AMO fraction |
| RoCE atomics are 8-byte only | `int` AMOs need DevX or a CAS loop | **1× or ≥2 RTT**, depending on DevX |
| RoCE has no non-fetching atomic | a response packet per `shmem_atomic_inc` | ~2× AMO wire traffic |

The honest summary: **against a hand-written RoCE-native OpenSHMEM, the boundary
costs little on put/get/fence/quiet and a lot on collectives and blocked waits.
Against a semantically richer NIC (Portals, CXI, UET), RoCE itself costs more
than the boundary does.**

---

## 2. What RoCE is, in the boundary's terms

Read the last row of CORE §4.2 (osoe AM over TCP) and a hypothetical RoCE row
side by side. The two backends are near mirror images:

| Column | osoe AM/TCP | RoCE (proposed) |
|---|---|---|
| A put/get | **E** — AM handler, target CPU | **N** — RDMA write/read |
| B word inline | C | **N** — `IBV_SEND_INLINE` |
| C keyless segments | **N** — `(region, offset)` on the wire | **C** — 16 B per *node*, §3.2 |
| D required rmw, exactly once | C — CPU atomic in handler | **N** — 8 B FA/CAS; **E** for 4 B without DevX |
| E optional rmw ops | C | **E** — CAS retry loop |
| F fence | **N** — single ordered stream | **N** — same-QP ordering; **E** if spraying |
| G counter completion | C | **N** — CQ, selective signalling |
| H notify (blocking) | **C** — watch table sees every write | **E** — no target counters; poll |
| I per-peer state | **N** — per node pair | **C** — XRC: one QP per node pair; DC: ~0 |
| J cheap context | **N** | **C** — a QP set per context |
| K no target CPU for A and D | **E always** | **N** |

The interesting line is **K**. osoe's AM backend needs the target's CPU for
everything, which is what forces the §3.12 per-node progress pool, the landing
zones, and the DS B1 problem of a progress thread in another process being
unable to write the origin PE's stack. **A RoCE backend needs the target CPU for
nothing on the data path.** Puts, gets and atomics all complete without the
target being scheduled. That removes a whole architecture — and replaces it with
one narrower problem, notification, treated in §4.7.

The second interesting line is **H**, which is the mirror: osoe's watch table is
sound because every write passes through a CPU that can bump a sequence word;
RoCE's writes are invisible to the target CPU, so the mechanism has nothing to
hang on.

---

## 3. Resource and addressing plan

This is where most of the design effort goes, and where osoe's existing
structure pays off unexpectedly well.

### 3.1 One memory region per node

`node.h` gives each PE its own named heap object, and every local process maps
every sibling's heap (§4.1). A verbs MR is per process and per protection
domain, so the naive plan is one MR per process over its own heap: PPN MRs per
node, PPN × heap_pages MTT entries per NIC, and a per-PE `{base, rkey}` table.

Better: **the node lead maps all `L` local heap objects a second time into one
contiguous reservation**, used only as something to register. Same physical
pages; `B` and `base_of` are untouched. The lead registers that reservation
once:

*(Corrected. This section first said to reserve `L × heap_size` per node and
`MAP_FIXED` each local heap into its slot, so that the heaps were contiguous in
**every** local process. That is wrong: it puts a PE's own heap at
`node_base + local_id × heap_size`, which differs per PE, destroying §4.1's
property that a PE's own heap sits at the same base `B` in every process — the
property §4.3 needs for symmetric statics under `-no-pie`. The second mapping
costs `L × heap_size` of address space in the lead alone and gives the same
contiguity for registration. Design §4.1 records the trap.)*

```
remote_addr = node_base[node_of(pe)] + local_id(pe) * heap_size + offset
rkey        = node_rkey[node_of(pe)]
```

One MR per node, one rkey per node, and a per-*node* table of
`{uint64_t base; uint32_t rkey;}` — 16 bytes, padded. At 10^6 PEs and 128 PPN
that is 7812 nodes × 16 B = **125 KB per process** (*arith*), against 16 MB per
process for a per-PE table, or 32 MB with `OSOE_REGION_INTERNAL` included. Even
unshared it fits in cache-friendly memory; put it in the node segment and it is
read-mostly node-shared as CORE §2.8 requires.

This **preserves invariant 7**. The wire still carries no assumption about
identical virtual addresses: the VA comes out of a per-node table filled at
bootstrap, exactly as the rkey does. Nothing requires two nodes to agree on
where anything is mapped.

`OSOE_REGION_INTERNAL` is a second MR per node over the node control segment,
which the watch slots and §5.8 collective control words already live in.

**Caveat.** Whether one process's MR can be targeted by traffic arriving on
another process's QP depends on the PD check at the responder. With XRC the
target side is an XRC target QP created on an `ibv_xrcd`, and an XRCD is
explicitly shareable between processes on a node through a file descriptor. That
is the mechanism this plan assumes. It is **the load-bearing unverified
assumption of the whole design** — see §10 V1. If it does not hold, fall back to
one MR per process per heap and a per-PE table, at 16 MB per node and no change
to anything above §3.

### 3.2 Queue pairs

RC per peer is dead at scale: CORE §2.8 collects the evidence (IBGDA 75 GiB at
10^5, GPI-2 10 QPs per peer, Open MPI/UCX 50–100 MB per context). Two survivable
choices, both of which the boundary hides completely:

- **XRC.** One XRC INI QP per `(context, target node)`. At 7812 nodes that is
  7812 QPs per process per context — large but bounded, and it matches osoe's
  existing *one connection per node pair* structure (§3.1, invariant 3) almost
  one to one. The node-pair shape that made free fence work over TCP is the same
  shape that makes ordering free here.
- **DC.** A pool of k DCI QPs per context (k ≈ 8–32) with LRU target affinity,
  zero steady-state per-peer state. The risk is DCI renegotiation: switching a
  DCI to a new target costs a handshake, which is the latency cliff CORE §4.2
  flags for IBGDA under all-to-all. All-to-all is exactly what ISx, Graph500 and
  bale do (CORE USE §2).

**Recommendation: XRC for the data path, with DC behind a build-time switch to
measure the renegotiation cliff on real hardware.** UD for bootstrap and control
so that connection setup does not need connections.

Per context: one QP set, one send CQ, one receive CQ. Under `SERIALIZED` the
issue path needs no lock. A future `MULTIPLE` (§4.11) gets lock-free issue for
free by giving `SHMEM_CTX_PRIVATE` contexts their own QPs and doorbells, which
is CORE §2.8's argument for keeping `PRIVATE` and `SERIALIZED` distinct.

### 3.3 Registration

One MR per node over the heap range, with
`LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ | REMOTE_ATOMIC`. Whether
`REMOTE_ATOMIC` costs plain RDMA anything on mlx5 is CORE §7.1 A8 / §7.2 C4,
still open; if it does, it becomes the first customer for the `set_range` hook
of §9.2. Pin at init or use implicit ODP, with huge pages so the MTT stays
small. Bounce buffers and blocking-get landing buffers are one small
pre-registered slab per PE.

---

## 4. Operation by operation

### 4.1 `osoe_tp_put`

Three size classes, chosen inside the backend:

| Size | Path | Why |
|---|---|---|
| ≤ inline max (~64–200 B) | `IBV_SEND_INLINE`, unsignalled | local completion at post; no copy, no CQE |
| ≤ bounce threshold (~16–32 KB) | copy into the pre-registered slab, unsignalled WQE | one memcpy is cheaper than waiting for a send CQE |
| larger | zero-copy from the caller's buffer, signalled, wait for the CQE | the copy would cost more than the wait |

`osoe_tp_put` promises to return at local completion — "src may be reused at
once" — which is what forces this. It is the same choice SOS makes (CORE §2.2),
and the spec permits either (CORE SEM §2.3).

Note that the first two classes make the source buffer the backend's, which
means **small puts may legally be deferred** and flushed with one doorbell
later. That is the batching opportunity of §8.1, and its hazard.

On-node targets keep the intranode `memcpy` path; `osoe_tp_direct` still returns
non-NULL for them, because the backend's on-node writes still apply in the
calling thread. Nothing about RoCE forces the intranode floor (§8.4) to
regress.

### 4.2 `osoe_tp_get`

RDMA read, then spin on the send CQ. Into symmetric memory it is zero-copy.
Into private memory — a stack buffer — the destination is unregistered, so it
reads into the per-PE slab and copies out. That is exactly what the AM backend
already does with its landing zone (§3.12), so there is **no regression**, and
unlike the AM backend there is no cross-process problem to solve (DS B1): the
NIC writes into the calling process's own memory.

Blocking and complete per operation, as the header requires. Phase 1 has no
`get_nbi`, so there is no read pipelining; see §8.3.

### 4.3 `osoe_tp_rmw`

| Case | Path |
|---|---|
| 8 B ADD, with or without fetch | `IBV_WR_ATOMIC_FETCH_AND_ADD` |
| 8 B CSWAP | `IBV_WR_ATOMIC_CMP_AND_SWP` |
| 8 B WRITE (swap) | masked CAS via DevX, or CAS retry loop |
| 8 B READ | zero-length or 8-byte RDMA read, **not** fetch-add of 0 (CORE §2.4: a zero add counts as a write on CXI/UET and would do so on any counter-bearing NIC) |
| 4 B anything | masked FA / masked CAS via DevX, else a CAS loop on the containing 8-byte word |
| AND / OR / XOR | CAS retry loop; ≥2 RTT, livelock-prone (CORE §6) |

Three RoCE facts shape this, all *recalled*:

- **There is no non-fetching atomic.** The response packet carrying the old
  value is sent whether or not anyone wants it. `OSOE_TP_FETCH` therefore
  controls only whether the *caller* waits — post unsignalled and return, which
  is precisely what the header allows ("an rmw may return before it executes").
  The wire cost is paid regardless.
- **Standard atomics are 8 bytes only.** 4-byte AMOs — `int`, `int32_t`,
  `float` — need mlx5 masked atomics, which CORE §7.2 C15 reconciles as
  reachable only through DevX/PRM. Without DevX the fallback is a CAS loop on
  the containing 8-byte word, which is correct only if the *other* four bytes
  are not concurrently written by a non-atomic path; the spec makes mixing
  AMO and store on a word undefined, so it is legal, but it is two round trips
  and it livelocks under contention.
- **Atomics are not coherent with CPU atomics.** mlx5 reports device-scope
  atomicity, not global scope, so a NIC fetch-add and a CPU `lock xadd` on the
  same word are not mutually exclusive.

That last fact decides the on-node case. `osoe_tp_rmw`'s contract is exclusivity
against every same-width rmw "issued through contexts in the same atomicity
domain — the domain of ctx's team". For a default context on `TEAM_WORLD` that
domain contains the whole job, so an on-node PE's rmw must be exclusive against
a remote PE's NIC rmw on the same word, and the only way to get that is **NIC
loopback for on-node atomics**. IBGDA does the same (CORE §2.4, NV §4).

The boundary anticipated this. §4.10's "Atomicity is the spec's, not osoe's"
paragraph exists precisely so a backend "whose NIC atomics are not coherent with
the CPU can then run one domain on the NIC and another on the CPU, instead of
pushing every on-node rmw through NIC loopback". So a RoCE backend may execute
`SHMEM_TEAM_SHARED`-derived contexts with CPU atomics and `TEAM_WORLD` contexts
on the NIC. That is real, spec-legal headroom — but it only helps applications
that actually create team-shared contexts, and CORE §3(b) records no users of
`team_create_ctx`. In practice, **plan for loopback**.

### 4.4 `osoe_tp_load`

An acquire load of the PE's own word. The concern the header raises — "a backend
whose NIC applies rmw out of the CPU's sight synchronizes here" — is about
*atomicity*, not visibility: a NIC atomic's writeback is a coherent DMA, so an
aligned 8-byte load will not tear and will not be stale on the reference
platform. No synchronization needed; the signature's lack of a PE or context
argument costs nothing here. It would bite on a platform with genuinely
non-coherent NIC writeback, where the backend has no hook to flush.

### 4.5 `osoe_tp_fence` and `OSOE_TP_FENCED`

**Free, if we do not spray.** Within one RC or XRC QP, RoCE places writes at the
responder in issue order, and an atomic issued after them is ordered behind
them. So with one QP per `(ctx, target node)`:

- `OSOE_TP_FENCED` is a no-op flag — nothing is added to the WQE.
- `osoe_tp_fence` is one serializing store for the §4.6 memory-store rule and
  nothing else.
- `put_signal` is a write WQE followed by an atomic WQE, with invariant 5
  holding by the transport's own ordering. Two WQEs, no drain, no round trip.

This puts the backend in CORE §2.5's tier 1 (`OC_CAP_ORDERED`), alongside
Portals-with-total-data-ordering and osoe's single TCP stream, and ahead of CXI,
which reaches tier 2 only by enabling a fence capability that costs PCIe relaxed
ordering for every RMA on the endpoint.

**The trade is the same one UET makes between RUD and ROD** (CORE §4.3): order
and packet spraying are exclusive. Multipathing a QP across paths, or striping a
context across several QPs per peer, breaks placement order and drops the
backend to tier 3 — drain before posting a fenced operation. Because
`OSOE_TP_FENCED` is per operation, a sprayed backend pays only on fenced
operations and keeps multipathing for the rest. **An ordered-by-default boundary
would have forfeited that, and §4.10 explicitly rejected it.** This is the
clearest case of the boundary's weak contract being worth something on real
hardware.

Two caveats, both to verify (§10 V2, V3): whether PCIe relaxed ordering on the
MR weakens the write-then-atomic placement order at the responder, and whether
mlx5's out-of-order data placement mode does.

### 4.6 `osoe_tp_wait`

1. Serializing store (§4.6 rule 2).
2. Reap the send CQ until every WQE issued on this context to the covered peers
   has completed. Selective signalling — sign every Nth WQE — keeps this to one
   poll rather than one CQE per put.
3. Unless transmit completion implies visibility, post a zero-length RDMA read
   to each dirty peer, then poll for all of them.

Step 3 is CORE §2.6's dirty-set flush and the cost is the same. One nuance the
core study understates: the flush reads can all be **posted first and polled
after**, so k dirty peers cost one round trip plus k issue costs, not k round
trips. At k = 1 — the shape of Graph500 and GUPs, which quiet after every
operation (CORE USE §2.1–2.2) — that is ~2 µs. After an all-to-all, k = 7812
nodes and it is ~1.2 ms (*arith*, at 150 ns issue).

Step 3 disappears entirely if a write CQE implies the data is globally visible,
which on RoCE plausibly holds when relaxed ordering is off. **That is a direct
trade: turn off relaxed ordering and quiet becomes free, at some large-message
bandwidth.** Both halves need measuring (§10 V3).

`osoe_tp_wait`'s lack of a deadline argument is fine — the abort-word check
belongs in the poll loop, per §4.10's error paragraph.

### 4.7 Notification — the real problem

RoCE has **no target-side counters**. A remote write or atomic generates nothing
the target CPU can observe. Every other substrate in CORE §4.2's column H has
something: Portals target CTs, CXI `FI_RMA_EVENT`, UET remote-write counters,
uGNI `POST_CQWRITE`. Verbs has only `WRITE_WITH_IMM` / `SEND_WITH_IMM`, which
produce a receive CQE and require a posted receive.

So `osoe_tp_notify` — a writer-side call made after applying a write — has no
caller in a RoCE backend. The header foresaw half of this: "a backend whose puts
the CPU never sees (RDMA) need not notify for them". It also says notify is
**exact for rmw**, and that "for rmw it bridges its NIC events into this call" —
but on RoCE there are no NIC events at the target to bridge.

Four ways out, in increasing order of merit:

1. **Initiator-side notify.** Follow every rmw with a `WRITE_WITH_IMM` to a
   doorbell, producing a target CQE. Correct and exact, but it doubles the WQE
   count on every AMO and needs a target thread on the receive CQ. On a NIC
   whose atomic rate is already the bottleneck, this is the wrong direction.
   Rejected unless a `set_range` hook (§9.2) tells us *which* words are watched,
   at which point it becomes attractive: signal words are rare and hot.
2. **Route watched words through AM.** Correct, needs the target CPU, and gives
   up the whole point of RoCE for those words.
3. **Waiter spins.** `osoe_tp_watch_seq` returns a constant, `osoe_tp_watch_wait`
   spins with `pause` until the caller's deadline and returns false. The §4.9
   loop re-checks the predicate every iteration, so this is **correct with no
   change above the boundary** — the protocol's tolerance of spurious and
   missing wakes is doing exactly the job it was designed for. Costs a core per
   blocked waiter. This is what SOS, OSSS-UCX, GPI-2 and NVSHMEM all do
   (CORE §2.7).
4. **A per-node wake poller.** One thread per node keeps a shadow copy of each
   local PE's watched block (`watch.c` already rounds the watch to an aligned
   power-of-two block and publishes it in the node segment), polls the live
   blocks, and on a difference bumps that PE's `watch_seq` and futex-wakes it —
   i.e. it calls `osoe_tp_notify` exactly as the AM progress thread does today.
   `watch.c` and `wait.c` need **no change at all**.

**Recommendation: (4), falling back to (3) for large watched blocks.** It is the
RoCE analogue of §3.12's per-node progress pool, and it converts "a core per
waiter" into "a shared core per node, plus a poll period and a futex wake" —
call it 2–5 µs of added wake latency (*recalled*). At 128 PPN that is a 128×
reduction in burned cores. The common case is an 8-byte ivar, so the poller
compares L ≤ 128 words per sweep, which is a few cache lines.

This is also the one place where the boundary's shape genuinely helps a backend
it was not written for: because notification is expressed as *the waiter
publishes a range, some writer bumps a sequence*, and not as *the network
delivers an event*, a polling implementation slots in without the SHMEM layer
knowing.

---

## 5. Build plan

Roughly in dependency order. Each step is testable against the existing
intranode backend using §8.1's logical node map and force-remote flag, which
already makes "same machine, real network path" a first-class configuration —
the single most useful thing this repository has for bringing up a new backend.

1. **`transport_roce.c` skeleton + bootstrap.** Device open, PD, CQ, XRCD, the
   per-node `{base, rkey}` table exchanged through §6.2's rendezvous. No data
   path. Success criterion: `shmem_init`/`finalize` on two nodes.
2. **Contiguous node heap mapping.** In the node lead only, map the `L` local
   heap objects a second time into one contiguous reservation and register it
   as one MR. Touches `node.c` and `§4.1`, and is the only change *outside* the
   backend that this plan requires. **Not** by slotting each heap by local id,
   which would move `B` — see §3.1.
3. **Put and get.** Three size classes, dispatch on `osoe_tp_direct` for
   on-node. Differential test against the intranode backend with force-remote.
4. **Fence and wait.** Selective signalling, dirty set of nodes (reuse the AM
   backend's structure — it is already per node, §4.6), flush read behind a
   build flag so the relaxed-ordering trade can be measured both ways.
5. **rmw.** 8-byte native first; then on-node loopback; then 4-byte and bitwise
   emulation; then DevX masked atomics behind a build flag.
6. **Wake poller.** Per-node thread, shadow compare, `osoe_tp_notify`. Spin
   fallback in `osoe_tp_watch_wait` for oversized blocks.
7. **XRC or DC switch, and multi-QP striping** — the two things that must be
   measured rather than reasoned about.

Steps 1–4 are a usable backend for everything in CORE §3(b) marked **fast**
except AMOs. Step 5 completes the hot set.

---

## 6. (a) Feature utilization, in detail

### Reached, and hidden entirely inside the backend

RDMA write / read; inline sends for `shmem_p` and word puts; 8-byte fetch-add
and compare-swap; same-QP ordering as free fence; selective signalling as
counter completion; CQ moderation and batched polling; XRC, DC and SRQ for
scalability; shared XRCD for one MR per node; scatter-gather lists; on-demand
paging; relaxed ordering; huge-page MRs; QP error state to job abort (§3.15
already wants abort, so RoCE's fail-fast RC semantics match).

Congestion control and adaptive routing are orthogonal to the API and fully
available, with the ordering trade of §4.5.

### Reached only partially

- **Doorbell batching / postlist.** The backend can defer small puts, because
  it owns the source bytes once they are inlined or bounced. But nothing in the
  API says whether more is coming, so it must guess a flush trigger. §8.1.
- **`WRITE_WITH_IMM` for exact notification.** Available, but without knowing
  which ranges are watched, using it means paying on every rmw. §9.2.
- **GPUDirect RDMA.** A GPU *buffer* can be registered and targeted — the
  boundary is address-agnostic, so this works. GPU-*initiated* operations do
  not: `transport.h` is a host-side blocking C API. CORE §7.2 C8 records the
  same limit for the core.

### Not reachable through this boundary

- **SHARP in-network reduction and barrier.** §4.10 keeps collectives above the
  boundary and builds them from put, rmw and wait. A backend cannot intercept
  `shmem_barrier_all`. CORE §1.1 makes the identical choice, so this is a
  considered cost, not an oversight — but it is the largest one. §8.4.
- **UD multicast for `shmem_broadcast`.** Same reason. Lower impact: broadcast
  is COLD in CORE §3(b).
- **Triggered operations / cross-channel chaining.** Offloading a whole
  collective schedule to the NIC needs operation *chaining*, which the boundary
  has no vocabulary for. Same reason, same fix.

### Not reachable because RoCE lacks it

Target-side counters; 4-byte and bitwise atomics in stock verbs; non-fetching
atomics; symmetric rkeys; CPU-coherent atomics; MIN/MAX atomics (CORE §7.2 C9
wants these for Graph500 and RoCE has none either).

---

## 7. (b) Performance

### 7.1 What the boundary costs

Approximately nothing on the data path. §4.10 binds backends at build time with
no function-pointer table, so `osoe_tp_put` of eight bytes compiles to the
backend's inline WQE construction. There is no per-operation dispatch, no
capability branch (§4.10 rejected capability bits precisely to avoid a branch
per operation), and no allocation. The arguments are already in the form the
backend wants: a rank, a small region enum, and an offset — the three things the
address translation of §3.1 needs, with no pointer chase to undo.

Two small frictions:

- **No `put_word`.** CORE §1.3 makes `oc_put_word` a data-path primitive
  because `shmem_p` is the hottest pattern in its corpus. Here the SHMEM layer
  must materialize the value in memory and pass `(&v, 8)`, and the backend
  branches on length. A spill and a compare; call it single-digit nanoseconds,
  and a good compiler removes most of it. Not worth a header change on its own.
- **`osoe_tp_get` is blocking.** Correct for `shmem_get`, and phase 1 has
  nothing else. §8.3.

### 7.2 Cost model

Order-of-magnitude, ***recalled***, ±50%, ConnectX-class at 200 GbE:

| Operation | Path | Issue cost | Time to effect |
|---|---|---|---|
| put 8 B, unfenced | inline WQE, unsignalled | ~120–200 ns | ~1.5–1.9 µs to target memory |
| put 8 KB | bounce copy + WQE | ~0.6 µs (the copy) | ~2 µs |
| put 1 MB | zero-copy, signalled | ~0.2 µs | ~42 µs (*arith*, 25 GB/s) |
| put 8 B on-node | `memcpy` | ~20 ns | applied before return |
| get 8 B | RDMA read + CQ spin | — | ~1.7–2.2 µs |
| rmw fetch-add 8 B | atomic + CQ spin | — | ~2.0–2.8 µs |
| rmw add 8 B, no fetch | atomic, unsignalled | ~150 ns | fire and forget |
| rmw 4 B via CAS loop | 2+ round trips | — | ≥4–6 µs, unbounded under contention |
| rmw on-node (WORLD domain) | NIC loopback | — | ~2 µs, against ~20 ns for the CPU atomic |
| fence | serializing store | ~20 ns | 0 |
| wait, 0 dirty peers | CQ check | ~50 ns | 0 |
| wait, 1 dirty peer | drain + flush read | — | ~2 µs |
| wait, k dirty peers | post k, poll k | k × ~150 ns | ~2 µs + k × 150 ns |
| load | acquire load | ~1–20 ns | — |
| watch wait, poller | shadow compare + futex wake | shared core/node | +2–5 µs after arrival |
| watch wait, spin | caller spins | a core | +50–200 ns after arrival |

### 7.3 Where this lands against the §1 performance floor

The three floors are an io_uring TCP echo, a streaming harness, and intranode
`memcpy`. A RoCE backend should beat the echo floor by roughly an order of
magnitude on the 8-byte round trip (~2 µs against ~20–30 µs for a TCP echo
through the kernel, *recalled*), reach line rate on large messages at a lower
CPU cost than the streaming harness, and **tie exactly** on the intranode floor,
because it keeps the same `memcpy` path. That last one matters: the RoCE backend
should not regress the case the project can already measure.

---

## 8. (c) What is left on the table

Two baselines, kept separate because only the first is actionable here.

### 8.1 Boundary-imposed: no batching hint

**Mechanism.** Every `osoe_tp_put` is a `post_send` and a doorbell. mlx5 can
post a list of WQEs with one doorbell, which is the difference between a
doorbell-bound and a WQE-bound issue path.

**Cost.** Estimated **1.5–3×** on small-put message rate for codes that issue
runs of puts — strip-mined all-to-all, bale's `lgp_*`, halo exchanges. Not a
latency cost; a rate cost.

**Partial recovery without an API change.** Because the backend owns the source
bytes for inline and bounced puts, it may defer them and ring one doorbell at
the next fence, wait, get, or rmw. Those boundaries are frequent in real codes —
CORE USE §2.1–2.2 finds Graph500 and GUPs quiet after *every* operation. The
hazard is a lone put with nothing following it, which would sit in the queue
indefinitely; a bounded batch count and a deferral watchdog bound it, at the
cost of one timer and a latency tail.

**Full recovery.** CORE §1.2's `OC_F_MORE`, fed by 1.6 sessions
(`shmem_ctx_session_start/stop` with a BATCH hint). §9.1.

### 8.2 Boundary-imposed: no `set_range`

**Mechanism.** The backend cannot learn which heap ranges carry signals or
atomics. Consequences: it must register the whole heap `REMOTE_ATOMIC` (free on
mlx5 or not — CORE §7.2 C4 is open); and it cannot afford exact notification,
because it would have to notify on every rmw rather than on the few that are
watched.

**Cost.** Every blocked wait polls. With the per-node wake poller of §4.7 that
is one core per node and +2–5 µs of wake latency; without it, one core per
waiter. For a code that blocks a thread per PE, one core per node out of 128 is
a **~0.8% throughput tax** (*arith*); the per-waiter version is a ~100% tax on
any PE that blocks.

**Recovery.** §4.10 already names the hook — "a later hook,
`set_range(region, offset, len, attributes)`, may tell a backend which ranges
carry atomics or signals" — and §4.4's allocator already grows hinted
allocations from the opposite end of the heap. With it, signal ranges get
initiator-side `WRITE_WITH_IMM` notification and genuine blocking waits, and the
poller disappears. **This is the highest-value change in this document.** §9.2.

### 8.3 Phase-imposed: no `get_nbi`

A `shmem_get` loop is one round trip per element, ~2 µs each. With `get_nbi`
the backend posts N reads and polls once: **up to ~10×** on gather-heavy loops
(*arith*, from ~2 µs serialized against ~0.2 µs issue-bound). This is a phase
boundary, not a design one — §7.3 already plans the call, and the header says
so. Nothing to fix; worth knowing that the gap is this large before anyone
benchmarks phase 1 against a mature implementation.

### 8.4 Boundary-imposed: collectives above the line

**Mechanism.** §4.10 keeps collectives above the boundary and builds them from
put, rmw and wait. SHARP, UD multicast, and triggered-operation chaining all
require the backend to see `shmem_barrier_all` as a barrier.

**Cost, barrier at 10^6 PEs** (*arith*, on §5.1's two-level structure):
intranode phase on node counters, then a tree over 7812 nodes ≈ 13 levels at
~1.8 µs ≈ **23 µs**, against a plausible ~4–6 µs for an in-network barrier.
Roughly **4–6×**. Barrier is the most-called collective in CORE USE §1, and bale
issues two per collective.

**Superseded on the remedy, not on the number.** This section first concluded
that keeping collectives above the boundary was still right, because a
capability-bearing collective interface would be a second boundary tested only on
hardware that has the feature. That reasoning holds for a *capability-bearing*
interface and is why negotiated collective offload stays rejected. It does not
hold for a mandatory operation with a generic fallback — an `osoe_tp_sync` over a
group of nodes, recorded as an open question in design §10 and deferred to §7.1
step 2, which writes its only caller.

What the follow-up did *not* find is a contradiction in the design. §2.2 scopes
the boundary claim to §4 and puts collectives on the AM layer deliberately, so
§5 calling AM is a decision, not an oversight. The case for a `sync` hook is
therefore the number below and nothing else: §2.2's decision was made when AM
was the only backend, and in an RDMA build it sends every barrier over TCP while
the data path runs on the NIC.

### 8.5 RoCE-imposed

| Item | Cost | Note |
|---|---|---|
| No target counters | a core per node (best case) | §4.7; Portals/CXI/UET all have them |
| Atomics not CPU-coherent | on-node rmw ~2 µs against ~20 ns | weighted by on-node AMO fraction; escape hatch in §4.3 |
| 8-byte atomics only | DevX, or ≥2 RTT and livelock risk | affects `int` and `float` AMOs |
| No non-fetching atomic | ~2× AMO wire traffic | `shmem_atomic_inc` still pays a response |
| NIC atomic rate ≪ write rate | unquantified; possibly the dominant limit on GUPs | **§10 V4 — measure first** |
| No symmetric rkeys | 125 KB/node table | mitigated to near-nothing by §3.1 |
| RC does not scale | XRC ~7812 QPs/process, or DC renegotiation | §3.2 |
| Order vs spraying | fenced ops only | §4.5; the per-op flag limits the blast radius |
| Lossless-fabric operations | PFC/DCQCN tuning, congestion collapse modes | a real cost of choosing RoCE, outside this API |

---

## 9. Changes to `transport.h` that would buy the most back

Ranked by value per line of interface. None is needed to *build* the backend.

*(Both of the changes recommended below were taken. Design §4.10 now specifies
`OSOE_TP_MORE` — with a rule this document did not state, that a backend may
defer an operation only if it owns a copy of the source bytes, since a large
zero-copy put's `src` may be reused the moment the call returns — and
`set_range`, with its call site scheduled into §7.1 step 3. Nothing is declared
in the header until there is a backend to implement it.)*

### 9.1 A batching hint

An `OSOE_TP_MORE` flag on put and rmw, or an explicit
`osoe_tp_flush(ctx)`. Recovers §8.1 without the deferral watchdog, and gives 1.6
sessions something to drive. Cost: one flag, one branch, ignored by the
intranode backend.

### 9.2 `osoe_tp_set_range(region, offset, len, attributes)`

Already named in §4.10 as a later hook. Attributes: carries atomics, carries
signals. Recovers §8.2 — exact notification and real blocking waits on RoCE, and
selective `REMOTE_ATOMIC` registration. Collective, cold, called from the
allocator. **Do this one.**

### 9.3 Nothing else

Specifically *not* recommended:

- **Capability bits.** §4.10 rejected them and this exercise supports the
  rejection. Every place a RoCE backend differs, it differed *underneath* the
  boundary: emulation, poller, loopback, size classes. The one case where the
  SHMEM layer might have wanted to know — whether to block or poll — is handled
  by `osoe_tp_watch_wait` being the backend's own function.
- **`put_word`.** Measurable only in a microbenchmark of the boundary itself.
- **A collective hook** — ***superseded***. Rejecting it here assumed a
  capability-bearing interface. A mandatory `sync` over a node group, with a
  generic tree as the fallback, carries no capability. It rests on §8.4's number
  alone — unmeasured, and at a job size four orders of magnitude beyond the
  target hardware — so design §10 records it as open and §7.1 step 2 decides it
  with the internode phase in hand.

---

## 10. Verification list

Ordered by how much of the plan rests on each. Nothing below has been measured
or read in source for this document.

- **V1 — XRC cross-process rkey validation.** Can a remote initiator target an
  MR registered by the node lead, arriving on an XRC target QP on a shared
  XRCD, when the MR's PD belongs to the lead? §3.1 depends on it. Fallback
  costs 16 MB/node and no redesign.
- **V2 — write-then-atomic placement order.** Does RoCE guarantee that an
  atomic issued after a write on the same QP is applied after the write lands
  at the responder? Invariant 5 and free `put_signal` depend on it.
- **V3 — relaxed ordering and visibility.** Does PCIe relaxed ordering on the
  MR break V2, and does a write CQE imply global visibility with RO off? This
  decides whether `osoe_tp_wait` needs a flush read at all. Same question CORE
  §7.2 C13 asks of CXI.
- **V4 — NIC atomic throughput.** Atomic operations per second per NIC against
  RDMA write operations per second. Possibly the single dominant number for
  AMO-heavy codes, and the one this document is least able to guess.
- **V5 — mlx5 masked atomics through DevX.** Are 4-byte masked FA and masked
  CAS actually reachable, and at what rate? CORE §7.2 C15's reconciled reading
  says yes through DevX/PRM only.
- **V6 — `REMOTE_ATOMIC` registration cost.** CORE §7.1 A8 / §7.2 C4, still
  open: does atomic-capable registration slow plain RDMA on mlx5?
- **V7 — DC renegotiation.** Cost of switching a DCI to a new target, and the
  resulting all-to-all latency distribution. Decides XRC against DC.
- **V8 — out-of-order data placement.** Does mlx5's OOO mode weaken same-QP
  placement order, i.e. does spraying cost the free fence outright? §4.5.
