# OpenSHMEM over Ethernet: Design Notes

**Status:** Path-finding. No implementation. Decisions recorded with rationale so
that they are not silently reversed.

**Target:** OpenSHMEM 1.6 (1st November 2024). Section references of the form
§9.x.y are to the specification; §N.M without a leading 9 are to this document.

---

## 1. Goals and Constraints

- Implement OpenSHMEM 1.6 over commodity Ethernet with no fabric offload.
- **Build with a C compiler and nothing else.** Primary goal, not a preference.
- Correctness first, reasonable performance second.
- Two-level structure: intranode shared memory, internode node-to-node paths
  shared by all PEs on a node. Nothing may scale with PE count where it can
  scale with node count.
- Target hardware: Raspberry Pi cluster. Design must also remain usable at 256
  PEs per node and hundreds of nodes.
- PEs are processes. Cores are over-provisioned.
- Slurm launch. No PMI link dependency (§6).
- TCP is acceptable. io_uring is required (kernel 6.7+ for futex ops; Trixie
  ships 6.12).

### Framing

On plain Ethernet, a one-sided API is implemented over a two-sided transport.
Remote memory *is* the progress thread applying active messages. This single
fact drives ordering, completion, atomicity, and the core budget.

### Why buildability is the goal

The motivating experience is concrete. Sandia OpenSHMEM requires libfabric, and
libfabric is not well supported on Raspberry Pi — or, in practice, on much
hardware outside the platforms its vendors care about. The existing
implementations are gated behind a dependency chain that does not survive
contact with commodity aarch64.

This is not a side benefit of the design. It is the reason the project exists,
and it settles questions the rest of this document then treats as closed.

> **Dependency budget: libc and a vendored liburing. Nothing else links.**

liburing is small and has no dependencies of its own. It is vendored (submodule
or in-tree source), not taken from the distribution — and vendoring is the
point, not laziness: Bookworm's liburing predates the futex operations §3.10
depends on, so a system liburing either fails to build or silently loses the
wakeup path on older nodes. A user needs a C compiler and a 6.7+ kernel. No
package manager, no `pkg-config`, no version probing.

*(Rejected: raw `io_uring_setup`/`io_uring_enter` with hand-mmapped rings, for
true zero-dependency. The subset used here is small enough that this is
tempting, but it hands you liburing's kernel-feature probing to own, and
vendoring already removes the distribution from the picture.)*

The same constraint removes the bootstrap's link dependency — see §6.

### Performance floor

"Faster than SOS" was the original framing and it is withdrawn. It is
unmeasurable here for precisely the reason the project exists: there is no SOS
build on the target hardware to measure against. A comparison that cannot be run
is not a goal, and carrying it invites tuning against an imagined opponent.

Targets are stated against a local, reproducible floor instead:

| Measurement | Floor it is stated against |
|---|---|
| 8-byte round trip, two nodes | A trivial io_uring TCP echo on the same link |
| Large-message bandwidth | The same harness streaming, cross-checked against `iperf3` where it happens to be installed |
| Intranode 8-byte put | `memcpy` plus the synchronization the API requires |

Each floor is a harness in this repository (§8.4), so it runs on any machine
with nothing installed, reproduces identically on x86 and aarch64, and cannot
rot the way a third-party build can. **The constant factors are deliberately not
written down yet** — they are pinned by the first run of the echo harness, and
pinning them before measuring would be
inventing a target rather than choosing one. Track all three per commit through
the §3.14 counters.

---

## 2. Layering

Two projects. The lower one is independently specified, tested, and useful.

### 2.1 AM layer (separate project)

An active-message layer in the GASNet AM-2 tradition, over TCP with io_uring.

```c
int  am_request(am_ep_t *ep, int node, int handler, const void *hdr, const void *payload, size_t len);
int  am_reply(am_token_t tok, int handler, const void *hdr, const void *payload, size_t len);
int  am_poll(am_ep_t *ep, int timeout_ms);
int  am_progress_start(am_ep_t *ep, int nthreads);
int  am_progress_stop(am_ep_t *ep);
```

`am_poll` serves both roles: `am_poll(ep, 0)` is caller-driven and
non-blocking, used by single-threaded deterministic tests;
`am_poll(ep, 100)` blocks and is what the progress thread loops on. There is
no policy enum. **Handlers must be written to run in either context** and may
never assume which thread they are on. Establish this in the first commit;
retrofitting means auditing every handler.

### 2.2 SHMEM layer

Puts, gets, AMOs, signalled puts, and all collectives are AM handlers. The
symmetric heap, address translation, and the intranode shared-memory path are
the actual product and do not come from any lower layer.

### 2.3 Rejected alternatives

| Candidate | Reason rejected |
|---|---|
| GASNet-EX | Large dependency with its own conduit machinery; inherits complexity rather than avoiding it |
| Portals 4 | Implementing Portals over TCP is strictly more work than implementing SHMEM over TCP; it is a NIC offload model emulating matching semantics not needed here |
| UCX | Large dependency whose fast paths are exactly the ones this hardware lacks |
| ARMCI | Legacy |

**For the first three the disqualifier is the same, and it is not complexity.**
Each drags in the toolchain problem of §1: a large build with its own dependency
tree, maintained for hardware this project does not have. Adopting any of them
reproduces the libfabric situation with a different name on it. Complexity is
the second argument and would not be sufficient alone.

---

## 3. AM Layer Design

### 3.1 Connection model

One TCP connection per **node pair**, not per PE pair. Connection state scales
with node count.

A single ordered stream per node pair, applied by the receiver in arrival
order. This yields the central simplification:

> **`shmem_fence` requires no per-operation tracking.** Stream order plus
> in-order application provides per-pair ordering by construction.

This property is destroyed by any second data socket. Do not add one.

### 3.2 Chunking

Application-level fragmentation at ~64 KB. A 4 MB payload holds the stream for
~32 ms on 1 GbE; a barrier queued behind it waits the full time. Chunking bounds
head-of-line blocking to one chunk time while keeping a single ordered stream.

Each chunk is a self-contained AM carrying `(region, offset, length)`. The
receiver lands each chunk at its own offset — no reassembly, no fragment
tracking. Header overhead is ~32 bytes per 64 KB.

**SHMEM chunks above AM's advertised maximum payload; AM never fragments
internally.** AM keeps one invariant: every message fits in a segment.

### 3.3 io_uring usage

**Sole issuer.** One ring per progress thread, `IORING_SETUP_SINGLE_ISSUER`.
Only the progress thread submits.

**Receive:** multishot recv with a registered provided buffer ring
(`IORING_REGISTER_PBUF_RING`). One SQE per connection yields many CQEs; the
kernel selects a buffer on arrival. Handle `ENOBUFS` (ring exhausted, recv
terminated) as a normal under-load event — replenish and re-arm. This path
must be tested; it is a good fault-injection target.

**Send:** **one in-flight send per connection.**

> io_uring does not guarantee that two independently submitted sends on the same
> file descriptor execute in submission order. Multiple in-flight sends can
> complete out of order and corrupt the stream. This would break correctness,
> not performance, and only under load.

On the send CQE, either advance past a short send and resubmit the remainder,
or submit the next queued buffer.

**Ring sizing — two rings, two different bounds.** An earlier draft asserted one
bound and applied it to "the ring." That conflated the submission and completion
queues, which are bounded by different things.

*Submission queue:* `2 × connections + constant` — one send, one multishot recv
per connection. Size above that and it can never fill. **Assert at init** — it
breaks silently the day someone adds a second in-flight send.

*Completion queue:* the SQ bound does **not** carry over, because one multishot
recv SQE produces many CQEs. Pending receive completions are bounded instead by
how many provided buffers the kernel can fill:

> `CQ entries ≥ pbuf_ring_entries + 2 × connections + slack`, set explicitly via
> `IORING_SETUP_CQSIZE`.

This restores "can never fill" for both rings and puts both asserts at init. The
cost is a few hundred KB of CQEs — irrelevant beside the buffer ring itself, and
cheap for deleting an error path.

*(Rejected: a small CQ relying on `IORING_FEAT_NODROP` to spill into the kernel
overflow list. No completions are lost, but it is a slow path entered exactly
under load and observable only through a counter someone must think to add.
Also rejected: shrinking the provided-buffer ring to keep the CQ small, which
just converts CQ pressure into routine `ENOBUFS`.)*

**`TCP_NODELAY` on every connection, set at setup.** §3.9 is already a
coalescer, and a better-informed one — it knows message boundaries, `fence`, and
`quiet`. Nagle is a second coalescer underneath that knows none of them: it
holds a small write while earlier data is unacknowledged, and paired with the
peer's delayed ACK it can sit on a segment for tens of milliseconds.

> The damage lands on `quiet`. The buffer *was* sealed and submitted, so the
> §3.10 safety sweep sees nothing wrong while the kernel holds the segment. And
> because §3.4 piggybacks acknowledgements, reverse traffic usually clocks it
> out — so this looks perfect under ping-pong benchmarks and stalls on one-way
> patterns. Comment the `setsockopt` call: the application layer owns
> coalescing.

**Not used:** `IORING_OP_SEND_ZC`. Two CQEs, buffer stability requirement, and
only pays above ~10–16 KB. Revisit only on a fast NIC.

### 3.4 Segment structure

Two buffers per connection: one filling, one in flight. A third smooths nothing.

```
[ segment header ][ message ][ message ][ message ] ...
```

Segment header (fixed size, 16–32 B, at offset 0, never reserved by writers,
filled by the progress thread at seal time):

- **Sequence number** — makes `quiet` nearly free (see §4.6)
- **Piggybacked acknowledgement** of the reverse direction
- **Credit return** (if credits are ever added; see §3.7)
- **Message count and total length** — deliberately redundant with per-message
  lengths. Cross-checking on receipt turns a framing bug into an immediate
  assertion rather than a garbage dispatch 50 MB later.

Each message within a segment is **padded to 8-byte alignment**. On aarch64 an
unaligned 64-bit atomic faults; an AMO payload at an odd offset crashes rather
than degrades.

Buffer size must be at least 2× maximum message size, or aggregation stops
working at the top of the range. A 64 KB message in a 64 KB buffer means one
message per segment, forever.

### 3.5 Reservation protocol

Writers are PE threads. Two atomics, no locks.

```c
for (;;) {
    uint32_t h = atomic_load_explicit(&buf->head, memory_order_acquire);
    if (buf->state != OPEN || h + n > buf->cap) { /* seal; try other buffer */ }
    if (atomic_compare_exchange_weak_explicit(&buf->head, &h, h + n,
            memory_order_acq_rel, memory_order_acquire)) { off = h; break; }
}
memcpy(buf->data + off, src, n);
atomic_fetch_add_explicit(&buf->filled, n, memory_order_release);
```

**CAS loop, not `fetch_add`.** With `fetch_add`, an overflowing writer has
already moved `head` and must unwind, and the unwind must still account for
itself in `filled` or the buffer never becomes sendable, while concurrent
overflowing writers race to seal. The CAS makes a failed reservation a
non-event.

**`filled` is a count, not a watermark.** Out-of-order completion means no
prefix property holds. This is fine: the buffer is sendable when
`filled == head`. Do not reach for a watermark and then discover it cannot be
maintained.

**Buffer states:** `OPEN → SEALED → SENDING → FREE`, plus `DRAINING` (§3.6).
Reservation succeeds only in `OPEN`. The progress thread seals, and submits
only once `filled == head`. Conflating sealing and draining is the race that
corrupts a segment.

**The progress thread must never spin waiting for `filled == head`.** A writer
may be descheduled mid-`memcpy`. Move to other connections and re-check later.
At 256 PEs on an oversubscribed node, a spin here is a hang.

**Memory ordering:** acquire on the CAS, release on the `filled` increment,
acquire when the progress thread reads `filled`. On aarch64 these are
`ldar`/`stlr`, nearly free. Omitting them produces corruption visible only
under load and never on x86.

**Generation numbers are self-stabilizing.** A writer reads the buffer's
sequence number after a successful reservation with no extra synchronization,
because the buffer cannot recycle until `filled == head` and this writer has
not completed. Comment this so nobody later "fixes" it with a lock.

### 3.6 Publication-window stall (accepted)

A PE holding a reservation mid-`memcpy` blocks submission of the whole segment.
No priority mechanism fixes this; the progress thread is not blocked on anything
the kernel can see.

Mitigation: bounded reservation size keeps the copy window to tens of
microseconds. **Instrument it** (stall count and duration). The contiguous
watermark escape hatch is *deliberately left out* — it costs an extra atomic on
a hot path. Revisit only if measurement demands it.

### 3.7 Handler rules

> **A handler may enqueue, but never reserve.**

This is the load-bearing invariant. It is stronger and cleaner than the
request/reply framing, because collective forwarding (§5.4) is a handler issuing
a *request*, not a reply.

#### The self-deadlock this prevents

1. A's two outbound buffers toward B are occupied (one submitted, CQE unreaped;
   one sealed).
2. A's progress thread parses a request and invokes the handler.
3. The handler must reply. Both buffers unavailable. It waits.
4. B is healthy. A's socket send buffer empties. The kernel completes A's
   in-flight send and posts a CQE.
5. **Nobody reaps it.** The only thread that reaps A's ring is waiting inside
   the handler.
6. The buffer is never freed. The wait never ends.

The progress thread waits on a resource only the progress thread releases. This
requires no peer misbehaviour and no symmetric load. **A single node deadlocks
on itself.**

Note: under io_uring there is no blocking send. `io_uring_submit` returns once
SQEs are consumed; reaping is a load from an mmapped ring. **Reserving outbound
space is the only place a progress thread can block.** Removing that removes
the class.

#### Why deferring-with-blocking does not help

An unbounded pending queue trades deadlock for OOM. A bounded queue that blocks
when full returns to the original deadlock; one that stops consuming receive
buffers exhausts the provided buffer ring, stalls the peer, and reproduces the
symmetric deadlock. **Any scheme where inbound processing can be halted by an
outbound resource shortage is a cycle waiting to close.**

### 3.8 Deferred reply queues

Per connection. Descriptors, not reserved payload space.

> Reserving reply *payload* space commits ~4 KB per outstanding get. Reserving a
> *descriptor* commits ~48 bytes. At `P=256`, that is the difference between
> roughly a gigabyte per node and roughly 12 MB. The reply's source data is
> already in the symmetric heap and needs no copy until send time.

Because reservation may now *fail* — the progress thread leaves the descriptor
in place and carries on reaping — there is no infallibility requirement and no
blocking path. This is what makes §3.7 satisfiable trivially rather than by
construction.

**Two queues:**

| Queue | Contents | Drain policy |
|---|---|---|
| Control | AMO results, collective forwards | Fully, first |
| Bulk | Get reply chunks | Round-robin, remaining space |

Barrier latency gates the whole job; a barrier forward behind a 1 MB get reply
stalls every PE. The control queue is small and drains to empty, so it cannot
starve the bulk queue in practice. The reverse would happen constantly.

**Round-robin, not FIFO,** within the bulk queue: one chunk per descriptor per
pass. Strict FIFO means a 1 MB get blocks ~8 ms of small replies behind it. Get
replies to different PEs have no mutual ordering requirement, so rotation is
legal.

**Peek / advance `sent` / pop when `sent == total`.** A 1 MB get costs one
descriptor.

#### Sampling semantics differ by type — do not unify

| Type | Applied | Descriptor carries |
|---|---|---|
| Fetching AMO | At handler time, atomically | The **result value** |
| Get reply | At drain time | A **reference** (region, offset, length) |

A fetching AMO *cannot* be deferred: it modifies the target, and that
modification is ordered on the stream relative to every later operation touching
the location. Deferring it reorders operations the stream ordered.

#### Starvation guard

If PE threads always win reservations, the queue never drains and an origin PE
blocked in `shmem_get` waits forever. Not a deadlock — everything is running —
but observably a hang.

**Fix:** a freed buffer enters `DRAINING`, in which only the progress thread may
reserve. It drains, then transitions to `OPEN`. One extra state, no counters, no
fairness arithmetic. Instrument bytes drained per generation and queue-depth
high-water.

#### Queue bounds — three separate arguments, each independently breakable

1. **AMO results:** bounded by the origin's outstanding-get cap. **Enforced at
   the origin, not locally** — a distributed invariant. A peer's accounting bug
   overflows *my* queue and the symptom appears on the wrong node.
2. **Barrier forwards:** bounded by tree degree × concurrent teams. The epoch
   argument: if X holds an unforwarded epoch-N arrival, the root has not
   released, so no PE entered N+1, so no N+1 arrival can exist. **This rests on
   collectives being blocking.** Nonblocking collectives would dissolve it —
   write this into the code, because a future contributor adding
   `shmem_barrier_nbi` would not know to check.
3. **Quiet acknowledgements:** coalescable to one outstanding per connection
   (a high-water mark subsumes older ones); piggybacked, so not really a queue
   entry.

Size as `P×G + degree×max_teams + slack`. Pre-allocate at maximum. **Assert
hard on overflow** with peer identity and queue contents. Export the high-water
mark so the counter history identifies which invariant broke.

**Known future break:** a tree `shmem_reduce` forwards a partial result of
`count × sizeof(type)` bytes — bulk-sized *and* latency-critical, fitting
neither queue. Not a phase-1 problem (barrier alone is fine), but the control
queue's fixed-entry assumption is a convenience, not a property. Do not let the
structure harden around it.

### 3.9 Aggregation

Self-clocking off the send CQE. While a send is in flight, operations
accumulate. On the CQE, submit everything queued as one segment. Batch size
grows under load and falls to one when idle, with no timers and no tuning
parameters. This is both the simplest implementation and close to the right
policy.

Explicit flush on `fence` and `quiet` covers the case where there is no reverse
traffic to clock it.

### 3.10 Idle and wakeup

`IORING_OP_FUTEX_WAIT` (6.7+). A counter in userspace; nothing to drain.

```c
uint32_t v = atomic_load_explicit(&ep->wake, memory_order_acquire);
reap_cqes(); submit_sealed_buffers();
if (work_pending()) continue;
io_uring_prep_futex_wait(sqe, &ep->wake, v, FUTEX_BITSET_MATCH_ANY,
                         FUTEX2_SIZE_U32, 0);
io_uring_submit_and_wait(&ep->ring, 1);
```

PE side: `atomic_fetch_add(&ep->wake, 1)` then `futex_wake`.

> **The kernel closes the missed-wakeup race.** If the value no longer matches at
> submission time, the wait completes immediately with `-EAGAIN`. The `blocked`
> flag survives only as an optional syscall-avoidance hint whose incorrectness
> is harmless. A subtle hand-proved memory-ordering argument is replaced by a
> kernel-provided compare-and-wait.

Handle `-EAGAIN` and `-EINTR` as ordinary loop-again outcomes.

**Rejected:** eventfd (requires a `read()` to drain, or it re-fires forever —
a syscall per wakeup); sleep loop (cannot pick a duration that is
latency-neutral, since the Pi will not honour a 10 µs `nanosleep` and a 1 ms
sleep roughly doubles round-trip latency); `IORING_OP_MSG_RING` (requires a ring
per PE to avoid one futex word).

**Safety sweep:** pass a ~100 ms timeout via `IORING_ENTER_EXT_ARG` and submit
anything sealed regardless of signalling. This converts a class of hangs into a
class of latency spikes with a findable counter. The wakeup mechanism must
affect only *how soon* a sealed buffer is submitted, never *whether*.

### 3.11 Waiter fairness

Waiters are held on a priority-sorted list; `FUTEX_WAKE(1)` dequeues the head,
FIFO for equal priority. *(Verify the priority-ordering half against
`kernel/futex/` on 6.12; the FIFO-for-equal-priority behaviour is safe to
assume.)*

**Wake count tracks divisibility:**

- Buffer becomes `OPEN` (divisible: 192 KB free, 30 waiters needing 200 B each)
  → **wake many.** Waking one creates a serial chain with a scheduling delay per
  link; at 30 deep on a Pi that is milliseconds of pointless latency.
- Single descriptor frees (indivisible) → **wake one.** Waking more is a
  thundering herd; at 256 local PEs, 255 threads wake and immediately re-sleep.

**Handoff bit** (as in Linux's `MUTEX_FLAG_HANDOFF`). Wake order is not acquire
order: a woken thread must be scheduled before it re-checks, and any
already-running thread can barge in that window. On four cores with 256 PEs this
is not theoretical. A releaser seeing waiters marks the resource handed off; a
fresh arrival seeing the flag must enqueue even though the resource appears
available.

Note this defeats a hand-built intrusive FIFO too — the problem is not queue
order but that acquisition is not mediated by the queue.

Applied to **indivisible resources only**; divisible ones do not starve.

**Short spin before blocking.** Count set at init from local PE count versus
available cores; **zero under oversubscription**, where a spinning PE consumes a
core that a progress thread or mid-copy writer needs. Instrument
spin-succeeded versus spin-expired.

**No real-time priority anywhere.** It is tempting on an oversubscribed Pi and
would introduce unbounded priority inversion. Use cpuset isolation for the
progress thread instead — isolation without a priority relationship.

Keep futex words few (one per connection, not one per PE per connection) and
cache-line aligned. Futex waits hash into kernel buckets by address; unrelated
words can collide on the same bucket spinlock.

### 3.12 Progress threads

**A small fixed pool per node, sharded by peer node — not one thread per PE.**

Target-side progress is fully discharged by a thread that shares the coherence
domain with local PEs, because incoming puts, AMOs, and signalled puts all land
in the shared symmetric heap. The origin side never needs a helper, because the
calling thread is inside the library by definition.

Pool size is set by memory bandwidth, not PE count. On a Pi it is one. At 256
PEs perhaps four. Shard by peer node so each connection has exactly one owner,
preserving single-threaded ordering per stream.

### 3.13 Handler table

**Fixed at init. No dynamic registration.** Identifiers are a compile-time enum;
dispatch is an array index with one bounds compare. No registration lock, no
lifetime question, no teardown race. The whole subsystem is a `static const`
array.

**Checksum the table at init and exchange through PMI.** Every PE must agree.
This catches mismatched binaries across nodes immediately with a clear message,
rather than as an undecodable message arriving at PE 47 an hour in.

### 3.14 Introspection (first-class, not retrofitted)

- **Counters:** per-thread, cache-line padded, summed only on read — free in the
  fast path, so they stay compiled in for production. Count messages and bytes
  per peer, handler invocations per handler id, coalescing batch sizes, queue
  depths with high-water marks, reservation stalls, spin outcomes, wake counts,
  failed-acquire-after-wake.
- **Named handlers** alongside ids, so traces and dumps are readable.
- **Tracing hooks** at enqueue / wire send / wire receive / handler entry /
  handler exit, default no-op. Sequence-number every AM so a trace is replayable
  and reordering is visible rather than inferred.
- **Fault injection** in the layer, not the tests — delay, drop, reorder,
  truncate, allocation failure. Injecting from outside cannot reach most error
  paths.
- **`am_dump_state(FILE *)`** — every queue, counter, and in-flight operation.
  The difference between a diagnosis and a guess when a 64-PE job hangs.
- **Quiescence primitive** — drain, then assert nothing in flight and all
  accounting balances. Most correctness bugs surface as a failed invariant here
  rather than as a wrong answer.
- **Slow mode as a policy setting, not a separate code path:** no coalescing,
  one message per operation, synchronous acknowledgement. Every test runs both
  ways and results are compared.

#### Fault-injection determinism

**Stateless, hash-derived.** A shared RNG destroys reproducibility, because the
sequence each site sees depends on thread arrival order. Per-thread generators
still fail, because which thread handles which message is nondeterministic.

```c
uint64_t h = hash64(seed ^ site_id ^ (seq * 0x9E3779B97F4A7C15ull));
bool inject = (h % 10000) < rate_per_myriad;
```

Same seed, same decision, same operation, same site — regardless of threading or
timing. Print the seed on every run including passes; allow override by
environment; sweep in CI; **commit any failing seed as a permanent regression
case.**

### 3.15 Error model

A connection dying mid-message leaves part of a payload written into the
symmetric heap, unrecoverably. For a library where connection loss means job
loss this is acceptable, but it is a **documented invariant**: AM's error path
is "abort the job loudly," not "recover."

---

## 4. SHMEM Layer

### 4.1 Symmetric heap

Shared `memfd`/hugetlbfs segment, mapped by every local PE. Intranode put/get
collapses to `memcpy` — no syscall, no proxy. The progress thread shares the
coherence domain with every local PE, so remote AMOs applied by the progress
thread and local AMOs issued by PEs are the same CPU atomic on the same cache
line. **Atomicity unifies for free.**

This is *stronger* than the spec requires, which is worth knowing before anyone
trades it away. Spec §3.2 guarantees exclusivity only between OpenSHMEM AMOs on
the same location, of the same datatype, in the same atomicity domain, and
explicitly makes AMOs concurrent with ordinary loads and stores undefined. The
shared mapping gives real CPU-level atomicity across that whole set for nothing.

Two consequences: the §4.8 `shmem_ptr` warning has spec text behind it and
should cite §3.2 item 4 rather than argue from first principles; and if
`SHMEM_TEAM_SHARED` is ever implemented — which the §8.1 node map nearly hands
over — note that the spec defines it as a **separate atomicity domain** from
`SHMEM_TEAM_WORLD`, so AMOs from the two are mutually undefined by the standard
even though this implementation would make them exclusive anyway.

Reserve a fixed virtual range with `MAP_FIXED_NOREPLACE` so a collision fails
loudly rather than silently unmapping something. `-no-pie` pins the executable
but **not** the heap — ASLR still randomizes heap, stack, mmap region, and
shared libraries.

### 4.2 Addressing

**Offset-based on the wire, always.** `(region id, offset)`, translated at both
ends. This survives ASLR, PIE, heterogeneous nodes, and future region types.

Identical-VA mapping is pursued *as well*, purely so intranode access and
`shmem_ptr` are raw pointer arithmetic — but **the wire format must never depend
on it.** This is the decision that is genuinely painful to revisit.

### 4.3 Symmetric statics

Two separable problems.

**Address agreement:** `-fno-pie` (compile) **and** `-no-pie` (link). Both are
needed and they do different things: `-fno-pie` selects absolute addressing for
local symbols; `-no-pie` produces `ET_EXEC` with a fixed load base. Debian
defaults both on, so the wrapper must inject both. `-fno-pie` alone fails to
link on aarch64 (`R_AARCH64_ADR_PREL_PG_HI21` against a symbol in a PIE).

**Shared mapping:** staged.

| Phase | Approach | Notes |
|---|---|---|
| 1 | Unsupported; error on statics | `shmem_malloc` covers most real codes |
| 1.5 | `-no-pie` + CMA (`process_vm_readv/writev`) | Correct but slow. Needs `PR_SET_PTRACER(PR_SET_PTRACER_ANY)` — Yama `ptrace_scope` defaults to 1, which permits only descendants, and PEs are siblings |
| 2 | Wrapper-generated linker script | Fast; no source changes |

Phase 2 needs no macro. The wrapper knows which objects it compiled from user
source and emits a fragment naming them:

```
.shmem_statics : ALIGN(4096) {
    __shmem_statics_start = .;
    user1.o(.data .data.* .bss .bss.*)
    user2.o(.data .data.* .bss .bss.*)
    __shmem_statics_end = .;
}
```

Mapped over with `MAP_FIXED` at init.

**Rejected:** runtime discovery of the whole data segment via `/proc/self/maps`
or `__bss_start`/`_end`. The main executable's data segment also holds the GOT
and `.init_array` pointers, which differ per process once libc is ASLR'd.
Sharing them corrupts every process simultaneously, and carving around them is
more fragile than generating the script.

### 4.4 Allocator

Fixed pool at init, sized by `SHMEM_SYMMETRIC_SIZE`.

> **The constraint is determinism, not nesting.** Symmetry requires that every PE
> holds the same offset for the same object. Since `shmem_malloc` and
> `shmem_free` are both collective and every PE executes the same sequence with
> the same sizes, any allocator that is a pure deterministic function of its call
> sequence produces **bit-identical** state on every PE. Out-of-order frees are
> fine. First-fit with coalescing, or a buddy allocator, both work. Fragmentation
> behaves exactly as in a single-threaded arena — every PE fragments identically,
> so symmetry is never at risk, only capacity.

The discipline is narrow but absolute: **never consult anything that varies
across PEs.** No timing, no PE rank, no address hashing, no randomization. Even
though `-no-pie` plus a fixed heap base makes pointer-derived decisions safe,
forbid them as hygiene.

**Metadata out of band.** Inline headers mean every local PE writes identical
bytes to the same shared pages — harmless but confusing, and it wastes shared
heap on bookkeeping. Private per-process metadata is cleaner, and since every PE
computes the same answer independently, **there is no cross-process allocator
lock at all.**

**Debug consistency check:** hash operation and size at each allocation, fold
into a running checksum, compare at the barrier the call already performs.
Mismatched sizes across PEs are undefined behaviour and will silently
desynchronize heaps; this catches it at the causing call.

`shmem_malloc_with_hints` is an alias for `shmem_malloc`. SOS itself added the
routine supporting no hint values.

### 4.5 Teams and allocation

**OpenSHMEM has no team-based allocation through 1.6.** The spec creates a
single symmetric heap implicitly, sized from `SHMEM_SYMMETRIC_SIZE`.

1.6 makes this explicit rather than inferred: change #10 states that
`shmem_{malloc, free, realloc, align, malloc_with_hints, calloc}` are **collective
operations on the world team** (§9.3.1–9.3.6). The §4.4 determinism argument
therefore rests on spec text, not on reading between the lines of 1.5.

Team-scoped allocation exists only as a **proposal** ("Memory Spaces"), where
allocation on a space is restricted to PEs of the creating team and child teams
inherit parent spaces for RMA/AMO/collectives but cannot allocate from them.
Future work, not 1.6.

**Consequence:** allocation stays world-collective, the deterministic allocator
is never challenged, and the identical-offset property is safe.

**Teams need no symmetric memory in this design.** Conventional `pSync` arrays
are an artifact of implementing collectives as RMA on symmetric scratch. Here
collectives are AMs: a barrier sends an arrival AM and waits for a release AM.
State is a private counter and a private flag. Nothing is the target of remote
RMA, so nothing must be symmetric. A team is a private descriptor: member list,
tree topology, counters. *Verify against each collective during the spec read.*

**Contexts** are purely local (`shmem_ctx_create` is not collective) and must
require no symmetric memory. A context is a sequence-number space. Confirm this
stays true as contexts are fleshed out; a context needing symmetric scratch
would be genuinely difficult.

### 4.6 Ordering and completion

Two completion domains that cannot be collapsed:

| Domain | Complete when | Evidence |
|---|---|---|
| Requests (put, signalled put, non-fetching AMO) | Applied at target | Peer's cumulative segment acknowledgement |
| Get replies | Delivered locally | Bytes landed in the destination |

The peer's acknowledgement of the segment containing a get *request* says only
that the descriptor was enqueued. The reply may not have been sent.

#### Gets do not participate in fence

Fence orders *writes becoming visible*. A blocking get has already returned its
data; a nonblocking get's remote effect is a read. Fence concerns puts,
signalled puts, and non-fetching AMOs. Gets are a quiet-only concern.

1.6 change #18 further clarifies that fence orders only operations issued on the
**same context** (§9.12.1). This is good news for §4.5's claim that a context is
nothing but a sequence-number space: per-context ordering is the weakest thing
the spec asks for, and a per-context dirty set delivers it directly.

#### Data structures

**Get completion — two cumulative counters per PE, no per-operation state:**

```c
uint64_t bytes_issued;   /* PE adds expected bytes on issue      */
uint64_t bytes_landed;   /* progress thread adds as chunks land  */
```

`quiet` waits for equality. This is exactly as expressive as the spec requires,
because `_nbi` completion is defined only at `quiet` — there is no
per-operation wait call. Cost: 16 bytes per PE. Byte-based accounting means the
responder may fragment replies freely.

**Peer scan — dirty set.** Each PE keeps the node indices it has touched since
its last `quiet`, plus its highest segment per node. Cost tracks the
application's communication degree, not job size: a halo exchange scans six
peers regardless of job scale.

*(Rejected: a node-wide global sequence with `acked_through` as the minimum
across connections. Cheapest per PE, but introduces false coupling — every
`quiet` everywhere waits for the slowest connection, so one straggler node taxes
the entire job.)*

```c
void shmem_quiet(void) {
    seal_and_submit_dirty_buffers();
    for (i = 0; i < pe->ndirty; i++)
        wait_until(acked[pe->dirty[i]] >= pe->high_seg[pe->dirty[i]]);
    wait_until(pe->bytes_landed == pe->bytes_issued);
    pe->ndirty = 0;
}

void shmem_fence(void) {
    for (i = 0; i < pe->ndirty; i++)
        seal_and_submit(pe->dirty[i]);
    /* does NOT clear the dirty set — those operations remain unacknowledged */
}
```

Conditional sealing costs **zero additional bytes**: the dirty set already
answers "which peers has this PE touched." Unconditional sealing would iterate
all connections — worse at 256 nodes, and it buys nothing.

Sealing forces out other PEs' operations too, since segments are shared. Always
correct, but a fence-heavy PE degrades aggregation for its neighbours. **Add a
counter.**

#### `shmem_pe_quiet` (1.6)

1.6 added `shmem_pe_quiet` (§9.12.3): the completion and visibility semantics of
`shmem_quiet`, restricted to a caller-supplied set of target PEs, returning
immediately when `npes` is 0.

This is the dirty set with a caller-supplied peer list substituted for the
accumulated one, so it is close to free here:

```c
void shmem_pe_quiet(shmem_ctx_t ctx, const int *target_pes, int npes) {
    /* intersect target node set with the dirty set; seal and wait on that */
}
```

Worth implementing in phase 1 despite being small, because it *validates the
data structure*: an implementation that tracked completion by a node-wide global
sequence (the alternative rejected above) could not implement this routine
without quieting far more than asked. The spec's own note — that performance
"might be equivalent to `shmem_quiet`" on some platforms — is an admission that
most implementations cannot do better. This one can.

Note the routine's completion set includes nonblocking **gets**, so it waits on
the `bytes_issued`/`bytes_landed` pair as well, which are per-PE and not
decomposable by peer. Either track landed bytes per peer node, or have
`shmem_pe_quiet` wait on the full get counter — the latter is conforming, costs
nothing to write, and should carry a comment saying why it over-waits.

#### Outstanding-get bound

`G` is **not** a spec concept. The spec permits unbounded outstanding `_nbi`
operations; an implementation that fails a loop of ten thousand
`shmem_get_nbi` calls before one `quiet` is broken.

A cap that causes **blocking** rather than failure is legal. Express it as a
**resource**, not an operation count: a per-connection descriptor pool sized in
bytes; exhaustion blocks the PE. Same bound, honest failure mode, with a counter
showing stall frequency. Size it large enough to be irrelevant to real codes —
16-byte descriptors make a pool of thousands cost megabytes.

#### Deferred-sampling hazard (documented, not fixed)

A get reply samples the heap at *drain* time. A 1 MB get draining over ~8 ms can
observe writes that arrived after its request was applied — including a put from
the same origin PE ordered behind it on the stream.

Strictly this weakens fence. The defence: modifying a remote object with an
outstanding nonblocking get on it is already undefined, so no conforming program
detects it. The drain window merely widens it from microseconds to
milliseconds, which will make racy applications fail more reliably.

**Debug-mode check:** per-region modification counter compared at drain time,
turning the undefined case into a loud diagnostic.

*(The alternative — sampling into a staging copy at handler time — reintroduces
the entire memory commitment §3.8 eliminated.)*

### 4.7 Get asymmetry (accepted, not fixed)

Blocking get is one round trip, unavoidably: the data does not exist locally
until it arrives. Prefetch, pipelining, and caching all break the memory model
or require unavailable analysis.

The asymmetry is in the **programming model**, not the implementation. Codes
that get-per-element are slow on every implementation on every fabric.

The node-level aggregation window is a genuine structural advantage, though:
gets issued by different local PEs coalesce into one segment, because the
connection is per node pair (§3.1) rather than per PE pair. An implementation
with per-PE endpoints cannot do this at all — there is nothing to coalesce
*into*. Stated as a property of the design rather than as a comparison, since
§1 no longer makes comparative claims.

Available: `_nbi` plus `quiet`; batching many gets into one segment;
`shmem_ptr` intranode, where get is a `memcpy` and the asymmetry vanishes.

Write replies **directly into the destination** rather than staging where
possible.

### 4.8 `shmem_ptr`

```c
void *shmem_ptr(const void *dest, int pe);
void *shmem_team_ptr(shmem_team_t team, const void *dest, int pe);  /* 1.6 §9.1.10 */
```

**The 1.6 variant is team-scoped, not context-scoped.** An earlier draft of this
document said otherwise; there is no context-taking `shmem_ptr` in any version.
With `SHMEM_TEAM_WORLD` the behaviour is identical to `shmem_ptr`; with
`SHMEM_TEAM_INVALID` it returns a null pointer. Given a working `shmem_ptr` it
is free, and it lands on the same single locality function as §8.1 — it must
return `NULL` across logical nodes too.

- `dest` must be a symmetric data object.
- Returns `NULL` when direct access is not possible. **Returning `NULL`
  unconditionally is conforming.** The intranode case is essentially free from
  the shared heap.
- Accesses through the returned pointer are **ordinary loads and stores, not
  OpenSHMEM operations.** `fence` and `quiet` do not order them. Completion
  routines do not apply. The application supplies its own synchronization,
  subject only to the architecture's memory model.
- Providing the returned address to a routine requiring a symmetric address is
  undefined.

**Design constraint:** a local store via `shmem_ptr` and a remote put to the same
location have no ordering relationship. Aggregation must not assume all writes
to the symmetric heap pass through the descriptor path.

**Document prominently:** code developed on x86 that omits barriers around
`shmem_ptr` access will fail on aarch64 under weak ordering. It is the
application's bug (spec §3.2 item 4 makes it undefined), but it will be reported
here.

### 4.9 Point-to-point synchronization

`shmem_wait_until` and `shmem_test` are phase 1 because they force the question
of **how a PE blocks without starving progress**. The answer has to hold when
the writer is the progress thread applying a remote AMO *and* when it is another
local PE storing through the shared heap.

> **Watch table for the wake decision; futex on the `ivar` itself for the wait.**

The `ivar` is a symmetric heap word in a shared mapping, so a cross-process
futex on that address works directly — no proxy word, no copy, and the value the
waiter compares is the value the waker wrote.

```c
/* waiter */
watch_insert(node->watch, ivar);              /* BEFORE the first check */
for (;;) {
    uint64_t v = atomic_load_explicit(ivar, memory_order_acquire);
    if (predicate(v, cmp, value)) break;
    futex_wait(ivar, v, SAFETY_SWEEP_MS);     /* -EAGAIN if changed: loop */
}
watch_remove(node->watch, ivar);
```

```c
/* progress thread, having applied an update to the heap */
if (watch_nonempty(node->watch) && watch_contains(node->watch, addr))
    futex_wake(addr, INT_MAX);
```

**The table is a set, not a registry.** It answers exactly one question — is
anyone waiting on this address — so it needs no PE list, no ownership, and
nothing beyond a small per-slot counter for concurrent waiters on one address. A
fixed open-addressed table of cache-line-padded slots, sized from local PE count
at init. Membership is the entire interface, which is why it stays cheap.

**Cost is one lookup per applied operation on the target side**, and only for
operations that write the heap. In the common case of an empty table it is one
load of a count and a branch — hence the `watch_nonempty` guard first.

**Insert before the first predicate check.** Checking and then inserting loses a
wake that lands in the window. This is the one ordering rule in the section that
is easy to get backwards and impossible to see in testing.

**Memory ordering** is the §3.5 discipline again: release on the waker's store,
acquire on the waiter's load. On aarch64 these are `stlr`/`ldar`.

**Use `futex2`, not `FUTEX_WAIT`.** `FUTEX_WAIT` compares 32 bits, and
OpenSHMEM's wait types include 64-bit ones. The `futex2` interface with
`FUTEX2_SIZE_U64` covers both and is available on the same 6.7+ kernel §3.10
already requires. The §3.4 8-byte alignment rule applies to the `ivar` as well:
an unaligned 64-bit futex word fails on aarch64.

**Pass the safety-sweep timeout**, for the same reason §3.10 does: a missed wake
becomes a latency spike with a counter rather than a hang. **Count
timeout-wakes.** A nonzero count in production means the watch table has a bug,
and that counter is the only thing that will ever tell you.

**`shmem_test` never blocks** — predicate check, no table interaction at all.
Likewise `shmem_test_all` and friends, which 1.6 change #19 clarifies return 1
on an empty test set.

**Spec constraints, both satisfied for free.** `ivar` must be updated using
OpenSHMEM AMOs (1.5 change #17), and §3.2 makes a wait or test on a location
exclusive with AMOs on that location of the same datatype. The progress thread
applies a remote AMO as an ordinary CPU atomic on the shared mapping (§4.1) —
the same instruction a local PE would issue — so both hold without further work.

**Rejected:**

- *Per-node generation counter with wake-all.* No table at all: the progress
  thread bumps a counter after any heap update and wakes every sleeper to
  re-check its own predicate. Trivially correct, and at 256 local PEs every
  update wakes 255 threads that immediately re-sleep. This is the §3.11
  thundering herd reached from a different direction.
- *Bounded spin then timed sleep.* No wake path to get wrong, but §3.10 already
  establishes that the Pi will not honour a short `nanosleep`, so the choice
  collapses to latency or a burned core — on hardware where a burned core is a
  progress thread or a mid-`memcpy` writer.

---

## 5. Collectives

### 5.1 Two-level structure

Reduce or gather within the node in shared memory, then one internode operation
between node leads. Internode cost becomes a function of **node count, not PE
count** — this is the answer to the PE-scaling concern.

### 5.2 Team accounting

Computed once at team creation, stored in the private team descriptor:

- **Local member count** — team members *on this node*, not local PE count. A
  node with eight PEs of which three are in team T has arrival target three.
- **Node participation** — the internode tree spans only nodes with at least one
  member. Topology is per team.
- **Local lead** — lowest team rank among local members. Must be a member, since
  its `dest` is the accumulator.

### 5.3 Intranode phase

**Flat counter to start.**

> At `L` local members all performing `fetch_sub` on one cache line, the line
> ping-pongs between cores and arrival cost is roughly `L` serialized cache-line
> transfers. Negligible at four. At 64 or 256 it dominates the intranode phase
> entirely. Fix: two-level intranode tree with fanout near core-cluster size,
> chosen at init from local member count.

The counter must be **alone on its cache line** regardless — false sharing with
the sense flag or team descriptor hurts even at four PEs, and one alignment
attribute prevents it.

**Arrival does not wait for a designated PE.** Whichever local member decrements
to zero enqueues the internode arrival AM and returns to waiting. One comparison
on the decrement result; no dependency on the lead being scheduled.

**Release** arrives at the progress thread, which flips the sense flag. The
progress thread never touches member `dest` buffers, so it does no
`O(members × count)` copying while other connections wait.

### 5.4 Slot reuse

**Sense reversal, one slot per team** for counter-and-flag collectives.

> Blocking constrains each PE individually and says nothing about *skew*. PE 0
> can return from a barrier and re-enter while PEs 1–3 have not yet observed the
> release. Resetting the counter corrupts the slot. With sense reversal the flag
> is never reset, only flipped; a PE from epoch N and one from N+1 look for
> opposite values.

Cost: one private boolean per PE per team.

**Two alternating slots by epoch parity** for data-carrying collectives, since
the data must be consumed before the next epoch overwrites it. Skew is bounded
at one epoch because a PE two epochs behind would have had to pass the
intervening barrier.

**Sense reversal assumes a PE participates in every epoch.** Guaranteed for
blocking collectives. Nonblocking collectives would break it — record the
assumption explicitly.

### 5.5 Reduction destination

**Accumulate into the local lead's `dest`.** Local members combine into it, the
progress thread exchanges it internode, the final value lands there, and each
member copies out after observing the release.

Legal because `dest` is undefined until the collective returns, so intermediate
values are unobservable by any conforming program. Handles the in-place case
naturally.

This eliminates the scratch-sizing problem entirely — no chunking loop, no
high-water allocation, no per-team data area. The team's shared footprint
reduces to a counter and a flag, ~128 bytes regardless of reduction size.

Contention on the lead's `dest` is identical to what shared scratch would have
had, so nothing is lost; at high local member counts both want the same tree
treatment as the counter.

*(Fallback for cases where `dest` cannot serve: chunk through fixed scratch. The
loop costs nothing on small reductions and adds pipelining on large ones.)*

### 5.6 Barrier versus sync

**`shmem_barrier_all` includes a quiet.** `shmem_team_sync` and `shmem_sync` do
**not** — they synchronize only, leaving completion to the caller.

`barrier_all` must seal the dirty set and wait for acknowledgements before
arriving; `team_sync` may arrive immediately. Implementing `team_sync` as a
barrier-with-quiet is correct but needlessly slow. Implementing `barrier_all`
without the quiet is incorrect and would pass most tests — the failure is
silent.

**Confirmed against 1.6 §9.10.1**, whose Notes state it outright: `shmem_barrier_all`
"is equivalent to calling `shmem_ctx_quiet` on the default context followed by
calling `shmem_team_sync` on the world team." Note the quiet is on the **default
context** specifically, which matters once contexts are more than a formality.

### 5.7 Team resources

Fixed pool carved at init, world-collectively, while that is still possible.
`shmem_team_split_strided` is collective over the *parent* team, not the world,
so it cannot perform a world-collective allocation.

**`max_teams` is a runtime value, read from the environment during init**, and
**the same parameter bounds the control queue (§3.8)** and the team range
allocator. Three constraints resolve against one number — one thing to document,
one place to fail loudly.

Runtime rather than build-time, despite the init-ordering cost: the pool cannot
be sized until the bootstrap has supplied local PE count, so team-pool sizing
must happen after §6 completes and before anything that allocates node-level
structures. That ordering is worth getting right once. A build-time constant
would be simpler, but forcing a rebuild to raise a limit is exactly the friction
§1 exists to remove.

*(A build-time ceiling with a runtime value below it was considered — it keeps
every structure statically sized. Rejected because the ceiling reintroduces the
rebuild, which is the whole objection.)*

**Team ranges leak until `finalize`**, with a counter. `shmem_team_destroy` is
described in 1.6 §9.4.8 only as "a collective operation that destroys the team
referenced by team" — **the spec never names the scope it is collective over.**
The natural reading is over the team being destroyed, in which case parent PEs
outside the child cannot observe it and ranges cannot be reclaimed without
divergence. Since the ambiguity is in the standard rather than in our reading of
it, leaking is the only safe choice, and it costs nothing: teams are typically
created once during setup.

Two related requirements from the same section: destroy also destroys every
**shareable** context created from the team, while **`SHMEM_CTX_PRIVATE`**
contexts are the user's responsibility to destroy first or behaviour is
undefined. Destroying a predefined team is undefined; destroying
`SHMEM_TEAM_INVALID` is a no-op. The no-op case is worth implementing literally
rather than asserting on it.

---

## 6. Bootstrap

Thin internal interface, as before. What changed is which backend is primary.

> **Environment variables plus a TCP rendezvous. No PMI link dependency.**

§1's dependency budget makes this a requirement rather than a preference: PMI-2
means linking `libpmi2-0t64` and PMIx means `libpmix2`, and both are exactly the
kind of system package the project exists to avoid. The PMIx-availability
question that blocked the previous draft is not answered here — it is deleted,
because nothing links against PMIx any more.

### 6.1 What Slurm already tells us

Everything the bootstrap needs is in the environment of every task:

| Variable | Supplies |
|---|---|
| `SLURM_PROCID` | Rank |
| `SLURM_NTASKS` | Size |
| `SLURM_NODEID` | **Node index — the node grouping, directly** |
| `SLURM_LOCALID` | **Local rank within the node — the election input, directly** |
| `SLURM_TASKS_PER_NODE` | Local member counts for every node (§5.2) |
| `SLURM_NODELIST` | Hostnames, for the rendezvous address |

This is *better* than the PMIx path it replaces. `SLURM_NODEID` and
`SLURM_LOCALID` hand over node grouping and local-lead election with no
agreement protocol at all, where the PMI-2 path required a hostname-hash-into-KVS
dance and the PMIx path required a library. The node map falls out of the
environment before a single socket is opened.

Note this is also exactly the shape §8.1 wants: locality is already a
configuration input rather than something derived from `gethostname()`, so
`SHMEM_NODE_MAP` simply overrides `SLURM_NODEID` and the two paths share one
code path.

### 6.2 Rendezvous

Rank 0 binds a listening socket; every other PE connects; ranks, addresses, and
the segment key are exchanged; the full node map and connection table are
broadcast back. One round trip, no KVS, no external daemon.

Finding rank 0's address is the only real work:

- `SLURM_NODELIST` is a compressed hostlist (`pi[01-04,07]`). Expanding it is a
  small parser — call it fifty lines, and **write it with unit tests in tier 1**,
  because it is pure string handling that will otherwise be debugged on a
  cluster.
- Under Slurm's default block distribution, rank 0 is on the first expanded
  host. **Do not rely on the default.** Have rank 0 publish its resolved address
  and port, and have everyone else learn it the same way regardless of
  distribution policy.
- Port: an environment variable with a default, plus retry on bind conflict so
  two concurrent jobs on one node do not collide.

**Non-Slurm launch** (plain `ssh`, a test harness, `mpirun`) sets the same
variables under a `SHMEM_` prefix that takes precedence. The tier-1 and tier-2
harnesses of §8 use this path, which means the bootstrap is exercised by every
test rather than only on a cluster.

### 6.3 PMI-2 as an optional backend

Keep the thin interface and keep a PMI-2 backend behind it, **off by default and
not linked unless requested at build time**. It costs little, and it is the
escape hatch for a launcher that does not export usable environment variables.
It is not on the default path and nothing in the build requires it to be
present.

### 6.4 Initialization lifecycle

1.6 change #2 allows `shmem_init` and `shmem_finalize` to be called multiple
times and adds `shmem_query_initialized` (§9.1.1, §9.1.4, §9.1.5). This is
architectural here even though the implementation is small, because §4.4's
allocator pool, §5.7's team pool, and §6.2's connection table are all carved once
at init and torn down at finalize.

**Reference count.** First `shmem_init` performs bootstrap, mapping, and pool
construction; subsequent calls increment and return. `shmem_finalize` decrements;
the last one tears down. `shmem_query_initialized` reports whether the count is
nonzero.

Three things to get right, each of which is a silent bug otherwise:

- **The count is per process, not per thread**, and must be updated atomically.
- **A re-init after a full teardown is a fresh job**, not a resumption: new
  rendezvous, new pools, and PE numbering that must come out identical or the
  symmetric heap is not symmetric. Simplest correct behaviour is to re-run the
  whole of §6 rather than caching anything across the boundary.
- **`max_teams` is read at the *first* init** (§5.7) and applies for that
  session's lifetime. If a later init in the same process picks up a changed
  environment, the pools differ from what the application saw last time. Either
  re-read it and document that, or latch it at first init and document *that* —
  but do not leave it accidental.

---

## 7. Spec Subset

**Criterion: what forces an architectural decision, however small.** Mechanical
expansion of an existing decision waits indefinitely.

### Phase 1

| Routine | Forces |
|---|---|
| `shmem_init`, `finalize`, `my_pe`, `n_pes` | Bootstrap, node topology, shared segment, election |
| `shmem_query_initialized`, repeated `init`/`finalize` | **Pool lifetime.** §4.4, §5.7 and §6.2 all carve once at init; refcounting is small but the teardown boundary is architectural (§6.4) |
| `shmem_malloc`, `shmem_free` | Deterministic allocator, identical offsets |
| `shmem_putmem`, `shmem_getmem` | Staging, descriptors, aggregation |
| `shmem_quiet`, `shmem_fence` | Completion model |
| `shmem_barrier_all` | Two-level collective skeleton all others inherit |
| `shmem_uint64_atomic_fetch_add`, `compare_swap` | **Unification of proxy-applied and locally-issued atomics.** Defer all AMOs and you build a transport that cannot express them |
| `shmem_wait_until`, `shmem_test` | How a PE blocks without starving the progress thread |
| `shmem_ptr`, `shmem_team_ptr` | Nearly free from the shared heap; large intranode win; forces the single locality function (§8.1) |
| `shmem_put_signal` | Fits aggregation well; how modern codes express producer-consumer |
| `shmem_signal_add`, `shmem_signal_set` | Signal-word update with no data transfer — AMO-shaped, not put-shaped, so they land on §3.8's handler-time rule rather than the payload path |
| `shmem_pe_quiet` | Validates the §4.6 dirty set: a global-sequence completion model could not implement it without over-quieting |

### Plumb but do not implement

A **context handle** on every RMA entry point and a **team parameter** on every
collective, implementing only `SHMEM_CTX_DEFAULT` and `SHMEM_TEAM_WORLD`.
Retrofitting either parameter later touches every function written.

### Deferred (mechanical)

Full type/operation cross product, strided transfers, nonblocking variants,
broadcast/reduce/collect/alltoall, locks.

### Deferred 1.6 additions

| Routine | Why it waits |
|---|---|
| `shmem_ctx_session_start` / `_stop` (§9.9) | Hints only. A conforming implementation may ignore them entirely, so the no-op is the whole phase-1 implementation |
| `shmem_sum_inscan`, `shmem_sum_exscan` (§9.10.10) | New collective *pattern*, but it inherits the §5.1 two-level skeleton; nothing architectural once barrier exists |
| `shmem_ibget`, `shmem_ibput` (§9.6.1.4, §9.6.1.8) | Interleaved block transfer — an addressing pattern above the chunking layer, mechanical given `(region, offset, length)` chunks |

Session routines are worth stubbing early precisely *because* they are ignorable:
an empty implementation is conforming, and having the entry points present stops
the question being reopened.

---

## 8. Testing

### 8.1 Logical node map

> **Locality is a configuration input, not something derived from
> `gethostname()`.**

Eight PEs on one machine configured as four logical nodes of two. PEs sharing a
logical node attach the same shared segment and take the intranode path; PEs in
different logical nodes open real TCP connections and take the internode path.
Both paths run in their real form on one machine.

```
SHMEM_NODE_MAP=0,0,1,1,2,2,3,3
```

Two details that must be planned rather than patched:

- **Shared segment name keyed on logical node id**, not physical host, or four
  logical nodes accidentally share memory.
- **`shmem_ptr` must return `NULL` across logical nodes** even though the pages
  are physically reachable. The locality decision lives in exactly one function
  that everything else consults.

Plus a **force-remote flag** routing even same-logical-node traffic over the
network. Running a program both ways and diffing is the strongest differential
test, since the shared-memory path is simple enough to trust as the reference.

**Three poles, not one:** all PEs in one logical node; one PE per logical node;
and a mixed asymmetric map (`0,0,0,1,2,2`) exercising uneven node sizes and
non-contiguous rank-to-node mapping. **Uneven sizes are where two-level
collectives break** and a uniform map will never show it. Multiply by fast and
slow mode: six passes over a suite that runs in seconds at tier 1.

### 8.2 Tiers

| Tier | Environment | Catches |
|---|---|---|
| 1 | In-process AM backend, queues not sockets, single-threaded, deterministic | Most semantic bugs. Runs in CI anywhere |
| 2 | Logical nodes over loopback | Integration, threading |
| 3 | Logical nodes in netns over veth with netem | Most transport bugs |
| 4 | Real cluster | Everything else |

> **Loopback hides the worst bugs.** Effectively unlimited MTU, no loss, no
> reordering, enormous buffers, microsecond latency. It will never produce a
> short write or partial read — and partial-write handling is the most common
> defect in hand-rolled TCP framing.

Mitigations: deliberately small `SO_SNDBUF`/`SO_RCVBUF` so partial writes
actually occur; separate network namespaces over veth with `netem` for delay,
loss, reordering, and small MTU. A Pi handles four namespaces; the setup is a
shell script written once.

### 8.3 Without Pi hardware

Tiers 1–3 run unchanged on x86. **x86 loopback is the adversarial case, not the
easy one:** every decision justified by "wire latency dominates by two orders of
magnitude" — copy into staging, futex over spin, generous chunking — is least
defensible when network and cores are fast. Holding on modern x86 loopback means
holding on a Pi with room to spare. Sweep netem delay from zero to Pi-like and
watch where the constants stop working; a better experiment than one point
measurement on real hardware.

**What x86 cannot give: weak memory ordering.** Every missing release store and
acquire load in the buffer protocol passes on x86 and fails on aarch64, and
those bugs appear under load months later. Given how much rests on the
`head`/`filled`/state protocol, **do not defer aarch64 validation.**

Options: a cloud ARM instance (Graviton, Ampere) as a per-commit CI target;
Apple Silicon under a Linux VM; or a single Pi, which covers intranode plus
loopback logical nodes though not a real fabric. ThreadSanitizer on x86 catches
a useful subset but models the C11 memory model rather than the hardware.

**Sequencing:** iterate on x86, run the full suite on one ARM target per commit,
defer the cluster until the AM suite passes on both.

### 8.4 Floor harness

§1 states performance against local floors rather than against another
implementation. That only works if the floors are themselves test artifacts, so
they are part of the suite rather than a benchmarking afterthought:

| Harness | Floor it establishes |
|---|---|
| `echo-uring` | ~200 lines: io_uring, one connection, ping-pong an 8-byte payload. The round-trip floor, and simultaneously the smallest possible reproducer for an io_uring bug |
| `echo-uring --stream` | Bandwidth floor on the same link. `iperf3` is a useful cross-check where it is installed, but it is not a dependency |
| `memcpy` plus the required synchronization | Intranode floor |

Two properties make this worth the effort. The floors need nothing installed, so
they run in CI on x86 and on the ARM target alike and give a **ratio** that is
comparable across wildly different hardware — which is the only way a number
measured on a laptop says anything about a Pi. And `echo-uring` doubles as the
§10 latency-assumption experiment: the whole design rests on wire latency
dominating memory bandwidth by two orders of magnitude, and this harness is how
that stops being an assumption.

**Pin the constants after the first run, then treat regressions as failures.**
Ratios, not absolute times, so the thresholds survive a hardware change.

---

## 9. Load-Bearing Invariants

Each is independently breakable, and each failure appears far from its cause.

1. **A handler may enqueue, never reserve.** (§3.7) Violating it reintroduces
   single-node self-deadlock.
2. **One in-flight send per connection.** (§3.3) Violating it silently reorders
   the stream and breaks fence, ordering, and signal-after-payload.
3. **A single ordered stream per node pair.** (§3.1) A second data socket
   destroys free fence.
4. **The progress thread never waits for `filled == head`.** (§3.5) Violating it
   hangs under oversubscription.
5. **Signals land after every payload byte**, guaranteed by in-order application
   of a single stream. (§3.2)
6. **The allocator is a pure deterministic function of its call sequence.**
   (§4.4) Violating it desynchronizes heaps silently.
7. **The wire format never depends on identical virtual addresses.** (§4.2)
8. **The wakeup mechanism affects only how soon a sealed buffer is submitted,
   never whether.** (§3.10)
9. **Barrier queue bounds rest on collectives being blocking.** (§3.8) A future
   `shmem_barrier_nbi` dissolves the argument.
10. **Sense reversal assumes participation in every epoch.** (§5.4) Same
    caveat.
11. **AMO result descriptors carry values; get descriptors carry references.**
    (§3.8) Unifying them reorders AMOs.
12. **The completion queue is sized from the provided-buffer ring, not from the
    submission-queue bound.** (§3.3) One multishot SQE yields many CQEs; applying
    the SQ bound to the CQ reintroduces an overflow path under load only.
13. **A waiter inserts into the watch table before its first predicate check.**
    (§4.9) Reversing them loses a wake that lands in the window, and no test will
    show it.
14. **The application layer owns coalescing; `TCP_NODELAY` is always set.**
    (§3.3) Nagle plus delayed ACK holds submitted segments, which `quiet` cannot
    see and the safety sweep does not catch.
15. **Nothing links but libc and the vendored liburing.** (§1) Not a style rule:
    it is the project's reason for existing, and each dependency added is one the
    target hardware may not be able to build.

Assert what can be asserted. Instrument the rest.

---

## 10. Open Questions

### Closed since the previous draft

Recorded rather than deleted, so they are not reopened from memory.

| Question | Resolution |
|---|---|
| Which SOS configuration is the baseline? | **Withdrawn.** libfabric does not build usably on the target hardware, which is the project's premise, not an obstacle to measuring it. Replaced by the §1 floors |
| `shmem_ptr` context-variant signature | **No such thing.** The variant is team-scoped: `shmem_team_ptr` (1.6 §9.1.10). §4.8 corrected |
| `barrier_all` versus `sync` quiet semantics | **Confirmed** (§9.10.1 Notes): `barrier_all` ≡ `shmem_ctx_quiet` on the default context, then `shmem_team_sync` on world |
| `shmem_team_destroy` scoping | **Ambiguous in the standard**, which never names the scope. Leak team ranges until finalize (§5.7) |
| Team-based allocation | **Does not exist through 1.6.** 1.6 change #10 states malloc and friends are world-team collective (§4.5) |
| PMIx on the actual nodes | **Moot.** Nothing links against PMIx or PMI-2 on the default path (§6) |
| `max_teams` runtime or build-time | **Runtime**, read at first init, bounding three structures (§5.7) |
| How a PE blocks in `wait_until` | **Watch table plus `futex2` on the `ivar`** (§4.9) |
| io_uring ring bound | **Two bounds, two rings.** CQ is sized from the provided-buffer ring (§3.3) |

### Still open — blocking

- **Thorough 1.6 spec read.** The targeted questions above are answered; the
  sweep is not done. Specifically still unchecked: whether any team query must
  return remotely accessible memory; the exact semantics of the 1.6 session
  routines if they are ever more than no-ops; and §9.10.9's clarification that
  source and dest arrays must match across PEs in reductions (1.6 change #17),
  which may constrain §5.5's accumulate-into-lead's-`dest` scheme.

### Still open — measurement, once hardware exists

- **8-byte round trip and small-message rate, two nodes** (§8.4). Everything
  above assumes wire latency dominates memory bandwidth by two orders of
  magnitude. **This assumption is load-bearing** for copy decisions, futex
  decisions, and chunk sizing. Probably right; worth an hour to confirm rather
  than carry. Now has a home in the test suite rather than being a one-off.
- **Pin the §1 floor ratios.** Deliberately unwritten until the harness runs.

### Still open — design, deferrable

- Does any collective need RMA on symmetric scratch rather than AM? (Answer so
  far: no.)
- Reduce-into-destination and `SHMEM_TEAM_INVALID` — confirm no path expects a
  non-member's `dest` to be written.
- Whether the AM layer's maximum payload should be advertised or fixed.
- **Does `shmem_pe_quiet` warrant per-peer landed-byte accounting?** (§4.6)
  Waiting on the global get counter is conforming and free; splitting it per peer
  node costs 16 bytes per peer and only pays for applications that actually
  restrict quiet by PE.
- **Does the watch table need a fast path for the single-waiter case?** (§4.9)
  The empty-table check is one branch; the one-entry case is plausibly the
  common one in real codes and might justify a cached last-inserted slot.
  Measure before adding.
