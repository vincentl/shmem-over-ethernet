# Is §4.10 the seam an RoCE port needs?

Second adversarial review, question 4. Reviewed against `main` at `7ff9343`
(slice merge `d542c63`), 2026-09-20. Judged at **4,096 nodes × 256 PEs ≈ 1M PEs**,
under the author's sharper goal: *the API and internal interfaces must never block
the use of RoCE features*.

Evidence labels: *documented* (a specification, paper, or vendor page read today),
*observed in source* (this tree, or upstream source fetched today — see
`evidence/phase1-step2-slice1/roce-seam/sources.md`), *measured* (run in the VM,
under contention from other reviewers), *recalled* (memory; unverified), *arith*
(my arithmetic on stated inputs). No RoCE hardware was available; nothing here is
a hardware measurement.

Supporting files, all under `docs/reviews/evidence/phase1-step2-slice1/roce-seam/`:
`include-graph.sh` and `include-graph.txt`, `force-remote-data-path.log`,
`sources.md`.

---

## 1. Verdict

**The data-path half of §4.10 is the right seam, and it is better than the first
round could show. The half that is not data path — collectives, lifecycle,
progress, bootstrap — is not a seam yet, and the slice closed the one question
that would have made it one for reasons that do not survive reading the code it
was closed against.**

What holds. The six §4 files that carry RMA, AMOs, signals, quiet and waits
(`rma.c`, `amo.c`, `signal.c`, `quiet.c`, `wait.c`, `malloc.c`) call nothing
below the boundary except `transport.h` (*observed in source*, §3 below). The
contract's weak ordering, per-operation `FENCED`, counters-not-handles,
`set_range`, and the no-capability-bits rule each map onto verbs without
friction, and I found **no RoCE data-path feature the header blocks**: DC or XRC,
on-demand paging, selective completion signalling, doorbell batching, multi-rail
striping and relaxed ordering all stay the backend's private choice. Checking the
first round's *recalled* hardware facts against upstream source made the picture
better, not worse: stock verbs on mlx5 does carry the two 8-byte atomics; mlx5's
extended atomics execute **every** spec AMO, at 4 and 8 bytes, in one work
request; and mlx5 has a PCIe-AtomicOp mode in which NIC atomics are CPU-coherent
(§6, F3).

What does not hold, in order of cost at 1M PEs:

1. **Collectives.** `osoe_transport_sync` was closed with two reasons — a second
   addressing model, and the futex release — that are properties of the
   *proposal's shape* (a blocking call over a node group, release as an rmw), not
   of the problem. The code already contains the seam the closure says cannot
   exist: `osoe_progress_host_post_arrival(team, epoch)` going down and
   `release(team, epoch)` coming up, addressed by *team*, which the SHMEM layer
   already owns. It simply lives in an AM-named header. And §10 prices the
   decision with the wrong number: "4–6×" is an RDMA tree against SHARP, whereas
   what "no" actually leaves an RoCE build with is a **flat star over TCP** beside
   an RDMA data path — about **500×** an RDMA tree at 4,096 nodes as coded, about
   12× with an AM tree (*arith*, F1). SHARP itself is not the loss: it does not
   exist on RoCE switches (*documented*).
2. **State that grows with PEs per PE.** `local_of_rank` is a private
   `world_size × 4 B` array in every process: 4 MiB per PE, **1 GiB per node** at
   1M PEs, and it still cannot answer what an RDMA backend needs (a *remote* PE's
   index on its node) (F7).
3. **Lifecycle and progress are not behind the boundary.** `init.c` and `agent.c`
   call the AM engine by name. §10's closed "a boundary progress call is not
   needed" is true of application threads and false of the agent thread (F5).

The counterfactual the prompt asks for — what a porter rewrites that the design
promises they would not — is §3.3 below: nothing in §4 proper except one
invariant counter, but `barrier.c`, `init.c`, `agent.c`, `job.c`, `node.c` and the
build all need edits, none large, all cheap to pre-empt now and more expensive
after §5.5's reductions are written against AM reply queues.

**Recommendation:** keep `transport.h`'s data operations exactly as they are.
Before step 3, (a) move the arrival/release pair into the boundary as a
team-addressed group interface with the present AM code as its generic
implementation (no capability bit, no node addressing above the seam — F1);
(b) let `osoe_transport_init/fini` own backend start-up and give the agent loop a
boundary-named poll (F5); (c) replace `local_of_rank` (F7); (d) record the
4-byte-emulation trap beside §4.1's registration trap (F2).

---

## 2. `transport.h` as a contract: what an RoCE backend does for each function

Reference platform as in the first round: RoCEv2, ConnectX-class, verbs plus
mlx5dv direct WQE construction where noted. "Native" = one work request, no target
CPU. Facts corrected against source today are marked **(corrected)**.

| Function | RoCE implementation | Native / emulated | What it needs that the seam does or does not give |
|---|---|---|---|
| `osoe_transport_init` / `fini` | Open device, PD, CQs, DCT (or XRC target) in the node lead; register heaps; exchange `{gid, dct number, dc key, heap base, rkeys}` per *node*; create DCIs per context | — | **Not given:** a slot in §6.2's rendezvous for a backend blob (F10); ownership of start-up — `init.c` starts the AM engine itself (F5). Given: called after heap mapping, which is when registration is possible |
| `osoe_transport_context_default` | One DCI pool (or XRC initiator set) + send CQ per context | — | Given. Contexts as QP sets is the natural mapping |
| `osoe_transport_direct` | Unchanged: non-NULL for on-node PEs, since on-node puts remain `memcpy` in the calling thread | — | Contract survives (§4.1 below). Code conflates it with routing (F4) |
| `osoe_transport_put` | ≤ inline size: inline WQE, unsignalled. ≤ ~16–32 KiB: copy to registered slab. Larger: zero-copy if `src` is in the (registered) heap, else register/ODP and **wait for the CQE before returning**, because `src` is reusable on return | Native (RDMA WRITE) | Given: `(pe, region, offset)` → `node_base[node] + local_index(pe) × heap_size + offset`, one rkey per node (V1) or per PE. **Not given:** `local_index` of a *remote* PE — the job map carries only `node_of` (F7). `FENCED` free on one QP/DCI-stream per `(context, node)`; `MORE` = hold the doorbell |
| `osoe_transport_get` | RDMA READ; spin on the CQ with the sweep check. Zero-copy if `dst` is in the heap; else read into a registered per-PE slab and copy out — the §3.12 landing slot, which is already per PE, shared and registrable, serves unchanged | Native | Given. **Not given:** a `flags` parameter — the header promises nbi "is a flag on this call" and `MORE` cannot be said for a gather loop (F9) |
| `osoe_transport_rmw`, 8 B ADD / CSWAP | `IBV_WR_ATOMIC_FETCH_AND_ADD` / `_CMP_AND_SWP` on **stock verbs**; mlx5 maps both (*observed in source*, rdma-core `providers/mlx5/qp.c` l.57–58, 898–910) **(corrected: first round said rxe and ionic only)** | Native | Given |
| … 8 B WRITE(swap), AND, OR, XOR | With mlx5 extended atomics: **one** masked-CS or masked-FA WQE each — AND/OR/SWAP are a masked CS whose compare mask is zero; XOR is a masked FA with every bit a field boundary (*observed in source*, UCX `rc_mlx5.inl` l.1896–1926) **(corrected: first round and §4.10 say read-then-CAS loop)**. Stock IBTA verbs only: CAS loop, ≥ 2 round trips, lock-free not wait-free | Native on mlx5dv; emulated on stock verbs | Given (the emulation is the backend's) |
| … 4 B anything | mlx5 extended atomics have a native 4-byte operand size (*observed*, `rc_mlx5.inl` l.779, 816). Stock verbs: **must not** be a CAS on the containing 8-byte word (F2) — the whole 4-byte class goes to the target CPU over an AM side channel | Native on mlx5dv; **emulated via AM** on stock verbs | **Needs an AM path and a target-side thread.** The seam need not provide it — the backend lives in the same library as §3 and the lead's agent already hosts progress — but it means §10's "does AM stay hot" is answered *yes* for any NIC without 4-byte atomics (§4.5) |
| … READ | 8- or 4-byte RDMA READ (never fetch-add 0) | Native | Given |
| … without `FETCH` | IB atomics always return the old value (*recalled*, and both verbs forms take a result buffer — *observed*); post unsignalled and do not wait. The response packet is still sent | Native, wire cost unchanged | Given: "may return before it executes" is exactly what is needed |
| … on-node target | Device-scope atomics: NIC loopback for every on-node rmw in the WORLD domain (~µs against ~20 ns, *recalled*). **PCI-atomic mode: NIC atomics are CPU-coherent, so on-node rmw stays a CPU atomic** — but then only ADD and CSWAP are native (*observed*, UCX `rc_mlx5_common.c` l.1143–1159) **(new)** | — | Given: dispatch per `(pe, operation class)` in the contract. Not real in the code (F4) |
| `osoe_transport_load` | Plain aligned load. A NIC atomic's write-back is a DMA write of one aligned word on a cache-coherent host; no synchronization. Cost: nothing | Native | Given. The signature has no context, so it cannot choose per atomicity domain — harmless, since a CPU load is coherent with both executors |
| `osoe_transport_set_range` | Register the hinted end of the heap as a separate atomic-capable key — UCX does precisely this, a distinct atomic mkey (*observed*, `ib_mlx5dv_md.c` l.598–630); arm initiator-side notification for `SIGNAL` ranges | — | Given, and well shaped (union, no withdrawal, non-blocking). **Limit:** hints are optional performance advice (spec §9.3.5, *documented*), so an rmw or a wait on unhinted memory must still work: the whole heap must be atomic-capable anyway, and unhinted waits poll (F8) |
| `osoe_transport_fence` | Serializing store; nothing sent if one ordered stream per `(context, node)` | Native | Given |
| `osoe_transport_wait` | Serializing store; reap the send CQ to the covered nodes; if a write CQE does not imply visibility, post one zero-length READ per dirty node, then poll (k posts, one round trip) | Native | Given. The per-node dirty set in `transport-am.c` is reusable as is |
| `osoe_transport_watch` / `_seq` / `_wait` / `unwatch` | `watch.c` unchanged: the slot is in the node segment and keyed by region and offset | — | Given — this is the part of the header that fits a backend it was not written for |
| `osoe_transport_notify` | No NIC event exists at an RoCE target (*recalled*; consistent with verbs.h, which has no target-side counter object). Either a per-node poller comparing shadows of watched blocks calls this, or initiators follow rmw to `SIGNAL` ranges with a send-with-immediate that the lead's agent turns into this call | Emulated | **The contract's "exact. Every applied rmw, by any path, notifies" cannot be met for unhinted words** except by polling; see F8. Who hosts the poller: the agent thread — which the seam does not name (F5) |

Cost of `load`, `set_range`, `notify` on a NIC that applies atomics out of the
CPU's sight, in one line each: **load** — free; **set_range** — one extra memory
key per node and one table column, registration at init only because the heap
never grows; **notify** — a shared core per node (poller) plus 2–5 µs wake latency
(*recalled*, first round), or one extra WQE per rmw to a `SIGNAL` range and a
completion-channel wake in the lead.

---

## 3. Leaks: what above the seam knows that only one backend should

### 3.1 The include graph

`evidence/.../include-graph.txt` is the full output. The transitive result
(*observed in source*, script in the same directory):

| SHMEM-layer file (API entry points) | AM-layer headers reached | By what path |
|---|---|---|
| `rma.c` (5), `amo.c` (26), `signal.c` (8), `quiet.c` (6), `wait.c` (17), `team.c` (12), `accessible.c` (2), `malloc.c` (6) | `am.h connection.h connection-area.h wire.h reply.h progress.h` | `symmetric.h` → `state.h` → everything (`state.h:10–17`) |
| `barrier.c` (3), `init.c` (7) | the same **plus `progress-host.h`, directly** | `barrier.c:10`, `init.c` includes |
| `watch.c`, `transport-local.c`, `fatal.c`, `metrics.c`, `watchdog.c` | the same six | `state.h` |
| `node.c`, `job.c` (shared infrastructure) | `connection-area.h` / `am.h` | `node.c` reads the AM chunk size; `job.c` the handler checksum |
| `deprecated.c`, `info.c`, `profile.c`, `session.c`, `allocator.c`, `bootstrap.c` | none | — |

So every SHMEM-layer translation unit *can* name `osoe.wire`, `osoe.connection`,
`osoe.progress` — the AM backend's state sits in the one global `struct
osoe_state` (`state.h:27–41`). **None of the eight §4 files does** (grep in
§3.2): the leak is one of visibility, not use. But nothing enforces it. The
project enforces the analogous rule for `osoe-stat` ("shares no headers", with a
tier-1 test) and for exported symbols (`shmem.exports`); the boundary that the
second project goal rests on has no test at all.

### 3.2 Direct calls around the boundary (file:line, *observed in source*)

| # | Location | What it knows | A porter must |
|---|---|---|---|
| L1 | `barrier.c:10`, `barrier.c:61` | Calls `osoe_progress_host_post_arrival` — the AM engine's host — for the internode phase; the comment at `:47–60` explains the lead's agent and its process | Edit `barrier.c`, or keep `progress-host.c` and replace what it forwards to (`progress.c:652–690`) |
| L2 | `init.c:120–131`, `init.c:159–162` | Sets `osoe.internode` / `osoe.lead`, then calls `osoe_transport_init`, **then** `osoe_progress_host_start`, then `osoe_agent_start`; the comment at `:113–119` records that this order already produced a null-dirty-set defect | Edit `init.c` to start a second engine, and re-derive the ordering |
| L3 | `agent.c:70–72` | The agent loop's body is `osoe_progress_host_poll()` and `osoe_progress_host_idle_ns()` | Edit `agent.c` to poll a CQ or run the wake poller. This *is* the boundary progress call §10 closed as "not needed" |
| L4 | `state.h:27–41` | The AM backend's wires, connections, reply pool, scratch and engine are fields of the global state | Add RoCE state beside them, or split the struct |
| L5 | `node.c:229–240` | The control segment is sized from `OSOE_AM_CHUNK`: a landing slot per local PE in **every** build, single-node included (16 MiB per node at 256 PEs and the 64 KiB default, *arith*) | Nothing — reusable as the registered get slab — but it is an AM quantity in the node layout |
| L6 | `job.c` (includes `am.h`) | The rendezvous payload is fixed: rank list, heap size, **AM handler-table checksum** | Extend the rendezvous to carry a backend blob (F10) |
| L7 | `wait.c:105–108`, `:128–129` | A wait of width ≥ 4 that finds its predicate true after a sweep counts `invariant.missed_wake` — an *invariant* only on a backend whose rmw notification is exact | Make the backend return spurious `true` from `watch_wait`, or change the oracle (F8) |
| L8 | `transport-local.c:84–111` | `osoe_transport_load` and `osoe_transport_set_range` — two of the header's entry points — are defined in the *intranode path's* file, which the AM file links beside | Move or `#if` them; and there is no CMake backend option, so "bound at build time" has nothing to bind with (`shmem/CMakeLists.txt:3–35`) |
| L9 | `team.c:45`, `barrier.c:46` | `t->internode = node_count > 1`: the collective layer knows there are nodes | Nothing. §5 is *defined* as two-level; this is legitimate knowledge, listed for completeness |
| L10 | `transport.h:101–103`, §4.10 | Says `shmem_addr_accessible` "consults [`direct`] and nothing else". It does not (`accessible.c:34–38`) and must not — accessibility is reachability through OpenSHMEM, not by load and store | Nothing; fix the sentence |

Not leaks, checked: no §4 file assumes stores are CPU-visible — every own-word
read goes through `osoe_transport_load` (`wait.c:124,169,247`, `signal.c:135`);
no §4 file calls `osoe_is_local` or `osoe_node_of` (only `transport-am.c` and
`job.h` do); `watch.c` reads only the node segment.

### 3.3 The counterfactual: what a porter rewrites that the design promises they would not

`transport.h:5–7` promises a different backend "replaces the implementation of
this header without touching §4". Scored against that sentence:

- **§4 proper: promise kept**, except L7 (one counter) and L10 (one sentence).
  Replace `transport-am.c` (618 lines); keep `transport-local.c`, `watch.c`,
  `rmw.h`.
- **§5: promise not made, and the cost is not where §10 says it is.** §10 says a
  backend wanting a native barrier "implements §5's internode phase". That phase
  is not in §5's file; it is `progress.c:200–249` and `:652–690`, driven through
  `progress-host.c`, reached from `barrier.c:61`. A porter edits three files
  below and one above.
- **§6: init.c, agent.c, job.c, node.c** (L2, L3, L6, and §4.1's second mapping
  for registration, which the first round already listed as the one change
  outside the backend). Four files the design does not mention a porter touching.
- **Build:** a backend selector.

None is large. All are the kind of edit that is fifteen lines today and a
restructuring once step 3 (sockets, io_uring) and §5.5 (reductions) have hardened
around the present shape.

---

## 4. The questions

### 4.1 The cut: does `direct`'s store-ordering contract survive a NIC that applies atomics out of the CPU's sight?

**Yes, as a contract.** Take the hard case, `put_signal` to an on-node PE with
device-scope atomics: the put is a `memcpy` in the calling thread, the signal is
a NIC loopback atomic carrying `FENCED`. The doorbell is an MMIO write issued by
the same thread after the `memcpy`; by the time the NIC acts, the copy's stores
have retired and are visible to any core and to DMA on a cache-coherent host
(*recalled* x86/aarch64 server behaviour), so the signal cannot announce bytes
that have not landed (invariant 5). A store through a `shmem_ptr` pointer racing
an AMO on the same word is undefined by spec §3.2, as the header says. `load` is
a plain load (§2). With mlx5's PCI-atomic mode the question disappears: NIC
atomics are then CPU-coherent (*observed in source*: UCX sets `ATOMIC_CPU`).

**Not as code.** Dispatch "per (pe, operation class)" is prose. Every entry point
in `transport-am.c` (`:462`, `:475`, `:491`) branches on the *same* predicate,
`osoe_is_local`, which is also what `osoe_transport_direct` (`:138`) answers. No
configuration can route one PE's rmw differently from its puts, so nothing tests
the claim, and the one place the pointer and the route are *supposed* to differ —
§8.1's force-remote flag, "denies the pointer … without changing the route"
(`job.h:78–80`, `transport-am.c:123–126`) — does change the route, into a fatal
(F4, *measured*).

### 4.2 What §4.10 keeps outside

| Outside item | Would a 1M-PE RoCE port want it inside? |
|---|---|
| **Collectives** | **Yes — the internode phase, as a team-addressed group interface.** F1. Not for SHARP (absent on RoCE, *documented*) but to get barriers, and later reductions, off TCP and onto the fabric the data path uses, and to leave room for NIC-chained schedules and whatever Ultra Ethernet's in-network collectives become |
| **Watch** | Already inside (`transport.h:352–402`), and correctly: the waiter publishes a range, *some* writer bumps a sequence. An RoCE poller slots in with no change to `wait.c`. The only defect is the exactness wording (F8) |
| **Landing zones** | No. They solve an AM-only problem (a progress thread in another process cannot write the caller's stack). RoCE writes the caller's memory directly. They are nonetheless the right object for a registered get slab, so nothing is lost by their being in the node segment (L5) |
| **Bootstrap** | No — out of band in every build is forced, and correct. But it needs one opaque per-node blob in the rendezvous (F10), and at 1M PEs the map itself needs care: node 0's lead sends a `world_size × 4 B` = 4 MiB map to 4,095 leads = **16 GiB from one socket** (*arith*; 13 s at 10 Gb/s, 130 s at 1 Gb/s). A block or cyclic Slurm distribution compresses to O(nodes) |
| **Allocation** | No. `set_range` is the right and sufficient hook |

### 4.3 Collectives: are the two reasons for closing `osoe_transport_sync` sound?

See F1 for the finding; the argument in brief.

*Reason 1, addressing ("a node group is a second addressing model").* What
crosses downward today is `osoe_progress_host_post_arrival(t->number, k + 1)`
(`barrier.c:61`) — a **team number and an epoch**. What comes back is
`release(context, team, epoch)` (`progress-host.c:99`, called from
`progress.c:235,248,674`). Neither names a node or a PE. The node set is computed
*below* that call, by `participants(team)` (`progress-host.c:130`). So the
interface the code actually has is team-addressed; the SHMEM layer already owns
teams; and no second addressing model leaks up. The reason is true of the
proposal as §10 worded it — `osoe_transport_sync(ctx, group)` "over a group of
*nodes*" — and false of the code written that day.

*Reason 2, the release ("an rmw on a futex word that notify does not cover").*
The release is not an rmw in the code either. It is an **upcall**: the engine
calls the host's `release`, which bumps the team's counter and futex-wakes
(`progress-host.c:99–116`). An upcall needs no `notify`, no watch, and leaves
§5.4's equality wait untouched. The reason refutes a design nobody needs.

*The cheap seam shape.* Three declarations, no capability bit, mandatory for
every backend with the present AM code as the generic implementation:

```c
/* Collective group interface (§5 internode phase). Addressed by team, never by
 * node: the backend derives the participating nodes from the team (§5.2). */
void osoe_transport_group_arrive(uint32_t team, uint32_t epoch);  /* any local PE's thread; posts, does not block */
/* upcall, implemented by the collective layer, called exactly once per
 * (team, epoch) on every participating node, on a backend thread: */
void osoe_collective_release(uint32_t team, uint32_t epoch);
```

and, when §5.5 lands, the data-carrying sibling called by the team's local lead
after the intranode combine (`osoe_transport_group_reduce(team, epoch, op, type,
count, accumulator)` with the same release). That is a rename and a header move
today. It keeps §4.10's operations point-to-point and PE-addressed, as the
closure wanted, by putting the group interface *beside* them rather than among
them.

*Cost at 4,096 nodes*, one barrier's internode phase (*arith*; per-message
figures *recalled*: TCP small message ≈ 2–5 µs of CPU per side and ≈ 15–30 µs one
way including waking a sleeping agent; RDMA ≈ 1–2 µs one way):

| Shape | Messages | Critical path | Estimate |
|---|---|---|---|
| **As coded: star on node 0 over TCP** (`progress.c:225–234`, `:667–672`; root fixed at `progress-host.c:138`) | 2 × 4,095 = 8,190, all through one thread | root handles 4,095 receives then 4,095 sends serially | 8,190 × ~3 µs ≈ **25 ms** (16–41 ms) |
| Binomial tree over TCP (what §5.2's text calls for) | 8,190 | 12 up + 12 down | 24 × ~25 µs ≈ **0.6 ms** |
| Binomial tree over RDMA, agents polling | 8,190 | 24 hops | 24 × ~2 µs ≈ **50 µs** (170–290 µs if agents sleep on a completion channel) |
| NIC-chained schedule (*recalled*: mlx5 cross-channel WAIT; unverified) | 8,190 | 24 hops, no CPU | ≈ 30 µs |
| SHARP | — | — | 2.83 µs at 128 hosts (*documented*), **InfiniBand only** |

So the decision's real price on an RoCE build is **TCP-star against RDMA-tree ≈
500×, or ≈ 12× once the AM phase is a tree** — not §10's "4–6×", which is
`roce-transport.md` §8.4's RDMA tree (13 levels × 1.8 µs) against SHARP and
presumed collectives already ran over the fabric. Every `shmem_malloc`, `free`,
`realloc` and `finalize` is a barrier (`malloc.c:96,174,207,246`); bale issues two
per collective (first round). The star is a defect against goal 3 on plain
Ethernet too, and belongs to the performance-and-scale deliverable; it is listed
here because it sets the baseline the seam decision is priced against.

### 4.4 Nonblocking and completion

- **Counters, no handles: blocks nothing I can find.** Completion at `quiet` is
  reaping a CQ until issued = completed per covered node. Selective signalling
  and CQ moderation are *enabled* by the absence of handles (no per-operation
  CQE is ever owed). On-demand paging is invisible (responder-side faults are
  retried by the transport, *recalled*). DC is invisible.
- **Multi-rail against `FENCED`:** the flag orders one operation after earlier
  writes on the same `(context, pe)`; a striping backend drains the other rails
  before posting a fenced operation, or pins a `(context, node)` to one rail.
  Because the flag is per operation, unfenced traffic keeps striping. Sound, and
  the first round's strongest point stands.
- **`OSOE_TRANSPORT_MORE` for doorbell batching:** sufficient for puts and rmw.
  "More follows on this context" does not say *to the same destination*, so a
  backend with a QP per node keeps a short list of QPs with a held doorbell and
  rings them all at the next unflagged operation — cheap. The source-ownership
  rule is exactly right: a zero-copy put must ring and wait regardless. **Gap:**
  `osoe_transport_get` has no `flags` parameter (`transport.h:212–213`), so the
  case the first round priced highest — pipelined reads, "up to ~10×" — can carry
  neither the promised nbi flag nor `MORE` without a signature change (F9).
- **Blocking put forces copy-or-wait.** `put` returns with `src` reusable, so a
  large put from unregistered memory must block on its CQE or copy. That is the
  OpenSHMEM API, not the seam; `put_nbi` is the natural RDMA primitive and enters
  as a flag. Noted under lessons.
- **GPU and device memory:** cleanly out of scope and not foreclosed. A device
  heap is one more `enum osoe_region`; `src`/`dst` are untyped pointers a backend
  can classify below the seam. GPU-*initiated* operations cannot be served by a
  host-side blocking C interface, as the first round said.

### 4.5 State at 1M PEs under `(pe, region, offset)`

| Table | Size at 4,096 × 256 (*arith*) | Verdict |
|---|---|---|
| Per-node `{heap base, rkey, atomic rkey}` if the lead registers all local heaps (V1) | 4,096 × 24 B ≈ 96 KiB, shareable in the node segment | Fine |
| Per-PE rkeys if V1 fails | 1M × 4 B = 4 MiB per region — **shareable per node**, because an rkey is a number valid at the target, not a per-initiator object | Fine: 4–8 MiB per node, not per PE |
| rank → node (`job.node_of`) | 4 MiB, one shared mapping per node (`job.c:98`) | Fine |
| rank → local index, for *remote* PEs (needed to form the remote address) | 1M × 1–2 B = 1–2 MiB shared | **Missing** (F7) |
| `node.local_of_rank` | 4 MiB **private per PE** = **1 GiB per node** (`node.c:391`) | **Not fine** (F7) |
| Dirty set per context | 13 B × 4,095 ≈ 52 KiB | Fine; node-scaled as claimed |
| `pe_quiet` scratch | `calloc(4,095)` per call (`transport-am.c:582`) | Fine for memory; a per-call allocation on a completion path |
| QPs with DC | ~8 DCIs × contexts per PE; one DCT per node in the lead | Fine |
| QPs with XRC, all-to-all | 4,095 initiator QPs per PE ⇒ 256 × 4,095 ≈ **1.05M initiator + 1.05M target QPs on one NIC** | Not fine; the first round's "7,812 QPs per process" hid the per-NIC product. *Recalled*: mlx5 defaults to 2^18 QPs. **DC is the only option at this tier**; it is below the seam, so the seam is unaffected |

Does the fixed heap base and per-PE heap help or hurt? **Helps.** The heap never
grows or moves, so registration happens once at init and `set_range` never has to
re-register; the second-mapping trick of §4.1 gives one memory region per node.
V1 looks safer than the first round feared (*recalled*, unverified): with DC the
target side is one DCT *owned by the lead*, and the region is registered *by the
lead* in the same protection domain, so the responder's key check never crosses a
process. The heap base `B` is chosen per node (§4.1), so a per-node base column
is required — invariant 7 already forbids assuming otherwise. Nothing above the
seam forces O(PEs) memory per PE; `local_of_rank` is beside the seam, in `node.c`.

### 4.6 §10's open question: does the AM data path stay hot in an RDMA build?

**Quietly settled for the control path, conditionally settled for the data path,
and the "separate connection area" claim does less than it says.**

- *Settled — control path stays hot, always.* Closing `osoe_transport_sync` with
  "no" means every barrier's internode phase is AM traffic. Wires, framing, the
  stream parser, the control queue, the lead's idle loop and its safety sweep run
  in every multi-node build. §10 asked the question and answered half of it
  eleven lines earlier.
- *Conditionally settled — data path.* On a NIC without 4-byte atomics (stock
  IBTA verbs), the 4-byte rmw class must execute on the target CPU (F2), so
  reservation, deferred replies, credits and landing zones all stay hot, for the
  commonest AMO type (`int`). On mlx5 with extended atomics, or in PCI-atomic
  mode, they do not.
- *The claim.* The connection area is a separate object
  (`/osoe.<job>.<step>.<node>.conn.<session>`, `connection-area.c:186`), so an
  RDMA build could omit it without a control-segment layout change — true. But
  it is created unconditionally for any multi-node job, eagerly, as a full mesh:
  at 4,096 nodes the default configuration **refuses to start** —
  4,095 × 2 × 128 KiB ≈ 1.0 GiB against the 256 MiB budget
  (`connection-area.h:59–62`, `connection-area.c:88–92`, *arith*; the header
  documents the ~1,000-node guard rail, so this is intended, not a defect) — and
  at the 1 KiB minimum chunk it is 16 MiB per node plus 4,095 sockets per lead,
  8.4M connections job-wide. Meanwhile the landing zones, equally an AM
  artefact, are *in* the control segment in every build (L5). What the separate
  object keeps open is a memory saving, not the question.
- *The uncounted cost §10 names — two completion domains — is real and small:*
  the dirty set is per node and a node is reached by one path per operation
  class, so `wait` reaps the CQ and then runs the present AM acknowledgement loop
  over the (few) nodes touched by AM-class operations.

**Deferrable?** Yes, *if* F1 is taken: with the group interface inside the
boundary an RDMA build can run its internode phase on the fabric and the AM layer
reduces to bootstrap plus optional emulation, which is the state in which
"needs a device" is an honest answer.

---

## 5. Findings

### F1. `osoe_transport_sync` was closed for reasons the code contradicts, and priced with the wrong number

- **Location:** design §10 (l.4280–4316), §5.3 (l.2931–2935); `progress.c:623–650`;
  `barrier.c:61`; `progress-host.c:99–116,130–138`; commit `1cb1770`.
- **Failure scenario:** an RoCE build at 4,096 nodes. Data path at ~2 µs; every
  `shmem_malloc` pays a barrier whose internode phase is 8,190 TCP messages
  through node 0's agent: ≈ 25 ms (*arith*, §4.3). A program that allocates 1,000
  times at start-up spends ≈ 25 s in barriers. To fix it the porter must edit
  `barrier.c` and `progress-host.c` — the restructuring §10 said the closure
  avoided. Reductions (§5.5, unbuilt) will be written against AM reply queues in
  the meantime, deepening it.
- **Evidence:** the downward call carries `(team, epoch)` and the upward one is a
  callback carrying `(team, epoch)` — *observed in source*; neither reason in the
  closure applies to that shape (§4.3). §10's "4–6×" is `roce-transport.md` §8.4's
  RDMA-tree-versus-SHARP figure — *documented* there — and SHARP is absent from
  RoCE switches — *documented* (Next Platform). Latency figures *recalled*;
  message counts *arith*.
- **Severity:** design change needed (cheap now: a header move and a rename).
- **Confidence:** high that the two reasons do not hold against the code; medium
  on the latency ratios. A two-node TCP measurement at step 3 would fix the
  per-message constant.

### F2. Emulating a 4-byte rmw by CAS on the containing 8-byte word corrupts a legal program

- **Location:** `docs/reviews/archive/roce-transport.md` §4.3 (l.262, l.272–278), carried
  into design §10's tally ("8-byte-only atomics" as a mere cost) and §4.10's
  emulation list.
- **Failure scenario:** `struct { int hits; int local_flag; } *s = shmem_malloc(8)`.
  PE 1 runs `shmem_atomic_inc(&s->hits, 0)`; PE 0 runs `s->local_flag = 1`, a plain
  store to **its own, different** variable. Backend emulation: NIC reads 8 bytes
  `{h, 0}`; PE 0's CPU stores `flag = 1`; the NIC's compare saw `{h, 0}` before
  the store and writes `{h+1, 0}`. `local_flag` is silently reset. The first
  round called this legal because "the spec makes mixing AMO and store on a word
  undefined" — but spec §3.2 speaks of the same *data object*, and these are two
  objects. Device-scope NIC atomics are not exclusive against CPU stores, so the
  8-byte window is unprotected.
- **Evidence:** *documented* (spec §3.2 scope); *observed in source* that UCX
  marks non-PCI mlx5 atomics `ATOMIC_DEVICE`, "consistent only with respect to
  other atomics". *Not determined:* whether mlx5's native 4-byte masked operations
  write back 4 or 8 bytes; if 8, the hazard extends to them whenever the
  neighbour is CPU-written.
- **Consequence for the seam:** none to the header. For the plan: on a NIC without
  native 4-byte atomics the entire 4-byte class must run on the target CPU via AM
  (a width class may have only one executor, or exclusivity within it is lost),
  which answers part of §10's open question (§4.6). The allocator could
  alternatively guarantee that `ATOMICS`-hinted blocks are 8-byte granular — but
  hints are optional, so that cannot be the whole answer.
- **Severity:** clarification needed — record it as a trap beside §4.1's, before
  anyone implements the first round's table.
- **Confidence:** high on the logic; medium on reach (depends on the undetermined
  write-back width).

### F3. The hardware premises under §4.10 and §10 are out of date in the design's favour

- **Location:** design §4.10 (l.2294–2298, "a bitwise rmw as a read-then-compare-and-swap
  loop"), §10 (l.4267–4270); `roce-transport.md` §1, §4.3, §8.5; `core-api.md` C15.
- **Evidence (*observed in source*, fetched today, `sources.md`):** (a) rdma-core's
  mlx5 provider maps both IBTA atomic opcodes in `ibv_post_send` and in the
  `ibv_qp_ex` path — the first round's "rxe and ionic only" no longer holds;
  (b) mlx5 extended atomics give ADD, AND, OR, XOR, SWAP, CSWAP at 4 and 8 bytes
  as single work requests; (c) mlx5 reports PCIe-AtomicOp capabilities
  (`fetch_add`, `swap`, `compare_swap` × 4/8/16-byte) and UCX treats such a device
  as CPU-coherent, offering only ADD and CSWAP there; (d) UCX registers a
  separate atomic memory key, which is direct support for `set_range` and the
  two-ended allocator.
- **Failure scenario:** none — this is a correction. The "four losses that are
  RoCE's own" shrink to two on mlx5 (no non-fetching atomic; no target counters),
  and the coherent/incoherent choice becomes a per-device mode the backend picks
  underneath, which is further evidence *for* rejecting capability bits.
- **Severity:** clarification needed. **Confidence:** high for what the source
  says; unknown for which firmware and root complexes enable (c).

### F4. Dispatch is per PE on one predicate; the pointer and the route cannot differ, and where they are documented to differ the job dies

- **Location:** `transport-am.c:138,462,475,491`; `job.h:78–100`;
  `transport.h:25–32,99–101`; `shmem/tests/CMakeLists.txt` (the unregistered
  force-remote half of `shmem.internode`).
- **Failure scenario (*measured*, 3 of 3, VM under contention,
  `force-remote-data-path.log`):** `OSOE_FORCE_REMOTE=1 osoe-launch -n 4 -m 0,0,1,1
  -- shmem_test_internode` → every PE aborts with `no connection toward node 0,
  which holds PE 0` (and the analogue on node 1); the same map without the flag
  passes. Three comments say the flag "denies the pointer … without changing the
  route". The CMake file acknowledges the data-path half is unregistered as "an
  open decision", so the *failure* is known; what is not recorded is that it
  falsifies the header's present-tense claim that dispatch is per `(pe,
  operation class)`. An RoCE backend needs `direct != NULL` while rmw goes
  elsewhere; force-remote needs `direct == NULL` while put stays local. Both are
  the same missing split.
- **Fix shape:** two predicates — `routes_in_process(pe, class)` and the pointer
  answer — and force-remote becomes testable end to end today, as the first
  *observable* instance of per-class dispatch.
- **Severity:** clarification needed (design change if force-remote is meant to
  be the bring-up harness for a second backend, as `roce-transport.md` §5 assumes).
- **Confidence:** high.

### F5. Lifecycle and progress are outside the boundary; §10's "no progress call needed" is refuted by `agent.c`

- **Location:** `init.c:120–131,159–162`; `agent.c:70–72`; design §10 l.4239.
- **Failure scenario:** an RoCE backend needs a thread for the wake poller or the
  immediate-data CQ, and start-up before it. `osoe_transport_init` is called
  *before* the engine exists and does not own it; the agent loop names the AM
  engine. The porter edits `init.c` and `agent.c` and re-derives an ordering that
  has already failed once (`init.c:113–119`, first internode put through a null
  dirty set). §10's argument — every place the SHMEM layer blocks is a backend
  function — covers application threads only.
- **Evidence:** *observed in source*.
- **Fix shape:** `osoe_transport_init` starts whatever the backend needs;
  `agent.c` calls `osoe_transport_agent_poll()` / `_idle_ns()` declared in
  `transport.h`. Same code, boundary-named.
- **Severity:** clarification needed. **Confidence:** high.

### F6. Nothing enforces the boundary; every §4 file can see the AM backend's state

- **Location:** `state.h:10–17,27–41`; `symmetric.h:8`; `include-graph.txt`.
- **Failure scenario:** a future `amo.c` optimization reads
  `osoe.connection[slot].acked` or `osoe.internode` directly. It compiles, passes
  all four presets (one backend exists), and the RoCE port finds it a year later.
  The project already knows this failure shape — CLAUDE.md's "wrong
  consistently" rename — and guards `osoe-stat` and the export list with tests.
- **Evidence:** *observed in source*; eight API files reach six AM headers
  transitively, none uses them today.
- **Fix shape:** split `struct osoe_state` so §4 files include only bootstrap,
  node and allocator state; a tier-1 test that compiles the §4 files with the AM
  headers poisoned (`#error`), or greps for the AM prefix set.
- **Severity:** minor now, cheap now. **Confidence:** high.

### F7. `local_of_rank` is O(PEs) private memory per PE, and the job map cannot form a remote address

- **Location:** `node.c:391–397`; users `watch.c:83`, `transport-am.c:140`,
  `team.c:91`, `progress-host.c:60,73`; `job.c:98` (map holds `node_of` only).
- **Failure scenario:** 1M PEs: 4 B × 2^20 = 4 MiB `malloc` per process, × 256
  PEs = **1.0 GiB per node** of private memory (*arith*) to answer a question
  whose domain is 256 entries. Separately, an RDMA backend computes
  `node_base + local_index(pe) × heap_size + offset` for a **remote** `pe`;
  `local_of_rank` is −1 there, and under Slurm's cyclic or arbitrary
  distributions the index is not derivable from `node_of`.
- **Evidence:** *observed in source*; *arith*.
- **Fix shape:** carry `uint8_t`/`uint16_t local_index[world]` in the shared job
  map (1–2 MiB per node, shared) and drop the private array; or for block
  distributions store per-node `first_rank` (O(nodes)).
- **Severity:** design change needed at the 1M tier (goal 3's "linear or better
  in memory"); minor at 500 × 256 (0.5 MiB per PE, 125 MiB per node).
- **Confidence:** high.

### F8. "Notification: exact. Every applied rmw, by any path, notifies" is a promise an RoCE backend cannot keep for unhinted words

- **Location:** `transport.h:242`; §4.10 l.2410–2413; `wait.c:105–108`; spec §9.3.5.
- **Failure scenario:** a conforming program waits on a `shmem_malloc`ed (unhinted)
  word that a remote PE updates with `shmem_atomic_set`. On RoCE no target event
  exists; `set_range` was never called for the word, so initiators add no
  immediate; the backend polls. A poller that compares shadows also misses an
  A→B→A change between polls — acceptable to the spec, not "exact". And if the
  backend's `watch_wait` spins to the deadline and returns `false`, `wait.c`
  books every successful wait as `invariant.missed_wake`, making the invariant
  class useless on that backend (L7).
- **Evidence:** *documented* (hints are optimisation advice only); *observed in
  source* (the oracle keys on width, not on backend).
- **Fix shape:** restate the contract as the property callers rely on — *a change
  that persists is not missed for longer than the backend's stated bound; rmw on
  declared `SIGNAL`/`ATOMICS` ranges notify promptly* — and let the backend, not
  `wait.c`, classify a late wake.
- **Severity:** clarification needed. **Confidence:** high.

### F9. Small signature gaps: `get` has no flags; two entry points live in the wrong file; no backend selector

- **Location:** `transport.h:203–213`; `transport-local.c:84–111`;
  `shmem/CMakeLists.txt:3–35`.
- **Failure scenario:** `shmem_get_nbi` lands and changes a signature the header
  says it would not; a gather loop cannot say `MORE`; an RoCE backend that needs
  its own `set_range` collides at link time with the intranode file's.
- **Severity:** minor. **Confidence:** high.

### F10. The rendezvous has no slot for a backend's addressing blob

- **Location:** `job.h:4–15`; `job.c` (includes `am.h` for the handler checksum).
- **Failure scenario:** RoCE needs a per-node `{gid, dct number, dc key, heap
  base, rkeys}` allgather (~64 B × 4,096 = 256 KiB, *arith*) before the first
  operation. The rendezvous exchanges exactly three AM-era fields. A porter edits
  §6.2's protocol.
- **Fix shape:** at step 3, when the rendezvous moves to sockets, add one opaque
  length-prefixed per-node blob filled by `osoe_transport_*`; the AM backend's
  blob is its handler checksum.
- **Severity:** clarification needed (cheap at step 3, a protocol revision after).
  **Confidence:** high.

### F11. §10's "AM stays hot?" is presented as open, but three built things have decided most of it

- **Location:** §10 l.4317–4328; what-changed note ("keeps … open rather than
  settling it by accident"); `node.c:229–240`; `connection-area.c:150–186`.
- **Evidence and scenario:** §4.6 above. **Severity:** clarification needed.
  **Confidence:** high on structure; the memory figures are *arith*.

### F12. The first round's XRC recommendation does not survive per-NIC arithmetic at this tier

- **Location:** `roce-transport.md` §3.2. **Scenario:** §4.5 — ≈ 2.1M QPs on one
  NIC for all-to-all at 4,096 × 256. **Evidence:** *arith*; device limit
  *recalled*. **Severity:** minor for this review (below the seam; DC fits);
  recorded so the plan is not implemented as written. **Confidence:** medium.

---

## 6. Hardware atomics compared, and what the AMO set shrinks to

The author's hypothesis — that PCIe atomics match RoCE atomics — is **half
right, and the right half is more interesting than expected**: the operation sets
differ, but mlx5 exposes a mode in which an InfiniBand atomic *is executed as* a
PCIe AtomicOp, and that is exactly the mode in which NIC atomics become coherent
with the CPU.

| Fabric | Operations | Operand sizes | Fetching? | Coherent with CPU atomics? | Source |
|---|---|---|---|---|---|
| **PCIe AtomicOp TLPs** (ECN to 2.0, in 2.1+) | FetchAdd, Swap, CAS | FetchAdd, Swap: 32, 64. CAS: 32, 64, 128. Address aligned to operand | Always (completion returns the original value) | Completed by the root port against host memory; every port on the path must route AtomicOps and the requester must be enabled. UCX treats the result as CPU-consistent | *documented*: PCI-SIG ECN; AMD ROCm. *observed*: UCX |
| **InfiniBand / RoCE (IBTA)** | FetchAdd, CmpSwap. No Swap, no bitwise | 64 only | Always | Capability enum: none / HCA-scope / global. Optional feature of the HCA | *observed*: `verbs.h`, mlx5 provider (8-byte). Optionality *recalled* |
| **mlx5 extended (vendor, via direct WQEs)** | masked CS, masked FA ⇒ ADD, AND, OR, XOR, SWAP, CSWAP | 32, 64 | Always | **Device scope only** | *observed*: UCX |
| **mlx5 PCI-atomic mode (vendor capability)** | fetch_add, swap, compare_swap | 4, 8, 16 bytes as reported | Always | **Yes** | *observed*: rdma-core `pci_atomic_caps`, UCX. Prevalence unknown |
| **Ultra Ethernet (UET)** | Non-fetching atomics in every profile; fetching (one element) in AI Full and HPC; compare-and-swap **HPC only**; SUM, bitwise, MSWAP | Datatype-sized elements | Both | CPU coherence is vendor-optional; atomics never on the idempotent (RUDI) service | First round, `evidence/q2/q2-future.md`, *documented* there from the UEC spec; not re-read today |
| **CXL** | No atomic transaction in CXL.mem or CXL.cache; atomicity is the CPU's instruction over coherent memory. CXL.io is PCIe, so AtomicOps are in principle available | — | — | Cross-host atomics need the 3.0 inter-host coherence protocol, absent on prototypes | First round FUT §B.4, *documented* there; CXL.io point *recalled* |
| Portals 4 / Cassini | SUM, MIN/MAX, bitwise, SWAP, CSWAP family, MSWAP | Datatype-sized | Both | Per-interface | First round |

**The hardware intersection** (PCIe ∩ IBTA, which is also what mlx5's coherent
mode offers and what UET HPC covers): **{fetch-and-add, compare-and-swap} on
64-bit words, always fetching.** Add 32-bit and you lose stock IBTA. Add Swap and
you lose IBTA (PCIe has it; IB does not).

Restricted to that intersection, OpenSHMEM's AMO set becomes:

| Spec routine | Survives? |
|---|---|
| `atomic_fetch_add`, `atomic_add`, `atomic_fetch_inc`, `atomic_inc` | Yes — one FetchAdd; the non-fetching forms discard the reply and need not wait |
| `atomic_compare_swap` | Yes |
| `atomic_fetch` | Yes — as a read (not FetchAdd of 0, which is a write on counter-bearing NICs) |
| `atomic_set`, `atomic_swap` | **No.** Derived: a CAS loop (lock-free, ≥ 2 round trips under contention). Native on PCIe and on vendor-extended mlx5 only |
| `atomic_and`, `_or`, `_xor` and fetch forms | **No.** CAS loop, or vendor masked operations |
| Every 32-bit type (`int`, `int32_t`, `uint32_t`, `float`) | **No** on stock IBTA — and not emulable on the containing word (F2) |
| `float`, `double` (fetch, set, swap only) | `double` keeps `fetch`; nothing else |

---

## 7. What is sound, and the evidence

- **The §4 call discipline is real.** `grep` over the eight §4 files finds no
  call to `osoe_progress*`, `osoe_is_local`, `osoe_node_of`, `osoe_connection*` or
  any AM state; every own-word read goes through `osoe_transport_load`
  (`wait.c:124,169,247`; `signal.c:135`). A porter really does replace one file
  for RMA, AMOs, signals, quiet and waits.
- **No capability bits** survived a harder test than the first round gave it:
  mlx5 turns out to have *two* atomic personalities (coherent with a thin op set,
  incoherent with a full one), and both fit underneath without the SHMEM layer
  knowing (F3).
- **`set_range`'s shape** — union, no withdrawal, non-blocking, identical on every
  PE by determinism — matches what UCX independently does (a separate atomic
  key, aligned to the operand size), and the two-ended allocator that feeds it is
  implemented (`allocator.c:63–92`).
- **Weak ordering plus per-operation `FENCED`**, counters without handles, and
  `MORE` with the source-ownership rule: I tried DC, multi-rail, ODP, selective
  signalling and doorbell batching against each and found nothing blocked (§4.4).
- **The fixed, never-growing, per-PE heap** is a gift to registration: once, at
  init, never again; and the landing slots double as the registered get slab.
- **The per-node dirty set** (`transport-am.c:66–71`) transfers to an RDMA backend
  unchanged and is node-scaled as claimed: ≈ 52 KiB per context at 4,096 nodes.
- **The watch interface** genuinely accommodates a backend it was not written
  for; only its exactness sentence over-promises (F8).
- **`direct` as a property, not a locality,** is the right phrasing and survives
  NIC-applied atomics (§4.1); the defect is in the code's single predicate, not
  the contract.

## 8. Could not determine

- Whether any shipping ConnectX firmware and host root complex enable the
  PCI-atomic capabilities; whether mlx5's 4-byte masked operations write back 4
  or 8 bytes (F2's reach); mlx5's per-device QP ceiling (F12); whether a DCT in
  the lead can serve a region covering siblings' heaps (V1 — argued plausible,
  not verified); every latency constant in §4.3. All need a device or the PRM.
- The UEC and CXL rows of §6 rest on the first round's reading, not re-verified.
- The NVIDIA MLNX_OFED 4.6 page would not load (404 after redirect); the 5.9
  edition was read instead.

### Not investigated

Planned and not done, because the review's budget ran out; none is covered by a
claim above.

- The slice's commit messages other than `1cb1770` were not read for boundary
  decisions (`3c19880`, `de540da`, `02279fa`, `aa4fc27` are cited for what their
  titles and the resulting code show, not for their text).
- `core-api.md` was consulted only for its atomics table and open items C4 and
  C15; its §8.3 proposal was not re-compared with `transport.h` line by line.
- No prototype of the team-addressed group interface (F1) was built in the
  private copy to show `barrier.c` compiles against it unchanged; the claim
  rests on reading the two call sites.
- No poisoned-header build (F6) was run to prove the §4 files compile without
  the AM headers.
- Triggered operations / cross-channel chaining on mlx5, UD multicast, and
  Ultra Ethernet in-network collectives were not sourced; they appear only as
  *recalled*.
- `allocator.c`'s hinted-block granularity and `team.c`'s rank translation were
  not examined beyond the lines cited.

---

## 9. Lessons for a successor API

1. **Define the atomics by the hardware intersection, not by C's operator list.**
   One AMO word type — 64-bit, 8-byte aligned — with `fetch_add`,
   `compare_swap`, `read`. Everything else (`set`, `swap`, bitwise, 32-bit,
   floating types) is either a library-level derivation with a stated cost or
   absent. OpenSHMEM's twenty-odd routines × nine types forced `rmw` to carry
   seven ops and two widths, forced the 4-byte emulation trap (F2), and is the
   reason a plain IBTA NIC cannot serve `shmem_atomic_inc(int *)` at all.
2. **Make "this memory takes remote atomics / wakes waiters" a property of the
   allocation, not an optional hint.** Because §9.3.5's hints are advice, a
   backend must keep the whole heap atomic-capable and cannot give exact wakes
   (F8). An `alloc(size, ATOMIC | WAITABLE)` that is *required* for those uses
   lets a NIC register a small range, lets notification be exact where it
   matters, and makes granularity (8-byte cells) enforceable.
3. **All hardware atomics fetch; "non-fetching" is a completion property.** Say so:
   one operation, with a flag for "do not wait for the result". The spec's
   separate routine families obscure that the wire cost is identical.
4. **Blocking put is the wrong primitive for RDMA.** "`src` reusable on return"
   forces copy-or-wait for every large transfer from unregistered memory. Make the
   nonblocking form primary and the blocking one `nbi + quiet(target)`. Completion
   by counters at a quiet, with no handles, was the right call and should be kept.
5. **Ordering should be opt-in per operation** (`FENCED`), never a stream
   property — osoe's boundary got this right against the spec's grain, and it is
   what keeps spraying and multi-rail available.
6. **Collectives need a fabric-visible group object.** OpenSHMEM teams are opaque
   handles with per-call `(team, epoch)` identity implicit in call order; the
   library had to invent epochs to name a collective instance. A successor should
   make the instance explicit and let the transport own the internode phase, so
   in-network or NIC-chained execution is an implementation choice rather than an
   API violation.
7. **Every collective allocation being a barrier** (`malloc`, `free`, `realloc`)
   puts the slowest operation in the library on the commonest administrative
   path. A successor can make allocation a pure local function of the call
   sequence (osoe's allocator already is) and require synchronization only before
   first remote use.
8. **`shmem_ptr` conflates "is local" with "stores are ordered with the
   library's".** osoe had to phrase `direct` as a property to keep CXL and
   incoherent NICs possible; a successor should expose that property directly.
9. **World ranks as the only address force a rank → (node, index) table
   somewhere.** Exposing `(node, local index)` as a first-class address — or
   guaranteeing a block distribution — removes an O(PEs) structure from every
   implementation (F7).

---

## 10. Sources

Upstream source and pages read today are listed with line references in
`evidence/phase1-step2-slice1/roce-seam/sources.md`:

- rdma-core `libibverbs/verbs.h`, `providers/mlx5/qp.c`, `providers/mlx5/verbs.c`
  — <https://github.com/linux-rdma/rdma-core>
- UCX `src/uct/ib/mlx5/rc/rc_mlx5.inl`, `rc_mlx5_common.c`,
  `dv/ib_mlx5dv_md.c`, `src/uct/api/uct.h`, `src/uct/ib/base/ib_device.h`
  — <https://github.com/openucx/ucx>
- PCI-SIG Atomic Operations ECN (mirror) —
  <https://www.yumpu.com/en/document/view/10780137/atomic-operations-ecn-pci-sig>
- AMD ROCm, "How ROCm uses PCIe atomics" —
  <https://rocm.docs.amd.com/en/docs-6.3.2/conceptual/pcie-atomics.html>
- Graham et al., SHArP, COMHPC 2016 —
  <https://dl.acm.org/doi/abs/10.5555/3018058.3018059>
- The Next Platform, 2023-12-01 (SHARP is InfiniBand-only) —
  <https://www.nextplatform.com/2023/12/01/if-you-want-to-sell-ai-to-enterprises-you-need-to-sell-ethernet/>
- NVIDIA MLNX_OFED "Advanced Transport" (XRC, DC, atomic reply endianness) —
  <https://networking-docs.nvidia.com/mlnxofedswum/590590/advanced-transport>
- Subramoni et al., DC transport in MPI, ISC 2014 —
  <https://link.springer.com/chapter/10.1007/978-3-319-07518-1_18>
- OpenSHMEM 1.6, §3.2, §9.3.5, §9.7, §9.12 (local PDF).
- This tree at `7ff9343`; commits `1cb1770`, `3c19880`, `de540da`, `02279fa`,
  `aa4fc27`; first-round `roce-transport.md`, `core-api.md`,
  `evidence/q2/q2-ofi.md`, `q2-ucx.md`, `q2-future.md`.
