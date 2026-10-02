# Q1 — Transport review of design §3 (AM layer), §4.6, §9

Reviewer: TRANSPORT. Design: `docs/design/shmem-ethernet-design.md` (1,712 lines, read in full).
Environment: Lima VM `shmem`, Debian 13, kernel `6.12.95+deb13-cloud-arm64`, liburing 2.9, gcc 14.2.

Experiment root (not in repo): `/private/tmp/claude-501/<repo, dash-encoded>/44d302eb-d705-4657-bc09-de98d0225328/scratchpad/exp/q1-transport/`
(abbreviated below as `EXP/`)

| Artifact | Path |
|---|---|
| Reservation-protocol model | `EXP/resv_model.py`, outputs `EXP/out-{literal,pt_seals,one_open,draining,fixed}.txt` |
| DRAINING starvation simulation | `EXP/draining_sim.py`, `EXP/draining_sim.out` |
| VM C experiments | `EXP/c/e1_futex_accum.c`, `e2_framing.c`, `e3_futex_order.c`, `e4_memorder.c`, `e5_shortsend.c`; combined output `EXP/vm-output.txt` |
| Kernel source (linux-6.12.y, git.kernel.org stable, fetched 2026-09-16) | `EXP/ksrc/` |

**Verdict.** §3 is not yet correct or complete. The single-node self-deadlock argument (§3.7) holds up, and so do one in-flight send per connection and the recv-side CQ formula. But four problems block implementation:

1. The process that hosts the progress thread is never named, and several mechanisms can't be implemented until it is.
2. The reservation snippet as written loses messages and tears segments.
3. The two-buffer and DRAINING state machine allows per-PE reordering and out-of-order sequence numbers.
4. Receive-side stream reassembly is not designed, and the obvious zero-copy approach brings back the §3.7 cycle.

All four can be fixed. A repaired variant of the protocol passes the same exhaustive model (§1.4).

---

## Summary by severity

| # | Finding | Severity | Confidence |
|---|---|---|---|
| B1 | Progress-thread host process undefined; get replies cannot reach private `dest`; PE-side `seal_and_submit` contradicts single issuer; lifecycle | **blocking** | high |
| B2 | §3.5 `state` check-then-CAS race: reservations land after the submit snapshot → lost/torn messages, `filled > head` in the next generation | **blocking** | high (model) |
| B3 | "try other buffer" and DRAINING→OPEN allow two reservable buffers → same-PE ops reordered, segment seqs non-monotone (early `quiet`) | **blocking** | high (model) |
| B4 | Receive-side framing across provided buffers undesigned; holding pbufs until a segment completes stalls the connection permanently | **blocking** | high (VM) |
| D1 | §3.10 idle loop re-arms `FUTEX_WAIT` every iteration → unbounded armed waits; one wake produced 1,000 CQEs into a 64-entry CQ | design change | high (VM) |
| D2 | Futex words shared across resource classes (or across progress-thread shards) + wake-one → lost wakeup | design change | high (VM) |
| D3 | DRAINING starvation guard starves PE writers under sustained reverse get load | design change | medium-high (sim) |
| D4 | No ack-only segment path; one-way traffic → `quiet` waits forever | design change | high |
| D5 | Deferred-queue pre-allocation is per connection: ~12.6 MB × (nodes−1) → ~6 GB/node at 500 nodes | design change | high (arithmetic) |
| D6 | Handlers enqueue collective forwards on connections owned by *other* shards; cross-thread queue + wake not designed | design change | high |
| D7 | Error model: no dead-peer detection; PE death mid-reservation hangs the node silently; no `shmem_global_exit` path | design change | high |
| C1 | Segment sequence number: "filled at seal time" (§3.4) vs "writer reads it after reserving" (§3.5) | clarification | high |
| C2 | Handoff bit protocol under-specified; plausible encodings leave a stuck flag or lose a wake | clarification | medium |
| C3 | Head-of-line bound for control messages is ≥ 1–2 full segments plus any publication stall, not "one chunk" | clarification | high |
| C4 | Multishot recv can end on any CQE without `F_MORE`, not only `-ENOBUFS`; SQ "assert at init" is nearly vacuous | clarification | medium |
| M1–M4 | Probe coverage gaps; PMI mention in §3.13; timeout API; memory-order wording | minor | — |

Sound items (with evidence) are in §S at the end.

---

## B1. Which process hosts the progress thread? — **blocking**, confidence high

**Location:** §3.12, §3.3 ("Sole issuer… Only the progress thread submits"), §4.1, §4.6 (`seal_and_submit_dirty_buffers()` in PE code), §4.7 ("Write replies directly into the destination"), §1 ("PEs are processes").

**Problem.** §3.12 says there is "a small fixed pool per node" of progress threads, but PEs are separate processes, and a thread lives in exactly one process. The doc never says which process that is. Every option breaks something the doc relies on.

1. **Get replies cannot land in the destination.** The spec defines `dest` for `shmem_get` as "Local address of the data object to be updated" (spec §9.6.1.5 Arguments; same for `shmem_get_nbi`, §9.6.2.2). It is private memory in the *origin* PE's address space, not symmetric. A progress thread in another process (another PE, or a daemon) can reach it only by:
   - **Shared staging plus a copy-out by the origin PE.** For blocking gets this is fine. For `_nbi` gets the data must be held until that PE calls `quiet`. §4.6 itself says unbounded outstanding `_nbi` gets must be accepted, so staging memory is unbounded. This moves the §3.8 memory commitment to the origin side.
   - **Holding the provided buffer until the origin copies.** This pins pbufs and triggers the B4 stall.
   - **`process_vm_writev`.** One syscall per chunk, and it needs ptrace permission between sibling processes (§4.3's own table notes Yama `ptrace_scope=1`).
   - **Each PE receiving its own replies.** That needs a second receiver on the stream, which §3.1 forbids.

   §4.6's `bytes_landed` "progress thread adds as chunks land" must also be in shared memory. That's easy, but it isn't stated.
2. **PE-initiated seal/submit conflicts with single issuer.** `shmem_fence`/`shmem_quiet` call `seal_and_submit(...)` from the PE (§4.6). With `IORING_SETUP_SINGLE_ISSUER`, only the task that owns the ring may submit (*observed in source*: `io_uring/io_uring.c` checks `ctx->submitter_task != current`). A PE in another process doesn't even have the ring fd. So fence/quiet must be a *request* to the owning shard: a shared flag, then a wake on the right shard's word (see D2). That handshake, and its interaction with the §3.10 sweep, is undesigned.
3. **Lifecycle.**
   - If a PE hosts the thread, its sockets, ring, and pbufs die with that process. Normal teardown is protected by the spec: the last `shmem_finalize` "performs an implicit global barrier… no resources are released until all PEs have entered shmem_finalize" (spec §9.1.4). But the spec also lets "the non-OpenSHMEM portion of a program" continue afterwards, so the host must stop its threads cleanly while other local PEs may already be gone.
   - Re-init (§6.4) must re-elect the host.
   - A host PE that crashes, or calls `exit()` from user code before others finalize, cuts the node off. Other nodes then see connection loss or, worse, nothing (D7).
   - `shmem_global_exit` (spec §9.1.6: "immediate notification to all PEs") needs a transport path that works even when outbound buffers are full. None is designed.
4. **Scheduling.** A thread in a PE process inherits that PE's CPU affinity and cgroup cpuset. Under Slurm's task/affinity or task/cgroup plugins, the progress thread would share the host PE's allotted core (*recalled*, Slurm behaviour, unverified here). §3.11's "use cpuset isolation for the progress thread" can't be done from inside a task's cpuset. A separate daemon avoids this but has no launcher: §6 has no daemon, and Slurm launches only tasks.
5. **fd passing** (`SCM_RIGHTS`) lets several processes hold the socket. It doesn't help, because single-issuer and one-in-flight-send require one owner anyway.

**Why blocking.** The choice decides where rings, sockets, pbufs, deferred queues, counters, and wake words live. It decides whether the get path needs staging, which reopens the §3.8 memory argument. And it decides the finalize and error semantics. None of §3.3–§3.12 can be implemented without it.

**Suggested direction** (not a finding). A per-node *helper process* forked by the local lead at init: it owns the rings and sockets, its lifetime is tied to all local PEs via pidfds, and gets use per-PE shared "landing zones" sized by the §4.6 outstanding-get resource. Copy-out happens in the blocked PE (blocking get) or in `quiet` (nbi), and the origin-side pool bounds staging. This keeps §3.8 honest, because the bound becomes an origin resource that blocks rather than a target memory commitment.

---

## B2. `state` check-then-CAS race in §3.5 — **blocking**, confidence high

**Location:** §3.5 snippet; "Reservation succeeds only in `OPEN`"; "Generation numbers are self-stabilizing"; §9 invariant 4.

**Problem.** The snippet reads `buf->state` as a plain field in the same step as the `head` load, then does the CAS on `head`. Sealing changes `state`, not `head`, so a writer that saw `OPEN` can still win the CAS after the progress thread has sealed and snapshotted. This is a check-then-act race even under sequential consistency. It is not a weak-memory-ordering issue.

**Evidence — exhaustive model** `EXP/resv_model.py`:
- One connection, two buffers, 1–2 writers with 2–3 messages, capacity 2–3 units, BFS over all interleavings.
- Every step is one atomic action, which is the strongest memory model, so any counterexample also exists on aarch64.
- Properties checked: overlap, lost, torn, accounting, per-writer order, segment-seq order, stale seq, reservation in DRAINING, stuck.

Variant `one_open` is the most charitable reading of the doc: only the progress thread seals, and a freed buffer becomes OPEN only when the other one is not OPEN. Its shortest counterexample (from `EXP/out-one_open.txt`):

```
W0 load b0; W0 check ok (state=OPEN); W0 CAS ok off=0; W0 read seq=0; W0 memcpy; W0 filled+=1
W0 load b0
W0 check ok (state=OPEN)          <- writer has passed the state check
PT seal b0
PT submit b0 len=1                <- filled == head == 1, snapshot taken
W0 CAS ok off=1 b0 (state now SENDING)
PT complete b0 seq=0              -> LOST: msg W0#1 at off 1 beyond snapshot 1 of b0
```

Results across configurations:

```
one_open: writers=[[1,1],[1]] cap=2 states=6215  violations=['ACCT','LOST','STALESEQ','STUCK','TORN']
one_open: writers=[[2,1],[1,1]] cap=3 states=76513 violations=['ACCT','LOST','STALESEQ','STUCK','TORN']
```

**Consequences:**
- **LOST:** the op is silently never sent.
- **ACCT:** the writer's `filled += n` lands after recycle, so `filled > head` in the next generation. That generation then either never becomes sendable (the progress thread never sees `filled == head`), or becomes sendable at the wrong moment, which gives **TORN** (a message sent before its memcpy).
- The PE's `quiet` waits on an ack for a segment that never contained its op, so `quiet` either hangs or returns early.
- The "self-stabilizing generation" argument (§3.5) assumes this race cannot happen ("the buffer cannot recycle until `filled == head` and this writer has not completed"). The trace shows recycle does happen, because the writer's CAS came *after* the progress thread's equality check.

**Fix validated by the model (variant `fixed`):**
- Encode sealed in the `head` word itself, e.g. a high bit. The progress thread seals by CAS on `head`, so any stale writer CAS fails.
- Writers never read `state`.
- FREE and DRAINING buffers also carry the bit, so a writer can't reserve into them.
- At most one reservable buffer per connection.
- Seq assigned when the buffer opens.

Result: 0 violations in every configuration.

```
fixed: writers=[[1,2]] cap=2 states=107 violations=[]
fixed: writers=[[1,2,1]] cap=2 states=279 violations=[]
fixed: writers=[[1,1],[1]] cap=2 states=4783 violations=[]
fixed: writers=[[1,2],[1]] cap=2 states=3490 violations=[]
fixed: writers=[[2,1],[1,1]] cap=3 states=44627 violations=[]
```

This stays at "two atomics, no locks", so the doc's cost claim survives. ABA on `head` across a recycle turns out harmless in this encoding: a stale writer's valid reservation just lands in the new generation, and it reads that generation's seq afterwards. The model checks STALESEQ and finds no violation. Limitation: `fixed` doesn't model progress-thread reservations in DRAINING.

---

## B3. Two reservable buffers → per-PE reordering and non-monotone segment seqs — **blocking**, confidence high

**Location:** §3.5 comment `/* seal; try other buffer */`; §3.4 "one filling, one in flight"; §3.8 "a freed buffer enters `DRAINING`… drains, then transitions to `OPEN`"; §3.1 ("per-pair ordering by construction"); §4.6 (`acked[node] >= high_seg`).

**Problem.** "Try other buffer" plus "freed → (DRAINING) → OPEN" allow both buffers to accept reservations at once. A PE whose op is too big for the current buffer's remaining space puts it in the other buffer, and its next small op still fits in the first. Which one goes out first then depends on progress-thread order. Either order breaks something: op3 before op2, or op1 after op2.

**Evidence — model**, variant `pt_seals` (only the progress thread seals; the writer just tries the other buffer):

```
W0 CAS ok off=0 b0 ... W0 filled+=1              (msg 0, size 1, in b0)
W0 load b0; W0 fail->other                       (msg 1, size 2, does not fit)
W0 load b1; PT recycle b1 -> OPEN seq=1
W0 CAS ok off=0 b1 ... W0 filled+=2              (msg 1 in b1)
PT seal b1; PT seal b0
PT submit b1 len=2; PT complete b1 seq=1
PT submit b0 len=1; PT complete b0 seq=0         -> ORDER: writer 0 msg 0 on wire after msg 1; SEQ: seq 0 after 1
```

Variant `draining` follows the doc's DRAINING text literally (DRAINING→OPEN regardless of the other buffer). It shows ORDER and SEQ, plus DRAINRES: a stale writer CAS succeeding in a DRAINING buffer.

```
PT recycle b1 -> DRAINING seq=2
W0 CAS ok off=0 b1 (state now DRAINING)           -> DRAINRES (PE reserved where only the PT may)
```

The `draining` state space is unbounded: pending drain items grow with each recycle. The BFS stopped at 3,000,000 states, so its counterexamples are real but its coverage is not exhaustive.

Why this matters beyond fence:
- **Fence and ordering:** §3.1, §9 invariants 3 and 5 ("signals land after every payload byte") depend on per-PE order within the stream.
- **`quiet` completes early:** if seq 1 goes out before seq 0, B's cumulative ack "through 1" satisfies a PE whose op is still in unsent seq 0.
- **Why the literal snippet can't be patched in place:** whether reservation *failure* implies seal is ambiguous. The doc says both "the progress thread seals" and the snippet comment "seal". Variant `literal` (writer blind-stores SEALED) adds state regression: `W0 store SEALED b0 (was SENDING)` corrupts the seal queue and leaves a message stuck forever (`STUCK` in `EXP/out-literal.txt`).

**Required invariants** (all enforced in `fixed`):
- At most one buffer per connection accepts PE reservations.
- A writer that fails for space waits for a seal rather than moving on.
- The next buffer opens only after the previous one is sealed.
- Buffers are submitted in seal order.
- Seq is assigned in that same order.
- DRAINING must be a state writers' CAS cannot enter.
- DRAINING must not turn OPEN while another buffer is OPEN. It should become *the* open buffer, with the progress thread's content first in the segment.

These belong in §9.

---

## B4. Receive-side framing across provided buffers is undesigned — **blocking**, confidence high

**Location:** §3.2 ("no reassembly, no fragment tracking"), §3.3 (multishot recv + pbuf ring), §3.4 (segment ≥ 2× max message), §3.7 ("Any scheme where inbound processing can be halted by an outbound resource shortage is a cycle"), §3.15.

**Problem.** "No reassembly" is true only at the SHMEM chunk level. TCP delivers a byte stream, and multishot recv cuts it at arbitrary points into pbufs:
- A segment is at least 128 KB (2 × 64 KB messages) and a pbuf is typically a page or a few, so almost every segment and every large message spans several CQEs.
- The design needs a per-connection reassembly state machine and a policy for bytes that span pbufs. It has neither.

**Evidence — `EXP/c/e2_framing.c`, VM output:**

```
(a) 100x3000B records, 4096B pbufs: 100 data CQEs, CQE sizes <=1K:0 <=2K:1 <=3000:98 <4096:0 =4096:1;
    CQE boundaries falling inside a record: 1; ENOBUFS=0
(b) 64KB segment, pbufs held until segment complete: got 32768/65536 bytes in 8 pbufs,
    last terminal res=-105 (No buffer space available), re-arms=4 -> segment never completes
```

- **(a)** Even on loopback with paced 3,000-byte writes, CQE boundaries fall inside records. The doc's §8.2 says loopback hides this, and real links with GRO/segmentation will do it constantly.
- **(b)** The zero-copy option is "parse the segment in place and recycle its pbufs after dispatch". It stalls as soon as a segment needs more pbufs than the ring has free. Re-arming just returns `-ENOBUFS`, nothing already held can be released, and the peer then stalls on the TCP window.

With one pbuf ring shared across all of a progress thread's connections (implied by §3.3's CQ formula), *partial segments on many connections* pin the ring together. Each connection waits for bytes that need a buffer another connection holds. This is exactly the §3.7 cycle arriving through the receive path, and it doesn't need to span nodes.

**Also affected:**
- `bytes_landed` (§4.6) and "write replies directly into the destination" (§4.7) need a partial-message cursor per connection anyway.
- §3.15's "abort loudly" needs framing cross-checks at the pbuf boundary, not just at segment end.

**Needed.** Choose between:
- **(i)** Copy the tail of any incomplete message into a per-connection reassembly buffer of max-message size, so pbufs are recycled on every CQE. Memory is connections × 64 KB, about 32 MB at 500 nodes.
- **(ii)** Stream-parse with a per-connection cursor that copies payload straight to its final place (heap offset or landing zone) as bytes arrive, keeping only a partial *header* (≤ 32 B) across CQEs.

Both keep "every pbuf is recycled before the next reap", which is also what the §3.3 CQ bound needs (see S3). Write that rule into §9.

---

## D1. §3.10 idle loop accumulates armed `FUTEX_WAIT`s; CQ bound broken — design change, confidence high

**Location:** §3.10 code; §3.3 CQ formula; §9 invariant 12.

**Problem.** Each idle iteration does `io_uring_prep_futex_wait(...)` and then `io_uring_submit_and_wait(...,1)`. When the wait returns because of a *different* CQE (recv, send), the futex wait is still queued in the kernel. The next idle iteration arms another. Nothing bounds how many pile up, and a single `futex_wake(INT_MAX)` then posts all of them at once.

**Evidence — `EXP/c/e1_futex_accum.c`.** It runs the §3.10 loop literally, with a pipe read standing in for "work arrived", then a separate *process* wakes the word on a shared memfd:

```
after 1000 idle-loop iterations: futex SQEs submitted=1000, futex CQEs=0, other CQEs=1000 -> 1000 futex waits still armed in kernel
PE process futex_wake(INT_MAX) woke 1000 io_uring waiters
CQEs from that single wake: 1000 (CQ size 64; kernel overflow flag=clear)
```

1,000 CQEs went into a 64-entry CQ. liburing's peek transparently flushed the kernel overflow list, which is why the flag reads clear afterwards. This is the `IORING_FEAT_NODROP` slow path §3.3 explicitly rejected, reached under load exactly as §3.3 warns. Each armed wait also holds a kernel `io_futex_data` allocation (*observed in source*: `io_uring/futex.c` `io_futex_wait` → `io_alloc_ifd`), so the per-iteration cost is kernel memory too.

With wake-one on the PE side, each wake completes one stale wait. That doesn't lose a wakeup, but the count never drains.

**Fix.** Keep an `armed` flag; arm only when no futex wait is pending, and clear it on the futex CQE. Then add `+1 futex CQE per ring` to the CQ formula (and `+cancel CQEs` for teardown).

**Also verified (sound):** a cross-process `futex_wake` syscall on a memfd shared mapping *does* complete an `IORING_OP_FUTEX_WAIT` armed with `FUTEX2_SIZE_U32` (no `FUTEX2_PRIVATE`). The existing probe doesn't test this: its io_uring futex check uses `FUTEX2_PRIVATE` in a single process. §3.10 relies on this combination, so `EXP/c/e1_futex_accum.c` is the first evidence for it.

---

## D2. Shared futex words + wake-one → lost wakeups — design change, confidence high

**Location:** §3.11 ("Keep futex words few (one per connection…)", "Single descriptor frees → wake one"); §3.10 (`ep->wake`) with §3.12 multiple shards.

**Problem.** A futex wake picks a queued waiter by queue position. It doesn't know what condition the waiter is waiting for. If buffer-space waiters (divisible) and descriptor waiters (indivisible, §4.6 outstanding-get pool) share one word per connection, a descriptor release with wake-one can wake a buffer waiter. That waiter re-checks, finds no buffer, and sleeps again. The descriptor waiter is never woken.

The same thing happens with §3.12 shards if PEs bump a single `ep->wake`: wake-one reaches a progress thread that doesn't own the connection with the sealed buffer, so submission waits for the 100 ms sweep.

**Evidence — `EXP/c/e3_futex_order.c` part (b):**

```
(b) released one descriptor; futex_wake(1) woke 1
(b) buffer-space waiter woken (wake #1), no buffer, re-sleeps
(b) descriptor waiter: TIMED OUT after 1.01s although a descriptor was freed and a wake issued
```

§3.11's waiters in the doc have no safety deadline (only §4.9's do), so without one this is a hang rather than a 1 s spike.

**Fix.** One futex word per (connection, resource class) and one per shard for progress-thread wakeups. Alternatively, a woken waiter that can't use the wake must pass it on with another `wake(1)`. Give every PE-side wait a deadline, as §4.9 does.

---

## D3. DRAINING starves PE writers under sustained reverse get load — design change, confidence medium-high

**Location:** §3.8 "Starvation guard".

**Problem.** The guard fixes progress-thread starvation by letting the progress thread fill every freed buffer first. If the bulk queue always holds at least a buffer's worth of replies, every generation is full of replies and local PE writers get nothing. Nothing deadlocks, but it looks like a hang for puts from A to B. A steady queue only needs B's PEs to issue their next blocking get as soon as a reply lands, which is normal behaviour for a get-heavy application.

**Evidence — `EXP/draining_sim.py`** (segment granularity; 256 KB buffer, 64 KB chunks, round-robin one chunk per descriptor, next get enqueued when a reply finishes):

```
B PEs=  1 get=   4096 B: A-PE share of segment bytes: doc policy 0.984, capped-at-half 0.984
B PEs=  4 get=  65536 B: A-PE share of segment bytes: doc policy 0.000, capped-at-half 0.500
B PEs= 16 get=  65536 B: A-PE share of segment bytes: doc policy 0.000, capped-at-half 0.500
B PEs=256 get=   4096 B: A-PE share of segment bytes: doc policy 0.000, capped-at-half 0.500
B PEs=256 get=1048576 B: A-PE share of segment bytes: doc policy 0.000, capped-at-half 0.500
```

**Caveats.** The simulation assumes zero think time at B and writers that always have data. Real round-trip latency makes the queue refill more slowly, so the starvation is less absolute but still unbounded. Whether "drains" means "to empty" or "one pass" doesn't matter once replies fill a whole buffer. Livelock isn't possible, because each generation sends bytes.

**Fix.** Cap DRAINING at a fraction of the buffer (e.g. half, or control queue fully plus bulk up to a limit), which is still "no counters". The simulation shows it guarantees both sides a share.

---

## D4. No acknowledgement path for one-way traffic — design change, confidence high

**Location:** §3.4 (piggybacked ack), §3.8 bound item 3 ("piggybacked, so not really a queue entry"), §3.9 ("Explicit flush on fence and quiet covers the case where there is no reverse traffic"), §4.6 `quiet` (`wait_until(acked[node] >= high_seg)`).

**Problem.** Scenario: node A's PEs only put to node B; B's PEs never talk to A.
1. A's `quiet` flushes A's segments, and they are applied at B.
2. B has no outbound traffic toward A, so no B→A segment exists to carry the ack. §3.9's explicit flush happens on the *sender's* side, which is A, and doesn't help.
3. The §3.10 sweep submits only buffers that are already sealed, and B has none.
4. A's `quiet` waits forever. The same happens to `shmem_barrier_all` (§5.6, which includes quiet) and `shmem_finalize`'s implicit barrier.

§3.8 item 3 implies a coalesced "ack owed" item exists, but no mechanism turns it into a segment:
- The progress thread may reserve only in DRAINING buffers.
- On an idle connection both buffers are OPEN/FREE, so none will reach DRAINING without a send CQE.

**Needed.**
- An ack-only (header-only) segment. The progress thread seals an empty or partly filled OPEN buffer and submits it. This doesn't break §3.7, because the header is progress-thread-owned and never "reserved".
- A trigger. The doc forbids timers, so options include "ack immediately when the received segment carried an ack-request flag set by the sender's fence/quiet flush", "ack at the end of each reap batch if anything was applied and nothing is outbound", or delayed ack from the sweep.
- A statement that the ack is sent after *apply*. Handlers apply synchronously, so the receipt-time ack is correct as long as B4's reassembly means "whole segment applied".

Slow mode's "synchronous acknowledgement" (§3.14) already implies such a path; it should be designed for fast mode too.

---

## D5. Deferred-queue memory is per connection, not per node — design change, confidence high

**Location:** §3.8 ("At P=256, that is the difference between roughly a gigabyte per node and roughly 12 MB"; "Per connection"; "Pre-allocate at maximum"; "Size as P×G + degree×max_teams + slack").

**Arithmetic** (the doc's own figures):
- 1 GB / 4 KB ⇒ P·G = 262,144 ⇒ G = 1,024 at P = 256.
- 262,144 × 48 B = 12.6 MB.
- The queues are per connection, so a node holds (N−1) × 12.6 MB: **~190 MB at 16 nodes, ~1.26 GB at 101, ~6.3 GB at 500.**

At the stated target (256 PEs/node, hundreds of nodes), that is pre-allocated and pinned in whichever process hosts the progress thread (B1). It is more than a Pi has.

**Fix.** One node-wide descriptor pool with a global bound, plus per-connection lists; or lazy growth up to the origin-enforced bound. Either keeps assert-on-overflow meaningful.

**Related (clarification).** Bound argument 1 counts AMO results under the "outstanding-get cap". Fetching AMOs are blocking, so the real bound is one per *calling thread*, which under `SHMEM_THREAD_MULTIPLE` isn't P. The resource-based origin pool of §4.6 has to cover AMO results explicitly, or the target assert can fire for a conforming program.

---

## D6. Handlers enqueue on connections owned by other shards — design change, confidence high

**Location:** §3.7 ("collective forwarding… is a handler issuing a request"), §3.8 (queues "Per connection"), §3.12 ("Shard by peer node so each connection has exactly one owner").

**Problem.** A barrier arrival received from child node C runs its handler on the shard that owns connection C. The forward goes to the parent node Q, whose connection may belong to a different shard. So the deferred queues are *not* single-owner. They need a multi-producer enqueue, and the owning shard must be woken with the right per-shard word (D2).

§3.7's proof that the progress thread never blocks still holds if the enqueue is lock-free and bounded. But the queue-bound arguments are per connection, and "one owner per connection" is false as written.

---

## D7. Error model gaps (§3.15) — design change, confidence high

**Location:** §3.15, §3.5–3.6, spec §9.1.6.

1. **Dead peer without RST.** A Pi losing power or its link sends no FIN or RST. With default keepalive (off, or a 2 h idle; *recalled*, tcp(7)), and unacked data retransmitting for ~15 minutes under the default `tcp_retries2` (*recalled*), the job hangs rather than "aborts loudly". Every waiter's safety sweep just loops. Set `TCP_USER_TIMEOUT` and `SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT` and write them into §3.3.
2. **Local PE death mid-reservation.** A PE that segfaults between CAS and `filled +=` leaves `filled != head` forever. The progress thread correctly never spins (§3.5) and never submits that segment, so the connection hangs with no error. §3.6's stall counter will climb but nothing aborts. Slurm does not necessarily kill sibling tasks on one task's failure (`KillOnBadExit` defaults off; *recalled*, unverified). Detection needs a per-PE liveness mechanism, e.g. pidfds polled by the host via `IORING_OP_POLL_ADD`, or a stall timeout that aborts.
3. **`shmem_global_exit`** requires "immediate notification to all PEs" (spec §9.1.6). It needs an out-of-band control message path that doesn't wait on reservation, full buffers, or a stalled segment.
4. "A connection dying mid-message leaves part of a payload written into the symmetric heap" is only true if handlers apply partial messages. With B4's reassembly, partial *messages* are never applied, but partial *segments* are. The invariant should be stated per message.

---

## C1. When is the segment sequence number assigned? — clarification, confidence high

§3.4 says the header, including the sequence number, is "filled by the progress thread at seal time". §3.5 says the writer "reads the buffer's sequence number after a successful reservation", before sealing, to record `high_seg` for `quiet` (§4.6).

If the seq is assigned at seal, the writer reads the *previous* generation's number, and `quiet` returns before the op is acked. The model's STALESEQ property flags this class. The seq must be assigned when the buffer becomes reservable (OPEN/DRAINING), in the same order as submission (B3). The header's *ack* field can still be filled at seal time.

## C2. Handoff bit is under-specified — clarification, confidence medium (reasoning, not modeled)

§3.11 names Linux `MUTEX_FLAG_HANDOFF`, but the kernel mutex hands off to a *specific* top waiter, whose task pointer is written into the owner field. A futex can't direct a wake to a chosen waiter. Two plausible encodings fail:
- **(a)** The releaser sets HANDOFF and calls `wake(1)`, but the woken waiter is the wrong class (D2) or is leaving via its deadline. Nobody clears HANDOFF, every new arrival enqueues behind it, and the resource sits idle until each waiter's timeout. Then either the timeout path may take the resource despite HANDOFF (which defeats handoff) or it can't (hang).
- **(b)** The releaser uses `wake()`'s return value 0 to clear HANDOFF. A new waiter that saw HANDOFF set and queued between the wake and the clear then sleeps with the resource free.

Specify the exact word layout (count / waiters / flag), who clears the flag, and the timeout path. Model it before building (the harness in `EXP/resv_model.py` can be adapted).

## C3. Head-of-line bound for control messages — clarification, confidence high

§3.2 claims chunking bounds head-of-line blocking "to one chunk time". But the progress thread can reserve only in DRAINING buffers (§3.8), and submission is in seal order with one in flight (§3.3). A barrier forward created while buffer X is in flight and Y is OPEN therefore waits for:
1. X to finish;
2. Y to be sealed *and* reach `filled == head`, which includes any §3.6 publication stall;
3. Y to be sent;
4. then its own DRAINING buffer.

That is up to two full *segments* (≥ 2 × 128 KB, about 2 ms at 1 GbE) plus stall time, not one 64 KB chunk. Probably acceptable, but §3.2 should state it correctly. Letting the control queue be copied into the *OPEN* buffer at seal time, as progress-thread-owned space reserved before sealing, would cut it to one segment.

## C4. Multishot termination and SQ bound — clarification, confidence medium

- **Multishot termination.** In 6.12, a multishot recv stops whenever it can't post a CQE with `F_MORE`. Handle "no `F_MORE`" generally (EOF 0, errors, `-ECANCELED`, CQ-post failure), not only `-ENOBUFS` (*observed in source*: `io_uring/net.c` `io_recv_finish`, terminal path at label `finish:` when `io_req_post_cqe` fails or `mshot_finished`). Also `MULTISHOT_MAX_RETRY = 32` requeues rather than terminating (`net.c:97, 905–909`): a fairness point, not a bug.
- **SQ bound.** The SQ empties on every `io_uring_submit`, so "SQ ≥ 2×connections" only bounds one batch, and `io_uring_get_sqe` returns NULL rather than breaking silently. The bound is harmless but mis-explained. It should also count futex-wait, cancel, and re-arm SQEs.

## Minor

- **M1** The probe's multishot check asserts `cqes > 1`, not the exact "8 + 1" shape §3.3 reports as verified. The shape does reproduce (`cqes=9 with_F_MORE=8`, below), but the probe would still pass if it broke. The probe also doesn't test cross-process `IORING_OP_FUTEX_WAIT` (D1 note).
- **M2** §3.13 "exchange through PMI" contradicts §6 (no PMI on the default path).
- **M3** §3.10 shows `io_uring_submit_and_wait` but describes an EXT_ARG timeout. The call needed is `io_uring_submit_and_wait_timeout`. On kernels without `IORING_FEAT_EXT_ARG`, liburing falls back to a timeout *SQE* that posts a CQE (*recalled*, liburing source unverified here); 6.12 has EXT_ARG. Separately, `tcp_autocorking` (default on) can briefly delay small sends even with `TCP_NODELAY` (*recalled*, `ip-sysctl.rst`). It is sub-millisecond and self-clearing, so worth a comment next to the NODELAY note, not a design change.
- **M4 — memory-order wording (§3.5, §8.3).** GCC 14 on aarch64 *does* honour the orders (E4 below), but plain `atomic_load` is `seq_cst` and emits the same `ldar` as `acquire`. "Omitting them" is harmless; the real hazards are `memory_order_relaxed` and non-atomic fields like `buf->state` (B2). Reword so no one "optimises" to `relaxed`.

---

## §S. What is SOUND (with evidence)

- **S1. Probe reproduces on the target kernel.** `build/debug/tools/probe/probe` (existing build) and a fresh `gcc tools/probe/probe.c -luring` in `/tmp` inside the VM both print `PROBE PASSED (0 failures)`:
  - `uring futex_wait stale value -> -EAGAIN res=-11`
  - `multishot recv: >1 CQE from one SQE cqes=9 with_F_MORE=8`
  - `buffer ring exhaustion -> -ENOBUFS`
  - `futex2 U64 wait … Invalid argument`
  - `cross-process futex on memfd mapping woke=1`

  I did not run `cmake --workflow` because it writes into the repo's `build/`.
- **S2. §3.7 self-deadlock argument.** Correct as stated. Under io_uring the only way a progress thread can block on the outbound side is waiting for reservable space, and "enqueue, never reserve" plus bounded descriptor queues removes it. A cross-node cycle (A and B both with full outbound buffers and full queues) can't form: neither progress thread ever stops consuming receive data, so TCP windows reopen and send CQEs arrive. A full queue is an assert, not a deadlock. **Conditions:** the receive path must never hold pbufs across reaps (B4), and queue bounds must actually be enforced at the origin (D5 note). Page faults on first touch of the memfd heap are the remaining in-kernel block, which is a latency matter.
- **S3. CQ formula for receive.** Data CQEs pending ≤ pbuf entries holds if every pbuf is recycled only after its CQE is consumed: a pbuf can't be in both the kernel ring and an unreaped CQE. Plus ≤1 terminal per armed recv and ≤1 send CQE per connection. Probe output matches. *Observed in source*: pbuf ring entries must be a power of two and < 65536 (`io_uring/kbuf.c:721–725`); CQ max 65536 (`io_uring/io_uring.c:106–107`). So hundreds of connections plus a 32,768-entry ring fits. The formula needs `+1 futex CQE` (D1) and teardown cancels.
- **S4. One in-flight send per connection is necessary.** `EXP/c/e5_shortsend.c` shows short sends really happen on io_uring TCP sends:

  ```
  send 4194304 bytes, flags=0: CQE res=32741  <-- SHORT SEND
  send 4194304 bytes, flags=MSG_WAITALL: CQE res=4194304
  ```

  With a short first send, a second concurrently submitted send would interleave bytes, which is why the design's "advance past a short send" is required. *Observed in source*: io_uring retries partial stream sends internally only with `MSG_WAITALL` (`io_uring/net.c` `io_net_retry`, lines 129–134). `MSG_WAITALL` is an option that removes the short-send path, but it holds the connection's only send slot until the whole segment is out, which is equivalent under one-in-flight.
- **S5. Futex waiter ordering (§3.11 "verify" note).** *Observed in source*, `kernel/futex/core.c` `__futex_queue` (6.12.y): `prio = min(current->normal_prio, MAX_RT_PRIO); plist_add(...)`, with the comment "all RT-threads are woken first in priority order, and the others are woken last, in FIFO order". `kernel/futex/waitwake.c` `futex_wake` walks the plist in order up to `nr_wake`. `io_uring/futex.c` `io_futex_wait` uses the same `futex_queue`, so the priority is that of the progress thread that submitted it. *Observed in VM* (`e3_futex_order.c` (a), run as root):

  ```
  (a) queue order: 0 1 2 3 4 then 99(SCHED_FIFO…). wake(1) order: 99 0 1 2 3 4
  ```

  The claim is confirmed, and it supports §3.11's "no real-time priority anywhere": one RT thread jumps every queue.
- **S6. Kernel-side compare-and-wait closes the missed-wakeup race** (§3.10). Stale value → `-EAGAIN` (probe), and a cross-process wake completes an io_uring futex wait (E1). Holds once D1's re-arm leak is fixed.
- **S7. Memory ordering is honoured by GCC 14 aarch64** (`EXP/c/e4_memorder.c`, `objdump`): `load_acq` and `load_seq` → `ldar`; `load_rel` → `ldr`; `fetch_add(release)` → `__aarch64_ldadd4_rel`; `cas(acquire)` → `__aarch64_cas4_acq`; `seq_cst` → `_acq_rel` variants. The §3.5 cost claim ("nearly free") holds; see M4 for the wording.
- **S8. CAS-not-fetch_add, and `filled` as a count rather than a watermark** (§3.5). Sound. The `fixed` model uses exactly these and passes; the failures in B2/B3 come from the `state` field and the buffer-selection rules, not from these choices.
- **S9. `TCP_NODELAY` rationale** (§3.3). Correct: Nagle holds small writes while data is unacked, and with delayed ACK it can stall one-way traffic (*recalled*, tcp(7) / RFC 1122 §4.2.3.4). A stalled segment is invisible to `quiet`'s sealed-buffer sweep, as stated.
- **S10. Flow control is memory-bounded on the send side.** Two buffers per connection plus blocking writers means a slow receiver only blocks PEs writing to *that* node. There's no node-wide head-of-line block from one slow peer, because each connection has its own buffers and the shard's progress thread never blocks. The unbounded or oversized pieces are D5 (queue pre-allocation) and B1 (get staging).
