# OpenSHMEM over Ethernet: Design Notes

**Status:** Path-finding. No implementation. Decisions recorded with rationale so
that they are not silently reversed.

**Project:** osoe (OpenSHMEM Over Ethernet) — installs `libosoe` and the spec's
`shmem.h`.

**Target:** OpenSHMEM 1.6 (1st November 2024). Section references of the form
§9.x.y are to the specification; §N.M without a leading 9 are to this document.

---

## 1. Goals and Constraints

- Implement OpenSHMEM 1.6 over commodity Ethernet with no fabric offload.
- **Build entirely from the distribution's own packages.** Primary goal, not a
  preference.
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

> **Dependency budget.** Link time: libc and liburing ≥ 2.6. Build time: a C11
> compiler, CMake ≥ 3.25, and pkg-config. All from the distribution. Nothing is
> bundled.

**Require a new enough OS rather than bundling.** Most of liburing's API is
inline functions in `liburing.h`, so the features available are fixed by the
headers present at *build* time, independently of the running kernel.
`io_uring_prep_futex_wait` (§3.10) first appeared in liburing 2.6. Debian 13
ships 2.9; Debian 12 ships 2.3.

An earlier draft vendored liburing to cover systems whose kernel is new enough
but whose userland is not — Raspberry Pi OS Bookworm, with a newer kernel over
Bookworm's liburing, is the likely case. **That case is deliberately
unsupported.** Bundling would solve it at the price of a submodule, a second
build system (liburing's own configure), and an embedded library the
distribution cannot patch — too much for an OS generation already being
replaced. The supported floor is an OS that ships both a 6.7+ kernel and
liburing 2.6+: Debian 13 and Raspberry Pi OS Trixie.

**Two checks, two failure times.** CMake requires `liburing >= 2.6` through
pkg-config and stops at configure time, saying why. At startup the library checks
that the *running kernel* supports what it needs — the `tools/probe` checks,
moved into init — and aborts loudly otherwise. The first catches old headers; the
second catches an old kernel under new headers.

**CMake is a build tool, not a dependency chain.** What this project exists to
avoid is a dependency tree that does not build on the target hardware, not the
need for a build tool. CMake 3.25 is in Debian 12 and 13 and in Raspberry Pi OS,
and it provides presets for the build variants, ctest labels for the test tiers
(§8.2), install rules, and `compile_commands.json`.

*(Rejected: bundling liburing, above. Raw `io_uring_setup`/`io_uring_enter` with
hand-mmapped rings — the subset used is small enough to tempt, but it hands over
liburing's kernel-feature handling to own. Plain Makefiles — dependency
tracking, install, sanitizer variants, and test tiers all hand-written. Meson —
cleaner, but Python and Ninja are two build dependencies where CMake is one.)*

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
head-of-line blocking while keeping a single ordered stream. The bound for a
control message is roughly **two segments** — the one in flight and the one
sealed behind it — plus any publication-window stall (§3.6); an earlier draft's
"one chunk time" understated it.

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
must be tested; it is a good fault-injection target. A multishot recv can also
end on any CQE that lacks `IORING_CQE_F_MORE`, not only on `-ENOBUFS`; every such
CQE re-arms.

**Parse as a stream; never hold a provided buffer.** Segments and messages span
provided buffers. Holding buffers until a segment is complete stalls once a
segment outgrows the ring — and with one ring serving several connections, two
connections each holding buffers for partial segments deadlock (review C1,
reproduced). So each connection keeps a parse cursor:

- header bytes accumulate in a small per-connection area;
- payload bytes are copied straight to their destination — heap offset, landing
  zone, or reply staging — as they arrive;
- AMO and signal messages are applied only once complete;
- **every provided buffer is recycled before the next reap.**

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

*Submission queue:* `2 × connections + constant` — one send and one multishot
recv per connection, plus the constant: at most one futex wait per ring (§3.10),
the cancel and re-arm SQEs in flight at once, and — on the lead's first ring — one
pidfd poll per local PE (§3.15). Size above that and it can never
fill. **Assert at init** — it
breaks silently the day someone adds a second in-flight send.

*Completion queue:* the SQ bound does **not** carry over, because one multishot
recv SQE produces many CQEs. Pending receive completions are bounded instead by
how many provided buffers the kernel can fill:

> `CQ entries ≥ pbuf_ring_entries + 2 × connections + 1 + slack`, set explicitly
> via `IORING_SETUP_CQSIZE`. The `+ 1` is the ring's single armed futex wait
> (§3.10); an idle loop that arms more than one breaks this bound. The lead's
> first ring adds `L`, one completion per local PE's pidfd poll (§3.15).

This restores "can never fill" for both rings and puts both asserts at init. The
cost is a few hundred KB of CQEs — irrelevant beside the buffer ring itself, and
cheap for deleting an error path.

**Verified** by `tools/probe` on 6.12 aarch64: a multishot recv over TCP
loopback with an 8-entry buffer ring and no replenishment produced exactly 8 data
CQEs carrying `IORING_CQE_F_MORE`, then one terminal `-ENOBUFS`. One completion
per provided buffer plus one terminal completion per armed recv — the shape of
the formula exactly.

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
**At most one of them is reservable at any moment** — a rule (§3.5), not a sizing
remark.

```
[ segment header ][ message ][ message ][ message ] ...
```

Segment header (fixed size, 16–32 B, at offset 0, never reserved by writers,
filled by the progress thread at seal time):

- **Sequence number** — makes `quiet` nearly free (see §4.6). *Assigned* when the
  buffer becomes reservable and *written* here at seal (§3.5).
- **Acknowledgement** of the reverse direction: the highest sequence number
  *applied*, not merely received, since completion means applied at the target
  (§4.6). Piggybacked when possible; see ack-only segments below.
- **Flags**, including *ack requested*.
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

**Ack-only segments.** Piggybacking alone never acknowledges one-way traffic. If
node A only puts to B, no B→A segment exists to carry the ack, and A's `quiet`,
`barrier_all`, and finalize wait forever: the safety sweep submits only sealed
buffers, and B has none.

- A `fence`, `quiet`, or barrier flush sets *ack requested* on the segments it
  seals.
- On applying such a segment, B's progress thread carries the ack on the next
  segment toward A if one is sealed or in flight, and otherwise sends a
  **header-only segment**: acknowledgement, zero messages.
- Header-only segments reserve nothing, so §3.7's handler rule is untouched. They
  use the connection's single in-flight send like any other segment.

### 3.5 Reservation protocol

Writers are application threads. Two atomics per buffer, no locks.

```c
/* head: reserved bytes in the low bits, SEALED in the top bit.
 * Every buffer that is not reservable carries SEALED. */
for (;;) {
    uint32_t h = atomic_load_explicit(&buf->head, memory_order_acquire);
    if (h & SEALED) {                       /* sealed: use the current buffer */
        buf = current_reservable(conn);     /* waits if there is none (§3.11) */
        continue;
    }
    if (h + n > buf->cap) {                 /* full: ask for a seal, then wait */
        request_seal(conn, buf);
        buf = current_reservable(conn);
        continue;
    }
    if (atomic_compare_exchange_weak_explicit(&buf->head, &h, h + n,
            memory_order_seq_cst, memory_order_acquire))
        break;
}
uint64_t seq = buf->seq;                    /* stable: see "Sequence numbers" */
memcpy(buf->data + h, src, n);
atomic_fetch_add_explicit(&buf->filled, n, memory_order_seq_cst);
```

**The seal is a bit in `head`, set by the progress thread's CAS.** Writers never
read a separate `state` field. An earlier draft checked `state == OPEN` and then
CASed `head`; the progress thread could seal and submit between the two, and the
writer's CAS still succeeded. The message was lost or torn, and `filled` exceeded
`head` in the next generation. Setting the seal bit changes the value every
later writer CAS compares, so a sealed buffer rejects them all. `state` survives
only as the progress thread's own bookkeeping.

**At most one reservable buffer per connection.** A freed buffer becomes
reservable only when the other buffer is not — sealed, in flight, or retired
because it is empty. Buffers are submitted in seal order. With two reservable
buffers, one PE's later operation can be sealed and sent first, which breaks
`fence`, signal-after-payload (invariant 5), and `quiet`. An earlier draft's
"seal; try other buffer" was exactly that bug.

Both defects were reproduced by two independent exhaustive models, and the
repaired rules above pass both, including DRAINING, progress-thread reply
reservations, and a liveness check (`docs/reviews/evidence/transport/`,
`docs/reviews/evidence/refutation/`).

**Sequence numbers are assigned when a buffer becomes reservable**, in submission
order, and written into the header at seal together with the ack, message count,
and length. A writer reads the buffer's sequence number after a successful
reservation with no extra synchronization: the buffer cannot recycle until
`filled == head`, and this writer has not completed. Comment this so nobody later
"fixes" it with a lock. *(Assigning the number at seal instead lets a writer
record the previous generation's number, so `quiet` returns early — also
model-checked.)* Numbers assigned at open are monotone on the wire only because
of the one-reservable rule.

**CAS loop, not `fetch_add`.** With `fetch_add`, an overflowing writer has
already moved `head` and must unwind, and the unwind must still account for
itself in `filled` or the buffer never becomes sendable, while concurrent
overflowing writers race to seal. The CAS makes a failed reservation a
non-event.

**`filled` is a count, not a watermark.** Out-of-order completion means no
prefix property holds. This is fine: the buffer is sendable when
`filled == head & ~SEALED`. Do not reach for a watermark and then discover it
cannot be maintained.

**Buffer states** are the progress thread's bookkeeping: `FREE → [DRAINING →]
OPEN → SEALED → SENDING → FREE`. Writers see only the seal bit. A `DRAINING`
buffer (§3.8) carries the seal bit, so writers skip it while the progress thread
fills it with replies; opening it clears the bit. The progress thread submits only
once `filled` equals the reserved count.

**The progress thread must never spin waiting for `filled` to catch up.** A
writer may be descheduled mid-`memcpy`. Move to other connections and re-check
later. At 256 PEs on an oversubscribed node, a spin here is a hang.

**Memory orders** follow §3.16: loads name `acquire`; the CAS and `fetch_add`
name `seq_cst` and compile to locked instructions on every target. This protocol
is single-writer publication — each `filled` update is a locked instruction the
progress thread later loads — so it is also safe under x86's store buffering.

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
request/reply framing, because collective forwarding (§3.8) is a handler issuing
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
> *descriptor* commits 48 bytes. For 250,000 outstanding replies that is the
> difference between roughly a gigabyte and roughly 12 MB. The reply's source data
> is already in the symmetric heap and needs no copy until send time.

**Bounded by a per-connection credit, not a per-PE count.** An earlier draft sized
each connection's queues as P × G — every PE on the peer node times a per-PE
outstanding-get cap. Summed over connections, that grows with *job* size: about
6 GB per node at 500 nodes if pre-allocated. Instead, each node pair has a credit
`C` for outstanding replies. The origin's lead takes a credit when one of its PEs
issues a get or fetching AMO toward that peer and returns it when the reply
lands; with no credit left, the issuing PE blocks, which the spec permits (§4.6).
The target's worst case is then `connections × C` descriptors — it scales with
node count, as §1 requires. Descriptors come from one **node-wide pool** that each
connection's queues link into, so idle connections cost nothing.

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
reserve. It drains, then transitions to `OPEN`. One extra state, no fairness
arithmetic. Instrument bytes drained per generation and queue-depth high-water.

**Cap each DRAINING pass at half the buffer.** Uncapped, the guard starves the
other direction: whenever outstanding reply bytes exceed roughly (round-trip
generations + 1) buffers, every freed buffer fills entirely with replies and local
PEs' puts to that peer never go out. A segment-level simulation shows local PEs'
share of segment bytes at zero with 16 PEs doing 64 KB gets at a realistic one-
to two-generation round trip (review D8, `docs/reviews/evidence/transport/`). The cap
guarantees both directions progress every generation.

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
   (a high-water mark subsumes older ones); piggybacked or header-only (§3.4), so
   not really a queue entry.

Size as `C + degree×max_teams + slack` per connection, drawn from the node-wide
pool.

**The control queue is multi-producer** once a node runs more than one progress
thread: a collective forward received on one shard's connection is enqueued
toward a peer owned by another shard, with a wake to that shard. Pre-allocate at maximum. **Assert
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
traffic to clock it. The flush is a request from the application thread to the
lead (§3.12), and it sets *ack requested* so a one-way peer still acknowledges
(§3.4).

### 3.10 Idle and wakeup

`IORING_OP_FUTEX_WAIT` (6.7+). A counter in userspace; nothing to drain.

```c
for (;;) {
    uint32_t v = atomic_load_explicit(&ep->wake, memory_order_acquire);
    reap_cqes();                     /* the futex CQE, when it arrives, clears armed */
    submit_sealed_buffers();
    if (work_pending()) continue;
    if (!ep->futex_armed) {          /* at most one armed wait per ring */
        io_uring_prep_futex_wait(sqe, &ep->wake, v, FUTEX_BITSET_MATCH_ANY,
                                 FUTEX2_SIZE_U32, 0);
        ep->futex_armed = true;
    }
    io_uring_submit_and_wait_timeout(&ep->ring, &cqe, 1, &sweep, NULL);
}
```

PE side: `atomic_fetch_add_explicit(&ep->wake, 1, memory_order_seq_cst)`, then
`futex_wake`.

**One armed wait per ring.** An earlier draft armed a new
`IORING_OP_FUTEX_WAIT` on every idle pass while earlier ones were still armed. One
`futex_wake` then completes all of them at once and overflows the completion
queue: 1,881 armed waits and the kernel overflow flag set, in a run with real TCP
receive (review D3). The `futex_armed` flag is private to the progress thread. A
wait armed on an older value stays correct: any later increment is followed by a
`futex_wake`, which completes it, and an increment between the load and the arm
makes the arm return `-EAGAIN`.

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

**Safety sweep:** pass a ~100 ms timeout (`io_uring_submit_and_wait_timeout`,
which uses `IORING_ENTER_EXT_ARG`) and submit
anything sealed regardless of signalling. This converts a class of hangs into a
class of latency spikes with a findable counter. The wakeup mechanism must
affect only *how soon* a sealed buffer is submitted, never *whether*.

**Two timeout conventions in one codebase.** The `IORING_ENTER_EXT_ARG` timeout
here is *relative*. The `futex_wait` syscall used for PE-side waits (§4.9, §5.4)
takes an *absolute* deadline on a named clock. A relative value passed there
does not hang: it names an instant shortly after boot, so every wait returns
`ETIMEDOUT` at once and the waiter silently becomes a busy loop. The environment
probe made exactly this mistake on its first run. Wrap each call once, expose
only a deadline-computing helper, and count timeouts — a busy loop disguised as a
wait shows up as a timeout rate orders of magnitude above normal.

### 3.11 Waiter fairness

Waiters are held on a priority-sorted list; `FUTEX_WAKE(1)` dequeues the head,
FIFO for equal priority. **Verified** against `kernel/futex/core.c` on 6.12:
real-time waiters first, then FIFO. In the development VM a `SCHED_FIFO` waiter
jumped the queue, which supports the no-real-time-priority rule below.

**One futex word per resource class, never shared.** Buffer space (divisible),
reply credits and descriptors (indivisible), and the wait for a reservable buffer
(§3.5) each get their own word per connection. If two classes share a word,
wake-one can reach the wrong class's waiter; with §3.10's deadlines that is a
latency spike rather than a hang, but it is avoidable (review C4).

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

The handoff bit shares a word with the resource it guards. **Model the word
layout** — which bits, which stores, which wakes — before implementing it, with
the same exhaustive tooling as §3.5.

Applied to **indivisible resources only**; divisible ones do not starve.

**No spinning before blocking, in phase 1.** A useful spin loop needs a CPU relax
hint, which is an intrinsic or assembly, and §3.16 allows neither. A spin without
one burns power and, on x86 with SMT, slows the sibling thread. Waits therefore go
straight to the futex. Under oversubscription — the Pi's normal case — a spin
would be wrong anyway, since it takes a core a progress thread or mid-copy writer
needs. Revisit only with measurements.

**No real-time priority anywhere.** It is tempting on an oversubscribed Pi and
would introduce unbounded priority inversion. Where isolation is needed, place
the progress thread on its own core with cpusets — isolation without a priority
relationship — bearing in mind that it runs inside the lead task's Slurm cpuset
(§3.12), so the job's CPU binding must leave room for it.

Keep futex words few (one per resource class per connection, not one per PE per
connection) and cache-line aligned with `_Alignas`. Futex waits hash into kernel buckets by address; unrelated
words can collide on the same bucket spinlock.

### 3.12 Progress threads and where they run

> **Every PE starts one agent thread in `shmem_init`. The local lead's agent is
> the node's progress engine. Every other agent is a service thread that does
> only work which must happen inside its own process.**

An earlier draft said "a small fixed pool per node" without saying which process
the pool lives in. PEs are processes, so that choice decides how every
cross-process mechanism works.

**The lead's agent owns the node's network state:** every io_uring ring, every
node-pair socket, every provided buffer. It seals, submits, reaps, applies
incoming operations to every local PE's heap (all local heaps are mapped in every
process, §4.1), and drains the deferred reply queues. It is the sole issuer
(§3.3).

**Non-lead agents never touch a ring or a socket.** Each sleeps on a futex word in
the node segment and is woken only for work that must run in its own address
space:

- operations on that PE's **symmetric statics** while they are private memory
  (§4.3), applied in FIFO order per PE;
- copying completed **nonblocking get** replies from the landing zone into a
  private destination, so landing-zone space is released without waiting for the
  application to call `quiet`;
- **`shmem_global_exit` and job abort**: the agent calls `exit(status)` inside its
  own process, so the exit status survives (§3.15).

**Application threads never submit.** They reserve and copy into connection
buffers (§3.5), and ask the lead for everything else — seal-and-flush for
`fence` and `quiet`, a wake for new work — with a flag plus a futex wake.

**Replies land in shared memory, never at a private address.** A get's `dest`
may be a stack or `malloc` buffer the lead's process cannot see. Each PE
therefore has a **landing zone** in the node segment, sized by the outstanding-get
resource (§4.6). The progress thread writes reply chunks and fetching-AMO results
there, and the *origin* copies into `dest`:

- blocking get or fetching AMO: the calling thread, already waiting in the
  library;
- nonblocking get: that PE's agent, or the application thread at `quiet`,
  whichever comes first.

This corrects the earlier claim that "the origin side never needs a helper." It
never needs one *for progress*; it does need its private memory written by a
thread inside it.

**Progress thread count** is set by memory bandwidth, not PE count. On a Pi it is
one: the lead's agent itself. At 256 PEs perhaps four, all in the lead process,
sharded by peer node so each connection has exactly one owner and each stream
keeps single-threaded ordering.

**Why threads in every PE rather than a helper process:**

| Concern | Agent threads | Helper process |
|---|---|---|
| Creation | `pthread_create` in `shmem_init` | `fork` from a PE that may already be multithreaded — only async-signal-safe calls in the child, so in practice `fork`+`exec` of an installed helper binary and path discovery |
| Lifetime | Dies with its PE | Needs its own supervision: pidfds both ways, orphan cleanup |
| Private statics | Already inside the address space | CMA, which needs ptrace permission (`process_vm_writev` fails with `EPERM` at Yama `ptrace_scope=1`) |
| `shmem_global_exit` | `exit(status)` in-process | Signals to each PE; exit status is lost |
| CPU binding, cgroup | Lead task's cpuset | The same: a helper forked by the lead inherits it |

*(The review that found this gap rejected "a thread in every PE" as contradicting
§3.1 and this section. That holds only if every thread did network I/O; here only
the lead's does.)*

**Costs accepted:**

- **The lead is special.** Pausing the lead PE in a debugger stops the node's
  network progress; debug with non-stop mode, or read `am_dump_state` from another
  PE, which sees the node segment. A crashed lead cuts the node off. Under §3.15
  the job aborts either way, but the other PEs must *notice*: every wait carries a
  deadline (§3.10), and on expiry the waiter checks the lead's liveness.
- **The lead's cores carry the progress work.** Under Slurm CPU binding the agent
  runs in the lead task's cpuset, competing with the lead's application thread. On
  a Pi with one core per task this is the oversubscription case §3.11 plans for;
  measure it (§8.4) rather than assume.
- **Internal threads in an application process.** Agents block all asynchronous
  signals before they are created, so application handlers keep running on
  application threads. A child created by `fork()` after `shmem_init` has no
  agents and must neither call OpenSHMEM nor run teardown (§6.4).

### 3.13 Handler table

**Fixed at init. No dynamic registration.** Identifiers are a compile-time enum;
dispatch is an array index with one bounds compare. No registration lock, no
lifetime question, no teardown race. The whole subsystem is a `static const`
array.

**Checksum the table at init and compare it at rendezvous (§6.2).** Every PE must agree.
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

The policy is right; an earlier draft lacked **detection and propagation**:

- **`shmem_global_exit(status)`** (§9.1.6) must terminate every PE "regardless of
  their current execution state" — including PEs deep in computation that never
  call the library.
  1. The calling PE's lead floods an abort AM to every node.
  2. Each lead sets an **abort word** in its node segment and wakes every local
     agent.
  3. Each agent calls `exit(status)` in its own process (§3.12), so the status
     survives and standard I/O is flushed as §9.1.6 requires.

  Every library wait also checks the abort word. `exit` from a non-main thread can
  block if an application thread holds a stdio lock, so an agent falls back to
  `_exit(status)` after a bounded delay.
- **A crashed local PE** can die mid-reservation, leaving a segment that never
  becomes sendable while every TCP connection stays healthy — so nothing "aborts
  loudly". The lead's agent holds a **pidfd for every local PE**, polled on its
  ring, using the pids recorded at local rendezvous (§6.2). A pidfd completion for
  a PE that has not finalized aborts the job. Non-lead PEs detect a dead lead when
  a wait deadline expires (§3.12).
- **A vanished peer node** surfaces through TCP keepalive with `TCP_USER_TIMEOUT`
  on every node-pair connection, as a connection error — which aborts.
- **Slurm's `KillOnBadExit` defaults to 0**, so one PE exiting nonzero does not by
  itself stop the others. Document `srun -K1`. The spec does not define crash
  behaviour, so this is operability, not conformance.

### 3.16 Memory-order discipline

> **ISO C11 `<stdatomic.h>` only, with a named memory order at every access to a
> shared protocol word.** No compiler intrinsics, no assembly, no `__atomic_*` or
> `__sync_*` builtins.

Rules:

- **Loads** name `memory_order_acquire`.
- **CAS and `fetch_add`/`fetch_sub`** name `memory_order_seq_cst`. They compile to
  locked instructions on every target whatever order is named — `lock cmpxchg`
  and `lock xadd` on x86-64, LSE instructions or libgcc's outline helpers on
  aarch64 — so naming `seq_cst` keeps the text honest.
- **Plain stores** name `memory_order_release` for single-writer publication,
  where one side stores and the other only loads.
- **Two-sided ("Dekker-shaped") protocols** — each side stores its own word and
  then loads the other's — name `memory_order_seq_cst` on **both** stores, or use a
  locked operation. The §4.9 watch-table insert against the `ivar` store is one;
  `shmem_signal_set` against a signal wait is another.
- Implicit-order calls (`atomic_load`, `atomic_store`, operator forms) and
  `memory_order_relaxed` are forbidden on protocol words, so every access shows its
  order.
- Only the four ISO orders. GCC's header also defines `memory_order_acq_rel` and
  `memory_order_consume`, and an earlier draft used `acq_rel`, but they are
  extensions and buy nothing here.
- Configure fails if the compiler defines `__STDC_NO_ATOMICS__`.

**Why two-sided stores need `seq_cst`.** GCC 14 compiles both `release` and
`seq_cst` stores to `stlr` on aarch64, but on x86-64 a `release` store is a plain
`mov`, which sits in the store buffer where another core cannot see it, while a
`seq_cst` store is `xchg`, which forces it out. In a two-sided protocol, both
sides can then miss each other's store and a wake is lost — shown by the review's
model (variants V4, V4b, V4c lose wakes and V5 does not) and confirmed by
disassembly in the development VM. §3.5's earlier claim that missing orders cause
corruption "never on x86" is true for single-writer publication and false for
this shape.

**Consequences:**

- **Futex words** are `_Atomic uint32_t`, with a `_Static_assert` that its size
  and alignment equal `uint32_t`'s (true for GCC on aarch64 and x86-64).
- **Outline atomics** on ARMv8.0 cores without LSE, such as the Pi 4's Cortex-A72,
  link statically from `libgcc.a`; a test binary's `NEEDED` list is libc only
  (review, lead-verified), so invariant 15 holds.
- **No spin-before-block** (§3.11): there is no ISO C relax hint.
- **A serializing point** — for `fence` and `quiet` ordering memory stores (§4.8) —
  is one `seq_cst` store in the calling thread.

---

## 4. SHMEM Layer

### 4.1 Symmetric heaps

> **One heap per PE, as the spec requires. Every local process maps every local
> PE's heap.**

Spec §9.2: "Each PE has a single symmetric data segment and symmetric heap", and
`SHMEM_SYMMETRIC_SIZE` is "the size (in bytes) of the symmetric heap memory per
PE" (§8). An earlier draft had all local PEs share one heap, which makes a put to
one local PE overwrite another's data and makes `shmem_ptr(x, 1) == shmem_ptr(x,
2)`.

Each PE's heap is its own shared memory object (§6.2 covers how siblings find
it). Because every local process maps all of them, an intranode put or get to
*any* local PE still collapses to `memcpy` — no syscall, no proxy — and the lead's
progress thread applies remote operations to any local heap directly.

**Layout.** Each process maps **its own heap at a fixed base `B`**, the same in
every PE, so a PE's symmetric addresses are the same numbers on every PE —
matching how symmetric statics behave under `-no-pie` (§4.3). Sibling heaps go
wherever the process can place them, recorded in a per-process table:

```c
/* per process; O(local PEs), which the §9.3 implementer notes allow */
void *base_of[L][NREGIONS];

/* symmetric address a on the calling PE, target local PE t: */
target = base_of[t][region] + (a - base_of[me][region]);
```

**Costs.** Every process reserves L × `SHMEM_SYMMETRIC_SIZE` of virtual address
space — 256 GiB for 256 PEs at 1 GiB, comfortable in 48-bit aarch64. `memfd` pages
are committed only when touched; hugetlbfs pages are reserved up front, so L
heaps' worth of huge pages must exist before init, with a fallback to regular
pages when they do not (the development VM has none). `SHMEM_SYMMETRIC_SIZE` must
agree across PEs and is checked at rendezvous.

The progress thread maps the same physical pages as the owning PE and its local
siblings, so remote AMOs applied by the progress thread and local AMOs issued by
PEs are the same CPU atomic on the same cache line. **Atomicity unifies for
free** — for single-word AMOs of one width. It does not extend to multi-byte puts,
or to AMOs of different widths on overlapping bytes; the spec makes neither
exclusive.

This is *stronger* than the spec requires, which is worth knowing before anyone
trades it away. Spec §3.2 guarantees exclusivity only between OpenSHMEM AMOs on
the same location, of the same datatype, in the same atomicity domain, and
explicitly makes AMOs concurrent with ordinary loads and stores undefined. The
shared mapping gives real CPU-level atomicity across that whole set for nothing.

Two consequences: the §4.8 `shmem_ptr` warning has spec text behind it and
should cite §3.2 item 4 rather than argue from first principles; and
`SHMEM_TEAM_SHARED`, a required predefined team (§7) equal to the set of local PEs
`shmem_ptr` can reach, is defined by the spec as a **separate atomicity domain** from
`SHMEM_TEAM_WORLD`, so AMOs from the two are mutually undefined by the standard
even though this implementation would make them exclusive anyway.

Reserve the own-heap range at `B` with `MAP_FIXED_NOREPLACE` so a collision fails
loudly rather than silently unmapping something. `-no-pie` pins the executable
but **not** the heap — ASLR still randomizes heap, stack, mmap region, and
shared libraries.

### 4.2 Addressing

**Offset-based on the wire, always.** `(region id, offset)`, translated at both
ends. This survives ASLR, PIE, heterogeneous nodes, and future region types.

Identical-VA mapping is pursued *as well*, purely so intranode access and
`shmem_ptr` are raw pointer arithmetic — but **the wire format must never depend
on it.** This is the decision that is genuinely painful to revisit.

"Identical" applies to each PE's *own* heap, at `B` in every process. Sibling heaps
sit at per-process addresses and are reached through `base_of` (§4.1), so
intranode code already exercises translation. The wire format's independence
from addresses cannot fail a test while every process maps its own heap at the
same base, so the test suite includes a mode that shifts `B` per PE (§8.2).

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
| 1.5 | `-no-pie` + agent threads (§3.12) | Correct but slow: the target PE's agent applies each put, get, or AMO in its own process, in FIFO order. No CMA, so no ptrace permission is needed |
| 2 | Wrapper-generated linker script | Fast; no source changes |

Phase 2 needs no macro. The wrapper knows which objects it compiled from user
source and emits a fragment naming them:

```
.shmem_statics : ALIGN(CONSTANT(MAXPAGESIZE)) {
    __shmem_statics_start = .;
    user1.o(.data .data.* .bss .bss.*)
    user2.o(.data .data.* .bss .bss.*)
    __shmem_statics_end = .;
}
```

Mapped over with `MAP_FIXED` at init. Aligned to the linker's maximum page size,
not 4096: a Pi 5 running Raspberry Pi OS defaults to 16K pages (§8.5).

**Phase 1.5 ordering and replies.** Statics and heaps are disjoint regions, so
applying statics operations through a per-PE FIFO agent queue while heap
operations apply directly never reorders operations on the same object. A get
*from* a PE's statics cannot use §3.8's reference descriptors — the lead cannot
read that memory at drain time — so the agent copies the requested bytes into
reply staging when it handles the request. That reintroduces the memory cost §3.8
avoids, on a path that is slow by design.

*(Rejected for phase 1.5: CMA via `process_vm_readv/writev`. It needs ptrace
permission between sibling processes, which depends on Yama `ptrace_scope` — 1 on
Ubuntu, 0 on the Debian 13 VM — and the agent threads already exist.)*

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

**Metadata out of band.** Inline headers would put bookkeeping inside every PE's
heap — harmless, but a waste of symmetric memory, and visible through
`shmem_ptr`. Private per-process metadata is cleaner, and since every PE
computes the same answer independently, **there is no cross-process allocator
lock at all.**

**Debug consistency check:** hash operation and size at each allocation, fold
into a running checksum, and compare it across PEs at the call's barrier.
Mismatched sizes across PEs are undefined behaviour and will silently
desynchronize heaps; this catches it at the causing call. The §5.3 barrier carries
no data, so debug builds extend its arrival with one 64-bit word, compared by each
local lead and again at the root.

**Edge semantics (§9.3), easy to get wrong:**

- `shmem_malloc`, `shmem_align`, `shmem_calloc`, and `shmem_realloc` barrier on
  **exit**; `shmem_free` barriers on **entry**.
- A size of 0 (for `calloc`, a count or size of 0) returns a null pointer **without
  a barrier**. Every PE must make the same call, so none waits.
- `shmem_calloc` treats `count × size` overflow as a failed allocation.
- `SHMEM_SYMMETRIC_SIZE` accepts the §8 grammar — a non-negative integer or
  floating-point prefix with an optional scaling character — and must agree across
  PEs, checked at rendezvous (§6.2).

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
| Requests (put, signalled put, non-fetching AMO) | Applied at target | Peer's cumulative acknowledgement of applied segments (§3.4) |
| Get replies | Delivered locally | Bytes landed in the landing zone, or directly in a symmetric `dest` (§3.12) |
| Memory stores by the calling thread | Visible to other cores | A serializing store in the calling thread (below) |

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

#### Memory stores

Table 14 (§9.12) places **memory stores under both fence and quiet**, and §9.10.1
says `barrier_all` "ensures completion of all previously issued memory stores". An
earlier draft of §4.8 said the opposite. The spec limits the guarantee to stores
made by the calling thread (§9.12.1 and §9.12.2 Notes), and contexts created with
`SHMEM_CTX_NOSTORE` are exempt. Two rules deliver it:

1. **`shmem_ptr` returns `NULL` for any target reached over the network** —
   including a local PE when §8.1's force-remote flag sends intranode traffic over
   TCP. A thread's store can then never race that same thread's queued put to the
   same object: either both are local and ordered by the thread itself, or no
   pointer exists.
2. **`fence`, `quiet`, `pe_quiet`, and the barriers begin with one `seq_cst` store in
   the calling thread** (§3.16). It forces that thread's earlier plain stores to
   become visible to other cores before the routine proceeds.

In normal mode `barrier_all` already happened to conform, because its arrival
`fetch_add` is a locked instruction. The rule makes that deliberate.

#### Data structures

**Thread level.** Phase 1 provides `SHMEM_THREAD_SINGLE`; §9.2.1 lets
`shmem_init_thread` provide less than requested. The structures below assume it
and are marked where `MULTIPLE` would change them: under `MULTIPLE` the
equality wait below can starve, and clearing the dirty set races.

**Get completion — two cumulative counters per PE, no per-operation state:**

```c
uint64_t bytes_issued;   /* PE adds expected bytes on issue      */
uint64_t bytes_landed;   /* progress thread adds as chunks land  */
```

`quiet` waits for equality. This is exactly as expressive as the spec requires,
because `_nbi` completion is defined only at `quiet` — there is no
per-operation wait call. Cost: 16 bytes per PE, in the node segment because the
progress thread runs in another process. Byte-based accounting means the responder
may fragment replies freely.

**Waiting on 64-bit counters.** The counters cannot be futexed (§4.9), so each
waitable counter has a companion 32-bit wake word. The progress thread adds landed
bytes, then bumps the wake word, then wakes; the waiter loads the wake word, then
compares the counters, then waits on the wake word. The per-connection `acked`
value that `quiet` waits on follows the same pattern.

**Blocking gets and fetching AMOs** do need per-operation completion: the caller
waits for *its* reply, not for a byte count. Under `SINGLE` a PE has at most one
such operation outstanding, so its landing zone holds one completion slot with its
own wake word.

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
    serializing_store();                          /* memory stores (above) */
    request_flush(pe->dirty, pe->ndirty, ACK_REQUESTED);   /* the lead seals (§3.12) */
    for (i = 0; i < pe->ndirty; i++)
        wait_until(acked[pe->dirty[i]] >= pe->high_seg[pe->dirty[i]]);
    wait_until(pe->bytes_landed == pe->bytes_issued);
    copy_out_completed_gets();                    /* landing zone -> private dest */
    pe->ndirty = 0;
}

void shmem_fence(void) {
    serializing_store();
    request_flush(pe->dirty, pe->ndirty, ACK_REQUESTED);
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
void shmem_pe_quiet(const int *target_pes, size_t npes);
void shmem_ctx_pe_quiet(shmem_ctx_t ctx, const int *target_pes, size_t npes);
/* intersect the targets' node set with the dirty set; flush and wait on that */
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
**resource**, not an operation count: the per-connection reply credit `C` of §3.8,
taken by the origin's lead and returned when the reply lands; exhaustion blocks the
issuing PE. Same bound, honest failure mode, with a counter showing stall
frequency. Size it large enough to be irrelevant to real codes: at 48 bytes per
descriptor, `C = 1,000` across 500 connections is 24 MB in the target's node-wide
pool.

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

Replies land in the origin PE's **landing zone** and the origin copies them into
`dest` (§3.12). When `dest` is itself in a local symmetric heap, the progress
thread may write it directly and skip the copy: `dest` is undefined until the get
returns, so no conforming program can observe the difference.

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
- `shmem_ptr` returns `NULL` for any target reached over the network, including a
  local PE under §8.1's force-remote flag (§4.6).
- Accesses through the returned pointer are ordinary loads and stores, which the
  spec calls **memory stores**. Table 14 puts memory stores under `fence` and
  `quiet`, for the calling thread only; §4.6 describes how that is met. An earlier
  draft said `fence` and `quiet` do not order them, which contradicts the spec.
  Beyond that guarantee, the application supplies its own synchronization, subject
  to the architecture's memory model.
- Providing the returned address to a routine requiring a symmetric address is
  undefined.

**Design constraint:** a store via `shmem_ptr` by one thread and a put to the same
location issued by a *different* thread or PE have no ordering relationship.
Aggregation must not assume all writes to a symmetric heap pass through the
descriptor path.

**Document prominently:** code developed on x86 that omits barriers around
`shmem_ptr` access will fail on aarch64 under weak ordering. It is the
application's bug (spec §3.2 item 4 makes it undefined), but it will be reported
here.

### 4.9 Point-to-point synchronization

`shmem_wait_until` and `shmem_test` are phase 1 because they force the question
of **how a PE blocks without starving progress**. The answer has to hold when
the writer is the progress thread applying a remote AMO *and* when it is another
local PE storing through the shared heap.

> **One watch slot per local PE for the wake decision; a 32-bit sequence word in
> the slot for the wait.**

A wait is always on the *caller's own* symmetric variable, so a writer that stores
to `(PE, offset)` only ever needs to look at that one PE's slot. Under
`SHMEM_THREAD_SINGLE` (§4.6) a PE waits on at most one variable at a time, so one
slot per PE is enough: no hash table, no probing, and no slot deletion.

An earlier draft had the waiter futex directly on the `ivar`. That works only for
32-bit types: **the kernel implements only 32-bit futexes.** `futex_wait` with
`FUTEX2_SIZE_U64` returns `EINVAL` — verified on 6.12 aarch64 by `tools/probe`
— and OpenSHMEM's wait types include 64-bit ones. Futexing on the low half of a
64-bit `ivar` is not a fix: an update that changes only the high bits between the
waiter's load and its wait passes the kernel's compare, and the wake is lost.

The slot's sequence word is library-owned, always 32-bit, always aligned, and
bumped on every update to the watched address, so the kernel's compare-and-wait
sees every change whatever the `ivar`'s width.

```c
/* waiter: PE `me`, waiting on its own ivar */
struct watch_slot *slot = &node->watch[me];
atomic_store_explicit(&slot->watched, offset_of(ivar) + 1,  /* 0 means none */
                      memory_order_seq_cst);                /* BEFORE the first check */
for (;;) {
    uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
    if (predicate(load_ivar(ivar), cmp, value)) break;
    futex_wait(&slot->seq, seq, deadline);        /* -EAGAIN if bumped: loop */
}
atomic_store_explicit(&slot->watched, 0, memory_order_seq_cst);
```

```c
/* progress thread or local PE, having stored to (pe, offset) with a seq_cst
 * store or a locked AMO (§3.16) */
struct watch_slot *slot = &node->watch[pe];
if (atomic_load_explicit(&slot->watched, memory_order_acquire) == offset + 1) {
    atomic_fetch_add_explicit(&slot->seq, 1, memory_order_seq_cst);
    futex_wake(&slot->seq, INT_MAX);
}
```

**Load `seq` before the `ivar`; store the `ivar` before bumping `seq`.** Any
update the waiter's predicate misses is then followed by a bump the kernel's
compare catches. Reversing either order reopens the lost-wake window.

The slot lives in the shared node segment, so the wait is a shared (not
`FUTEX2_PRIVATE`) futex on a shared mapping — also verified by the probe.

**Slots are keyed by `(PE, offset)` — never by address**, because a PE's heap
sits at a different address in each sibling process (§4.1). Slots are
cache-line padded with `_Alignas` and live in the node segment.

**Cost is one load per applied operation on the target side**, and only for
operations that write a heap: the target PE's `watched` word, compared with the
offset just written.

**Under `MULTIPLE`**, several threads of one PE can wait on different variables,
so a slot becomes a small per-PE list. The review found that the obvious list
designs lose wakes — a naive reference count, and linear-probe deletion even with
one waiter per key — and model-checked rules that do not
(`docs/reviews/evidence/semantics/`). Adopt those rules, or re-run the model,
before adding `MULTIPLE`.

**Insert before the first predicate check.** Checking and then inserting loses a
wake that lands in the window. This is the one ordering rule in the section that
is easy to get backwards and impossible to see in testing.

**Memory ordering: this is a two-sided protocol** (§3.16). The waiter stores
`watched` and then loads the `ivar`; the writer stores the `ivar` and then loads
`watched`. Both stores must be `seq_cst` or locked, or on x86 each side can miss
the other's store and the wake is lost. AMOs that are CAS or `fetch_add` are
locked already; a set-style AMO, and `shmem_signal_set`, must use a `seq_cst`
store. An earlier draft applied §3.5's single-writer rule here, which is not
enough.

**Pass a safety-sweep deadline**, for the same reason §3.10 does: a missed wake
becomes a latency spike with a counter rather than a hang. It is an **absolute**
deadline — see §3.10 on the two timeout conventions. **Count
timeout-wakes.** A nonzero count in production means the watch table has a bug,
and that counter is the only thing that will ever tell you.

**`shmem_test` never blocks** — predicate check, no slot interaction at all.
Likewise the multiple-element tests. On an empty test set, `shmem_test_all` and
`shmem_test_all_vector` return 1 (1.6 change #19), `shmem_test_any` returns
`SIZE_MAX`, and `shmem_test_some` returns 0.

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
  its `dest` is the accumulator and it is the only PE that writes that `dest`
  before release (§5.5).

### 5.3 Intranode phase

**Flat counter to start.**

> At `L` local members all performing `fetch_add` on one cache line, the line
> ping-pongs between cores and arrival cost is roughly `L` serialized cache-line
> transfers. Negligible at four. At 64 or 256 it dominates the intranode phase
> entirely. Fix: two-level intranode tree with fanout near core-cluster size,
> chosen at init from local member count.

Each §5.4 counter must be **alone on its cache line** — false sharing between
counters, or with the team descriptor, hurts even at four PEs, and one alignment
attribute prevents it.

**Barrier and sync: arrival does not wait for a designated PE.** Whichever local
member's increment reaches the epoch's arrival target enqueues the internode
arrival AM and returns to waiting. One comparison on the `fetch_add` result; no
dependency on the lead being scheduled.

**Data-carrying collectives do wait for the lead**, which combines every local
contribution itself (§5.5). That dependency is the price of a single writer, and
the lead is blocked in the same call regardless.

**Release** arrives at the progress thread, which advances the team's `released`
counter and wakes all local waiters — a divisible event, so wake-many per §3.11.
The progress thread never touches non-lead `dest` buffers, so it does no
`O(members × count)` copying while other connections wait.

### 5.4 Epochs and slot reuse

> Blocking constrains each PE individually and says nothing about *skew*. A
> non-lead can copy out of epoch *k*, return, and re-enter at *k+1* while the
> lead is still in *k* waiting for the remaining copy-outs. Any per-team state
> that is *reset* between epochs, and any flag that only says "released,"
> confuses the two.

**Three cumulative 32-bit counters per team per node. Never reset.**

```c
_Atomic uint32_t arrived;    /* all local members, every collective     */
_Atomic uint32_t released;   /* epochs released; progress thread only   */
_Atomic uint32_t copied;     /* non-lead copy-outs, data collectives    */
```

Each PE keeps two private counts per team: `k`, collectives completed on the
team, and `d`, data-carrying collectives completed. Every member agrees on both,
because members call collectives in the same order. The targets for the current
epoch follow directly:

| Counter | Target | Who waits |
|---|---|---|
| `arrived` | `(k+1)·L` | Lead, in data-carrying collectives; nobody in barrier/sync |
| `released` | `k+1` | Every member |
| `copied` | `(d+1)·(L−1)` | Lead only |

`copied` is indexed by `d`, not `k`, because barrier and sync have nothing to
copy out and never touch it. With `L == 1` its target is zero and the lead's wait
returns immediately — no special case.

**Waits are equality, not `>=`.** `wait_eq(&counter, target)` loads, compares,
and `futex_wait`s on the counter itself with the loaded value.

Equality is sound because skew is bounded, so no counter can move *past* a value
someone is waiting on. Only the lead waits on `arrived` and `copied`, and no
member can reach the next epoch's copy-out without the lead having finished the
current one. A member waiting on `released == k+1` must see *k+1* before *k+2*,
because *k+2* needs this member's own arrival. A counter can run ahead of the
value someone waits on by at most `L − 1` (next-epoch arrivals), so wraparound is
harmless whenever the modulus `M ≥ L` — for 32-bit counters, always. The review's
model checked moduli down to that boundary. So wraparound is harmless,
and each counter is directly a futex word — which matters, because the kernel
implements only 32-bit futexes (§4.9).

**The race, walked through.** Non-lead P1 copies out of *k* and re-enters. Its
arrival moves `arrived` past `(k+1)·L` toward a target nobody is watching yet.
It waits for `released == k+2`; `released` is *k+1*; it sleeps. It cannot copy
out or touch `copied` until the lead has left *k*, re-entered, and *k+1* has
been released. A plain `released` flag would still read "set" from *k*: P1 would
copy stale data and bump `copied`, corrupting the lead's count.

**Rejected:**

- *Sense reversal plus parity-slotted counters.* The previous design. Sense
  reversal fixes the release flag but not the counters: if the lead resets
  `arrived` at *k+1*, P1's early arrival is lost. Fixing that needs a second
  counter set selected by parity — double the state, reasoning in parity, and no
  path to nonblocking collectives.
- *64-bit counters, `>=` predicates, and a separate 32-bit wake word.* Correct
  under unbounded skew, at one extra word and one extra store per transition.
  **This is the upgrade path** for nonblocking collectives: equality is the one
  assumption they break.

**Give the counter a type that is not a bare integer**, whose only comparison is
`wait_eq`. A contributor adding `shmem_*_nbi` collectives should meet the
equality assumption at compile time, not as a hang.

### 5.5 Reduction destination

**Accumulate into the local lead's `dest`, with the lead as its only writer
before release, and the lead leaving last.**

```c
/* every member */
if (atomic_fetch_add(&t->arrived, 1) + 1 == (k+1)*L && !is_lead)
    futex_wake(&t->arrived, 1);

/* lead only: sole writer of dest before release */
wait_eq(&t->arrived, (k+1)*L);
if (source != dest) memcpy(dest, source, n);     /* in-place: already there */
for (m in local members, m != lead)
    combine(dest, source_of(m), n);
enqueue_internode_arrival(team, k, dest);        /* epoch on the wire; assert on receipt */

/* every member */
wait_eq(&t->released, k+1);                      /* result is in the lead's dest */

/* non-lead */
memcpy(my_dest, lead_dest, n);
if (atomic_fetch_add(&t->copied, 1) + 1 == (d+1)*(L-1))
    futex_wake(&t->copied, 1);

/* lead: leaves last, so its dest stays valid for every copy-out */
wait_eq(&t->copied, (d+1)*(L-1));
```

Legal because `dest` is undefined until the collective returns, so intermediate
values are unobservable by any conforming program. The epoch number travels in
the internode AM and is asserted on receipt — the collective analogue of §3.13's
handler-table checksum.

**Why a single writer.** An earlier draft had local members combine into the
lead's `dest` as they arrived. That fails three ways:

1. **Lost updates.** Concurrent `dest[i] = op(dest[i], src[i])` from several
   processes is a read-modify-write race.
2. **In-place corruption.** With `source == dest`, the lead's `dest` *is* its
   source, and the spec guarantees it is ready only once the lead itself has
   called. A member combining earlier races the application still preparing the
   buffer.
3. **Epoch clobber.** There is exactly one application `dest`, so the old
   two-slot parity scheme cannot apply. A lead that returned early could seed the
   next epoch while members were still copying out of this one.

The lead is blocked in the call regardless, so doing the combine costs nothing
in availability, and parallel combines would ping-pong the same cache lines
anyway (§5.3). At high local member counts the §5.3 tree applies unchanged: each
sub-lead is the single writer of its own `dest`.

**Push, never pull — for every data-carrying collective.** 1.6 §9.10.5 and
§9.10.7–§9.10.10 all state that "the application does not need to synchronize
to ensure that the source array is ready across all PEs prior to calling this
routine." A PE may read another PE's `source` only after that PE has called. The
lead reading members' sources *after they have arrived* satisfies this; an early
arrival pulling from a peer that has not yet called does not. A pull-based
collect or alltoall — the obvious implementation — is **non-conforming**, not
merely slow.

**Symmetric addresses are the precondition, not a threat.** §9.10.9 (1.6 change
#17): "The same source and dest arrays must be passed by all PEs that participate
in the collective." Identical
offsets (§4.2, §4.4) make that the same symmetric object in every process, which
is what guarantees the lead's `dest` is the right size and place to accumulate
into. Source and dest must be fully overlapping or fully disjoint; both cases
are the single `memcpy` line above.

**Requires `dest` and every `source` in a shared mapping.** Always true for the
symmetric heap. For symmetric statics, only from §4.3 phase 2: under phase 1.5's
CMA the lead would read each member's source with `process_vm_readv` — correct,
but a syscall per member per collective. Phase 1 rejects statics, so this is a
sequencing note, not a blocker.

**Release ordering and downward forwards.** State these, because the exhaustive
model assumes them and cannot show what breaks without them:

- **A child node's partial result is merged only after the local lead has enqueued
  its own arrival**, which is after its local combine. Merging earlier races the
  combine and loses updates.
- **Write the final `dest`, then bump `released`** — never the reverse, or members
  copy out a partial result. For a team that spans a single node, the lead's own
  arrival triggers the release locally, with no internode message.
- **A result forwarded down the internode tree by reference** reads the lead's
  `dest` when the forward drains (§3.8). If that drain happens after `released` is
  bumped, the lead can see every local copy-out complete, leave, and let the
  application overwrite `dest` before the forward is sent — the review's model
  shows exactly this (`docs/reviews/evidence/collectives/`, forwarding after the
  bump). Either count each downward forward in `copied`, or finish the forwards
  before bumping `released`. This arrives with reductions, not with barrier.

**Scope of this scheme.** Accumulating into the lead's `dest` works for barrier,
sync, reductions, and broadcast, where every member ends with the same value. It
does **not** generalize:

- **alltoall and alltoalls** send a distinct block from every PE to every PE, so
  there is nothing to accumulate;
- **scans** give each PE a different result that depends on team rank order, which
  is wrong to combine node-locally when a node's members are not contiguous in
  rank;
- **collect** with a different `nelems` per PE needs per-PE sizes and offsets.

Each needs its own design before it is implemented (§7).

For the collectives it covers, this still eliminates scratch sizing — no chunking
loop, no high-water allocation, no per-team data area. Their shared footprint is
**three counters on three cache lines**, regardless of reduction size.

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

**It counts teams in existence job-wide at once, including the predefined teams —
not teams per PE.** The control-queue bound is per connection: the number of
distinct teams whose trees use that node pair. `shmem_team_split_2d` can put `L`
different teams on one node pair while each PE belongs to only two, so a per-PE
count makes the §3.8 assert fire on conforming programs (review C10). Teams per
edge never exceed teams job-wide, so the job-wide count is a safe bound, and the
node-wide pool (§3.8) keeps it from costing memory per connection.

**Open: the team range allocator and the split protocol are not yet designed.**
This section refers to "team ranges" without defining them, and nothing yet says
how PEs agree on a new team's identifier or claim its counter blocks during
`shmem_team_split_strided` and `shmem_team_split_2d`. Phase 1 has no team creation,
but this becomes blocking when team creation lands (§10).

Runtime rather than build-time, despite the init-ordering cost: the pool cannot
be sized until the bootstrap has supplied local PE count, so team-pool sizing
must happen after §6 completes and before anything that allocates node-level
structures. That ordering is worth getting right once. A build-time constant
would be simpler, but forcing a rebuild to raise a limit is exactly the friction
§1 exists to remove.

*(A build-time ceiling with a runtime value below it was considered — it keeps
every structure statically sized. Rejected because the ceiling reintroduces the
rebuild, which is the whole objection.)*

**Team ranges leak until `finalize`**, with a counter. §9.4.8 calls
`shmem_team_destroy` "a collective operation that destroys the team referenced by
team" without naming a scope in the routine text, but the rest of the spec fixes
it: a parent-team PE that is not in a new team "will receive a value of
SHMEM_TEAM_INVALID" (§9.4), and destroying `SHMEM_TEAM_INVALID` is a no-op. So
destroy is collective over the team's members. An earlier draft called the scope
ambiguous; it is not. The conclusion stands anyway: parent PEs outside the child
cannot observe the destroy, so ranges cannot be reclaimed without divergence, and
leaking costs nothing because teams are typically created once during setup.

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

### 6.1 What Slurm tells each task

| Variable | Supplies |
|---|---|
| `SLURM_PROCID` | Rank: step-relative and dense |
| `SLURM_STEP_NUM_TASKS` | Size |
| `SLURM_NODEID` | This task's node index within the step |
| `SLURM_LOCALID` | This task's rank within its node — the lead is `0` |
| `SLURM_GTIDS` | Global ranks of the tasks on this node, and so the local count `L` |
| `SLURM_STEP_NODELIST` | The step's hosts, in the order `SLURM_NODEID` indexes |
| `SLURM_JOB_ID`, `SLURM_STEP_ID` | Rendezvous port derivation and handshake identity |

**What the environment does not give is placement.** An earlier draft claimed the
node map "falls out of the environment before a single socket is opened". Two
things made that wrong (review D4, from Slurm's `env.c` and
`slurm_step_layout.c`):

- `SLURM_TASKS_PER_NODE` gives counts, not which ranks. Under cyclic, plane,
  arbitrary, or topology-ordered distribution — the default whenever a topology
  plugin is configured — ranks are not contiguous per node, so per-node member
  counts derived from it are wrong (§5.2) and barrier targets are never reached.
- `SLURM_NODELIST` is the *job's* node list, while `SLURM_NODEID` indexes the
  *step's*. Inside `sbatch -N4`, a task of `srun -N2 -r2` running on `pi03` has
  `SLURM_NODEID=0`, which `SLURM_NODELIST` maps to `pi01`. Use `SLURM_STEP_*`
  variables throughout.

Each task therefore knows only its own node. The **rank→node map is assembled at
rendezvous** from each node's `SLURM_GTIDS` (§6.2).

`SLURM_NODEID` is still the locality input, which is the shape §8.1 wants:
`OSOE_NODE_MAP` overrides it, and both paths share one code path.

### 6.2 Rendezvous

> **Local PEs meet on their node; only each node's lead crosses the network.
> Rendezvous connections scale with node count.**

**Node-local.** The lead creates the node segment as a named shared memory object,
`/osoe.<job>.<step>.<node>`, so concurrent steps on one node cannot collide. Every
local PE opens it by name, records its rank, pid, and heap object name in its slot,
and maps its siblings' heaps as they appear (§4.1). Heaps are named objects too,
opened the same way: no sockets and no descriptor passing between local PEs. The
lead waits on a futex until all `L` local PEs have arrived, then unlinks every name,
so a later crash leaves nothing behind in `/dev/shm`; only a crash during init can
leak names. Where hugetlbfs is mounted and has pages, heaps use it under the same
names; otherwise they fall back to regular shared memory.

**Global.** The step's rendezvous server is the lead of node 0: the task with
`SLURM_NODEID == 0` and `SLURM_LOCALID == 0`, on the first host of
`SLURM_STEP_NODELIST`. That is the same list `SLURM_NODEID` indexes, so every task
computes the same host under any distribution.

- **Port:** a deterministic sequence of candidate ports hashed from the job and
  step IDs, within a configurable range. The server binds the first free
  candidate; clients try candidates in the same order. The handshake carries the
  job and step IDs, so a client that reaches another job's server is rejected and
  moves on.
- **Who connects:** only node leads — one connection per node. Each sends its
  node's rank list (`SLURM_GTIDS`), `SHMEM_SYMMETRIC_SIZE`, and handler-table
  checksum (§3.13). The server checks agreement, then broadcasts the rank→node map
  and node addresses.
- **Node-pair connections:** the lower node index connects to the higher, so each
  pair opens exactly one stream (§3.1).

`SLURM_STEP_NODELIST` is a compressed hostlist (`pi[01-04,07]`). Expanding it is a
small parser — call it fifty lines — **written with unit tests in tier 1**, because
it is pure string handling that would otherwise be debugged on a cluster.

**Failure handling:**

- Clients retry with jittered backoff until `OSOE_BOOTSTRAP_TIMEOUT`, then abort
  naming what they were waiting for.
- `shmem_init` raises the soft `RLIMIT_NOFILE` to the hard limit. Slurm propagates
  the submitting shell's limits, commonly a soft limit of 1,024, and a lead on a
  500-node job holds one socket per peer node.
- **Bare `sbatch` launch.** If `SLURM_JOB_ID` is set but `SLURM_STEP_NUM_TASKS` is
  not, the program was started directly by a batch script rather than by `srun`.
  Fail at once with that message rather than waiting for peers that will never
  start.

*(Rejected: every PE connecting to rank 0 — about 77,000 connections into one
listener at 256 PEs × 300 nodes, violating §1, and failing at the 1,021st
connection under a 1,024 file limit. Rejected: rank 0 "publishing" its address, as
an earlier draft proposed — there is no channel to publish on without a KVS or a
shared filesystem.)*

**Non-Slurm launch** (plain `ssh`, a test harness) sets the same variables under
the `OSOE_` prefix — `OSOE_PROCID`, `OSOE_NODEID`, and so on — which take
precedence. The `SHMEM_` prefix is the specification's, not ours. The tier-1 and
tier-2 harnesses of §8 use this path, so the bootstrap runs in every test rather
than only on a cluster.

### 6.3 PMI-2 as an optional backend

Keep the thin interface and keep a PMI-2 backend behind it, **off by default and
not linked unless requested at build time**. It costs little, and it is the
escape hatch for a launcher that does not export usable environment variables.
It is not on the default path and nothing in the build requires it to be
present. A PMI-2 *wire* client speaking the protocol directly over `PMI_FD` would
need no library at all; the protocol details are recalled, not verified, so
confirm them before relying on it.

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

Things to get right, each of which is a silent bug otherwise:

- **A non-last `shmem_finalize` performs `shmem_barrier_all`.** §9.1.4: "All other
  calls to shmem_finalize perform an operation semantically equivalent to
  shmem_barrier_all". An earlier draft only decremented the count, which skips
  completion and lets §5.4's epoch counts diverge across PEs. The last call
  performs its implicit barrier, then tears down.
- **The count is per process, not per thread**, and must be updated atomically.
- **Concurrent first `shmem_init`** from two application threads passes through a
  once-gate, so bootstrap runs exactly once.
- **A child created by `fork()`** after init inherits the mappings but no agent
  threads (§3.12). It must not call OpenSHMEM, and teardown code compares the
  current pid with the initializing pid and does nothing in a child.
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

The collectives are mechanical *given* §5.5's rules, which they must inherit: the
local lead is the single writer before release and leaves last, and a peer's
`source` is read only after that peer has called. A pull-based collect or
alltoall is non-conforming.

### Deferred 1.6 additions

| Routine | Why it waits |
|---|---|
| `shmem_ctx_session_start` / `_stop` (§9.9) | Hints only. A conforming implementation may ignore them entirely, so the no-op is the whole phase-1 implementation |
| `shmem_sum_inscan`, `shmem_sum_exscan` (§9.10.10) | Inherits the §5.1 two-level skeleton, but **not** §5.5's accumulator: each PE's result depends on team rank order, so it needs its own design |
| `shmem_ibput`, `shmem_ibget` (§9.6.1.4, §9.6.1.8) | Interleaved block transfer — an addressing pattern above the chunking layer, mechanical given `(region, offset, length)` chunks |

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
OSOE_NODE_MAP=0,0,1,1,2,2,3,3
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

Options considered: a cloud ARM instance (Graviton, Ampere) as a per-commit CI target;
Apple Silicon under a Linux VM; or a single Pi, which covers intranode plus
loopback logical nodes though not a real fabric. ThreadSanitizer on x86 catches
a useful subset but models the C11 memory model rather than the hardware.

The choice is recorded in §8.5.

**Sequencing:** iterate on aarch64 in the development VM (§8.5), run the full
suite on x86 and ARM CI per commit, and defer the cluster until the AM suite
passes on both.

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

### 8.5 Development environment

Development happens on an Apple Silicon Mac, which has neither io_uring nor
futex2. The environment is chosen so the daily loop is still native aarch64:

| Environment | Role |
|---|---|
| **Lima VM, Debian 13** (`vz`, repository mounted over virtiofs; `dev/lima/create.sh`) | Daily build and test loop. Kernel 6.12, the same line as Raspberry Pi OS Trixie. Native aarch64, so weak memory ordering is exercised on every run. Tiers 1–3 fit: network namespaces, veth pairs and `sch_netem` all verified in the cloud kernel image |
| GitHub Actions `ubuntu-24.04` and `ubuntu-24.04-arm`, directly on the runner | Per-commit CI, and the only x86 leg — the case §8.3 calls adversarial for performance constants |
| One physical Pi over SSH | The Pi kernel fork, the real NIC and driver path, and — on a Pi 5 — 16K pages |

**Rejected:** QEMU's `raspi4b` board model — no Ethernet controller, no PCIe
root port, no Pi 5 model, and TCG-only, which destroys both timing and the
native-ARM concurrency that makes the VM worth having. Docker and other
containers — the default seccomp profile blocks io_uring, and the kernel is the
container VM's anyway.

**The VM is defined in the repository.** `dev/lima/osoe.yaml` fixes the
image, resources, and provisioned packages; `dev/lima/create.sh` mounts the
repository writable **at the same absolute path it has on the host**. That one
choice is what makes the rest of the workflow work: `limactl shell osoe …` run
from the repository root starts in the same directory inside the VM, and every
path in a build tree or `compile_commands.json` is valid on both sides, so an
editor on the host can use the VM's build output.

**Build directories.** Every CMake preset builds under `build/<preset>` in the
repository — `debug`, `release`, `asan`, `tsan` — and `/build/` is ignored by
git. Four properties are deliberate:

- **In the repository, on the shared mount, not VM-local disk.** Object files
  cross virtiofs, which is slower than VM-local disk, but the build tree is then
  visible from the host: `build/debug/compile_commands.json` is what an editor on
  macOS points clangd at, and the same-path mount keeps its paths valid there.
  If build time on the mount ever hurts, a VM-local `binaryDir` preset is a
  one-line addition; nothing else changes.
- **One tree per preset, never shared.** Sanitizer flags change every object
  file, so `asan` and `tsan` builds cannot reuse `debug` objects, and switching
  presets never triggers a full rebuild of the others.
- **Configured only on Linux.** The presets carry a host-system condition, so on
  macOS they are hidden and refuse to run, and the top-level `CMakeLists.txt`
  stops any non-preset configure with a pointer to the VM. This matters because
  `build/` is shared: a host-side configure would write a macOS compiler into a
  cache the VM then reuses.
- **No in-source builds.** Refused at configure time; the two files CMake writes
  before the refusal can run are ignored.

`cmake --workflow --preset <name>` configures, builds, and runs the tests in one
step. Tests carry ctest labels: `env` for `tools/probe`, and tier labels
(`tier1`, …, §8.2) as the suites arrive, so `ctest --preset debug -L tier1` runs
what CI can run anywhere.

**Page size.** The VM runs 4K pages; Raspberry Pi OS on a Pi 5 defaults to 16K.
Nothing may hardcode 4096 — including the §4.3 linker script, which aligns to
`CONSTANT(MAXPAGESIZE)`. If page-size bugs surface before a Pi is available, a
16K kernel boots in the VM (Apple Silicon supports 4K and 16K granules, not 64K).

**`tools/probe`** checks every kernel assumption this document makes and prints
a pass/fail line for each: single-issuer rings with an explicit CQ size,
`IORING_OP_FUTEX_WAIT` returning `-EAGAIN` on a stale value, multishot recv
producing one CQE per provided buffer plus a terminal `-ENOBUFS`, cross-process
futexes on a shared mapping, and the absence of 64-bit futexes. **Run it first on
any new machine**, before debugging anything else there.

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
10. **Collective epoch counters are compared for equality, which assumes bounded
    skew.** (§5.4) Guaranteed by blocking collectives. Nonblocking collectives
    break it and must move to 64-bit `>=` predicates with a separate wake word.
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
15. **Nothing links but libc and the distribution's liburing.** (§1) Not a style rule:
    it is the project's reason for existing, and each dependency added is one the
    target hardware may not be able to build.
16. **Before release, the local lead is the only writer of its `dest`, and it
    leaves the collective last.** (§5.5) Concurrent combines lose updates; a lead
    that leaves early lets the next epoch clobber a result still being copied out.
17. **A data-carrying collective reads a peer's `source` only after that peer has
    called.** (§5.5) Pulling before arrival is non-conforming under 1.6 §9.10.
18. **Futex words are 32-bit, and `futex_wait` deadlines are absolute.** (§3.10,
    §4.9) The first is a kernel limit (`FUTEX2_SIZE_U64` returns `EINVAL`); the
    second fails silently, as a busy loop.

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
| How a PE blocks in `wait_until` | **Watch table plus a 32-bit futex sequence word per slot** (§4.9). Futexing on the `ivar` itself was withdrawn: 64-bit futexes do not exist |
| io_uring ring bound | **Two bounds, two rings.** CQ is sized from the provided-buffer ring (§3.3). Verified by `tools/probe` |
| Reduction `dest` versus 1.6 change #17 | **Not a conflict** — same arrays means same symmetric offset. The surrounding text exposed three real bugs, fixed by a single-writer lead that leaves last (§5.5) |
| Collective epoch identification | **Cumulative 32-bit counters with equality waits** (§5.4). Sense reversal retired |
| 64-bit futex support | **Not implemented.** `EINVAL` on 6.12 aarch64 (`tools/probe`) |
| Development environment | **Lima + Debian 13 on Apple Silicon**; x86 and ARM in CI; a physical Pi for the kernel fork, NIC, and page size (§8.5) |
| Vendor liburing or use the distribution's? | **Distribution's, ≥ 2.6.** Older OS releases are unsupported rather than accommodated with a bundled copy (§1) |
| Build tool | **CMake ≥ 3.25** (§1) |
| Project name | **osoe** (OpenSHMEM Over Ethernet). Library `libosoe`, built with hidden visibility so only spec API symbols are exported (enforced by the `shmem.exports` test); internal non-static symbols `osoe_`; implementation-specific environment variables `OSOE_`, since the spec reserves `SHMEM_`. Chosen over `eoshmem`, one letter from Open MPI's `oshmem` |
| Public header name | **`shmem.h`**, as OpenSHMEM programs expect. Coexistence with another implementation is by install prefix, as with MPI |

### Still open — blocking

- **Thorough 1.6 spec read.** The targeted questions above are answered; the
  sweep is not done. Specifically still unchecked: whether any team query must
  return remotely accessible memory; and the exact semantics of the 1.6 session
  routines if they are ever more than no-ops.

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

### Still open — project

- **License.** None chosen. The repository carries no `LICENSE` file, and the
  README states that no rights are granted until one is added. Nothing should be
  published or accept outside contributions before this is settled.
