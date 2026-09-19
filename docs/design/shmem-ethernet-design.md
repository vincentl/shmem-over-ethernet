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

Inside the SHMEM layer, §4 reaches other PEs only through the transport boundary
of §4.10: put, get, rmw, fence, wait, and notify. AM is one implementation of
that boundary.

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

#### Where the buffers live, and how they are sized

**A separate shared object per node, not part of the control segment.** The
connection buffers are the only AM-specific allocation in the design, and §10's
open question — whether §3's data path stays hot in an RDMA build — counts their
memory among its uncounted costs. Putting them in the control segment would
settle that question by accident, in the direction of "always allocated", in the
one segment `osoe-stat` maps read-only and whose size would then vary by build.
Separate, a build that does not keep the AM data path hot never creates it, an
RDMA backend registers one contiguous region rather than N, and a put can go
from the caller's `src` straight to the NIC rather than through a staging copy
nothing needs. Writers are application threads in every local PE and the reaper
is the lead's agent in another process (§3.12), so it is shared memory either
way; the question was only which object.

**Sizing scales with node count and has no PE dimension at all** (§1). Per node
the area is `2 × (nodes − 1) × buffer`: every local PE contends on the same open
buffer through §3.5's CAS, so 256 PEs on a node cost exactly what one PE costs.
Giving each PE its own buffer per peer would make it `L × 2 × nodes` and destroy
the single ordered stream §3.1 depends on for free `fence` ordering — the same
mistake as a second data socket, in a different place.

| Variable | Default | Meaning |
|---|---|---|
| `OSOE_AM_CHUNK` | 64 KiB | §3.2's fragmentation size, and so the largest message on the wire |
| `OSOE_AM_BUFFER` | `2 × chunk` | Bytes per connection buffer; a power of two, refused below the floor |
| `OSOE_AM_BUFFER_BUDGET` | 256 MiB | Per-node ceiling on `2 × (nodes − 1) × buffer` |
| `OSOE_AM_HUGEPAGES` | 0 | Opt in to huge pages for the area |

**The floor is the default, because everything except aggregation pushes down.**
§3.2 bounds head-of-line blocking at roughly two segments — about 2 ms on 1 GbE
at 128 KiB, doubling with the buffer — and §3.9's self-clocking already grows the
batch under load without needing headroom, since the buffer only has to hold
what accumulates during one in-flight send. Going above the floor is an opt-in
for links fast enough that a millisecond of wire time stops mattering.

**The budget is a guard rail, not a routine mechanism.** At these defaults it is
not reached until about a thousand nodes, past what §1 targets, so inside the
supported range the behaviour is a constant buffer size with no heuristic to
reason about: 3.75 MB per node at 16 nodes, 125 MB at 500. When it is exceeded
the buffer halves — staying a power of two, which the ring arithmetic needs —
and keeps the *largest* size that fits; the floor is a limit, not a target. If
the floor itself does not fit, that is an error naming both knobs, not a silent
drop below it, because below the floor aggregation fails at the top of the range
and surfaces as a performance mystery rather than a message.

**The case that actually needs tuning is testing, not production.** §8.1's
logical node map puts every node on one machine, so the whole-host cost is
quadratic: 60 MB at 16 logical nodes, about 1 GB at 64, 16 GB at 256. Lowering
`OSOE_AM_CHUNK` is the knob for it, since it lowers the floor with it — 64
logical nodes cost about 1 GB at the default chunk and 129 MB at 8 KiB. Tier-2
tests set the sizes explicitly rather than inheriting the production default.

**Huge pages are off by default, the opposite of the rule for heaps** (§6.2),
and the inversion is deliberate. Heaps are touched densely; connection buffers
are touched sparsely, because a halo exchange faults six peers' worth of the
whole area. `hugetlbfs` pages are reserved up front, so opting in trades away
exactly the demand paging that makes a large node count affordable.

**Known and not addressed: NUMA first touch.** On a multi-socket node the lead
creates the area, so first touch places every buffer on the lead's NUMA node and
PEs on the other socket pay for it on every `memcpy`. The target hardware is
single-socket, and fixing it means a first-touch policy per PE or per peer.
Recorded rather than built.

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

**A connection starts with both buffers sealed, and that has to be written
down.** Shared memory arrives zeroed — `ftruncate` guarantees it, and §6.2
relies on that for every other counter and futex word in the design — so a
connection that is merely zeroed has both heads at 0, which reads as *unsealed
and empty*: two reservable buffers, the state this section forbids, before the
progress thread has opened anything. Writers would reserve into a buffer the
progress thread believes is free, and it would reset the head over their
messages when it opened it. Initializing a connection therefore means setting
the seal bit on both buffers, not zeroing them. *(Found in implementation at
§7.1 step 2: the zeroed state is reachable precisely because zeroing is
otherwise the right default everywhere else.)*

**Writers find the reservable buffer by the seal bit and nothing else.** With
two buffers that is two loads, and no separate word names the open one. An index
would be this section's rejected draft in a new place: a writer reads it and
then CASes a head the progress thread sealed in between. The CAS would still
fail, so the index would not be *wrong* — it would be worse than that, leaving
the seal bit doing nothing the index had not already done, while §3.8's
`DRAINING` guard rests on the bit alone. Keeping the bit as the only decider is
what keeps the guard's rule load-bearing rather than decorative.

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

**Taking a credit is a CAS loop, not a test and a decrement.** The credit is per
*connection*, so the PEs of a node share it and contend on it. A PE that tests
`credits > 0` and then decrements can be overtaken between the two steps by a
sibling doing the same, and both take the last credit — the count goes negative
and the target's queue is over-subscribed by exactly the amount nobody
accounted for. This is the same argument §3.5 makes for the reservation itself,
and it is stated here because it was missing: the model's own first encoding of
the take had this bug, which is how it was found
(`docs/design/evidence/am-backend/`, the `race_take` case).

Two further results from that model, each easy to lose:

- **Every breakage passes with one PE per node.** The credit binds only when a
  node has more than one PE contending for it, which is every real node. A model
  or a test run only at `L = 1` calls all of these correct.
- **The credit returns when the reply *lands*, not when it is sent**, and the
  difference is invisible while gets are blocking. The credit keeps a bound at
  each end: reply descriptors at the target, and replies in flight toward the
  origin's landing zone (§3.12). Returning it at handler time breaks both.
  Returning it when the reply is drained onto the wire keeps descriptors bounded
  and breaks only the landing zone — and under `SERIALIZED` with blocking gets a
  PE waits for its own reply before issuing again (§4.6), so nothing can show it
  until nonblocking gets arrive. Do not simplify "lands" to "sent" on the
  evidence of the blocking cases.

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
  network progress; debug with non-stop mode, or read its state with `osoe-stat`
  (§3.14), which needs no cooperation from the stopped process. A crashed lead cuts the node off. Under §3.15
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

### 3.14 Introspection and metrics (first-class, not retrofitted)

> **The library writes introspection state into shared memory; an external tool
> reads it. The library serves nothing and writes no files.**

The case that matters most is a hung job. Any export the job serves or writes
fails exactly then, because the process that would serve or write it is the one
that is stuck. Memory can be read without the job's cooperation.

#### Where the state lives

- **In the node's control segment**, `/osoe.<job>.<step>.<node>` (§6.2), which
  **keeps its name until the last finalize** so a reader can open it while the job
  runs. Heap names are still removed once every local PE has mapped them.
- **A crash leaves the control segment in `/dev/shm`** — a few MB. That is also the
  post-mortem: the state at the moment of the crash stays readable until someone
  removes it. `osoe-stat --gc` removes segments whose recorded pids are all gone,
  and a Slurm epilog can remove `/dev/shm/osoe.<jobid>.*`.
- **Mode 0600.** The segment holds connection buffers, and so application data;
  only the job's user can read it.
- **Self-describing.** A header carries a layout version and a table of metrics —
  name, type, unit, offset — checksummed like the handler table (§3.13), so
  `osoe-stat` need not be the same build as the job. Handler names live in the
  same table.

  **A frozen directory makes that real.** The segment opens with
  `struct osoe_segment_directory`: magic, layout, and where the three arrays are
  with the size of each element. It is the *only* part a reader may depend on by
  layout, and it exists so nothing else has to be — fields are never reordered,
  resized or removed there, only appended, and `directory_bytes` says how much
  of it the writer filled in. Everything after it is read through the table, by
  name. Without a directory that is separated and frozen on purpose, "self
  describing" is a claim rather than a mechanism, and the first structural
  change quietly breaks every tool.

  **`struct osoe_metric_desc` is frozen with it, and for the same reason.** The
  table is what makes every other field readable, so its own element cannot be
  described by the table; something has to be the floor. The directory carries
  `metric_desc_bytes` so that a reader *checks* this assumption rather than
  making it, and a reader that meets a descriptor size it does not know prints
  the directory and stops, which is the same behaviour as an unknown `layout`.
  Two structures, both frozen, both checked: that is the whole contract between
  a job and a tool from another commit.

  Four sections, each starting on a cache line: the header, the PE slots, the
  metric table, and the counter blocks. A metric names its **scope** — node,
  PE, or thread — which says which record its offset is into.

- **Counters are bounded per PE, and exhaustion is visible rather than fatal.**
  `OSOE_MAX_THREADS` blocks per PE, claimed by CAS on first use. A thread that
  finds none gets a **process-private block**, and the *fact* is recorded as
  `invariant.blocks_exhausted` **in the PE's slot, not in a counter block**.
  That distinction is the whole point and an earlier version of this line got it
  wrong: a thread with no block writes to memory nobody outside the process can
  read, so a counter kept there is exactly the one counter that can never be
  seen — including by the §8.2 assertion that exists to see it. It is written by
  whichever thread ran out, so it is a `seq_cst` read-modify-write rather than
  this PE's single-writer publication, on a path taken at most once per thread. Not an abort: §4.11 proposes aborting when a
  PE's thread table fills, because there correctness depends on having a record,
  but here only the metrics do — and a job that dies because it made a ninth
  thread would be a worse failure than a job whose ninth thread is not counted.
  Not a shared block either, which would break §3.16's single-writer rule on
  every counter. The private block makes the fallback path identical to the
  normal one, so nothing has to test whether it is initialized.
- **On abort**, the library prints the control segment's name to standard error, so
  whoever reads the message knows what to point `osoe-stat` at.

#### `osoe-stat`

An **installed** tool that maps the control segment read-only and reads without
locks and without the job's cooperation. It is the only tool under `tools/` that
is installed, because the thing it is for happens on a machine that has no build
tree.

It is written **against the segment, not the library**: it includes nothing from
`shmem/src` and nothing from `base/`, only libc and the two frozen structures
above. That constraint is the mechanism. A reader with access to the library's
headers would find fields by `offsetof` like the library does, the table would
describe nothing anyone depended on, and the first structural change would break
every tool while the build still passed. Having no other way to find anything is
what keeps "self-describing" true.

What it does:

- a **live view of counters**, per PE, with `--interval` to repeat. A PE's value
  for a thread-scope metric is the **sum** of its blocks for a counter and the
  **maximum** for anything else: adding two threads' last-wake timestamps would
  produce a number that means nothing, and `kind` is exactly that distinction,
  which is why it is in the table;
- a **state dump** — every field of the header, every PE slot, every watch slot,
  epoch counter and "blocked in" record, and every claimed counter block. This
  replaces the earlier `am_dump_state(FILE *)`, which needed a call from inside
  the job;
- per-thread **CPU time and involuntary context switches**, read from
  `/proc/<pid>/task/<tid>/` using the tid each block records and the pid its PE's
  slot records — the cost rule below, that anything the kernel already tracks is
  read by the tool;
- **human-readable and JSON** output;
- **`--gc`** for segments left by crashed jobs. A segment is collectable when
  every PE that ever joined it is gone; one where *no* PE has joined yet is left
  alone, because that is a job in bootstrap and not a corpse. Heap objects are
  removed first, by the name each slot records, since removing the control
  segment first would strand them. A recycled pid can only make a dead job look
  alive, which costs a few megabytes of `/dev/shm` rather than removing a
  running job's segment.

Readers load with `memory_order_acquire` and accept values a moment stale.

**One field is decoded, not merely printed: the §4.9 watch word.** Its encoding
is part of what `layout` covers (§10), so the tool carries its own small copy of
the decode and uses it only when the segment's layout is the one it was built
against; any other layout gets the raw word and a note. That is what a layout
version is *for*, and it is the only place the tool knows an encoding rather
than an offset.

**Deferred, and why.** *Prometheus output* is a third formatter over the same
table and waits for a reason to exist; node_exporter's textfile collector is
still where it would go, so osoe would contain no HTTP code. *Netlink
`sock_diag`* for per-connection round-trip time, retransmits and congestion
window needs connections, which arrive with the AM backend (§7.1 step 2). *The
flight recorder* is deferred below, for the same reason.

*(Rejected: an HTTP endpoint in the library. It fails during a hang, when it is
needed most; ports per ephemeral job do not fit scrape discovery; it exposes a
network port; and it puts socket code in the progress process. If a scrape endpoint
is ever wanted, it belongs in a separate process built on `osoe-stat`. Rejected:
metrics files written by the library. The library writes no files; the persistent
control segment already provides post-mortem state after a crash.)*

#### Two claims, and what tests them

Both are claims a tool cannot make about itself.

**"A tool built from a different commit reads a job correctly."** Running
`osoe-stat` against a job from the same tree tests none of this: every offset
matches by accident. So a tier-1 test writes a control segment by hand with
record layouts unlike the library's, a metric table in a different order,
metrics this build has never heard of, and a layout version from the future, and
requires the reader to find every value it does know, by name, anyway. It also
requires the refusals: a metric whose offset runs past the end of its record, one
whose offset is not a multiple of its width, and an index outside its scope are
each refused rather than followed, because a corrupt segment is read on a
machine where something has already gone wrong.

**"Every `invariant.*` counter is zero at quiescence."** Until something asserts
it, an invariant counter is a diagnostic and not an oracle: `missed_wake` could
fire on every run of every test and nothing would say so. Every tier-2 test now
calls `shmem/tests/invariants.h` before its last `shmem_finalize`. It takes a
barrier as the definition of quiescence, then walks the metric table and asserts
every metric of kind `OSOE_MKIND_INVARIANT` is zero in every record of its
scope — **found by kind, not by name**, so it covers every invariant counter
added later without being edited. It uses the same reader `osoe-stat` uses.
Sharing that reader with the tool is the point; sharing it with the library
would not be, since a check that read the counters through the library's own
headers would pass whenever the library agreed with itself.

#### Cost rules

- **One counter block per thread** — application thread, agent, progress thread —
  cache-line aligned with `_Alignas` in the control segment. The alignment constant
  is 128 bytes on aarch64 and 64 elsewhere. Apple cores, where the development VM
  runs, use 128-byte lines although the guest reports 64. 128 is a multiple of the
  Pis' 64, so one value separates words on both, at the cost of some padding. Only the owning thread
  updates it, with an `acquire` load and a `release` store: §3.16's single-writer
  case, with no locked instruction.
- **Per-peer detail only in progress-thread blocks**, one set per connection: about
  500 connections × 30 counters × 8 B ≈ 120 KB. PE-side counters aggregate across
  peers; per-PE-per-peer counters would be hundreds of MB at 256 PEs and 500 peers.
- **Timing only on slow paths:** blocking operations, waits, and once per segment.
  Never per put or AMO. `CLOCK_MONOTONIC`, through the vDSO.
- **Histograms use log2 buckets**, about 32 per histogram. The bucket index comes
  from `stdc_leading_zeros_ull` in C23 `<stdbit.h>` — verified to build under
  `-std=gnu11` with glibc 2.41 — rather than `__builtin_clzll`, which §3.16 rules
  out.
- **Anything the kernel already tracks is read by `osoe-stat`, not maintained by
  the job.** Per-thread CPU time and context switches come from
  `/proc/<pid>/task/<tid>/`, using the thread ids recorded in the segment (verified
  readable by another process). TCP round-trip time, retransmits, and congestion
  window come from netlink `sock_diag` — the mechanism `ss -ti` uses — keyed by the
  socket addresses recorded in the segment.

#### Catalogue, by the question each metric answers

**1. Is it hung, and where?** State, not counters:

- **Progress heartbeat:** loop iterations and the time of the last completion, per
  progress thread. Two reads showing the same values mean the lead is stalled.
- **Per team:** `arrived`, `released`, `copied`, and each PE's epoch counts `k` and
  `d` (§5.4). This shows which PE did not reach a barrier — the most common real
  hang.
- **Per PE:** a "blocked in" record (routine, target, and since when), its watch
  slot (§4.9), outstanding get bytes, bootstrap phase, and the abort word.

  It is the first record here with more than one word in it, so it is the first
  that can be read torn while the PE publishes its next wait. A **generation
  word bumped with two `seq_cst` read-modify-writes** around the field writes,
  and a reader that loads it, reads the fields, and loads it again, accepting
  only an unchanged even value. Two locked operations rather than a textbook
  seqlock, because that needs relaxed stores and a release fence and §3.16
  permits neither, while `fetch_add` naming `seq_cst` is a rule the design
  already has and is a full barrier on both targets. Model-checked in
  `docs/design/evidence/blocked-record/`, which also shows the four ways to get
  it wrong: no generation, bumping after the writes, letting a field write
  escape above the opening bump, and a reader that does not re-read.

  **Published only when a wait is about to sleep**, never on entry. A wait whose
  predicate is already true touches no locked instruction and no clock — which
  is exactly what §8.6's U1 and U3 measure, "wait with nothing pending", so the
  record must not appear in that path's cost. The consequence to accept is that
  a PE spinning through satisfied waits shows no record; it is also not blocked.
- **Per connection:** sequence numbers sent, acknowledged as applied, and received
  and applied; the age of the in-flight send; each buffer's reserved and filled
  counts and seal bit; reply queue depths; credits left; waiters per wake word; the
  armed-futex flag.

**2. Is the network the bottleneck?**

- **Per peer:** bytes, segments, and messages in each direction; messages per
  segment and bytes per segment, as histograms; send latency from submit to
  completion, as a histogram; short sends; `-ENOBUFS` re-arms; multishot
  terminations; header-only ack segments sent.
- **Per connection, at read time:** TCP round-trip time, retransmits, and congestion
  window. These separate "the network is slow" from "osoe is slow".

**3. Are aggregation and progress healthy?**

- Publication-window stalls, count and duration (§3.6).
- Bytes per DRAINING pass and how often the cap hits (§3.8).
- Seals forced by `fence` and `quiet`, per PE (§4.6).
- Waits for a reservable buffer, and seal requests.
- Wakes issued and wakes avoided; failed acquisitions after a wake (§3.11).

**4. What is the application doing?** The SHMEM layer, per PE:

- Calls and bytes per routine family, split into intranode and internode.
- Latency histograms for blocking gets, `quiet`, barriers, and `wait_until`.
- Outstanding-get high-water mark; how often and for how long a missing reply
  credit blocks the PE.
- Heap bytes in use, high-water mark, and largest free block.
- Agent wakeups, by reason.

**5. Is the node itself the problem?**

- The progress thread's CPU time and **involuntary context switches**, read at read
  time. Rising involuntary switches mean application threads on the lead's cores
  are starving progress — the cost §3.12 accepts.

**6. Invariant counters, which must stay zero:**

- Safety-sweep submissions that found sealed work nobody had signalled (§3.10).
- Deadline expiries after which the awaited condition was already true: a missed
  wake, in the watch slots (§4.9) or any other wait — **but only where the spec
  requires the variable to be updated with an AMO**, which is every width of 4
  or 8 bytes. At two bytes there is no AMO type at all: the deprecated `short`
  waits of Table 12 predate 1.5 change #17, so the only remote update is a put,
  for which notification is best effort by design (§4.10) and finishing at a
  sweep is how such a wait is *meant* to end. Those go to `tp.watch.late_wake`,
  an ordinary counter.

  The split is what makes this an oracle rather than a diagnostic. One counter
  was nonzero for reasons that were not bugs, so the only way to keep the §8.2
  assertion true was to write the tests around it — `shmem.waitmany` used the
  non-blocking `shmem_short_test` and said so — and a test suite shaped to avoid
  a counter is not checking it. With the split, the blocking two-byte path is
  exercised for the first time, and it is the only user of the boundary's
  two-byte coherent load and two-byte watch range.

  **Measured: the late counter is zero on this backend.** Thirty runs at four
  PEs, with the writer delayed so the waiter is certainly asleep, produced no
  lost wake — the intranode backend's `notify` after a `memcpy` does reach the
  waiter in practice on aarch64. The split is therefore justified by the
  *contract* and not by an observed failure, which is the right basis: §4.10
  promises nothing here, and a NIC that writes without the CPU seeing it will
  lose these wakes even where this memcpy does not.
- A thread that found no counter block (above). Not a correctness failure, but a
  silent loss of every other counter that thread would have kept, which is the
  same thing from a reader's point of view.

**The test suite asserts every invariant counter is zero at quiescence after every
test** (§8.2), so these counters double as a correctness oracle.

#### Flight recorder

**Deferred, and not for cost.** Every event listed below is a transport
event — seal, submit, send completion, segment received, handler dispatch, ack —
and there is no transport until §7.1 step 2. A ring designed now would be
designed against imagined events and their imagined arguments, and would be
rewritten by the first real one. It arrives with the backend that produces the
events, and the design below stands until then.

Two smaller deferrals for the same reason. **Prometheus output** is a third
formatter over the same table and waits for a reason to exist. The **intranode
versus internode split** on the RMA counters needs a dispatch to split on, and
so does `tp.wait.dirty_peers`; both land with the AM backend, which is also when
the per-peer and per-connection counters of questions 2 and 3 first have write
sites.

**On by default, for now, for transport events.** Each progress thread has a ring
of fixed-size records in the control segment — for example 4,096 × 32 B = 128 KB:

- **Events:** seal, submit, send completion, segment received, handler dispatch,
  ack sent, ack received.
- **Record:** monotonic timestamp, event type, connection, sequence number, and one
  argument. Writing one is a few single-writer stores and an index update.
- **PE-side events** are off by default and enabled through an `OSOE_` environment
  variable.
- **Reading:** `osoe-stat` copies the ring and discards records overwritten during
  the copy, detected by comparing the write index before and after.

When a job hangs, the last events before it stopped are usually the difference
between reading counters and knowing the answer. Revisit the default if
measurement shows a cost.

#### Other introspection

- **Tracing hooks** at enqueue, wire send, wire receive, handler entry, and handler
  exit, default no-op. Sequence-number every AM so a trace is replayable and
  reordering is visible rather than inferred.
- **Fault injection** in the layer, not the tests — delay, drop, reorder, truncate,
  allocation failure. Injecting from outside cannot reach most error paths.
- **Quiescence primitive** — drain, then assert nothing in flight, all accounting
  balances, and every invariant counter is zero. Most correctness bugs surface as a
  failed invariant here rather than as a wrong answer.
- **Slow mode as a policy setting, not a separate code path:** no coalescing, one
  message per operation, synchronous acknowledgement. Every test runs both ways and
  results are compared.

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

#### The development watchdog

> **A development aid, not the release path: on in the `debug`, `asan` and `tsan`
> presets and off in `release`, with `OSOE_WATCHDOG` forcing either way.**

Everything above is the production answer, and none of it exists before §7.1
step 4. Until then a PE blocked in `wait_until` or a barrier whose sibling has
died waits for the launcher's timeout, which on a development machine means
minutes of nothing. The watchdog gives the developer a loud failure instead:

- **An abort word in the control segment header**, plus a liveness check of the
  local PEs' recorded pids, **run only when a library wait's safety-sweep
  deadline expires** (§3.10). Never on the wake path, so a healthy wait pays
  nothing at all for it — no load, no branch — and a wait that is merely slow
  pays one pass over the local slots per sweep.
- **On a dead PE or a set abort word, abort loudly**, naming the PE, the routine
  the caller is inside, and the control segment to point `osoe-stat` at (§3.14).
  The PE that notices sets the abort word first, so its siblings stop at their
  own next sweep instead of each rediscovering the same corpse.
- **Liveness is `kill(pid, 0)`**, using the pid each PE publishes in its slot
  before the release store of its phase. `ESRCH` is unambiguous: no process holds
  that pid. A recycled pid can only make a dead PE look alive, which is the hang
  this replaces — never a false abort on a healthy job.

**The scoping is the point.** Production must not come to depend on it, because
it is strictly weaker than what step 4 brings and is absent from the build that
ships. Off in `release` is what keeps that honest. What it deliberately does not
do:

- **No pidfd polling.** Detection is a poll at a sweep, so it is up to one sweep
  late, and it cannot see a PE that is alive but wedged. The lead's ring holding
  a pidfd per local PE is step 4's answer.
- **No cross-node detection.** One node's PEs check one node's pids. A vanished
  peer node is TCP keepalive's business, and there is no TCP yet.
- **No `shmem_global_exit` and no abort flood.** The abort word is written and
  read within one node by whichever PE noticed. `global_exit` (§9.1.6) needs the
  agent-calls-`exit` path above and an AM to carry the abort between nodes.
- **Not during rendezvous.** The bootstrap waits already carry
  `OSOE_BOOTSTRAP_TIMEOUT` and a `failed` word, and run before the slots a
  liveness check would read are all published.

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
loudly rather than silently unmapping something.

**A trap to record before anyone falls into it.** An RDMA backend wants one
registrable memory region per *node* rather than one per PE — 16 bytes of
`{base, rkey}` per node instead of per PE, and one registration instead of `L`.
That needs a node's heaps contiguous in the registering process's address space,
and the obvious way to get it is to slot each heap by local id at
`N + local_id × SHMEM_SYMMETRIC_SIZE`. **That is wrong**, and wrong in a way that
would not surface for a long time: it destroys the property that a PE's own heap
sits at the same `B` in every process, which is what makes a PE's symmetric
addresses the same numbers everywhere and what §4.3 relies on for statics under
`-no-pie`. The right shape is a **second mapping** — the node lead maps all `L`
heap objects again into one contiguous reservation, used only as something to
register. Same physical pages, no change to `B`, no change to `base_of`, and the
cost is `L × SHMEM_SYMMETRIC_SIZE` of address space in the lead alone, against
the same amount every process already reserves. It is an optimization, not a
requirement: skipping it costs one registration per PE and a per-PE table, with
no redesign anywhere. Record it so nobody buys contiguity by moving `B`.

**Choosing `B`.** No single address works for every build. In the development VM
(aarch64, 48-bit addresses), a 16 GiB fixed mapping succeeds from `0x10_0000_0000`
upward in a plain build. AddressSanitizer allows only
`[0x2010_0000_0000, 0x5000_0000_0000)` and above `0x5410_0000_0000`.
ThreadSanitizer allows only below `0x0a00_0000_0000`, a small band near
`0xaab0_0000_0000`, and above `0xfc00_0000_0000`. Above `0xfc00_0000_0000` is common
to all three, but it does not exist in a 39-bit or x86-64 address space. So:
- **The lead chooses `B`** by mapping its own heap at the first of a short
  candidate list that succeeds: `0x1000_0000_0000`, `0x3000_0000_0000`,
  `0x100_0000_0000`, `0x10_0000_0000`. That is a plain build, ASan, TSan, and a
  39-bit address space, in order.
- **It publishes `B` in the control segment** before any sibling reads it.
- **Every other PE maps its own heap at the published `B`**, and fails loudly if it
  cannot. All local PEs run the same binary, so the lead's choice suits them.
- **`OSOE_HEAP_BASE` replaces the list** for systems where none of the candidates
  is free. It is also the hook for §8.2's heap-base-shift test mode.

*(Rejected: a compile-time `B` selected by sanitizer macros. It gives each build one
fixed guess, whereas the lead's search adapts to address spaces nobody listed.)* `-no-pie` pins the executable
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
> fine. First-fit with coalescing, or a buddy allocator, both work.

**Chosen: first fit over an address-ordered block list, splitting on allocation and
coalescing with both neighbours on free.** Blocks are aligned to 16 bytes, which is
`_Alignof(max_align_t)` on both targets and is what spec §9.3.1 asks for. The list
is a private array, so allocation is O(blocks). That is acceptable because
allocation is cold in every code the usage study read (§8.6), and the array's size
depends only on the call sequence, so determinism holds. A buddy allocator was
rejected: it rounds every request up to a power of two, which wastes up to half the
heap. A tier-1 test runs two allocators through 20,000 random allocations and
out-of-order frees and requires identical offsets.

Running out of private memory for the metadata is fatal rather than a null return:
a failure on one PE and not another would desynchronize the heaps. Fragmentation
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

- `shmem_malloc`, `shmem_align` and `shmem_calloc` barrier on **exit**;
  `shmem_free` barriers on **entry**.
- **`shmem_realloc` barriers on entry *and* exit**, which an earlier draft of
  this list got wrong by giving it the exit barrier alone. Spec §9.3.3 permits
  both — "`shmem_realloc` may call barriers on both entry and exit, depending on
  whether an existing allocation is modified and whether new memory is
  allocated" — and both are needed here: it frees the old block, which no PE may
  still be reading, and it publishes a new one, which every PE must see before
  any returns. It is `free` and `malloc` in one call and takes each one's
  barrier. A null `ptr` makes it `shmem_malloc`, so it takes only the exit
  barrier; a zero `size` with a non-null `ptr` makes it `shmem_free`, so it takes
  only the entry one.
- **`shmem_align` aligns an address, but the allocator works in offsets.** The
  two agree only while the heap's base `B` is itself a multiple of the requested
  alignment, which every candidate base in §4.1 is up to a terabyte. Above that,
  `shmem_align` reports a failed allocation rather than returning an address
  that is not what was asked for: a null return is a value the spec defines, and
  every PE reaches it identically because `B` is the same number on all of them.
  A non-conforming alignment — not a power of two, or not a multiple of
  `sizeof(void *)` — is undefined by spec §9.3.4 and is a loud failure here,
  because the alternative is a symmetric object that is not where the program
  thinks it is.
- **`shmem_realloc` resizes in place where it can**, growing into a free
  neighbour above and shrinking by releasing the tail, before falling back to
  allocate-copy-free. That is not an optimization: the fallback needs the old
  and the new block to fit in the heap at once, so a program that grows one
  large array would run out at half the heap instead of all of it.
- A size of 0 (for `calloc`, a count or size of 0) returns a null pointer **without
  a barrier**. Every PE must make the same call, so none waits.
- `shmem_free(NULL)` does nothing, also **without a barrier** (spec §9.3.2).
- A failed allocation still barriers on exit. Every PE fails identically, so the
  collective completes and all of them return a null pointer.
- `shmem_calloc` treats `count × size` overflow as a failed allocation.
- `SHMEM_SYMMETRIC_SIZE` accepts the §8 grammar — a non-negative integer or
  floating-point prefix with an optional scaling character — and must agree across
  PEs, checked at rendezvous (§6.2).
  - **The size is exact.** It is the integer ceiling of the number times the
    multiplier, computed in integer arithmetic, so "3.1M" is the spec's 3250586
    on every machine, with no dependence on binary floating point.
  - **Decimal notation only.** The spec says "floating point value" without
    defining it. An exponent ("1e3"), a sign, surrounding space, or any character
    other than a multiplier after the number is an error, which the spec lets init
    report by terminating. Characters after the multiplier are ignored, as the spec
    requires. Accepting `strtod`'s wider grammar would silently turn a typo such as
    `1e` into a size.
  - **A value that does not fit in 64 bits is an error.**
  - The grammar lives in `base/size.c`, with its tier-1 test next to it.

**Default size.** Without `SHMEM_SYMMETRIC_SIZE` a heap is 256 MiB. Heap objects
commit pages only when touched, so the default costs address space: L × 256 MiB
per process, 64 GiB at 256 local PEs. A size of 0 is valid and maps one page, so
every allocation of nonzero size fails.

**Hints are recorded, not discarded.** An earlier note here called
`shmem_malloc_with_hints` an alias for `shmem_malloc`, on the grounds that SOS
added the routine supporting no hint values. That is a fine *implementation* of
the hints and a bad *structure*, because two other decisions need the hint to
have been kept:

- spec §9.3.3 requires `shmem_realloc` to **preserve** an allocation's hints, and
  to return a null pointer "if the space cannot be allocated **or if hints cannot
  be preserved**". So a hinted block that cannot be grown at the hinted end fails
  the call — it never silently migrates to the unhinted end, which would be a
  quiet loss of the property the program asked for.
- §4.10's `set_range` tells a backend which ranges carry atomics or signals, and
  its call site is the moment a hinted allocation extends the hinted end.

So the allocator records a hint with each allocation and grows hinted
allocations from the opposite end of the heap from unhinted ones. Both ends are
a pure function of the call sequence, so determinism and invariant 6 are
untouched. The hints change no behaviour on one node and are not required to
change any behaviour ever — they are advisory — but retrofitting hint plumbing
into a finished allocator is the expensive version of this.

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

**Thread level.** Phase 1 provides up to `SHMEM_THREAD_SERIALIZED`: `SINGLE`,
`FUNNELED`, and `SERIALIZED` are all supported, and a request for `MULTIPLE`
provides `SERIALIZED`, which §9.2.1 permits. The structures below assume that at
most one application thread of a PE is inside the library at a time. They are
marked where `MULTIPLE` would change them: under `MULTIPLE` the equality wait below
can starve, and clearing the dirty set races. §4.11 designs both changes. Contexts
that promise serialization keep these structures as they are.

The three levels cost the same, because every structure in this section is safe
under "one caller at a time". Which thread the caller is does not matter.
`FUNNELED` is `SERIALIZED` with one thread. Three things do depend on the calling
thread, and each is already per-thread or checked:

- **Memory stores.** Fence and wait order only the calling thread's stores (spec
  §9.12.1, §9.12.2). Stores by another thread are the application's to make
  visible.
- **Library state handed between threads.** Under `SERIALIZED`, thread A may leave
  plain stores in library-private state, such as the dirty set or a team's epoch
  count, and thread B may load them next on another core. The spec requires the
  application to keep calls from overlapping. Any mechanism that does so is a
  synchronization under the platform's memory model, and that makes A's stores
  visible to B. This is the same argument the spec makes for memory stores, and the
  library relies on it rather than adding a serializing store to every entry point.
- **Finalize and fork.** `shmem_finalize` must come from the thread that called
  `shmem_init_thread` (spec §9.2, rule 1). The fork check (§6.4) compares pids, not
  threads.
- **`shmem_init_thread` answers, it does not refuse.** A request for
  `SHMEM_THREAD_MULTIPLE` returns 0 with `provided` set to
  `SHMEM_THREAD_SERIALIZED`. The return value is about whether the library came
  up, and the `provided` argument exists precisely so that a program can ask for
  more than it gets and find out; a nonzero return would make an ordinary
  negotiation look like a failed initialization.
- **The level is fixed by the first initialization of a session** and a nested
  one does not change it, which spec §9.2.1 permits in as many words. Between
  sessions it is free again. `shmem_query_thread` may be called at any time,
  including before any `shmem_init`, where the answer is the level a
  `shmem_init` would provide — which, since `shmem_init` lets the implementation
  choose (§9.2.2), is the most osoe has.

Per-thread records (a watch, a blocking-get completion slot) are allocated per
calling thread even though `SERIALIZED` needs only one per PE. That way `MULTIPLE`
adds concurrency to them rather than multiplying them.

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
waits for *its* reply, not for a byte count. Up to `SERIALIZED` a PE has at most one
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
  local PE under §8.1's force-remote flag (§4.6). It also returns `NULL` for an
  address outside the symmetric heap and for a PE number out of range. RMA
  routines treat those two cases as fatal instead, naming the routine, because
  there a silent no-op would hide the bug.
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

A wait is always on the *caller's own* symmetric variables, so a writer that
stores to `(PE, offset)` only ever needs to look at that one PE's slot. Under
`SHMEM_THREAD_SERIALIZED` and below (§4.6) a PE is in at most one wait at a time, so one slot per
PE is enough: no hash table, no probing, and no slot deletion.

**A slot watches a byte range, not one offset.** An earlier draft said a PE waits on
one variable at a time. The multiple-element waits contradict that:
`shmem_wait_until_{all,any,some}` and their `_vector` forms (spec §9.11.2–§9.11.7)
block on an array `ivars[0..nelems)`. That array is one contiguous range, so the
slot publishes the range and a writer wakes on overlap. A write to an unwaited
element of the array wakes spuriously, and the loop re-checks.

**The range is one word: region, first grain, and a saturating grain count**
(`shmem/src/watch_range.h`), which is the same word §4.11 item 3 publishes per
thread under `MULTIPLE`. The grain is 4 bytes, the width of the narrowest AMO
type, so an ivar at its natural alignment is filtered exactly and no two
AMO-type objects share a grain unless they are adjacent elements of one array.
Zero means no watch. The writer's test is one load, two shifts and two compares,
so §4.9's "one load per applied operation" still holds. One word is published and
read atomically, so no writer can read a stale half of a range.

**The word names its region**, because `OSOE_REGION_INTERNAL` (§5.8) and statics
(§4.3 phase 1.5) both start at offset 0, as the heap does, and spec Example 27 —
the canonical put-with-signal example — waits on a `static uint64_t`. Without the
region a heap waiter at a low offset and a static at a similar offset collide
*systematically* rather than by chance. Only the heap has waiters today, so
`watch.c` rejects the others; removing that guard is all either needs, and not a
second layout version.

**Saturation is always permissive.** Above 16 MiB the count saturates to "to the
end of the region", and an offset the field cannot name saturates to the whole
region. Both over-wake and neither loses a wake. Encoding an over-long range as
*no watch* does lose one, which the model records as `satzero`. The offset field
addresses 4 TiB, and `shmem_init` refuses a larger `SHMEM_SYMMETRIC_SIZE` rather
than let the filter silently stop filtering.

*(Rejected: **the smallest aligned power-of-two block containing the range**,
encoded as `((offset >> k) << 6) | (k + 1)`. It is exact for a scalar ivar and
for an aligned power-of-two array, and badly wrong otherwise, because the block
must contain **both ends**: measured, an 8 KiB array wait publishes 122 KiB on
average and exceeds 64 KiB in 7.3% of real allocator placements, and under the
adjacent-write pattern of a halo exchange or a put-with-signal — a payload buffer
beside the flag — it wakes on 35.7% of writes at the mean and 90.3% at the 99th
percentile. An earlier draft of this section claimed the inflation was "at worst
twice its length", which is why nobody looked. It also spent all 64 bits, 58 on
the block and 6 on the shift, leaving no room for the region; and decoding a
shift count from a packed field produced two latent defects — a shift of
4294967295 in the saturating case, and a loop that does not terminate when a
range's ends differ in bit 63. The replacement has no variable shift and no loop.
`docs/reviews/watch-notification-options.md` has the measurements and the
alternatives, including why a 128-bit atomic word and a Bloom-filter signature
were rejected.)*

*(Rejected: a hash table keyed by offset, which the earlier draft's per-address
watch table needed. One slot per PE is enough while a PE is in one wait at a
time.)*

**Model-checked** (`docs/design/evidence/watch/`), on sequential consistency and
x86-TSO, for a scalar wait, an array wait, two writers, a range straddling a
boundary, and a PE that watches one range and then another. The same model finds
that a **two-word** publication — offset and length, stored and loaded separately —
is sound as well, because the writer updates the variable before it reads the
slot: if it reads the new offset, the update is already visible to the waiter's
next check. The single word is chosen for the atomic read and the cheaper test,
not for correctness. The model's elements are one grain apart deliberately: its
`endpoints` variant, a predicate covering a range's two ends and not its middle,
**passes** every scalar and two-element case and loses a wake only on a
three-element array. A model whose cases all fall inside one grain cannot tell a
predicate with a hole from a correct one, and an earlier version of this one
could not.

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

**Cost is a load or two per applied operation on the target side**, and only for
operations that write a heap: the target PE's watched range, compared with the
bytes just written.

**Exact for AMOs, best effort for puts** (§4.10). The writer side above assumes a
locked or `seq_cst` store. A put's `memcpy` is neither, so a put notifies straight
after the copy, and on x86 the wake can be lost. The safety-sweep deadline bounds
that loss. Spec 1.5 change #17 requires `ivar` to be updated with AMOs, so only a
non-conforming program sees it, or one using the deprecated `short` waits, which
have no AMO type. Serializing every put was rejected: it costs a store on every
intranode put for programs the spec does not require it for, and it would force an
RDMA backend to raise a CPU event on every NIC write.

**Under `MULTIPLE`**, several threads of one PE can wait at once. Each thread then
owns a watch record, and a per-PE bitmap tells a writer which records to check
(§4.11 item 3). The review's rules for a keyed list (`docs/reviews/evidence/semantics/`)
are not needed, because the records have no keys. The per-thread design was
model-checked instead, under x86-TSO as well
(`docs/design/evidence/multiple/watch_list_model.py`).

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
timeout-wakes.** A nonzero count in production means the watch slots have a bug,
or the program updates wait variables with puts. That counter is the only thing
that will ever tell you.

**`shmem_test` never blocks** — predicate check, no slot interaction at all.
Likewise the multiple-element tests. On an empty test set, `shmem_test_all` and
`shmem_test_all_vector` return 1 (1.6 change #19), `shmem_test_any` returns
`SIZE_MAX`, and `shmem_test_some` returns 0.

#### Waiting on many, and why it is left simple

One watch covers the whole array, so a write anywhere in it wakes the waiter,
which then re-evaluates the predicate over every element of the test set: **O(N)
per wake**. That is inherent, not an artifact of the encoding. The kernel can
tell a waiter which *word changed*; what the operation needs is which *predicate
became true*, and nothing but evaluating the predicate answers that — the gap
between the two is exactly the spurious wake §4.9 already tolerates.

**Watch the span of the *unmasked* elements, not the whole array.**
`wait_until_some` and its vector form take a `status` mask excluding elements
from the test set, and the wait already walks it, so taking the first and last
unmasked element costs nothing. Spec Example 44 masks each element as it
completes, so the watched span shrinks to the outstanding ones and the waiter
stops being woken by elements it has already finished with. This is the whole of
what is worth doing here.

**Two kernel mechanisms could narrow the re-check. Neither is taken.** Recorded
so the next reader does not have to rediscover that the channel exists, since
one of them is an argument we already pass:

- **The futex bitmask.** `futex_wait` and `futex_wake` each take a 32-bit mask
  and the kernel wakes a waiter only when the two intersect; `base/futex.c`
  passes `FUTEX_BITSET_MATCH_ANY` today. A waiter could set the bits of its
  unmasked elements and a writer the bit of what it wrote, and the kernel would
  drop the rest. It degrades safely in both directions, since `MATCH_ANY`
  intersects every narrow mask, so scalar and range waiters mix without either
  knowing; and a wrong mask costs a safety sweep, not a hang. What it does *not*
  buy is anything for the writer, which still pays the `futex_wake` syscall
  because the kernel filters after the call. Only the userspace watch word saves
  the writer, and that is already exact.
- **`futex_waitv` over K sequence words.** It waits on up to `FUTEX_WAITV_MAX`
  (128, verified on 6.12) words and **returns the index of the one that woke
  it** — literally "which item". With `bucket = written_grain % K` the writer
  bumps one word instead of one, at the cost of an AND, and the waiter rescans
  `N/K` elements rather than `N`. It needs no key and no element stride, only
  the grain arithmetic the writer already does. It costs K words per waiter and
  caps the narrowing at 128.

**Why neither is worth building.** The usage study found **no public application
users** of `wait_until_{all,any,some}[_vector]` or the corresponding tests
(`docs/reviews/evidence/q2/q2-usage.md`) — only unit tests and spec examples.
And the idiom the spec itself demonstrates is the *non-blocking* one: Example 44
spins on `shmem_test_some` with computation overlapped, and `shmem_test*`
touches no slot at all, so the path that idiom actually uses is already nothing
but loads and compares. The blocking forms are the weird ones, and they are the
ones with no callers. **Revisit when a real user appears, or when a measurement
shows the re-check dominating** — not before.

**Signedness is a parameter of the comparison, not a property of the word.**
Every value crosses the library's internal wait boundary as a `uint64_t`,
because that is what the boundary's `load` and `rmw` take, and Table 6 has
signed types: `SHMEM_CMP_LT` against 0 on an `int` holding −1 is true, and on a
`uint32_t` holding the same bits it is false. So a value is extended to 64 bits
according to its type's signedness and compared as an `int64_t` or a `uint64_t`
accordingly. This is the one place where the uniform `uint64_t` carriage that
makes the rest of the wait layer one piece of code would silently give the
wrong answer, and it is worth naming because the first implementation of
`wait_until` had only unsigned types and was therefore correct by accident.

**Spec constraints, both satisfied for free.** `ivar` must be updated using
OpenSHMEM AMOs (1.5 change #17), and §3.2 makes a wait or test on a location
exclusive with AMOs on that location of the same datatype. The progress thread
applies a remote AMO as an ordinary CPU atomic on the shared mapping (§4.1) —
the same instruction a local PE would issue — so both hold without further work.

**The signal routines are the same wait, on a `uint64_t`.** A signal data object
is a symmetric `uint64_t` (spec §9.8.1), every update to it is a boundary rmw,
and `shmem_signal_wait_until` is `wait_until` on it. Two spec requirements are
worth naming because they are satisfied by the structure rather than by code:

- §9.11.15 requires that `shmem_signal_wait_until` **not return before the update
  of the memory indicated by `sig_addr` is fully complete**, and §9.11.1 and
  §9.11.8–§9.11.14 say the same of every wait and test. Here the predicate's value
  comes from the boundary's `load`, which is exclusive against rmw on that word
  (§4.10), so a half-applied update is not a value a predicate can ever see. The
  same sentence is why the predicate may not read the variable directly: on a
  backend whose NIC applies remote atomics out of the CPU's sight, an ordinary
  load could return a value the update had not finished producing. This is the
  gap `load` was added to close.
- §9.11.15 also returns *the value that satisfied the condition*. The wait loop
  therefore keeps the value its last predicate check loaded rather than re-reading
  the variable after the loop: a re-read can see a later update, and would return
  a value that never satisfied the condition the caller asked about.

`shmem_signal_set` is a set-style update, so §3.16's two-sided rule applies to it
exactly as it does to a set-style AMO: a `seq_cst` store, not a `release` one.

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

### 4.10 The SHMEM↔transport boundary

> **The SHMEM layer reaches other PEs only through seven operations — put, get,
> read-modify-write, fence, wait, notify, and a coherent load of its own words —
> addressed as `(pe, region, offset)`. It never calls the AM layer.**

AM is the only network implementation, and §2.2 makes puts and gets AM handlers.
Nothing in §4 should know that. The core-API study (`docs/reviews/core-api.md`
§8.3) proposed naming the boundary in these terms so a future RDMA backend can
replace the handlers without touching this section. It costs one internal header,
`shmem/src/transport.h`, which states the full contract. This section records the
decisions in it. The core-API study named six operations; the seventh, `load`,
closes a gap found against it: without it, `shmem_test` and the `wait_until`
predicate would read the variable directly, which is incoherent on a network whose
NIC atomics the CPU cannot see.

**Addressing.** A world rank, a region that exists in every PE (the heap, the
library's internal region of §5.8, and statics with §4.3 phase 1.5), and an offset from that region's start in the
*target*. The SHMEM layer converts an address to an offset against its own base
and checks bounds. It also converts team-relative ranks to world ranks. Nothing
below the boundary sees an address (invariants 7 and 20).

**Dispatch is per `(pe, operation class)`, not per PE.** Each call chooses a
backend through the one locality function of §8.1, and a single context can use
both backends at once — but so can a single *PE*. A backend may carry one PE's
puts and gets while another carries that PE's rmw, and a NIC whose atomics are
not coherent with the CPU must do exactly that: a CPU atomic and a NIC atomic on
one word are not exclusive, while `rmw` promises exclusivity across the whole
atomicity domain, so on-node atomics go to the NIC by loopback while on-node put
and get stay `memcpy`. This is the general case rather than one fabric's quirk —
Portals, UET and UCX each carry a capability flag saying whether their NIC
atomics are CPU-coherent, which means all three expect the incoherent case.

Consequently `osoe_tp_direct` returning non-NULL says that *stores* through the
pointer are ordered as described; it does not say that rmw to that PE executes
locally. Nothing above the boundary changes, because the spec already makes a
plain store racing an AMO on the same word undefined (spec §3.2).
`osoe_tp_direct(pe, region, offset)` *is* that function. It returns the target's
address in this process only where the backend orders the calling thread's stores
through that address against its own writes by the next fence or wait (§4.6). osoe's
intranode backend does, because it applies writes in the calling thread. The AM
backend does not, so every PE it carries gets `NULL`, including a local PE under
force-remote. The rule is phrased as a property rather than "local or not" so that a
future direct path, such as CXL memory, is not excluded. `shmem_ptr`,
`shmem_team_ptr`, and `shmem_addr_accessible` consult it and nothing else.

**Binding.** Backends are chosen at build time. There is no table of function
pointers, and a backend's fast path may become `static inline` behind the same
names. The intranode floor (§8.4) is `memcpy` plus the synchronization the API
requires; an indirect call per put is not part of that.

**No capability queries above the boundary.** Every operation is mandatory for
every backend, and nothing above the boundary asks what a backend supports. Where
a network lacks something natively, the backend emulates it underneath:
- a bitwise rmw as a read-then-compare-and-swap loop;
- a blocking watch as a poll;
- a fence by draining;
- a visibility flush inside wait.

What a capability bit would otherwise have exposed becomes either a weaker
guarantee that all callers tolerate (best-effort notify for puts, spurious wakes)
or a resource limit the spec already lets fail (`shmem_ctx_create`). The cost is
that the SHMEM layer cannot choose a different algorithm per network. Build-time
binding gives backends that freedom instead.

*(Rejected: the core-API study's capability bits. Each one is a branch above the
boundary that is tested only on the network that sets it.)*

**Errors.** No operation returns one. A transport failure aborts the job (§3.15).
That is the right contract precisely because something outside the job owns
tearing it down — a resource manager on a cluster, the launcher otherwise — so a
boundary returning recoverable errors would be inventing a job it does not own.

**Every blocking call sweeps, on a bound.** A wait that cannot finish checks the
abort word and the lead's liveness at least every `OSOE_SWEEP_NS`, whether or not
the call takes a deadline. `osoe_tp_wait`, a blocking `osoe_tp_get` and any
future blocking call sweep on the same schedule as `osoe_tp_watch_wait`. Neither
blocks on the intranode backend, so nothing has forced the question yet; on the
AM backend `osoe_tp_wait` blocks on acknowledgements and on an RDMA backend it is
a completion-queue spin, and the rule is the difference between a hang and a
clean abort.

**A batching hint, `OSOE_TP_MORE`.** One flag on put and rmw saying more
operations follow on this context, so a backend may hold one back and submit it
with the next.

*What it defers on the AM backend is the wake, not the seal.* §3.9's aggregation
self-clocks off the send completion, so an operation on an idle connection is
sealed and submitted at once — batch of one — and nothing ever waits for a
buffer to fill. The only thing an application thread controls is whether it asks
the lead for a wake (§3.12), so `OSOE_TP_MORE` means "do not wake the lead yet";
the next operation without it, or any fence, wait or get, does. The source-bytes
rule below is satisfied unconditionally here, because reservation copies into
the connection buffer, so the flag is safe and its only saving is futex wakes on
a burst. **Ignoring it is correct and nearly free**: §3.10 requires the safety
sweep to submit anything sealed regardless of signalling, so the worst case is
one sweep interval of added latency, and in practice the next operation wakes
the lead anyway. Its first *acting* caller is a backend that posts work requests
referencing the caller's `src` rather than a copy — an RDMA one — which is the
case the rule was written for. Phase 1 implements it as a no-op on both
backends and says so. Purely a hint: it changes nothing about completion, ordering or
when `src` may be reused, and a backend that ignores it is correct — which is how
the intranode backend implements it. The rule that makes it safe is that **a
backend may defer an operation only if it has taken its own copy of the source
bytes**; put's local-completion contract is unconditional, so deferring a large
zero-copy put is wrong however the flag is set. Nothing sets it on one node,
where every put is a `memcpy`. It exists because an RDMA backend can post a list
of work requests under one doorbell instead of ringing one per put, which is
where the small-message rate is won, and because §3.9's aggregation wants to be
told when not to seal a buffer early. Its drivers are the 1.6 session routines
(§7.6) and the strided, block and collective loops.

**Nonblocking completion is counters, never per-operation handles.** A
nonblocking operation is a flag on the existing call, completed by the existing
`osoe_tp_wait`. The spec defines `_nbi` completion only at `quiet`, so there is
no per-operation wait call to serve; §4.6's `bytes_issued`/`bytes_landed` pair
already *is* this design; and the core-API study found counters sufficient in all
six production transports it surveyed, with per-operation handles an open
question nobody needed. Recorded before a benchmark applies pressure for them.

| Operation | Guarantee | Intranode backend | AM backend |
|---|---|---|---|
| put | Returns at local completion. Applied later; complete and visible once a covering wait returns. No ordering between puts to one PE except across a fence (spec §9.6.1.1). Notify best effort | `memcpy` into the target's mapping, applied before return, then notify | Reserve and copy into the node-pair buffer (§3.5); the dirty set records the node |
| get | Blocking. Complete per operation: `dst` holds the bytes on return. Never fenced | `memcpy` out of the target's mapping | Request; the reply lands in the landing zone; the calling thread copies it out (§3.12) |
| rmw | One 4- or 8-byte word. Exactly once. Exclusive against same-width rmw on the word within one atomicity domain, and against waiters and tests (spec §3.2). Fetching: complete on return. Non-fetching: as a put. Notify exact | One C11 atomic on the shared mapping (§3.16 orders), then notify | Handler-time CPU atomic at the target; a fetched value is a value descriptor (§3.8) |
| fence | Writes issued on the context before the call are placed at each target before later ones (spec §9.12.1). Unless the context is `NOSTORE`, the calling thread's stores become visible. Does not wait, and need not send anything | Serializing store only; every write is already placed | Serializing store, then seal and flush the dirty set with ack requested (§4.6) |
| wait | Every write and nonblocking get on the context — to listed PEs, or to all — is complete and visible, and unless the context is `NOSTORE` so are the calling thread's stores (spec §9.12.2, §9.12.3). May over-wait | Serializing store only; nothing is ever outstanding | The §4.6 `shmem_quiet`, restricted to the listed PEs' nodes |
| load | A coherent load of a word of the caller's own region, 2, 4 or 8 bytes. Exclusive against rmw on it (spec §3.2). For `shmem_test*`, the `wait_until` predicate, and `shmem_signal_fetch` | One `acquire` load of the word | The same: every writer is a CPU atomic on the shared mapping |
| notify | Wakes a PE's waiters whose watched range a write overlaps (§4.9). Exact for rmw, best effort for puts; wakes may be spurious | §4.9 watch slot | The same function, called by the progress thread after it applies a write |

**Ordering is the weak contract, with a per-operation fence flag.** The boundary
promises only what the spec promises: writes to one PE are ordered across a
fence. An operation may also carry `OSOE_TP_FENCED`, which places it after every
earlier write on the same `(context, PE)`. `put_signal` is a put followed by an
rmw with that flag, which is how invariant 5 survives a backend with no stream
order. Both osoe backends place writes in issue order already: intranode because
writes apply in the calling thread, and AM because of §3.1. So the flag costs
them nothing. Stating the weaker contract is what keeps an RDMA backend possible.

*(Rejected: requiring every backend to place writes in issue order. It is true
of both osoe backends. But it would make order a hidden property of the boundary,
and an RDMA backend could meet it only by serializing everything to a peer. The
core-API study also rejects ordered-by-default delivery, because it forfeits
packet spraying across paths.)*

**Notify belongs to the backends.** The SHMEM layer uses the waiter half: watch,
load the sequence word, check, wait. A backend calls the writer half after it
applies any write, whether in the calling thread or on the progress thread
applying a remote operation. A backend whose writes the CPU never sees, such as
RDMA, would bridge its NIC events into that call.

**Notify is exact for rmw and best effort for puts.** The §4.9 protocol is
two-sided (§3.16), and a `memcpy` does not make the writer's side sound. Making it
sound would take a serializing store on every put, which spec 1.5 change #17 does
not require (§4.9). A backend whose puts the CPU never sees need not notify for them.

**Atomicity is the spec's, not osoe's.** osoe gets exclusivity across atomicity
domains for free, because every path is the same CPU atomic on the same mapping
(§4.1). The boundary does not promise it. A backend whose NIC atomics are not
coherent with the CPU can then run one domain on the NIC and another on the CPU,
instead of pushing every on-node rmw through NIC loopback.

**Outside the boundary:**

- **Collectives (§5).** The intranode phase runs on node-segment counters, which
  are the same under any backend. The internode phase is AM traffic between
  leads. A future backend implements it either natively or generically over this
  boundary, using `OSOE_REGION_INTERNAL` for scratch and control words (§5.8). The
  core-API study also keeps collectives above its core.
- **Allocation (§4.4)** is a pure function of the call sequence and makes one
  transport call: `set_range(region, offset, len, attributes)` tells a backend
  which ranges carry atomics or signals, called for each hinted allocation. That
  hook is how an RDMA backend avoids registering the whole heap for NIC atomics
  or counters (on CXI, a counter on a region disables its optimized RMA). The
  allocator therefore keeps hinted allocations (`SHMEM_MALLOC_ATOMICS_REMOTE`,
  `SHMEM_MALLOC_SIGNAL_REMOTE`, spec §9.3.5) able to grow from the opposite end of
  the heap from unhinted ones. That is still deterministic.

  Three properties it must have, each easy to lose in implementation. It is
  **not a collective and must not block**: every PE calls it with identical
  arguments in the same order as a *consequence* of the allocator's determinism
  (§4.4, invariant 6), not by coordination, and a backend that put a barrier in
  it would be synchronizing for no reason. It is **advisory in both directions**
  — a backend may ignore it entirely, an operation outside a declared range is
  still correct, and nothing above the boundary asks what a backend did with it,
  which is what keeps it from becoming a capability query. And repeated or
  overlapping calls take the **union, never a replacement**, because `realloc`
  preserves an allocation's hints and so must survive a grown or moved block.

  It is the largest recoverable loss for an RDMA backend: without it, a backend
  cannot know which words a PE may wait on, so it cannot afford exact
  notification — it would have to add a second work request to *every* atomic —
  and every blocked wait degenerates to polling, a core per node at best.

  **It lands at §7.1 step 2**, with the allocator call site it needs. An earlier
  draft scheduled it for step 3, with the first backend that can act on the
  answer, and that is still where the first backend that *uses* it arrives — but
  the call site is in the allocator, not in a backend, and step 2 is the step
  that writes the code around it. Landing it later would mean revisiting
  `shmem_malloc_with_hints` and `shmem_realloc` once the structures near them had
  hardened. The intranode backend ignores it, which is the contract: advisory in
  both directions, so a backend that does nothing with it is correct.
- **Nonblocking gets and contexts beyond the default.** They add calls to the
  header when they arrive (§7.3). They do not change the operations above.

### 4.11 `SHMEM_THREAD_MULTIPLE`

> **Designed and model-checked, not scheduled.** Phase 1 still provides up to
> `SERIALIZED` (§4.6). Under `MULTIPLE`, a context that promises serialization
> (`SHMEM_CTX_PRIVATE` or `SHMEM_CTX_SERIALIZED`) keeps the §4.6 structures unchanged.
> A shareable context, `SHMEM_CTX_DEFAULT` included, keeps a lock-free issue path and
> serializes its quiets with a per-context mutex. Under that mutex, a generation word
> bounds get completion and dirty bits are cleared. Each application thread owns
> one thread block, which holds its watch record and its completion slot.

The usage study is why this is worth designing now: Kokkos and hclib call
`shmem_init_thread`, and the threaded GUPs and Graph500 variants issue their hot
operations on one context per thread (§8.6 U6;
`docs/reviews/evidence/q2/q2-usage.md` §1, §4).

**Evidence.** Three exhaustive models and their engine are in
`docs/design/evidence/multiple/`, in the style of the review's models, with ghost
state that states each guarantee. Each model checks the design and deliberately
broken variants. Every broken variant fails with a trace, and every design case
passes. `selftest.py` reruns small cases without the engine's state-space
reductions and gets the same verdicts. Run any model as
`python3 <model>.py [case substring]`; the `.out` files are the recorded runs.

| Model | Question | Largest design case |
|---|---|---|
| `get_completion_model.py` | Does every quiet complete the nonblocking gets issued before it was called? | 2 issuers × 2 gets, 2 quiets: 18.9 M states |
| `dirty_set_model.py` | Does every quiet wait for the puts issued before it was called? | 2 issuers × 2 puts, 2 quiets: 4.2 M states |
| `watch_list_model.py` | Is a wake lost when several threads of one PE wait at once, under SC and x86-TSO? | 3 records, 2 writers: 0.9 M states |

#### What does not change

- **The transport under the SHMEM layer.** Reservation (§3.5), reply credits
  (§3.8), acknowledgements (§3.4), and every futex word of §3.11 already take
  concurrent writers, because PEs are processes and they already share them.
- **The intranode backend's data path.** Operations apply in the calling thread,
  and fence and wait are a serializing store. Only notify changes (item 3).
- **Memory stores.** Fence and quiet order only the calling thread's stores (spec
  §9.12.1, §9.12.2 Notes), so the per-thread serializing store of §4.6 is already
  right.
- **Collectives and allocation.** A team may not be used concurrently for
  collectives by threads of one PE (spec §9.4, Thread Safety), and allocation is a
  world-team collective whose order the program fixes (spec §9.2 rule 5). Per-team
  state is already separate (§5.4).
- **Init and finalize.** The count is already atomic behind a once-gate (§6.4).

What does change is the §4.6 argument that "library state handed between threads"
is made visible by the application's own serialization. On a shareable context
under `MULTIPLE` there is no such serialization. Every word two threads can touch
at once becomes a §3.16 protocol word.

#### Context modes

Each context gets a mode at creation, and the mode never changes:

| Thread level | Context options | Mode |
|---|---|---|
| Below `MULTIPLE` | any | serial |
| `MULTIPLE` | `PRIVATE` or `SERIALIZED` | serial |
| `MULTIPLE` | neither, including `SHMEM_CTX_DEFAULT` | shared |

**Serial mode is the §4.6 code, unchanged.** The spec makes the application
serialize every operation on a `SERIALIZED` context, quiet included. So no issue
overlaps a quiet on that context and the equality wait cannot starve. The progress
thread and agent touch only landed counters and acknowledgements, which are atomics
already. `PRIVATE` adds one thing: the context caches its creating thread's block
(below), so a blocking operation skips the thread-local lookup. `NOSTORE` skips the
serializing store in either mode.

Operations branch once on the mode. Programs that never request `MULTIPLE` run only
serial mode, so phase-1 costs do not move.

| Per operation | Serial | Shared |
|---|---|---|
| Put, non-fetching rmw (AM) | Reservation; plain stores for `high_seg` and the dirty bit | Reservation; one load each of `high_seg` and the bit word. A CAS only when the segment number rises, and a `fetch_or` only when the bit is clear: about once per segment per node |
| Nonblocking get | One add to `issued` | Load `gen`, locked add, load `gen` again |
| Blocking get, fetching rmw | Per-thread slot (item 5) | The same |
| Quiet | §4.6 | Lock, bump `gen`, wait, clear under the lock, unlock |

#### 1. Get completion

**The problem.** §4.6 waits for `bytes_landed == bytes_issued`. With another thread
issuing nonblocking gets on the same context, equality may never hold.

**The review's fix is unsound.** Review D6 item 2 proposed snapshotting `issued` at
entry and waiting for `landed >= snapshot`. Landed bytes are fungible: a small get
issued *after* the call lands first and covers the bytes of an earlier get that has
not landed. The model finds this in 412 states: a 1-byte get returns, quiet
snapshots 1, a 2-byte get lands one byte, and quiet returns with the first get
incomplete.

**Decision: a generation word, two counter pairs, and a quiet mutex per shareable
context.** All of them live in the context's block in the node segment, because
the progress thread and the agents credit landed bytes from other threads and other
processes.

```c
/* issue a nonblocking get (or fetching nonblocking rmw) of n bytes */
for (;;) {
    uint32_t g = atomic_load_explicit(&ctx->gen, memory_order_acquire);
    atomic_fetch_add_explicit(&ctx->issued[g & 1], n, memory_order_seq_cst);
    if (atomic_load_explicit(&ctx->gen, memory_order_acquire) == g) {
        req.pair = g & 1;                  /* the reply credits landed[g & 1] */
        break;
    }
    atomic_fetch_sub_explicit(&ctx->issued[g & 1], n, memory_order_seq_cst);
    wake_bump_and_wake(&ctx->wake);        /* a waiting quiet may now see equality */
}

/* landing: the copy-out actor, after copying into dest */
atomic_fetch_add_explicit(&ctx->landed[pair], n, memory_order_seq_cst);
wake_bump_and_wake(&ctx->wake);

void quiet_shared(struct ctx *ctx) {
    serializing_store();                                   /* §4.6 memory stores */
    uint32_t e = atomic_load_explicit(&ctx->gen, memory_order_acquire);
    mutex_lock(&ctx->qlock);
    uint32_t g = atomic_load_explicit(&ctx->gen, memory_order_acquire);
    if (g == e) {                             /* else piggyback: see below */
        atomic_store_explicit(&ctx->gen, g + 1, memory_order_seq_cst);
        wait_until(landed[g & 1] == issued[g & 1]);        /* §4.6 wake-word pattern */
        flush_and_clear_dirty(ctx);                        /* item 2 */
        copy_out_completed_gets(ctx);
    }
    mutex_unlock(&ctx->qlock);                             /* wakes every waiter */
}
```

**Why it holds.** The issuer's add and re-load, against the quiet's bump and later
loads of `issued`, is a two-sided protocol (§3.16), and every store in it is locked
or `seq_cst`. If the issuer's re-load sees the old generation, the quiet's loads
see the add. If the re-load sees the new generation, the issuer takes its add back
and retries in the other pair, and its reply never lands in the closed pair. So
the closed pair counts exactly the gets issued before the bump, plus older
generations, which earlier holders of the mutex already completed. Gets issued
after the bump go to the other pair, and the wait for the closed pair ends once
those earlier gets have landed. Two pairs suffice because the mutex lets only one
quiet close a generation at a time.

**Piggyback.** A get that returned before this quiet was called is tagged with a
generation no later than `e`. If the generation has moved past `e` by the time the
lock is taken, another quiet bumped it after this call began and completed while
holding the lock. So every such get is complete, and this quiet returns at once.
Concurrent quiets on one context therefore collapse into one wait, and the mutex's
release wakes every waiter, the "divisible" case of §3.11. The model passes with and
without piggyback.

**Found by the models, not assumed:**
- **Without the retag** (`noretag`), an issuer whose add lands after a quiet's
  loads is missed by the *next* quiet, which closes the other pair.
- **Without the mutex** (`nolock`), a second quiet closes the other pair while the
  first still waits, and returns before a get the first was waiting for has landed.
- **The order of the two loads does not matter** (`order` passes). Once late adds
  are taken back, no byte of a later get can reach the closed pair. §4.6's serial
  path therefore needs no load-order rule either.

**Starvation, measured.** One quiet, two issuers each issuing N one-byte gets. The
table gives, over all interleavings, the most gets the quiet waited for that began
issuing after its wait began. For the design, the wait begins at the generation
bump. For equality, which has no bump, it begins at the call:

| N | 1 | 2 | 3 |
|---|---|---|---|
| Equality (§4.6) | 2 | 4 | 6 |
| This design | 0 | 0 | 0 |

Equality can be made to wait for every get the other threads issue, however many
there are. The design never waits for a get begun after its bump. The adversary
can still delay the quiet before the bump, but those gets are then genuinely
earlier than its wait. N = 4 was not run: equality took 1.7 M states, and the design,
growing about twenty-fold per step, would need a few hundred million.

**Lock-free, not wait-free.** An issuer retries only when a quiet bumps the
generation between its two loads. Each bump costs a quiet a network round trip, so
retries are rare. A retry counter goes in the thread's counter block (§3.14).

*(Rejected:*
- *The review's `landed >= snapshot`: unsound, above.*
- *Per-thread counters. A quiet must complete every thread's earlier gets on the
  context, so it would sum every thread block, O(threads) per quiet. Each thread's
  pair has the same fungibility problem as soon as that thread issues concurrently.*
- *Blocking issuers while a quiet runs. It cannot starve, but a quiet on the default
  context would stall every thread's `get_nbi` for a network round trip, which
  defeats U6.*
- *Per-operation records scanned by quiet against a sequence snapshot. It is exact,
  but every quiet costs O(outstanding gets), and codes that quiet after every
  operation (U1, U3) pay it each time.*
- *Equality on shared contexts: starves, above.)*

#### 2. Dirty set

**The problem.** §4.6 clears the dirty set (`ndirty = 0`) after waiting. A put
issued by another thread between the wait and the clear loses its entry, and the
next quiet returns before that put is applied.

**Decision.** A shareable context keeps a bitmap over node indices (⌈nodes / 8⌉
bytes) and a `high_seg` word per node (8 bytes per node).

- **Issue**, after reserving and reading the segment number `s` (§3.5): raise
  `high_seg[n]` to at least `s` with a load-compare-CAS loop, *then* set bit `n` if
  it is clear. Every operation in one segment reads the same `s`, so the CAS and the
  `fetch_or` happen about once per segment, not once per operation.
- **Quiet**, under the item 1 mutex and after the generation bump: for each set bit,
  load `high_seg[n]`, flush with ack requested, and wait for `acked[n] >= high_seg`.
  Clear the bit, re-load `high_seg[n]` and `acked[n]`, and if the node is still
  behind, set the bit again and repeat.
- **Fence** reads the bits and flushes, and never clears, so it takes no lock.
- **`shmem_ctx_pe_quiet`** clears only its listed nodes' bits, under the same mutex.
  It still bumps the generation for gets, since it waits on every get (§4.6). It
  must never satisfy a full quiet's piggyback, so the piggyback test in item 1 reads
  a second word, the generation closed by the last *full* quiet. This rule is not
  in the models (§10).

**Found by the models:**
- **Clears without the mutex** (`unlocked`). Between one quiet's clear and its
  re-set, a second quiet reads the bit as clear and returns early. This is why the
  quiet mutex exists for puts as well as gets.
- **Clearing without the re-check** (`naive`, §4.6 as written) loses a put.
- **Setting the bit before raising `high_seg`** (`bitfirst`). A quiet sees the bit
  and an old `high_seg`, clears the bit, and the raise lands after its re-check.
- **Raising `high_seg` with load-compare-store** (`plainmax`). A lower segment
  number overwrites a higher one.

*(Rejected:*
- *A per-node "done" watermark, cleared by CAS without a lock. It needs no mutex,
  but finding dirty nodes then means either scanning every node the context ever
  touched, or a summary bitmap whose clears reopen the same window.*
- *Per-thread dirty sets. A quiet would take the union of every thread's set, and
  a thread that exits leaves entries nobody clears.)*

#### 3. Watch records for concurrent waiters

**Decision: one watch record per application thread, never keyed.** A record
lives in the thread's block in the node segment, and holds:
- `rng`, one 64-bit word packing region, offset, and a saturating length, with 0
  meaning none. A saturated length means "to the end of the region", which can
  cause only spurious wakes;
- `seq`, the 32-bit futex word of §4.9.

Each PE also has an `active` bitmap, ⌈T/64⌉ words, with a bit for each record that
holds a published range.

```c
/* waiter: thread record r of PE me */
atomic_store_explicit(&rec->rng, pack(region, offset, len), memory_order_seq_cst);
atomic_fetch_or_explicit(&node->active[me][r / 64], bit(r), memory_order_seq_cst);
for (;;) {                                            /* the §4.9 loop, unchanged */
    uint32_t seq = atomic_load_explicit(&rec->seq, memory_order_acquire);
    if (predicate(osoe_tp_load(...))) break;
    futex_wait(&rec->seq, seq, deadline);
}
atomic_fetch_and_explicit(&node->active[me][r / 64], ~bit(r), memory_order_seq_cst);
atomic_store_explicit(&rec->rng, 0, memory_order_seq_cst);

/* writer, after a locked or seq_cst update of bytes [o, o + n) of PE pe */
for (each word w of node->active[pe], loaded with acquire)
    for (each set bit r in w)
        if (overlaps(atomic_load_explicit(&record(pe, r)->rng, memory_order_acquire), o, n)) {
            atomic_fetch_add_explicit(&record(pe, r)->seq, 1, memory_order_seq_cst);
            futex_wake(&record(pe, r)->seq, INT_MAX);
        }                                             /* every match, not the first */
```

**Adopt or re-check the review's rules?** Neither rule is needed, and the model was
re-run. The review's rules (V8fix, a CAS-joined reference count with a teardown
marker; V9fix, bump every matching slot) repair a *keyed* table, where waiters on
one address share a slot. A record found by thread index has no key, no sharing,
no join and no probe chain, so V8 and V9 cannot occur. What remained unchecked is
new to this design: the bitmap as the writer's filter, overlapping byte ranges, an
array wait, a thread reusing its record for a second wait, and x86-TSO. The model
covers each of these, and finds:
- **A writer must bump every overlapping record** (`firstmatch` loses a wake): two
  threads waiting on one variable is ordinary.
- **Publish before the first check** (`checkfirst`) and **update before loading
  `active`** (`notifyfirst`): the §4.9 rules, unchanged.
- **Setting the bit with a plain load and store** loses a wake under TSO
  (`plainbit`), as §3.16 predicts.
- **Publishing `rng` with a plain store is safe** (`plainrng` passes), because the
  locked `fetch_or` that follows drains the store buffer. It stays `seq_cst`
  anyway, so the argument does not depend on the next line.
- **A two-word range is also safe** (`tworange` passes), even while a thread reuses
  its record. One word is chosen because it is one store fewer, not for
  correctness.
- **A put's plain `memcpy` loses a wake under TSO** (`plainput`), as §4.9 and §4.10
  already accept: best effort for puts, bounded by the safety-sweep deadline.

§4.9 publishes this same packed word, so phase 1 and `MULTIPLE` share one
encoding rather than two: the phase-1 slot is this record with at most one bit
set, and `MULTIPLE` adds the `active` bitmap and the scan over it. (An earlier
draft of this paragraph said §4.9 publishes the range in two words. It does not,
and the two sections specified different encodings for a while.)

**Cost.** When nobody on the PE waits, a writer loads the `active` word, which is
the same single load as phase 1's `watched` word. With `k` threads waiting, it adds
one load and a compare per waiting thread. The phase-1 slot of §4.9 is this record
with at most one bit set.

*(Rejected:*
- *A keyed table with the review's reference-count and probe rules. Correct, but
  every blocked thread needs its own futex word anyway, so sharing slots saves
  nothing and adds the V8 and V9 rules.*
- *One sequence word per PE, waking all of the PE's waiters: the §3.11 thundering
  herd, among one PE's threads.*
- *`futex_waitv` over several words. One word per waiter is enough.)*

#### 4. Shareable-context state: lock-free issue, locked quiet

**Decision.** Issue paths on a shareable context are lock-free, as in items 1 and 2.
Quiet, `pe_quiet`, and `shmem_ctx_destroy`'s implicit quiet take a per-context
futex mutex. Context blocks come from a per-PE pool in the node segment, claimed and
released by CAS. `shmem_ctx_create` fails, as spec §9.5.1 allows, when the pool is
empty.

- **Issue is the hot path** (U1, U3, U6). On an oversubscribed Pi, a lock holder
  preempted mid-operation stalls every thread on the context. §3.11 rules out
  spinning before blocking, so every contended acquisition would be a syscall. The
  transport below is already lock-free (§3.5), so a lock above it adds contention
  and protects nothing.
- **Quiet already blocks** on a network round trip. Two locked instructions for the
  mutex cost nothing by comparison. Serializing quiets is what makes the two-pair
  generation and the bit clear sound (the `nolock` and `unlocked` variants), and
  piggyback turns concurrent quiets into one wait.
- **The fast paths are serial mode.** `PRIVATE` and `SERIALIZED` contexts pay none
  of the shared-mode costs. `PRIVATE` also skips the thread-block lookup. Codes with
  one context per thread (U6) therefore run the phase-1 code on every thread.

*(Rejected:*
- *A mutex around every operation on a shareable context. It costs two locked
  instructions per operation, and a descheduled holder convoys every other thread.*
- *A lock-free quiet. Both models show quiets racing each other. Repairing that
  without a lock needs a second generation-style protocol for bit clears, which
  costs more than the mutex it replaces.)*

#### 5. Thread blocks and completion slots

**One completion slot per thread.** A blocking get or fetching rmw blocks its
thread until the reply lands, so a thread holds at most one outstanding. The slot
sits in the thread's block, holding a state word, a nonce, the fetched value, and a
32-bit wake word. The request carries `(pe, thread index, nonce)`, and the progress
thread writes the result, bumps the wake word, and wakes. This is §4.6's
wake-word pattern with one waiter and one writer, the shape V0 and V5 of the
review's model already check. The nonce cannot mismatch, since an operation is
never cancelled (§3.15); it is asserted.

**Thread blocks.** The same block also holds the thread's watch record (item 3) and
its §3.14 counter block and "blocked in" record.
- A thread claims a block by CAS on its PE's table at its first library call, and
  caches the index in a `_Thread_local`, as `base/order.c` already does for the
  serializing store.
- A `pthread_key_create` destructor releases the block at thread exit. A thread
  cannot exit inside a call, so the block's bit and slot are idle then.
- `PRIVATE` contexts cache the creator's block.

**How many.** `T` blocks per PE, read from `OSOE_MAX_THREADS` at the first init, as
`max_teams` is (§5.7). The default is the node's online CPU count. At 128 bytes per
block, 256 PEs × 64 blocks is 2 MiB of node segment. What happens when a PE's
table is full is open (§10).

*(Rejected:*
- *One slot per PE behind a mutex: a 1 MB get would hold up every other thread's
  8-byte get.*
- *Slots from a per-operation pool: a claim on every operation, and exhaustion
  couples unrelated threads through a second limit.)*

#### 6. The transport boundary under `MULTIPLE`

The operations of §4.10 do not change. The contract gains concurrency rules, and
`transport.h`'s "Threads" paragraph becomes, when `MULTIPLE` is implemented:

```c
 * Threads. Under SHMEM_THREAD_MULTIPLE every operation may be called by any
 * number of threads at once, on the same context or different ones, except
 * where the context's mode forbids it: a context created with
 * SHMEM_CTX_PRIVATE or SHMEM_CTX_SERIALIZED, or any context below
 * SHMEM_THREAD_MULTIPLE, is serial, and the caller guarantees one call on it at
 * a time (§4.11). An operation takes effect before it returns. A fence or wait
 * covers every operation on its context whose call returned before the fence
 * or wait was called; operations that overlap it may or may not be covered.
 * OSOE_TP_FENCED orders an operation after every write on the same (ctx, pe)
 * whose call returned before this one began. A thread holds at most one watch.
 * osoe_tp_notify may run in any number of threads and processes at once.
```

Two declarations follow from it:
- `osoe_tp_ctx_create(options)`, which may fail and records the mode;
- a per-thread handle behind the watch and blocking calls, obtained by
  `osoe_tp_watch` and `osoe_tp_get` themselves, so callers do not change.

"Returned before it was called" is exactly spec §9.12.2's "posted prior to calling
shmem_quiet". The models check it in that form.

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
  copy out a partial result. For a team that spans a single node, the release
  happens locally, with no internode message. In a data-carrying collective the lead
  bumps `released` after its combine. In barrier and sync, whichever member's
  arrival reaches the target bumps it (§5.3). That member knows it is the last
  arrival from its `fetch_add` result, so no member waits on a designated one.
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

Each needs its own design before it is implemented (§7). Scans have one (§5.8);
alltoall and collect do not yet.

For the collectives it covers, this still eliminates scratch sizing — no chunking
loop, no high-water allocation, no per-team data area. Their shared footprint is
**three counters on three cache lines**, regardless of reduction size.

*(Fallback for cases where `dest` cannot serve: chunk through fixed scratch. The
loop costs nothing on small reductions and adds pipelining on large ones. §5.8 turns
this into the large-reduction design.)*

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
the team handle argument team" without naming a scope in the routine text, but the rest of the spec fixes
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

### 5.8 Algorithms beyond the accumulator

§5.5's accumulator is right for barrier, sync, small reductions, and small
broadcasts, which the usage study shows dominate real codes: two barriers around
every collective in bale, and one-element reductions per iteration (§8.6 U7).
Two cases need more:
- **large reductions**, where the accumulator's internode tree wastes bandwidth;
- **prefix scans**, which it cannot express at all.

This section records the decisions that must hold when those arrive. Nothing here
uses an OpenSHMEM extension.

#### What the surface provides, and what it lacks

osoe's collective machinery is:
- counters in the node segment (§5.4);
- a local lead that writes its own `dest` (§5.5);
- AM messages between node leads, carrying values or references (§3.8);
- handlers that forward down a tree without waking an application thread.

That is enough to express fast algorithms for every collective below.

The transport boundary (§4.10) is not enough on its own. It lacks a region the
library owns, so a generic implementation has nowhere to land a chunk it has not
yet combined. **`OSOE_REGION_INTERNAL`** closes that gap. It is a per-PE region
for library scratch and collective control words, addressed `(pe, region, offset)`
like the heap. With it, a generic collective can run on any backend over put,
fenced rmw, and watch, while osoe's AM backend keeps its handlers. Adding the
region is additive and costs nothing until a collective uses it.

*(Deferred: a separate internal collective-transport header. Both implementations
would sit behind the same collective routines and be chosen at build time, so the
header can wait until reductions land and show what it must contain.)*

#### Large reductions

The accumulator's internode phase sends each node's whole vector up a tree and the
result back down: about twice the bandwidth of a ring (reduce-scatter then
allgather) or of Rabenseifner's algorithm at large element counts. The rules for
replacing it:

1. **Chunks land in scratch, never in `dest`.** A ring step receives a chunk and
   combines it into a `dest` chunk that still holds live data, and with
   `source == dest` that data is also the source. The receiving lead needs space
   that is not `dest`: `OSOE_REGION_INTERNAL`, sized to one chunk per step, which
   bounds it independently of the reduction.
2. **The lead's application thread combines, never the progress thread.** The lead
   is blocked in the call regardless. §5.3 keeps O(count) work off the progress
   thread, because every connection waits behind it. Invariant 16 keeps a second
   writer out of `dest`, and the progress thread would be one. Handlers only land
   chunks and signal their arrival.
3. **Every PE ends with bit-identical results.** Floating-point `SUM` and `PROD`
   are not associative. Each element's final value is computed once and copied to
   every PE, never recomputed along different paths. A ring's allgather phase and
   the accumulator both satisfy this. A scheme where PEs combine the same inputs in
   different orders does not, and it gives PEs results that differ in the last bit.
4. **The algorithm is chosen from element count, team node count, and local member
   count**, above the boundary. That is not a capability query (§4.10). The
   crossover sizes come from measurement (§10).

The intranode phase is unchanged: the lead combines local members' sources into
its `dest` after they arrive (§5.5). The internode algorithm then runs between
leads, and release and copy-out follow the §5.4 counters as before.

#### Prefix scans

`shmem_sum_inscan` and `shmem_sum_exscan` (spec §9.10.10) give PE *i*, in team rank
order, the sum of sources 0..*i* (inclusive) or 0..*i*−1 (exclusive). Every PE's
result differs, so there is no accumulator. The spec allows reading a PE's `source`
once that PE has called, so the design pushes as §5.5 requires.

- **Runs.** At team creation, split the team's rank order into maximal runs of
  consecutive team ranks on one node. A contiguous layout gives one run per node.
  Slurm's cyclic distribution (§6.1) can give one run per PE. Team resources (§5.7)
  record the runs.
- **Within a run**, the node's lead computes the run's local prefix from members'
  sources once they have arrived.
- **Across runs**, a parallel prefix (up-sweep and down-sweep) over the run
  sequence gives each run the offset vector of everything before it: O(log runs)
  rounds of `nelems` elements each. Every round's messages between runs on the same
  node pair share that pair's stream and aggregate (§3.9). A contiguous layout
  therefore costs O(log nodes) rounds; the cyclic worst case costs O(log PEs).
- **Each member writes its own `dest`**: the run offset plus its local prefix
  (exclusive: minus its own source). No PE writes another's `dest`, so invariant 16
  does not arise.
- **Floating point.** The spec defines the scan mathematically. The rounding of a
  floating-point result depends on the combination order, and the order depends on
  the run layout, so results can differ between two layouts of the same job. That
  is conforming. Document it, because a user comparing two runs will notice.

*(Rejected: a sequential chain through the ranks — O(PEs) rounds, and a dead PE
stalls everything behind it. Rejected: a rank-level parallel prefix even when runs
are long, which multiplies the rounds by the local member count for nothing.)*

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
`/osoe.<job>.<step>.<node>.<session>`, so concurrent steps on one node cannot
collide. The session is the number of full `shmem_init` sessions this process has
started, counting from 0 (§6.4). Every
local PE opens it by name, records its rank, pid, and heap object name in its slot,
and maps its siblings' heaps as they appear (§4.1). Heaps are named objects too,
opened the same way: no sockets and no descriptor passing between local PEs. The
lead waits on a futex until all `L` local PEs have arrived, then removes every heap
name, so a later crash leaves no heaps behind in `/dev/shm`.

The sequence, as implemented:
1. **The lead creates the control segment** with `O_EXCL`, mode 0600. A name that
   already exists is a crashed job's leftover and is reported, never reused. The
   lead writes the versioned header, maps its own heap (choosing `B`, §4.1), and
   publishes `ready`.
2. **Every other PE opens the segment by name.** It polls with backoff until the
   name exists, since there is nothing to wait on before it does. It then waits on
   `ready`, checks the magic number, layout version, and job description, and maps
   its own heap `/…<session>.heap.<local id>` at `B`.
3. **Each PE writes its own slot** (rank, pid, parsed `SHMEM_SYMMETRIC_SIZE`, heap
   name and length), publishes `phase = JOINED`, and bumps `joined`.
4. **Once `joined == L`**, every PE checks that all slots agree on the size and
   that slot *i* holds `OSOE_GTIDS[i]`. Every PE runs the same check on the same
   data, so a mismatch fails all of them with the same message. Each PE then maps
   its siblings' heaps and bumps `mapped`.
5. **Once `mapped == L`**, the lead removes every heap name.

Any PE that fails stores a `failed` word and wakes the rendezvous words, so its
siblings stop within one safety sweep instead of waiting out
`OSOE_BOOTSTRAP_TIMEOUT` (60 s by default). A timeout names the local ids that
never arrived.

**Removing the control segment's name.** At the last finalize each PE bumps
`finalized`. Exactly one increment reaches `L`, and that PE removes the name. The
others' mappings stay valid. The session number is what makes this safe: without
it, a PE that had finished and immediately re-initialized could recreate the name,
and a slower sibling would then remove the new segment. The control segment
**keeps its name until the last finalize**, so `osoe-stat` can open it while the
job runs (§3.14). A crash therefore leaves the control segment — a few MB, and the
post-mortem — for `osoe-stat --gc` or a Slurm epilog to remove; a crash during
init can also leave heap names. Where hugetlbfs is mounted and has pages, heaps use it under the same
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
the `OSOE_` prefix, which take precedence. The `SHMEM_` prefix is the
specification's, not ours. The tier-1 and tier-2 harnesses of §8 use this path, so
the bootstrap runs in every test rather than only on a cluster. The names mirror
§6.1 exactly:

| `OSOE_` variable | Mirrors | Read by phase 1 step 1 |
|---|---|---|
| `OSOE_PROCID` | `SLURM_PROCID` | Yes |
| `OSOE_STEP_NUM_TASKS` | `SLURM_STEP_NUM_TASKS` | Yes |
| `OSOE_NODEID` | `SLURM_NODEID` | Yes |
| `OSOE_LOCALID` | `SLURM_LOCALID` | Yes |
| `OSOE_GTIDS` | `SLURM_GTIDS` | Yes |
| `OSOE_STEP_NODELIST` | `SLURM_STEP_NODELIST` | No: one node needs no host list |
| `OSOE_JOB_ID` | `SLURM_JOB_ID` | Yes, in segment names |
| `OSOE_STEP_ID` | `SLURM_STEP_ID` | Yes, in segment names |

The rule is the `SLURM_` name with the prefix replaced, including `STEP_` where
Slurm has it, so nobody has to remember a second vocabulary.

**A program started with no bootstrap variables fails at init** ("no launcher
environment"). It does not run as a single PE. Silently becoming one PE is how a
mis-launched job ends up as N independent jobs, each succeeding. Every variable
marked "Yes" above is required, and the README shows how to launch a one-node job
by hand. *(Rejected: singleton launch when the variables are absent, as some
implementations do. It saves typing `osoe-launch -n 1` and hides the mistake above.)*
`OSOE_BOOTSTRAP_TIMEOUT` (above) is osoe's own, read by the library.
`OSOE_NODE_MAP` (§8.1) is osoe's own too but is read by `osoe-launch`, which
turns it into the `OSOE_NODEID`, `OSOE_LOCALID` and `OSOE_GTIDS` above; the
library never sees it.

The test launcher, `tools/launch/osoe-launch`, starts L processes of a one-node
job with these variables set. Its job id is its own pid, so concurrent launches
never share a name. It fails on a nonzero exit, a signal, a timeout, or a clean
job that left `/dev/shm/osoe.<job>.*` behind. That last check tests the naming
rules above on every multi-process test.

**Bootstrap is out of band in every build, and always will be.** An RDMA
transport cannot bootstrap itself: the GIDs, queue-pair numbers and memory keys
that make RDMA possible have to be exchanged before any RDMA exists, so the
rendezvous above stays the mechanism whatever carries the data path afterwards.
That is arithmetic rather than a decision, and it is true of every RDMA stack.
Note what it does *not* imply: the rendezvous is a socket and a handshake, not
§3's reservation protocol, handler table or aggregation, and an RDMA build
needing the former says nothing about whether it keeps the latter running (§10).

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

As implemented, a process-private mutex serves as the once-gate and guards the
count. A published `initialized` word mirrors the count, stored with `release`,
because the spec lets `shmem_query_initialized` be called from any thread at any
time without blocking behind an init in progress. Each full session gets the next
session number, which goes into every shared-memory name (§6.2). A finalize without
an init does nothing.

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

## 7. Spec Subset and Sequencing

**Criterion: what forces an architectural decision, however small.** Mechanical
expansion of an existing decision waits indefinitely.

### 7.1 Phase 1, in order

> **Every step ends with something that runs.**

An earlier draft listed phase 1 as one set of routines, every one of which depended
on the whole transport, bootstrap, and io_uring stack at once: nothing would work
until everything did, and the review's two blocking findings sat in exactly that
machinery. The review's order is adopted.

0. **Model before code.** Model-check the designs that are subtle and cheap to
   model: the repaired reservation protocol (§3.5), the handoff word layout
   (§3.11), and the collective counters with release ordering and downward
   forwards (§5.5). The review's harnesses are in `docs/reviews/evidence/`.
1. **One node, no network.** A functional OpenSHMEM on one machine without TCP:
   - `shmem_init`, `shmem_init_thread` and `shmem_query_thread` providing
     up to `SHMEM_THREAD_SERIALIZED` (§4.6), `shmem_finalize` with the non-last barrier,
     `shmem_query_initialized`, `shmem_my_pe`, `shmem_n_pes`;
   - node-local rendezvous and agent threads (§3.12, §6.2);
   - `shmem_malloc`, `shmem_free`, `shmem_calloc`, `shmem_realloc`, `shmem_align`,
     and `shmem_malloc_with_hints` over per-PE heaps;
   - intranode `shmem_putmem` and `shmem_getmem` as `memcpy`;
   - AMOs, including one 32-bit type so the 32-bit wait path is exercised (§8.2);
   - `shmem_wait_until`, `shmem_test`, and the signal routines;
   - `shmem_ptr`, `shmem_team_ptr`, `SHMEM_TEAM_SHARED`;
   - `shmem_barrier_all` on intranode counters, `shmem_quiet`, `shmem_fence`,
     `shmem_pe_quiet`;
   - the mandatory surface of §7.4;
   - the control segment's counters and `osoe-stat` (§3.14), so single-node hangs
     are debuggable from the start. The flight recorder is **not** here: every
     event it would record is a transport event and there is no transport until
     step 2, so it arrives with the backend that produces them (§3.14).

   This exercises the heap layout, watch slots, allocator, epoch counters, agents,
   lifecycle, and introspection.
2. **In-process AM backend (tier 1), slow mode first.** Internode put, get, and AMOs
   with landing zones and reply credits; `quiet`, `fence`, and `pe_quiet` with
   ack-only segments; `put_signal`; internode `barrier_all`. Semantics are
   validated before a socket exists.

   **This is the boundary's first real test**, and the reason it is worth
   noticing: it is the second backend behind §4.10, so §8.1's force-remote flag
   can run the same program over both paths and diff. Two open items are timed
   to land with it, because this step writes the code they would otherwise have
   to restructure: **decide `osoe_tp_sync`** (§10 — whether a collective's
   internode phase crosses the boundary) while writing the internode phase that
   is its only caller, and **`OSOE_TP_MORE`'s first caller**, since this is the
   first backend for which deferring a submission means anything.

   **`osoe_tp_set_range` also lands here** (§4.10), with the allocator call site
   it needs, for the same reason: the call site is in the allocator, which this
   step is already working around.
3. **Real transport.** Start with the `echo-uring` floor harness (§8.4), the
   transport's smallest reproducer. Then io_uring with the repaired reservation
   protocol, stream-parsed receive, the armed-flag idle loop, per-class wake words,
   reply credits and DRAINING caps, and the global rendezvous with its timeout.

   The first backend that *acts* on `osoe_tp_set_range` is here — it is the
   first that must prepare memory before the network can touch it — but the hook
   and its allocator call site landed at step 2 (§4.10), because the call site is
   in the allocator rather than in a backend. The allocator half — recording
   hints and growing hinted allocations from the opposite end — is older still,
   from step 1, because retrofitting hint plumbing into a finished allocator is
   the expensive version.
4. **Robustness.** Pidfd liveness, abort propagation, `shmem_global_exit`,
   keepalive; Slurm environment fixtures and the heap-base-shift test mode (§8.2).

Symmetric statics and the compiler wrapper follow step 4 (§7.4).

### 7.2 What each phase-1 routine forces

| Routine | Forces |
|---|---|
| `shmem_init`, `finalize`, `my_pe`, `n_pes` | Bootstrap, node topology, shared segment, election, agent threads |
| `shmem_init_thread`, `shmem_query_thread` | The thread level every completion structure assumes (§4.6) |
| `shmem_query_initialized`, repeated `init`/`finalize` | **Pool lifetime.** §4.4, §5.7 and §6.2 all carve once at init; refcounting is small but the teardown boundary is architectural (§6.4) |
| `shmem_malloc`, `shmem_free` | Deterministic allocator, identical offsets, per-PE heaps |
| `shmem_calloc`, `shmem_realloc`, `shmem_align` | Allocator edge semantics: barrier on exit, size 0 without a barrier (§4.4) |
| `shmem_putmem`, `shmem_getmem` | Descriptors, landing zones, reply credits, aggregation |
| `shmem_quiet`, `shmem_fence` | Completion model, ack-only segments, memory stores |
| `shmem_barrier_all` | Two-level collective skeleton all others inherit |
| `shmem_uint64_atomic_fetch_add`, `compare_swap` | **Unification of proxy-applied and locally-issued atomics.** Defer all AMOs and you build a transport that cannot express them |
| `shmem_wait_until`, `shmem_test` | How a PE blocks without starving the progress thread |
| `shmem_ptr`, `shmem_team_ptr`, `SHMEM_TEAM_SHARED` | Nearly free from shared heaps; large intranode win; forces the single locality function (§8.1) |
| `shmem_put_signal` | Fits aggregation well; how modern codes express producer-consumer |
| `shmem_signal_add`, `shmem_signal_set` | Signal-word update with no data transfer — AMO-shaped, not put-shaped, so they land on §3.8's handler-time rule rather than the payload path |
| `shmem_pe_quiet` | Validates the §4.6 dirty set: a global-sequence completion model could not implement it without over-quieting |
| `shmem_global_exit` | Termination of PEs that are not inside the library (§3.15) |

### 7.3 Plumb but do not implement

A **context handle** on every RMA entry point and a **team parameter** on every
collective, implementing only `SHMEM_CTX_DEFAULT`, `SHMEM_TEAM_WORLD` and
`SHMEM_TEAM_SHARED`. Retrofitting either parameter later touches every function
written. `shmem_ctx_create`, `shmem_team_create_ctx` and the team split routines
exist and return nonzero until implemented, which the spec permits: §9.5.1 says
an unsuccessful context creation "is not treated as an error", and §9.4 gives a
PE that is not a member of a new team `SHMEM_TEAM_INVALID` — so a program
written against the spec already has the branch these take.

**The context form is the definition; the plain form calls it with
`SHMEM_CTX_DEFAULT`.** The other way round — a plain form that does the work and
a context form that discards its argument — passes every test that can be
written while one context exists, and is the shape that breaks when a second
one appears. Every AMO, `putmem`, `getmem`, `putmem_signal`, the signal updates,
`fence`, `quiet` and `pe_quiet` are generated this way from one body.

**Handles are opaque pointers with macro constants, not exported objects.**
`shmem_team_t` and `shmem_ctx_t` are pointers to incomplete types, so passing a
team where a context is expected is a compile error, and the predefined handles
are small integers cast to those types. A library that exported
`SHMEM_TEAM_WORLD` as data would be exporting a symbol outside the `shmem_`
namespace the export test enforces, for no gain.

**`SHMEM_TEAM_SHARED` has its own §5.4 epoch counters**, although on one node
its membership is the world's. The spec makes the two distinct predefined teams
forming separate atomicity domains, and sharing the counters is not merely
untidy: a sync on one team would find `released` already at its target and
return without waiting for anybody — a collective that does not collect, which
no value check would notice. It is caught by the arrival table instead
(`shmem/tests/arrivals.h`), and by each PE's published epoch count for each
team.

**`SHMEM_CTX_INVALID` is ignored by `fence` and `quiet`, and only by them.**
Spec §9.12.1 and §9.12.2 say "no operation is performed" for those two; nothing
says it for an RMA or an AMO, where silently doing nothing would turn a program
bug into lost data. So the test for it is in those two routines rather than in
the shared handle resolution, which is loud.

**The C11 generic AMOs dispatch on arity first.** Each has two spellings, with
and without a leading context, and `_Generic` cannot tell them apart — they
differ in how many arguments they take, not in the type of one. The argument
count selects between two selectors, and each selector then makes the ordinary
type selection on whichever argument is the target pointer.

### 7.4 Mandatory surface

Required for conformance even though none of it forces a design decision: stubs,
aliases, and thin wrappers, but they must exist. An earlier draft scheduled none of
it.

| Item | Requirement | Plan |
|---|---|---|
| `shmemx.h` | "The shmemx.h header file must exist, even if no extensions are provided"; extensions use the `shmemx_` prefix | Step 1, and **empty by design**. Introspection is deliberately not an extension: §3.14's rule is that the library writes state into shared memory and a tool reads it, so `osoe-stat` needs no entry point and a hung job stays readable. The export check admits `^shmemx_` |
| Profiling interface (§10) | Mandatory: `pshmem_` names for every routine, `pshmem.h`, `shmem_pcontrol` | Step 1. **`pshmem_X` is the strong definition and `shmem_X` a weak alias**, the MPI convention: an application defines its own `shmem_X` and calls `pshmem_X` to reach the implementation. With the strong definition on the `shmem_` name, static linking would be a duplicate symbol. The library's own internal calls go to `pshmem_`, so a profiler counts what the application called, not what the library did about it. `pshmem.h` declares the generated families from the same lists and the same macros as `shmem.h`, with the prefix as a parameter. The export check widens to `^p?shmem_\|^shmemx_` and a second check asserts the *binding* of every pair, which no behavioural test can see: in a shared-library build the executable's definition preempts whatever the library's binding is |
| Deprecated routines and headers (Annex F) | `start_pes` — "Implementations must include it" — and the other deprecated names; the `mpp/shmem.h` header location | Step 1. Each is the modern routine under its old name, so the two spellings cannot drift. **`start_pes` registers a finalize at exit**, because a program written against it never calls one — finalization used to be implicit (F.2.3) — and without it every such program leaves its segment and heaps in `/dev/shm`, which is the state `osoe-stat --gc` exists to clean up after a *crash*. A program that exits normally must not look like one that crashed. A second `start_pes` is a no-op: it predates the reference counting that makes a repeated `shmem_init` meaningful, and a second implicit finalize would tear down a library the first call owns. `SHMEM_CACHE` (F.2.7) is six no-ops, which is the *correct* implementation on a cache-coherent machine rather than an unimplemented one — §4.1 already rests on coherence. `mpp/shmem.h` is a redirection to `shmem.h` and nothing else. The unprefixed names get no `pshmem_` spelling: §10's profiling interface is defined over the `shmem_` namespace and a `pstart_pes` would be an invention |
| Environment variables (§8) | `SHMEM_SYMMETRIC_SIZE`, `SHMEM_DEBUG`, `SHMEM_VERSION`, `SHMEM_INFO`, and their deprecated `SMA_*` forms | Step 1. The `SHMEM_`-prefixed spelling controls when both are set, written as that rule rather than as the shortcut the three flag variables would allow, since `SHMEM_SYMMETRIC_SIZE` has a value and will need it. `SHMEM_VERSION` prints before the rendezvous, so a job that fails to start still says which library it was; `SHMEM_DEBUG` prints after it, because the two things worth knowing are the ones it produces — the control segment to point `osoe-stat` at, and the heap base every PE agreed on. `SHMEM_INFO` documents the `OSOE_` variables too, which a user would otherwise have to read the source to find |
| C11 type-generic interfaces | Part of the API | Step 1, for the types implemented. `_Generic` selects on *type*, and the exact-width types of Tables 6–8 are typedefs of the canonical ones on LP64 Linux, so the selection lists one association per distinct type and the library's build asserts that each typedef is the type assumed. Each list emits a *leading* comma per entry, so there is no `default:` association and a type outside the table is a compile error naming the call rather than a silent route to another type's routine |
| `shmem_pe_accessible`, `shmem_addr_accessible` | Part of the API | Step 1, and not stubs. Every PE of an osoe job runs the same executable by construction, so `pe_accessible` is whether `pe` names a PE of this job. `addr_accessible` additionally requires the address to be in the symmetric heap, so a static or automatic address answers 0 — the known non-conformance below, stated by the query rather than discovered at the first put |
| `shmem_signal_fetch`, `shmem_signal_wait_until` | Part of §9.8 and §9.11 | Step 1, with the other signal routines |
| Name-shifted deprecated routines | §10 item 1 wants a `pshmem_` entry point for every function | **Not done**, and recorded as known non-conformance below: the spec does not say how to shift a name that never had a `shmem_` prefix, and guessing wrong helps nobody |
| Deprecated active-set constants (F.2.8) | `SHMEM_SYNC_VALUE` and the `pSync` sizes | Step 1: **the constants, and not the routines that take them.** A program that declares `static long pSync[SHMEM_BARRIER_SYNC_SIZE]` must compile; a program that then calls `shmem_barrier(...)` must fail at the link rather than reach a routine whose machinery — active sets, `pSync` ownership, the reuse rules — does not exist. A wrong answer is worse than a missing symbol |
| `shmem_ctx_session_start`, `_stop` (§9.9) | Part of the API | Step 1, as no-ops. The spec says sessions "do not affect the completion or ordering semantics of any OpenSHMEM routines", so doing nothing conforms. They are worth having now because the thing a session would inform is §3.9's aggregation through `OSOE_TP_MORE`, and §4.10 already records them as that flag's first driver: the backend that can act on them should not also have to introduce them |

**Known non-conformance: the deprecated routines have no name-shifted entry
points.** §10 item 1 requires "an alternate entry point name, with the prefix
`pshmem_` for each OpenSHMEM function", and the seven unprefixed deprecated
routines — `start_pes`, `_my_pe`, `_num_pes`, `shmalloc`, `shfree`,
`shrealloc`, `shmemalign` — have none. The spec's wording does not say what the
shifted name of a function that never had a `shmem_` prefix should be:
`pstart_pes` is what Sandia OpenSHMEM chose, and `pshmem_start_pes` is the
literal reading, which also yields `pshmem__my_pe`. Deferred rather than
guessed, because a profiling tool that looks for the wrong one is no better off
than one that finds nothing, and every other routine — all 526 — has its pair.

Two of §10's other requirements are satisfied and worth recording as such, since
both look like omissions otherwise. Item 3 states that the C11 type-generic
interfaces "cannot have any equivalent `pshmem_` interfaces because [they] are
implemented as macros", so their absence is required rather than missing. Item 6
requires the types and constants the `pshmem_` interfaces need to be defined in
`pshmem.h`, which it satisfies by including `shmem.h`.

**Known non-conformance: symmetric statics.** Most of the spec's examples use
global or static variables as symmetric objects, including the canonical
put-with-signal example. Until §4.3's phase 1.5 lands, osoe rejects them: an address
in neither a symmetric heap nor the statics range fails with a diagnostic naming the
routine, and `shmem_addr_accessible` returns 0 for it. Statics need the compiler
wrapper for `-fno-pie`/`-no-pie` address agreement, so the two are scheduled
together, immediately after step 4.

### 7.5 Deferred (mechanical)

Full type/operation cross product, strided transfers, nonblocking variants,
broadcast/reduce/collect/alltoall, locks.

The collectives are mechanical *given* §5.5's rules, which they must inherit: the
local lead is the single writer before release and leaves last, and a peer's
`source` is read only after that peer has called. A pull-based collect or
alltoall is non-conforming.

### 7.6 Deferred 1.6 additions

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

**The map is the launcher's input, not the library's.** `osoe-launch` reads it
(or takes `-m`) and derives each rank's `OSOE_NODEID`, `OSOE_LOCALID` and
`OSOE_GTIDS` — exactly what Slurm computes from a distribution on a real step,
with a rank's local id being the number of earlier ranks on its node. Nothing
under `shmem/src` reads `OSOE_NODE_MAP`, so the library sees the same
environment in a test as in production and carries no test-only bootstrap path.
*(Decided at §7.1 step 2, when the map was first implemented. The alternative —
the library reading the map and overriding the variables it was given — builds a
second, test-only bootstrap into the one piece of code whose whole job is to
agree with the launcher.)*

Two details that must be planned rather than patched. Both then hold **by
construction** rather than by a check, which is the reason the split above is
worth stating:

- **Shared segment name keyed on logical node id**, not physical host, or four
  logical nodes accidentally share memory. The control segment is already named
  `/osoe.<job>.<step>.<node>.<session>` where `<node>` is `OSOE_NODEID` (§6.2),
  and the launcher sets that to the logical node id.
- **`shmem_ptr` must return `NULL` across logical nodes** even though the pages
  are physically reachable. The locality decision lives in exactly one function
  that everything else consults. PEs rendezvous only with the PEs named in their
  own node's `OSOE_GTIDS`, so a PE never maps a heap outside its logical node and
  the function has no address to return.

Plus a **force-remote flag** routing even same-logical-node traffic over the
network. Running a program both ways and diffing is the strongest differential
test, since the shared-memory path is simple enough to trust as the reference.

**It becomes a three-way switch** — intranode, AM, and any third backend — the
moment a second network backend exists, so the differential test extends to it
for free. This is also the answer to "is the §4.10 boundary really carrying two
backends": not analysis, but running the same program over both and diffing. It
does not need new hardware. §7.1 step 2's in-process AM backend is the second
backend, so the boundary gets its first real test one step from here.

**Four poles, not one:**

- all PEs in one logical node;
- one PE per logical node;
- uneven node sizes, `0,0,0,1,2,2`;
- **ranks not contiguous per node**, `0,1,2,0,1,0` — the shape Slurm's cyclic
  distribution produces (§6.1).

An earlier draft called `0,0,0,1,2,2` non-contiguous; it is contiguous, so nothing
exercised the case that breaks per-node member counts. **Uneven sizes and
non-contiguous ranks are where two-level collectives break**, and a uniform map
will never show it. Multiply by fast and slow mode: eight passes over a suite that
runs in seconds at tier 1.

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
actually occur (verified: 20 of 20 sends came back short); separate network
namespaces over veth with `netem` for delay, loss, and small MTU. `netem` reordering
is invisible to a TCP application — the kernel reorders before delivery — so it
tests TCP, not osoe. A Pi handles four namespaces; the setup is a shell script
written once.

**Tests the review found missing:**

- **Invariant counters at zero.** *Done.* Every tier-2 test calls
  `shmem/tests/invariants.h` before its last `shmem_finalize`: a barrier, then
  every metric of kind `OSOE_MKIND_INVARIANT` in every record of its scope,
  found through the metric table by kind rather than by name. Checked by
  breaking it: a deliberate extra `missed_wake` bump made the wait tests fail,
  naming the counter and the block.
- **Introspection against a live job.** *Done.* `shmem.stat` runs `osoe-stat`
  against a running job at L = 1, 2 and 4, and at L ≥ 2 makes a PE sit in a wait
  nothing will satisfy while another reads it: the "blocked in" record names the
  routine, and `pe.sweeps` rises while the PE sleeps, which is the difference
  between a PE that is blocked and one that is dead. Both were checked by
  breaking them — suppressing the record, and suppressing the sweep count — and
  the test failed each time.
- **Bootstrap fixtures.** No tier fed real `SLURM_*` values. Capture `srun`
  environments for block, cyclic, plane, and arbitrary distributions, relative node
  selection (`-r`) and exclusions (`-x`), heterogeneous jobs, and a bare `sbatch`
  launch, and replay them into bootstrap parsing at tier 1. Test node-pair
  connection direction and the rendezvous handshake, including a client reaching
  the wrong job's server.
- **Heap-base shift.** Invariant 7 cannot fail while every process maps its own
  heap at the same base. A test mode shifts `B` per PE.
- **Timeouts on every test**, so a hang fails instead of stalling the suite.
- **A `NEEDED`-libraries check** on built binaries for invariant 15.
- **Collective timing checks must not depend on scheduling.** A first version of
  the barrier tests had each PE measure its wait from its own clock reading after
  the previous collective returned. A PE descheduled after that return starts late
  and measures less than the stagger, although the barrier worked; it failed about
  once in a hundred parallel runs under the sanitizers. The tests now check what a
  barrier actually guarantees. Each PE records its arrival time on
  `CLOCK_MONOTONIC` (shared by every process on a machine) in a table outside the
  library, and every PE must leave no earlier than the latest arrival. A mutation
  that skips the release wait fails every staggered check.

**What ThreadSanitizer proves here.** TSan instruments one process. The `tsan`
preset therefore checks races between threads of one PE: the agent against the
application thread, and the futex and epoch tests' threads. It cannot see a race
between two PEs on the control segment or on a heap, which is where most of this
design's shared words live. Those protocols rest on §3.16's memory-order rules, on
the review's exhaustive models, and on tests that run many epochs with skewed
arrivals on native aarch64, where weak memory ordering is possible (§8.5). A
cross-process race that TSan would otherwise report can pass the `tsan` preset
silently.
- **A 32-bit wait path.** Phase-1 AMOs were all `uint64`, which never exercises it;
  step 1 includes one 32-bit AMO type (§7.1).
- **The probe's receive check** asserts only more than one completion; tighten it to
  the verified shape of one per provided buffer plus a terminal `-ENOBUFS`.

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
| **Lima VM, Debian 13** (`vz`, repository mounted over virtiofs; `dev/lima/create.sh`) | Daily build and test loop. Kernel 6.12, the same line as Raspberry Pi OS Trixie. Native aarch64, so weak-memory-ordering failures are *possible* on every run, though not guaranteed to be observed. Tiers 1–3 fit: network namespaces, veth pairs and `sch_netem` all verified in the cloud kernel image |
| CI — **deferred** | The VM is sufficient for now. When CI is added it is also the only x86 leg, the case §8.3 calls adversarial for performance constants. Note that `ubuntu-24.04` ships liburing 2.5, below the §1 floor, so it needs a newer image |
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

### 8.6 Reference workloads

> **A small, fixed set of workloads drawn from real OpenSHMEM codes defines what
> must be fast.** Each is a tier-2 program when its routines exist, a performance
> harness beside §8.4 later, and a checklist for any change to the §4.10 contract.

They come from the usage study (`docs/reviews/evidence/q2/q2-usage.md` §1–§2),
which read the hot loops of 18 public code bases. A workload is the idiom, not the
application: a few dozen lines reproducing the call pattern and its sizes.

| # | Workload | Idiom, from | Boundary operations | What must be fast |
|---|---|---|---|---|
| U1 | Scalar remote access | One-word typed get or put per element, sometimes a quiet after each: Kokkos Remote Spaces, GUPs, bale `lgp_*` | 8-byte put and get; wait | Small-operation latency; wait with nothing pending |
| U2 | Halo exchange | Bulk put per neighbour, fence, increment a flag, `wait_until` on the receiver: NAS SP/BT/MG, PRK stencil | Bulk put and get; fence; rmw ADD; watch | Bandwidth; head-of-line blocking behind bulk data |
| U3 | Remote atomic per element | add, fetch-add, compare-and-swap per element or edge, quiet after each: Graph500, bale, ISx, Kokkos | rmw ADD, CSWAP, with and without fetch; wait | Atomic latency; wait with nothing pending |
| U4 | Put-with-signal | Payload put then a counter add or set; the receiver waits on the counter: bale convey, PRK Synch_p2p, and the NVSHMEM MoE kernels | put; fenced rmw; watch and wait | End-to-end message latency; wake latency; spurious wakes |
| U5 | Aggregated alltoallv | Remote offset reservation by fetch-add, then a bulk nonblocking put to many peers, then quiet: ISx, bale convey | rmw ADD with fetch; put; wait over many PEs | Dirty-set cost at high degree; aggregation |
| U6 | One context per thread | U1 and U3 on per-thread contexts: threaded GUPs and Graph500, hclib | U1 and U3 operations | Scaling with threads (needs `MULTIPLE`) |
| U7 | Phase boundaries | Barriers around every collective and one-element reductions per iteration: every code; bale issues two barriers per collective | Barrier; small reduce | Barrier latency against local PE count and node count |
| U8 | Intranode direct access | `shmem_ptr` once, then loads and stores, then fence: bale convey | direct; fence | A plain load or store |

Each workload runs under the four §8.1 node maps, so both paths are measured.
U1, U3 and U7 exercise the §10 fence-eagerness question directly. U6 waits for
`MULTIPLE`.

**Corpus and traces.** `tools/corpus/manifest.tsv` pins the usage study's code
bases at full commit SHAs (the OSU tarball by SHA-256), and
`tools/corpus/fetch.sh` fetches exactly those into `corpus/`, which git ignores.
Pins make every `file:line` citation reproducible. The corpus is read, never
built or copied: this repository has no license yet, and corpus licenses vary.
Each workload is backed by a *virtual trace* in `docs/design/workloads/traces/`,
one hot-loop iteration as ordered calls with sizes, targets, repeat counts, and
contexts, each step cited. Traces come from reading, not running, since building
C, Fortran, UPC, and CUDA codes tells us little before osoe exists; frequencies
and data-dependent sizes therefore stay symbolic. Reading already corrected the
table in three places, noted in the traces: ISx uses blocking puts and a barrier,
not nonblocking puts and a quiet; its offset counter is a symmetric static, so U5
needs §4.3; and convey's on-node path never fences. Dynamic traces come later
with no changes to the codes: §7.4 makes `pshmem_` names mandatory with `shmem_`
as weak aliases, so an interposition library defining `shmem_*` and forwarding to
`pshmem_*` can record real call sequences to check the virtual traces and fill in
their symbols.

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
    submission-queue bound, and each ring arms at most one futex wait.** (§3.3,
    §3.10) One multishot SQE yields many CQEs, and every extra armed wait adds one
    more; either mistake reintroduces an overflow path under load only.
13. **A waiter publishes its watch slot before its first predicate check.**
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
19. **Only the local lead's agent submits, and it alone owns rings and sockets.**
    (§3.12) Application threads and other agents request; they never submit.
20. **One symmetric heap per PE, addressed as `(PE, region, offset)` everywhere
    outside the owning process.** (§4.1, §4.9) Sharing a heap between PEs makes
    one PE's puts overwrite another's data; keying by address loses cross-process
    wakes.
21. **A non-reservable buffer carries the seal bit, at most one buffer per
    connection is reservable, and sequence numbers are assigned when a buffer
    opens.** (§3.5) Each rule alone was shown by a model to lose, tear, or reorder
    messages when broken.
22. **Every applied segment is eventually acknowledged, with or without reverse
    traffic.** (§3.4) Without ack-only segments, `quiet` on one-way traffic never
    returns.
23. **A provided buffer is never held across reaps.** (§3.3) Holding one stalls a
    large segment and can deadlock two connections sharing a ring.
24. **Every access to a shared protocol word is a `<stdatomic.h>` call naming an ISO
    memory order, and two-sided protocols use `seq_cst` or locked stores on both
    sides.** (§3.16) A `release` store in a two-sided protocol loses wakes on x86.
25. **Bootstrap connections scale with node count.** (§6.2) Only node leads cross
    the network.
26. **A non-last `shmem_finalize` is a barrier.** (§6.4) Skipping it lets epoch
    counts diverge across PEs.
27. **Introspection never needs the job's cooperation.** (§3.14) Everything an
    investigator needs is in the named control segment, written by single writers;
    the library serves nothing and writes no files. An export that depends on the
    job fails during a hang, the one time it is essential.
28. **The SHMEM layer reaches other PEs only through the transport boundary, and
    the boundary promises no more than the spec.** (§4.10) A direct AM call from §4
    ties the layer to one backend. A guarantee stronger than the spec's, relied on
    above the boundary, rules out every backend that cannot give it.

29. **Under `MULTIPLE`, a quiet on a shareable context bumps the generation, while
    holding the context's quiet mutex, before any of its waits; an issuer re-loads
    the generation after its add and moves the add if it changed.** (§4.11) Without
    the re-load, the next quiet closes the wrong pair. Without the mutex, two quiets
    close both. Either way a quiet returns before an earlier get has landed.
30. **Dirty bits are cleared only under the context's quiet mutex, and an issuer
    raises `high_seg` before it sets the bit.** (§4.11) Each rule alone was shown by
    a model to let a quiet return before an earlier put is applied.
31. **A writer wakes every watch record whose range overlaps the bytes it wrote,
    not the first.** (§4.11) Two threads waiting on one variable is ordinary.

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
| `shmem_team_destroy` scoping | **Collective over the team's members**: non-members hold `SHMEM_TEAM_INVALID`, on which destroy is a no-op. An earlier resolution called the scope ambiguous. Team ranges still leak until finalize (§5.7) |
| Team-based allocation | **Does not exist through 1.6.** 1.6 change #10 states malloc and friends are world-team collective (§4.5) |
| PMIx on the actual nodes | **Moot.** Nothing links against PMIx or PMI-2 on the default path (§6) |
| `max_teams` runtime or build-time | **Runtime**, read at first init, bounding three structures (§5.7) |
| How a PE blocks in `wait_until` | **One watch slot per local PE with a 32-bit futex sequence word** (§4.9). Futexing on the `ivar` itself was withdrawn, since 64-bit futexes do not exist, and the hash table was replaced once waits were seen to be on the caller's own variable |
| io_uring ring bound | **Two bounds, two rings.** CQ is sized from the provided-buffer ring (§3.3). Verified by `tools/probe` |
| Reduction `dest` versus 1.6 change #17 | **Not a conflict** — same arrays means same symmetric offset. The surrounding text exposed three real bugs, fixed by a single-writer lead that leaves last (§5.5) |
| Collective epoch identification | **Cumulative 32-bit counters with equality waits** (§5.4). Sense reversal retired |
| 64-bit futex support | **Not implemented.** `EINVAL` on 6.12 aarch64 (`tools/probe`) |
| Development environment | **Lima + Debian 13 on Apple Silicon**; CI deferred; a physical Pi for the kernel fork, NIC, and page size (§8.5) |
| Which process hosts progress | **An agent thread in every PE; the local lead's runs the transport** (§3.12). A helper process was rejected on setup complexity, supervision, statics access, and exit status |
| Phase-1 thread level | **Up to `SHMEM_THREAD_SERIALIZED`** (§4.6). Revised from `SINGLE`: nothing in phase 1 depends on which thread calls, only on one caller at a time. `MULTIPLE` is designed but not scheduled (below) |
| `SHMEM_THREAD_MULTIPLE` design | **Designed and model-checked, not scheduled** (§4.11). Serial mode for `PRIVATE` and `SERIALIZED` contexts. On shareable contexts, lock-free issue and a per-context quiet mutex, with a generation word and two get-counter pairs; dirty bits cleared under the mutex; one block per thread holding its watch record and completion slot. Review D6 item 2's `landed >= snapshot` rule was found unsound and not adopted. Open points below |
| Atomics | **ISO C11 `<stdatomic.h>` with named orders only; no intrinsics or assembly** (§3.16). Consequence: no spin-before-block |
| Introspection export | **Shared-memory control segment read by an installed `osoe-stat`** (§3.14). No HTTP server and no metrics files in the library; Prometheus through node_exporter's textfile collector, deferred until there is a reason for a third formatter. The control segment keeps its name until finalize, accepting a small leak on crash. The reader shares only the frozen directory and the metric descriptor with the writer and finds everything else by name, which a tier-1 test enforces by handing it a segment this build did not write. The flight recorder is deferred to step 2, with its design standing: its events do not exist yet |
| Vendor liburing or use the distribution's? | **Distribution's, ≥ 2.6.** Older OS releases are unsupported rather than accommodated with a bundled copy (§1) |
| Build tool | **CMake ≥ 3.25** (§1) |
| Project name | **osoe** (OpenSHMEM Over Ethernet). Library `libosoe`, built with hidden visibility so only spec API symbols are exported (enforced by the `shmem.exports` test); internal non-static symbols `osoe_`; implementation-specific environment variables `OSOE_`, since the spec reserves `SHMEM_`. Chosen over `eoshmem`, one letter from Open MPI's `oshmem` |
| Public header name | **`shmem.h`**, as OpenSHMEM programs expect. Coexistence with another implementation is by install prefix, as with MPI |
| What `OSOE_NODE_LAYOUT` covers | **The encoding of every word in the control segment, not only the offsets of fields.** A word written by one process and interpreted by another is part of the inter-process contract even when its width and position do not move; changing what its bits mean without bumping the version gives mixed builds silent lost wakes rather than the loud refusal `open_control` exists to produce. Recorded because a proposal got it wrong, arguing the §4.9 watch word could change encoding without a bump since it is "an opaque filter" — it is opaque to its *reader*, which is another process |
| Capability bits at the boundary | **Rejected twice, from two directions.** §4.10 rejected them on design grounds; the RoCE exercise supports the rejection empirically — every place a RoCE backend differs, it differs *underneath* the boundary (emulation, poller, loopback, size classes, queue-pair choice). The one case where the layer above might have wanted to know whether to block or poll is already answered by `osoe_tp_watch_wait` being the backend's own function |
| A boundary progress call | **Not needed.** The core-API study's core has one; osoe does not, because every place the SHMEM layer can block is already a backend function — `osoe_tp_wait`, `osoe_tp_get`, `osoe_tp_watch_wait` — so a backend pumps its own progress there. A real advantage of the current shape, recorded as one |
| `osoe_tp_put_word` | **Rejected.** `shmem_p` is the hottest pattern in the corpus, but here the backend branches on `len` and the caller spills a value it usually has in a register: single-digit nanoseconds, visible only in a microbenchmark of the boundary itself |

### Still open — blocking

- **Thorough 1.6 spec read.** The review's omissions pass covered the phase-1
  routines and the general sections, and closed "does any team query return
  remotely accessible memory" (none does). Still unchecked: the exact semantics of
  the 1.6 session routines if they are ever more than no-ops, and the routines
  beyond phase 1.

### Still open — measurement, once hardware exists

- **8-byte round trip and small-message rate, two nodes** (§8.4). Everything
  above assumes wire latency dominates memory bandwidth by two orders of
  magnitude. **This assumption is load-bearing** for copy decisions, futex
  decisions, and chunk sizing. Probably right; worth an hour to confirm rather
  than carry. Now has a home in the test suite rather than being a one-off.
- **Pin the §1 floor ratios.** Deliberately unwritten until the harness runs.

### Still open — the RoCE exercise of 2026-09-17

`docs/reviews/roce-transport.md` planned a RoCEv2 backend behind §4.10 to test
whether the boundary can carry RDMA at all, and estimated what it leaves on the
table. Its hardware facts are **recalled**, not measured — its §10 lists eight
things to verify before any number is quoted as a result — so nothing below is
implemented on its authority alone.

**The headline is that nothing in the boundary prevents an RDMA backend.** Of six
losses it identifies, four are RoCE's own (8-byte-only atomics, no non-fetching
atomic, no target-side counters, NIC atomics not CPU-coherent) or a phase
boundary (`get_nbi`, whose shape §4.10 now fixes as counters). The two that are
the boundary's cost one flag and one function, both now specified in §4.10, and
they are the *same two* the core-API study arrived at from a different direction.
Two independent searches converging on the same two gaps is the useful result.

Folded in already: dispatch per `(pe, operation class)`, the sweep bound on
blocking calls, `OSOE_TP_MORE` with the source-ownership rule that makes it safe,
nonblocking-as-counters, `set_range`'s contract, §4.1's registration trap, §6.2's
out-of-band bootstrap, and §8.1's three-way switch.

- **`osoe_tp_sync`: does a collective's internode phase cross the boundary?**
  §2.2 puts collectives on AM, decided when AM was the only backend; in an RDMA
  build that sends every barrier over TCP while the data path runs on the NIC.
  The proposal is a mandatory `osoe_tp_sync(ctx, group)` over a group of *nodes*,
  called after the intranode phase (§5.3) by one PE per node that has a member —
  note that is the team's local lead (§5.2), not necessarily the node lead of
  §3.12, so it asks the node lead's agent to send rather than sending itself
  (invariant 19). A backend with no native group barrier gets a generic tree over
  put and watch, so it carries no capability and §4.10's rejection of capability
  bits still holds.

  **Deferred to step 2, deliberately.** The case is an estimated 4–6× on barrier
  at 10^6 PEs — unmeasured, and four orders of magnitude beyond the target
  hardware. An earlier draft argued §4.10 contradicts itself on this and
  **withdrew it**: §2.2 scopes the boundary claim to §4 and puts collectives on
  AM on purpose, so there is a decision here, not an oversight. What is left is a
  trade between fifteen lines now and restructuring `barrier.c` later, and step 2
  writes the internode phase that is `osoe_tp_sync`'s only caller and its only
  implementation. Deciding it there means deciding it with the code in front of
  us rather than an estimate — and we may notice something on the way that
  changes the answer. *(Not rejected. Recorded so it is decided at step 2 rather
  than rediscovered.)*
- **Does the AM data path stay hot in an RDMA build?** Bootstrap is out of band
  in every build (§6.2) and that is forced. Whether §3's *data* path — the
  reservation protocol, deferred reply queues, aggregation — stays running is
  not. The argument for keeping it is that it cannot rot if it runs on every
  run; the flaw in that argument is that if RDMA carries the traffic, those are
  exactly the parts that would not run, so what gets exercised is bootstrap and
  control, which is less than the claim. The argument from abort propagation is
  weak on the machines RDMA runs on, where a resource manager tears the job down
  (§3.15). Three costs nobody has counted: the lead's agent still runs its idle
  loop; a PE that touched a peer over both paths must wait on two completion
  domains, so §4.6's dirty set grows a second dimension; and §3.4's connection
  buffers cost memory whether or not they carry traffic. Needs a device.
- **Verification before any RoCE number is quoted.** `roce-transport.md` §10's
  V1–V8. V1 — whether a remote initiator can target a memory region registered
  by the node lead, arriving on an XRC target queue pair on a shared XRCD — is
  the load-bearing assumption of the whole plan; its fallback costs 16 MB per
  node and no redesign.

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
- **The value of the reply credit `C`** (§3.8, §4.6). Large enough to be
  irrelevant to real codes, small enough that `connections × C` descriptors stay
  cheap; pick it from measurement.
- **Team range allocator and split protocol** (§5.7). Undesigned; becomes blocking
  when team creation lands.
- **`SHMEM_THREAD_MULTIPLE`: points §4.11 leaves open.** None blocks phase 1.
  - **A full thread table.** A PE's `OSOE_MAX_THREADS` blocks can all be taken by
    live threads. Aborting with a diagnostic naming the variable is loud and simple.
    A shared overflow block needs its own locking and wake-all rules, which only
    that rare case would exercise. Aborting is the proposal; decide before
    implementing.
  - **Landing-zone space under concurrent blocking gets.** §3.12 sizes the landing
    zone per PE, and phase 1 allocates from it with one caller at a time. Several
    threads claiming reply space at once need a concurrent allocator, such as
    fixed extents claimed by CAS over a bitmap. It is undesigned.
  - **Copy-out claims.** The agent and any quiet may both copy a landed
    nonblocking get to its destination. A CAS on the record's state gives exactly
    one copier. The idea is standard but not modeled.
  - **`pe_quiet` and piggyback.** The rule that a full quiet never piggybacks on a
    `pe_quiet` is stated in §4.11 item 2 but is not in the models. Add `pe_quiet`
    to `get_completion_model.py` before implementing it.
  - **A quiet with nothing outstanding.** It would be cheaper to load both counter
    pairs and the bitmap and return, without taking the mutex, when all show
    nothing outstanding. That looks sound, but it is not modeled. It matters only
    for shared contexts that quiet after every operation. U6 codes use serial
    contexts.
- **Fence eagerness** (§4.6, §4.10). The boundary lets fence send nothing, but
  §4.6 seals and flushes the dirty set on every `shmem_fence`. PRK and Graph500
  fence or quiet after every operation, so eager sealing costs neighbouring PEs
  their aggregation. Measure before choosing.
- **Designs for alltoall and variable-size collect** (§5.5). Scans and large
  reductions are designed in §5.8. collect can follow the spec's Example 37:
  exclusive-scan the sizes, then each PE pushes its block after it has called.
- **Reduction algorithm crossovers** (§5.8). The element counts at which a ring or
  Rabenseifner replaces the accumulator's internode tree are chosen from measurement
  on the §8.4 harness, not fixed in advance. The
  lead's-`dest` accumulator does not cover them.

### Review of 2026-09-16

`docs/reviews/design-soundness.md`, an adversarial review with a refutation pass,
reported 2 blocking findings, 9 design changes, 18 clarifications, and minor
items. Each was checked independently against the spec, the design text, and the
development VM before being folded in.

| Finding | Outcome | Where |
|---|---|---|
| B1 No process hosts the progress threads | **Accepted.** Agent thread per PE; lead's runs the transport; landing zones | §3.12, §4.7 |
| B2 One heap shared by all local PEs | **Accepted.** One heap per PE, `base_of` translation | §4.1, §4.2, §4.4 |
| D1 Reservation races | **Accepted** as model-checked | §3.4, §3.5 |
| D2 No acks for one-way traffic | **Accepted.** Ack-only segments | §3.4, §3.9 |
| D3 Accumulating armed futex waits | **Accepted.** One armed wait per ring | §3.3, §3.10 |
| D4 Bootstrap discovery and node map | **Accepted** | §6.1, §6.2 |
| D5 Rendezvous scales with PE count | **Accepted.** Leads only | §6.2 |
| D6 Fence and quiet cover memory stores | **Accepted** | §4.6, §4.8 |
| D7 Memory-order discipline | **Accepted for two-sided protocols; one claim discarded** (below) | §3.16, §4.9 |
| D8 DRAINING starvation | **Accepted.** Capped passes | §3.8 |
| D9 `global_exit`, non-last finalize, crashes | **Accepted** | §3.15, §6.4 |
| C1–C18 | **Accepted.** C5 resolved with a per-connection credit rather than only a node-wide pool, since the pool alone leaves the worst case growing with job size | §3, §4, §5, §6, §7, §8 |
| Minor items | **Accepted except one** (below) | Throughout |

**Discarded:**

- **D7's claim that §3.5's "never on x86" is wrong.** For §3.5's single-writer
  publication, which ends in a locked `fetch_add`, the claim holds. The defect was
  §4.9 applying the same rule to a two-sided protocol, which §3.16 now separates.
- **"§8.5 says `limactl shell osoe` but the instance is `shmem`."** The document
  correctly names the VM `dev/lima/create.sh` creates; the reviewer's VM predates
  the script.

### Still open — project

- **License.** None chosen. The repository carries no `LICENSE` file, and the
  README states that no rights are granted until one is added. Nothing should be
  published or accept outside contributions before this is settled.
