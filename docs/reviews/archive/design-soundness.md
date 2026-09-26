# Design soundness review: `docs/design/shmem-ethernet-design.md`

**Date:** 2026-09-16. **Design at:** commit `9475e45`. **Target:** OpenSHMEM 1.6.

**Method.**

1. **Review.** Five reviewers worked in parallel: transport (§3), SHMEM semantics (§4, §7), collectives and teams (§5), bootstrap/lifecycle/testing (§6, §3.15, §8), and an omissions red team working from the spec.
2. **Cross-check.** The lead read the full design and checked the load-bearing facts.
3. **Refutation.** Every finding rated *design change needed* or worse went to a fresh refuter whose job was to disprove it: reproduce the failure, find doc text that already handles it, or find spec text that permits the behaviour.

Severities below are **after** refutation. Reviewer reports, refutation reports, models and experiment code are in [`evidence/`](../evidence/). Where a report names a session-scratchpad path (`scratchpad/exp/<area>/`), look in `evidence/<area>/` instead.

**Evidence labels.**
- **documented** — spec section or URL.
- **observed** — experiment or source code.
- **recalled** — from memory, unverified.
- **lead-verified** — the lead re-checked it personally.

**Limits.**
- The 3-node collectives run with L=(2,1,2) BRR was stopped before it finished.
- Two refuter VM commands were blocked by a permission prompt. Those claims rest on reading kernel source or the reviewer's existing measurements; each report says which.
- BXI hardware internals and uTofu details are thin; that matters for [core-api.md](core-api.md), not for this document.

---

## Verdict

**Proceed, after specific changes.** The central architecture is sound:
- one ordered TCP stream per node pair, applied in order;
- intranode shared memory;
- a deterministic, world-collective allocator;
- cumulative epoch counters for collectives, which an exhaustive model checked across 1.24 million states;
- completion by dirty set and acknowledgement.

The §3.7 self-deadlock argument, the one-in-flight-send rule, the receive-side completion-queue formula and the kernel compare-and-wait wakeup all hold up under experiment.

**Two gaps block implementation as written:**

1. **No process is named to host the per-node progress threads (B1).** Every mechanism that crosses a process boundary depends on that choice.
2. **The design assumes one heap shared by all local PEs (B2).** The spec requires a heap per PE, so as written a put to one PE overwrites another PE's data.

Both can be fixed without changing the design's character.

Behind them is a set of real but local defects: the reservation-protocol races, the missing ack path, the idle-loop futex leak, bootstrap discovery, completion bookkeeping, and error propagation. Each has a known fix, several of them already model-checked.

**Phase 1 should be resequenced** so a single-node build works before any network code (see *Sequencing*).

---

## Findings

Each finding lists: **Where** · **Failure** · **Evidence** · **Severity** · **Confidence** · **Refutation**.

### Blocking

#### B1. No process is named to host the progress threads

- **Where.** §3.12 ("a small fixed pool per node"); §3.3 (sole issuer); §4.6 (`bytes_landed`: "progress thread adds as chunks land"); §4.7 ("Write replies directly into the destination"); §1 ("PEs are processes").
- **Failure.** PE 3 calls `shmem_getmem(buf_on_stack, …, pe_on_B)`. The reply arrives at node A's progress thread, which runs in some other process and cannot write PE 3's stack. Fetching-AMO results have the same problem.

  The obvious hosts each break something the doc relies on:
  - *A thread in every PE* contradicts §3.1 (one stream per node pair) and §3.12 ("not one thread per PE").
  - *Threads in the local lead* force a copy-out that stalls receive for the whole node pair and reopens the §3.8 memory argument. Worse, pausing the lead in a debugger — the §3.14 hang-diagnosis workflow — or losing it to a crash cuts the whole node off.
- **Evidence.**
  - Spec §9.6.1.5: `dest` is "Local address of the data object to be updated". Spec §9.7: fetching AMOs return after the value is "delivered to the calling PE". (documented)
  - Sibling `process_vm_writev` succeeds at Yama `ptrace_scope=0` and fails with EPERM at 1 (observed, `evidence/semantics/cma.c`).
  - Found independently by three reviewers: transport B1, semantics B1, omissions O1.
- **Severity.** Blocking. **Confidence.** High.
- **Refutation.** Survives as blocking (`evidence/refutation/refute-host.md`). No doc text names a host.
  - The get-reply sub-claim drops to *design change*: phase 1 has only blocking gets, and §4.7 says "where possible".
  - The "PE-side submit contradicts single issuer" sub-claim drops to *clarification*: §3.5 and §3.10 imply that the PE seals and wakes, and only the progress thread submits.
  - The refuter also found a contradiction inside §3.12: "the origin side never needs a helper" assumes a PE can receive its own replies.
- **Fix direction.**
  - Record a host. The transport reviewer suggests a per-node **helper process** forked by the local lead, owning rings, sockets and provided buffers, with its lifetime tied to local PEs by pidfds.
  - Add per-PE shared **landing zones** sized by the §4.6 outstanding-get resource. The origin PE copies into `dest` itself.
  - Make fence and quiet a request to the owning shard (a flag plus a wake) rather than a submit.

#### B2. One heap is shared by all local PEs; the spec requires one heap per PE

- **Where.** §4.1 ("Shared memfd/hugetlbfs segment, mapped by every local PE. Intranode put/get collapses to memcpy"); §4.4 ("every local PE writes identical bytes to the same shared pages"); §4.2 (identical virtual addresses, "raw pointer arithmetic"); §4.9 (`watch_find(node->watch, addr)`).
- **Failure.**
  - Two PEs on one node each allocate `x`. Under §4.4 both allocations are the same shared bytes. `shmem_putmem(x, a, 8, pe1)` and `shmem_putmem(x, b, 8, pe2)` then write the same memory, and `shmem_ptr(x, pe1) == shmem_ptr(x, pe2)`. PE 1 observes PE 2's data.
  - Once heaps are made per PE, "identical virtual address" can hold only for a PE's *own* heap; sibling heaps sit at different addresses in each process.
  - The §4.9 watch table then keys on an address that differs between waiter and writer, so cross-process wakes are lost.
  - Also unaddressed: L × `SHMEM_SYMMETRIC_SIZE` of virtual address space in every process, and reserving that much memory.
- **Evidence.**
  - Spec §8: `SHMEM_SYMMETRIC_SIZE` is "the size (in bytes) of the symmetric heap memory per PE". Spec §9.2 item 2: "Each PE has a single symmetric data segment and symmetric heap". (documented)
  - The §4.4 wording (lead-verified).
  - Model variant V6 loses a wake when the writer's key differs from the waiter's (`evidence/semantics/watch_model.out`).
- **Severity.** Blocking. The reviewers rated it design change; the lead raised it after the refuter showed §4.4 states the single-heap model explicitly. **Confidence.** High.
- **Refutation.** Survives (`refute-semantics.md` D1/D2). "(region id, offset)" does not imply a heap per PE: §4.4 says "same shared pages".
- **Fix direction.**
  - Lay out L heaps in the node segment.
  - Keep a per-process `base_of(pe)` table: O(local PEs), which the §9.3 implementer note allows.
  - Build intranode puts as `base_of(pe) + offset`.
  - Key the watch table by `(pe, region, offset)`.
  - Agree `SHMEM_SYMMETRIC_SIZE` across PEs at rendezvous.

### Design change needed

#### D1. The reservation protocol loses, tears and reorders messages

- **Where.** §3.5 snippet and buffer states; §3.4 ("one filling, one in flight", stated as sizing, not as a rule); §3.8 DRAINING→OPEN; §9.
- **Failure.**
  1. **State check, then CAS.** A writer reads `buf->state == OPEN` as a plain field. The progress thread then seals, sees `filled == head`, and submits. The writer's CAS on `head` still succeeds.
     - The message is lost, or sent before its copy completes.
     - `filled > head` in the next generation.
     - The "generation numbers are self-stabilizing" argument (§3.5) is false as written.
  2. **Two reservable buffers.** When the send completes, the filling buffer is often empty, so it is not sealed, and the freed buffer goes DRAINING→OPEN.
     - Two buffers now accept reservations.
     - A fence seal can put a PE's *later* op, carrying a higher sequence number, on the wire first.
     - That breaks fence, signal-after-payload (invariant 5) and `quiet`, which can return while an op is still unsent.
- **Evidence.**
  - Reviewer's exhaustive model `evidence/transport/resv_model.py`: LOST, TORN, ACCT, STALESEQ and ORDER violations.
  - Refuter's independent bounded-exhaustive model `evidence/refutation/refute_model.py` reproduces both, including a stale writer reserving inside a DRAINING buffer.
  - The repaired protocol passes in both models with zero violations, including DRAINING, progress-thread reply reservations and a liveness check.
- **Severity.** Design change needed. The reviewer rated it blocking; the refuter downgraded it because the fixes are local. **Confidence.** High.
- **Refutation.** Survives (`refute-reservation.md`).
- **Fix direction.** This keeps "two atomics, no locks":
  - Seal by setting a bit in the `head` word via CAS.
  - Writers never read `state`.
  - At most one buffer per connection is reservable.
  - Sequence numbers are assigned when a buffer opens and written into the header at seal.
  - List these rules in §9.

#### D2. One-way traffic never gets an acknowledgement

- **Where.** §3.4 (piggybacked ack); §3.8 item 3 ("piggybacked, so not really a queue entry"); §3.9 (the explicit flush is on the *sender*); §4.6 `quiet`.
- **Failure.** Node A only puts to B. No B→A segment exists to carry the ack. The safety sweep submits only sealed buffers. `quiet`, `barrier_all` and finalize's implicit barrier on A wait forever.
- **Evidence.** The refuter searched §3.4–§3.14 and found no ack-only path; §3.14 mentions synchronous acks only for slow mode (lead-verified).
- **Severity.** Design change needed. **Confidence.** High.
- **Refutation.** Survives (`refute-uring.md` D4).
- **Fix direction.** A header-only ack segment sealed by the progress thread. The header is never reserved, so invariant 1 holds. Trigger it with an ack-request flag set by the sender's fence/quiet flush.

#### D3. The idle loop accumulates armed futex waits and breaks the CQ bound

- **Where.** §3.10 code; §3.3 CQ formula; invariant 12.
- **Failure.** Every idle pass arms a new `IORING_OP_FUTEX_WAIT` while the earlier ones are still armed. One `futex_wake(INT_MAX)` completes them all at once, overflowing the completion queue through the kernel overflow path §3.3 explicitly rejects.
- **Evidence.**
  - Reviewer: 1,000 armed waits posted 1,000 CQEs into a 64-entry CQ (`evidence/transport/c/e1_futex_accum.c`).
  - Refuter, with real TCP receive: 1,881 waits armed, and the kernel overflow flag set. The reviewer's run had shown the flag clear only because liburing had already flushed the overflow.
  - With an "armed" flag: at most 1 armed wait, no overflow, no lost wakeup (`evidence/refutation/uring/r1_idle.c`, `vm-output.txt`).
- **Severity.** Design change needed (small fix). **Confidence.** High.
- **Refutation.** Survives.
- **Fix direction.** An `armed` flag cleared on the futex CQE, plus one futex CQE per ring in the formula.

#### D4. Bootstrap discovery and the node map are wrong as written

- **Where.** §6.1 table; §6.2.
- **Failure.**
  1. **Rank 0 cannot be found.** "Rank 0 publishes its resolved address" has no channel: no KVS, no PMI, no shared filesystem. Rank 0 is not on step node 0 under topology-ordered or arbitrary distributions, and "retry on bind conflict" leaves its port unknown.
  2. **Job-level versus step-level node lists.** `SLURM_NODELIST` is the *job's* node list, while `SLURM_NODEID` is relative to the *step*. Inside `sbatch -N4`, `srun -N2 -r2` on `pi03` has `NODEID=0`, which maps to `pi01`.
  3. **The environment has no node map.** `SLURM_TASKS_PER_NODE` gives counts, not placement. Under cyclic, plane, arbitrary or topology-ordered distributions, ranks are not contiguous per node, local member counts come out wrong (§5.2), and barrier targets are never reached.
  4. **The §8.1 test map misses this.** Its "non-contiguous" map `0,0,0,1,2,2` is actually contiguous.
- **Evidence.**
  - Slurm source: `src/common/env.c`, `slurmstepd_job.c`, `task.c`, `slurm_step_layout.c`. (observed)
  - srun.html: cyclic example; "If a topology plugin is configured … tasks are distributed following topology order". (documented)
  - Reports: `evidence/reports/q1-bootstrap.md` B1–B3; `evidence/refutation/refute-bootstrap.md`.
- **Severity.** Design change needed. The reviewer rated it blocking; the refuter downgraded it because a KVS-free fix exists. As written, a multi-node job cannot start. **Confidence.** High.
- **Refutation.** Survives.
- **Fix direction.**
  - Listen on the task with `NODEID==0 && LOCALID==0`, at `hostlist_first(SLURM_STEP_NODELIST)`. `SLURM_NODEID` and `SLURM_STEP_NODELIST` come from the same list.
  - Derive the port from a deterministic candidate sequence hashed from job and step IDs, and put those IDs in the handshake.
  - Build the rank→node map from each node's `SLURM_GTIDS` at rendezvous.
  - Use `SLURM_STEP_*` variables throughout.
  - Alternative: a PMI-2 *wire* client over `PMI_FD`, which links no library (wire details recalled).

#### D5. Rendezvous fan-in scales with PE count

- **Where.** §6.2 ("every other PE connects").
- **Failure.**
  - About 77,000 connections into one listener at 256 PEs × 300 nodes. That violates §1's rule that "nothing may scale with PE count where it can scale with node count".
  - The soft `RLIMIT_NOFILE` of 1024, propagated by Slurm, fails the listener at the 1,021st connection.
- **Evidence.**
  - Loopback burst of 60,000 connects: 55,902 listen overflows, 19.7 s (`evidence/bootstrap/burst/burst.c`).
  - The refuter could not rerun it and warns that the ">300 s" worst-case rows are loopback-specific and should not be quoted as expectations.
- **Severity.** Design change needed. **Confidence.** High.
- **Refutation.** Survives.
- **Fix direction.**
  - Only `LOCALID==0` connects over TCP to the rendezvous.
  - Local PEs meet on a node-local socket.
  - Retry with jittered backoff under an `OSOE_BOOTSTRAP_TIMEOUT`.
  - Detect a bare `sbatch` launch by the absence of `SLURM_STEP_NUM_TASKS`.

#### D6. Fence and quiet must cover memory stores

- **Where.** §4.8 ("`fence` and `quiet` do not order them. Completion routines do not apply"), which cites spec §3.2 item 4.
- **Failure.** With the §8.1 force-remote flag on:
  1. `putmem(x,a)` is queued over TCP.
  2. `shmem_fence()` only seals.
  3. `*shmem_ptr(x,1) = b` stores immediately.
  4. The put then lands, leaving `x == a`: fence ordering is violated.

  In normal mode `barrier_all` happens to conform, because its arrival `fetch_add` is a lock-prefixed, serializing instruction. The documented rationale is still wrong.
- **Evidence.**
  - Table 14 (§9.12) marks "Memory Store" under both Fence and Quiet.
  - `SHMEM_CTX_NOSTORE` says quiet and fence on such a context "are not required to enforce completion and ordering of memory store operations"; the default context has no options.
  - §9.10.1: `barrier_all` "ensures completion of all previously issued memory stores".
  - Lead-verified against spec text. The refuter found no spec text narrowing "memory store".
- **Severity.** Design change needed (small). **Confidence.** High.
- **Refutation.** Survives (`refute-semantics.md` D5).
- **Fix direction.**
  - `shmem_ptr` returns NULL wherever intranode traffic is force-routed remote.
  - Quiet, fence, pe_quiet and barrier each execute one serializing operation in the calling thread; the spec limits this to the calling thread (§9.12.1/§9.12.2 Notes).
  - Correct the §4.8 text.

#### D7. Memory-order discipline: require `<stdatomic.h>`, and name the order at every access

- **Where.** §3.5 ("acquire on the CAS, release on the `filled` increment"; "Omitting them produces corruption visible only under load and never on x86"); §4.9 ("release on the waker's stores, acquire on the waiter's loads"); §8.3.
- **Failure.**
  - The §4.9 waiter (count++, then load ivar) and waker (store ivar, then load count) form a Dekker pattern. Each side's store must be visible to the other side's later load.
  - On x86-64, GCC compiles a `memory_order_release` store to a plain `mov`, which does not serialize. A waiter's plain store into the table, or `shmem_signal_set` implemented as a release store (phase 1), then loses a wake: refuter model R-V5x.
  - §4.9 names only two of the four accesses, and the doc's "never on x86" is wrong.
- **Evidence.** Lead experiment (`evidence/lead/mo_check.c`, `mo_x86.c`), GCC 14.2:

  | Target | release store | seq_cst store | acquire load | relaxed load | fetch_add |
  |---|---|---|---|---|---|
  | aarch64 | `stlr` | `stlr` | `ldar` | `ldr` | LSE helper (`__aarch64_ldadd4_*`) |
  | x86-64 | `mov` | `xchg` | `mov` | — | `lock xadd` |

  Semantics model V4/V4b/V4c lose wakes and V5 does not (`evidence/semantics/watch_model.out`). The refuter re-checked codegen in the VM.
- **Severity.** Design change needed. The refuter rated it clarification/minor for aarch64; the lead keeps design change because the x86 CI leg is in scope and a phase-1 routine is affected. **Confidence.** High on codegen and the model; medium on ARM cross-core semantics, which were not measured on hardware.
- **Refutation.** Survives in narrowed form.
- **Fix direction (project decision taken during this review).**
  - osoe **requires `<stdatomic.h>`**. Every access to a shared protocol word uses an `atomic_*_explicit` call with a **named `memory_order`**. Implicit-order calls and `memory_order_relaxed` are forbidden on protocol words.
  - `acquire`/`release` is enough for single-writer publication, such as §3.5's `filled`.
  - **Dekker-shaped protocols** (§4.9 insert versus store, signal set versus wait) must use `seq_cst` or a locked atomic (`fetch_add`, CAS) on both stores.
  - Configure fails if `__STDC_NO_ATOMICS__` is defined.
  - Outline atomics link statically from `libgcc.a`; a test binary's `NEEDED` list is only libc (lead-verified). So this stays within invariant 15.

#### D8. DRAINING starves local PE writers under sustained reverse get load

- **Where.** §3.8 starvation guard.
- **Failure.** When outstanding reply bytes exceed roughly (round-trip generations + 1) buffers, every freed buffer fills with replies, and local PEs' puts to that peer never go out. Example: 16 PEs issuing 1 MB gets at a 5-generation delay.
- **Evidence.** Reviewer's segment-level simulation (`evidence/transport/draining_sim.py`). The refuter reran it with a realistic 1–2 generation round trip: local PE share is still 0.00 at 8 or more PEs × 64 KB, or 256 × 4 KB.
- **Severity.** Design change needed. **Confidence.** Medium-high.
- **Refutation.** Survives.
- **Fix direction.** Cap each DRAINING pass at a fraction of the buffer.

#### D9. Error propagation, `shmem_global_exit`, and non-last finalize

- **Where.** §3.15, §6.4; spec §9.1.4, §9.1.6.
- **Failure.**
  - **`shmem_global_exit`** appears nowhere in the design. §9.1.6 requires every PE to terminate "regardless of their current execution state", including PEs in pure computation.
  - **Non-last `shmem_finalize`.** It must be "semantically equivalent to shmem_barrier_all" (§9.1.4). §6.4 only decrements a count, so completion is skipped and the §5.4 epoch counts diverge.
  - **A crashed PE.** A crash mid-reservation stalls its segment while the TCP connection stays healthy, so "abort loudly" never fires. Slurm's `KillOnBadExit` defaults to 0.
- **Evidence.** Spec §9.1.4 and §9.1.6 (documented); slurm.conf.html (documented); `src/srun/launch.c` (observed).
- **Severity.**
  - Global exit and finalize: design change needed.
  - Crash detection: downgraded by the refuter to clarification, since the spec does not specify crash behaviour and `srun -K1` exists.
- **Confidence.** High.
- **Refutation.** Survives.
- **Fix direction.**
  - An abort AM flooded to all nodes, plus a node-segment abort word observed by every PE wait.
  - A per-PE watcher that calls `exit(status)`, so the exit status survives.
  - A barrier in non-last finalize.
  - Local PE liveness via pidfds on the ring.
  - Keepalive plus `TCP_USER_TIMEOUT`.
  - Document `srun -K1`.

### Clarification needed

Most rows below were rated *design change* or *blocking* by a reviewer and downgraded on refutation; the **Refutation** column records the verdict.

| # | Where | Point | Refutation |
|---|---|---|---|
| C1 | §3.2, §3.3 | **Receive reassembly across provided buffers is undesigned.** Holding buffers for an unfinished segment stalls once the segment outgrows the ring. The refuter also reproduced a real two-connection deadlock on one shared ring, with each connection holding buffers for partial segments (`refutation/uring/r3_shared_ring.c`). State the invariant "every provided buffer is recycled before the next reap", and parse as a stream with a per-connection cursor. | Downgraded from blocking; the doc implies a streaming parse (§3.7, §3.15) |
| C2 | §4.6, §9.2 | **No thread level is chosen, and completion bookkeeping follows from it.** `shmem_init_thread`/`query_thread` are not in phase 1. `bytes_landed == bytes_issued` compares 64-bit counters that cannot be futexed, so a 32-bit wake word is needed (the invariant 10/18 pattern). Under MULTIPLE, the equality wait can starve and `ndirty = 0` races. Blocking gets and fetching AMOs need per-operation completion slots, which B1's landing zones provide. | Downgraded from blocking; §9.2.1 Notes let phase 1 provide SINGLE. The downgrade does not extend to D1/D7, which are cross-process |
| C3 | §4.9 | **Watch-table slot lifecycle.** A naive refcount (V8) loses wakes. Linear-probe deletion loses a wake even with one waiter per key (refuter R-V9a). One slot per local PE, or the model-checked V8fix/V9fix rules, removes both. | Downgraded from design change |
| C4 | §3.11 | **Futex word sharing and the handoff bit.** A literal reading lets buffer-space and descriptor waiters share one word, so wake-one can reach the wrong waiter (`transport/c/e3_futex_order.c`). §3.10 implies deadlines, so the result is a latency spike, not a hang. Specify one word per resource class, and model the handoff bit's word layout. | Downgraded; the cross-shard half is refuted (`ep->wake` is per shard) |
| C5 | §3.8, §4.6 | **Deferred-queue memory.** The "12 MB" figure is per connection: about 6.3 GB per node at 500 nodes if pre-allocated per connection. §4.6's pool of "thousands" gives about 100 MB. The doc gives the descriptor as 48 B in §3.8 and 16 B in §4.6; pick one, and use a node-wide pool. | Downgraded |
| C6 | §3.7, §3.12 | Collective forwards cross progress-thread shards, so the control queue must be multi-producer, with a wake to the owning shard. This only matters with more than one progress thread per node. | Downgraded |
| C7 | §5.5, §3.8 | **Reduction result forwarded by reference races the lead leaving.** It fails when the downward drain happens after the local `released++` (`evidence/collectives/`, fwd=after). Count downward forwards in `copied`, or finish them before the bump. Deferred with reduce. | Downgraded from design change |
| C8 | §5.5, §7 | The lead's-`dest` accumulator does not generalise to alltoall[s], scans (wrong when node membership is not contiguous in rank) or variable-`nelems` collect. "Three counters, no per-team data area" and "scan: nothing architectural" are overstated. | Downgraded |
| C9 | §5.7 | The **team range allocator** is referenced but never defined, and neither is the split protocol (id agreement, claiming counter blocks). It becomes blocking once team creation lands. (The spec's "12 teams" split_2d example is a spec typo; its own grid shows 7.) | Downgraded; open gap |
| C10 | §3.8, §5.7 | **What `max_teams` counts.** The per-connection bound is distinct teams whose trees use that edge, and `split_2d` can put L of them on one node pair while each PE holds 2. Counted per PE, the §3.8 assert fires on conforming programs. | Survives as clarification |
| C11 | §5 | **Unstated assumption behind the exhaustive model:** a child node's partial result is merged only after the local lead has enqueued its arrival. Merging earlier loses updates the model cannot show. Also state the single-node-team release path, and "write final `dest`, then bump `released`". | New, from the refuter |
| C12 | §4.3, §7 | Rejecting statics is a known **non-conformance**: most spec examples use `static`, including Ex. 5 and the canonical put_signal Ex. 27. Define detection (a range check; `shmem_addr_accessible` returns 0), and schedule the compiler wrapper. | — |
| C13 | §4.4 | Allocator edge semantics (§9.3): malloc barriers on *exit*, free on *entry*; size 0 returns NULL with **no barrier**; calloc overflow; the phase for calloc/realloc/align. The debug checksum cannot ride the dataless §5.3 barrier. `SHMEM_SYMMETRIC_SIZE` needs a grammar and agreement across PEs. | — |
| C14 | §7 | **Mandatory surface absent from every phase:** `shmemx.h` (must exist), `start_pes` with implicit finalize, deprecated names and `mpp/shmem.h`, `SMA_*`, `SHMEM_VERSION/INFO/DEBUG`, C11 generics, `pe/addr_accessible`, `signal_fetch`/`signal_wait_until`, the profiling interface (`pshmem_*`, `pshmem.h`, `shmem_pcontrol`), `SHMEM_TEAM_SHARED` (required, equal to the §8.1 locality set), and the exact behaviour of context/team stubs (a `ctx_create` that always fails conforms). | Profiling "export conflict" refuted (see below) |
| C15 | §4.1, §8.1 | Getting the segment to sibling PEs: `shm_open`/hugetlbfs names (keyed by job.step.node), or `/proc/<pid>/fd`. Crash cleanup. Huge-page fallback (the VM has `HugePages_Total: 0`). | Downgraded; no UNIX socket required |
| C16 | §6, §8.2 | Bootstrap testing: no tier feeds real `SLURM_*` values. Add captured `srun` environment fixtures (block, cyclic, plane, arbitrary, `-r`/`-x`, `-E`, het, bare `sbatch`) and a cyclic node-map pole. Test node-pair connection direction and the handshake. | Downgraded |
| C17 | §3.2, §3.3 | The head-of-line bound for control messages is up to two segments plus any stall, not "one chunk". Multishot recv can end on any CQE without `F_MORE`. The SQ bound must count futex, cancel and re-arm SQEs. Sequence-number timing (see D1). | — |
| C18 | §3.11, §4.2, §6.4 | Slurm CPU binding versus "cpuset isolation for the progress thread". Heterogeneity: byte order, `long double`, and page size on mixed Pi 4 (4K) and Pi 5 (16K) clusters. Concurrent init needs a once-gate. Fork children must not run teardown. | — |

### Minor

- **Tests.**
  - Invariant 7 cannot fail while every process maps at the same address; add a heap-base-shift test mode.
  - Tests have no timeouts.
  - The probe asserts `ncqe > 1`, not the verified 8 + 1 shape.
  - There is no `NEEDED`-libraries test for invariant 15.
- **CI.** The §8.5 row names `ubuntu-24.04`, which ships liburing 2.5 (lead-verified at packages.ubuntu.com). `ubuntu-26.04` runners, with liburing 2.14, exist but are marked preview. No `.github/` exists yet, so only the doc row is wrong. (Downgraded from blocking.)
- **Bare `sbatch` launch** hangs; add a detector and a timeout (see D5). (Downgraded.)
- **Citations** (lead spot-checked):
  - The §9.10.9 quote is truncated inside its quotation marks.
  - Change #19 covers only `test_all`/`test_all_vector`; on an empty set `test_any` returns `SIZE_MAX` and `test_some` returns 0.
  - The `shmem_pe_quiet` signature is wrong: the spec has `(const int*, size_t)` plus `shmem_ctx_pe_quiet`.
  - The §9.4.8 quote is not verbatim.
  - §9.6.1.4/§9.6.1.8 are swapped between ibput and ibget.
  - "Yama `ptrace_scope` defaults to 1": the Debian 13 VM has 0.
- **Consistency.**
  - §3.13 exchanges the checksum "through PMI", contradicting §6.
  - §3.10 should use `io_uring_submit_and_wait_timeout`.
  - `SHMEM_` versus `OSOE_` environment prefixes.
  - §8.5 says `limactl shell osoe`, but the instance is `shmem`.
- **Overclaims.**
  - §4.1's atomicity does not hold for multi-byte puts or mixed-width AMOs.
  - "Wraparound harmless" needs the stated margin, M ≥ L on `arrived`.
  - "Weak memory ordering is exercised on every run" means *possible*, not *observed*.
- **Scattered.**
  - Phase-1 AMOs are all uint64, so the 32-bit wait path is never exercised.
  - The progress thread must block asynchronous signals.
  - Raise the soft `RLIMIT_NOFILE` to the hard limit.
  - §5.7 overstates that team-destroy scope is ambiguous: INVALID handles for non-members plus spec Example 12 fix it to members.

---

## What is sound

| Claim | Evidence |
|---|---|
| **§5.4–§5.5 epoch counters**: equality waits, `!is_lead` guard, wake counts, "whichever increment hits target" | Exhaustive BFS with kernel futex semantics and nondeterministic wakes. L=1–3; 3–4 mixed barrier/reduction epochs; 1–3 nodes (chain and star); spurious-wakeup variants; moduli down to the M ≥ L boundary. All pass, up to 1.24M states. The checker catches 4 injected mutations. Re-run by the refuter. (`evidence/collectives/`, `refute-collectives.md`) |
| **§3.7 self-deadlock argument** | Holds; no cross-node cycle forms while neither progress thread stops consuming receive. Conditional on C1. |
| **One in-flight send per connection** is necessary | A 4 MB io_uring TCP send completed short (32,741 B); small `SO_SNDBUF` gave short sends 20 of 20, so tiers 2/3 reach the path. |
| **§3.3 receive-side CQ formula** | Probe shape reproduced (`cqes=9 with_F_MORE=8`, then `-ENOBUFS`); ring limits in `io_uring/kbuf.c`. Needs +1 futex CQE (D3). |
| **§3.10 kernel compare-and-wait** | Stale value returns `-EAGAIN`. A cross-process `futex_wake` completes an `IORING_OP_FUTEX_WAIT` on a shared memfd mapping — new evidence; the probe tested only the private case. |
| **§3.11 waiter order** (the doc's "verify" note) | `kernel/futex/core.c` on 6.12: RT waiters first, then FIFO. In the VM a SCHED_FIFO waiter jumped the queue, which supports "no real-time priority anywhere". |
| **§3.5 CAS instead of fetch_add; `filled` as a count** | The repaired protocol keeps both and passes two independent models. |
| **§4.9 ordering rules** (insert before check; load seq before ivar; store ivar before bump) | Model V0/V7 find no lost wake; each deliberate reversal (V1–V3) loses one. Under sequential consistency the "nonempty check before insert is visible" race cannot occur. |
| **§4.4/§4.5 world-collective deterministic allocator**, no cross-process lock | Spec §9.3.1–§9.3.6 "collective operation on the world team" (G.1 #10); §9.4 forbids concurrent collectives on one team within a PE. |
| **§5.6 barrier versus sync** | §9.10.1 Notes, verbatim. |
| **§5.5 push-not-pull** | The sentence is verbatim in §9.10.5 and §9.10.7–.10, matching G.1 #14. Collective buffers are symmetric throughout §9.10 (lead-verified), so the shared-mapping precondition holds. |
| **Per-team `k`/`d` under multithreaded PEs** | §9.4 and §9.10 intro: one collective per team at a time; different teams may run in parallel. |
| **§4.6 gets not in fence; `_nbi` complete at quiet; blocking cap on outstanding gets** | Table 14; §9.6.2.2; the spec sets no bound on outstanding operations. |
| **`shmem_team_ptr`**, sessions as no-ops, `malloc_with_hints` as alias, no Fortran | §9.1.10; §9.9; §9.3.5; G.2 #18. |
| **`SLURM_PROCID`/`LOCALID`/`NODEID`** | Step-relative, dense, and set regardless of `--mpi` (Slurm `task.c`, `slurmstepd_job.c`). |
| **Tier-3 environment** | netns, veth and `sch_netem` (delay, loss, reorder, MTU 576) work in the VM kernel, including unprivileged. Caveat: netem reorder is invisible to TCP applications. |
| **Build and dependency budget** | debug/release/asan/tsan presets build and pass 3/3 with no warnings. `RLIMIT_MEMLOCK` is not a problem for rings or provided-buffer rings on 6.12, even at limit 0. Outline atomics add no link dependency. |
| **§3.15 policy** ("abort loudly") | Right policy; detection and propagation are the gap (D9). |
| **Open question "does any team query return remotely accessible memory"** | Closed: none does. |

---

## Sequencing

§7 phase 1 makes every routine depend on the whole transport, bootstrap and io_uring stack at once, so nothing works until everything does. Both blocking findings and D1–D5 sit in that machinery. Suggested order:

1. **Decide on paper, and model-check.** Record:
   - the host process and landing zones (B1);
   - the per-PE heap layout and watch key (B2);
   - the phase-1 thread level (C2);
   - the memory-order discipline (D7);
   - the bootstrap scheme (D4, D5).

   Model-check the repaired reservation protocol, the handoff word and the watch slot lifecycle; the harnesses are in `evidence/`.
2. **Single node, no network.** Functional on one Pi, no TCP:
   - `init`/`finalize` (with the non-last barrier), `my_pe`/`n_pes`;
   - `malloc`/`free` over per-PE heaps;
   - intranode `putmem`/`getmem` as memcpy;
   - AMOs, `wait_until`/`test`, signal ops;
   - `shmem_ptr`/`team_ptr`, `SHMEM_TEAM_SHARED`;
   - `barrier_all` on intranode counters;
   - `shmemx.h` and stubs.

   This exercises the heap layout, watch table, allocator and epoch counters.
3. **Tier-1 in-process AM backend, slow mode first.**
   - Internode put/get/AMO with landing zones.
   - `quiet`/`fence`/`pe_quiet` with ack-only segments.
   - `put_signal`.

   This validates semantics before any socket exists.
4. **Real transport.**
   - io_uring with the repaired reservation protocol.
   - Stream-parsed receive and the armed-flag idle loop.
   - Per-class wake words.
   - Multi-node bootstrap with a timeout.
   - Internode barrier.
5. **Robustness.**
   - Liveness watch, abort propagation, `shmem_global_exit`, keepalive.
   - Slurm environment fixtures and a heap-base-shift test mode.

---

## Refuted or withdrawn

- **Profiling interface conflicts with the export policy** (omissions O8). **Refuted:** `pshmem_*` and `shmem_pcontrol` are spec API names, so the "only spec API symbols" rule already admits them. `cmake/CheckExports.cmake` needs its pattern widened from `^shmem_` to `^p?shmem_`, and weak aliases already pass. The missing interface itself survives in C14.
- **"GCC ignores `memory_order` on plain loads and stores"** (semantics sub-claim). **Refuted** by lead experiment: `relaxed` compiles to `ldr`/`str`, while `acquire`/`release` compile to `ldar`/`stlr`. The reviewer had compared `acquire` with `seq_cst`, which compile identically.
- **Cross-shard `ep->wake` loses submissions** (transport D2, half). **Refuted:** the wake word sits beside the per-thread ring, so it is per shard.
- **"A writer must wait rather than try the other buffer"** (transport B3's extra rule). **Unnecessary:** "at most one reservable buffer" suffices (`refute-reservation.md`).
- **Suspected `RLIMIT_MEMLOCK` failure** for provided-buffer rings. **Refuted:** rings were created even at `ulimit -l 0`.
- **"The progress thread may reserve only in DRAINING buffers"** (transport side claim). **Refuted** by §3.8 text.
- **Memfd sharing requires a UNIX socket** (bootstrap B5). **Refuted:** a named `shm_open` or hugetlbfs object, which §4.1 and §8.1 allow, is enough.
- **Downgrades from the reviewers' ratings** are listed in the Refutation column and fields above.

---

## Sources

- **OpenSHMEM 1.6 specification:** `docs/reference/OpenSHMEM-1.6.pdf` (`pdftotext -layout`); sections cited inline.
- **Slurm documentation** (fetched 2026-09-16):
  - https://slurm.schedmd.com/srun.html
  - https://slurm.schedmd.com/sbatch.html
  - https://slurm.schedmd.com/heterogeneous_jobs.html
  - https://slurm.schedmd.com/slurm.conf.html
  - https://slurm.schedmd.com/mpi_guide.html
- **Slurm source** (https://github.com/SchedMD/slurm, master):
  - `src/common/env.c`, `src/common/slurm_step_layout.c`, `src/common/hostlist.c`
  - `src/slurmd/slurmstepd/{slurmstepd_job.c,task.c,mgr.c}`
  - `src/srun/{launch.c,srun_job.c}`
  - `src/plugins/mpi/pmi2/{mpi_pmi2.c,setup.c}`
- **Linux 6.12.y stable** (git.kernel.org): `io_uring/{io_uring.c,net.c,kbuf.c,futex.c}`, `kernel/futex/{core.c,waitwake.c}`.
- **Ubuntu packages:** https://packages.ubuntu.com/noble/liburing-dev (2.5-1build1); https://packages.ubuntu.com/resolute/liburing-dev (2.14-1).
- **GitHub runner images:** https://github.com/actions/runner-images (Ubuntu 24.04 and 26.04 READMEs).
- **Experiments:** Lima VM, Debian 13, kernel `6.12.95+deb13-cloud-arm64`, liburing 2.9, GCC 14.2 (aarch64 and x86-64 cross).
  - Code and outputs: [`evidence/`](../evidence/).
  - Reviewer reports: [`evidence/reports/`](../evidence/reports/).
  - Refutation reports: [`evidence/refutation/`](../evidence/refutation/).
