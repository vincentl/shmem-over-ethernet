# OpenSHMEM over Ethernet: Design Notes

**Status:** Phase 1 (§7.1) is built, every step of it, and the team redesign
(`team-resources-proposal.md`) is being finished before the first release;
`docs/prompts/README.md` says where the work stands. Each section says what
was decided and what was built, so that a decision is not silently reversed.

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
- PEs are processes. Cores are **not** assumed spare: a Pi runs a PE per core with
  the node's progress thread beside them, so oversubscription is the normal case
  (§3.11, §3.12), and nothing may spin.
- Slurm launch. No PMI link dependency (§6).
- TCP is acceptable. io_uring is required (kernel 6.7+ for futex ops; Trixie
  ships 6.12).
- **RoCE is a design constraint, not this project's work.** The transport and
  collective seams (§4.10, `internode.h`) must keep an RDMA backend possible at
  hundreds of nodes and toward 10⁶ PEs, and every review grades them so. But no
  RoCE hardware will be available before this project reaches its release point
  (below), so no RoCE backend is built here. Decided 2026-09-26. The one local
  road to a *functional* verbs backend is Soft-RoCE (`rdma_rxe`, in Debian's
  generic kernel, not its cloud kernel) over tier 3's veth pairs; it can test
  that the seams carry verbs, and nothing about performance, XRC (V1 of
  `docs/reviews/archive/roce-transport.md` §10) or NIC atomics. It is post-release
  work, if hardware is still far off then.

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
moved into init — and refuses by name otherwise, before it creates anything in
`/dev/shm`. The first catches old headers; the second catches an old kernel under
new headers. Init checks what the library uses: the futex2 calls always, and
since phase 1 step 3, for a job that spans nodes - the one kind that opens a ring
- an io_uring created `SINGLE_ISSUER`, `R_DISABLED` and with an explicit CQ size,
then enabled and entered as the agent's first pass does with its own, a
registered provided-buffer ring, and the `RECV`, `SEND` and `FUTEX_WAIT` opcodes
(`base/ring-check.c`; the enabling and the entering since the release fixes'
Part 6, whose adversarial reads found a job ending after its PEs had left
`shmem_init` where only those were refused). `env.probe` makes the same check
through the same function. A one-node job on a machine whose
`kernel.io_uring_disabled` sysctl forbids io_uring still runs.

*As built, v0.2 step 1 (`docs/decisions/2026-10-06-single-node-build.md`): the
single-node build.* The floor above is the full build's, and it is unchanged.
Beside it is an opt-in, `-DOSOE_SINGLE_NODE=ON`, for a machine that runs jobs
of one node only - a desktop, to develop programs at small PE counts, which
the audience of the first release's talk asked for above all: its library
links no liburing, configures with no pkg-config, opens no ring and makes no
socket, and waits with the classic `futex(2)` - `FUTEX_WAIT_BITSET` and
`FUTEX_WAKE_BITSET`, with the private flag and an absolute monotonic deadline,
every futex2 feature the library uses on one node - in place of Linux 6.7's
futex2 calls, so it runs on Red Hat 9's 5.14 kernel and any distribution's
since `MAP_FIXED_NOREPLACE` (4.17). One wait path a build, chosen when
configuring, never at run time; the full build keeps futex2 alone, since its
ring's `IORING_OP_FUTEX_WAIT` needs 6.7 anyway. A job of more than one node is
refused at `shmem_init` by name, before anything is made in `/dev/shm`, by a
backend that stands in for the internode one (§4.10). The author's reason for
an opt-in rather than an opt-out: "we want the builder to opt in to this, not
out of the full build" (`docs/decisions/2026-10-05-v0.2.md`, items 2 to 4).
Its build-time budget is a C11 compiler and CMake 3.25; its link-time budget
libc alone. Init's futex check names what each build needs
(`osoe_futex_requirement`), and its io_uring check is the transport's since the
same step (§4.10, as built).

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
rot the way a third-party build can. **The constant factors were deliberately not
written down before the echo harness first ran**, since pinning them before
measuring would be inventing a target rather than choosing one. The harness has
now run (§8.4, phase 1 step 3 slice 1) and its own io_uring-to-blocking ratio is
pinned. So are the library's ratios to it (§8.4, phase 1 step 3 slice 2;
`shmem.floor`): a get's, a fetch-add's and a put and quiet's fastest round
trip at most 3× the floor's, measured 1.4–2.2×; a put and quiet and a
fetch-add at most 1.5× a get, measured 0.98–1.00×, the pin a wait that polls
fails; and a stream at the default connection buffer at least 0.1× the
floor's, measured 0.21–0.43×. *Since v0.2 step 4* the default buffer is 256 KiB
(§3.4) and a put fills the room a buffer has (§3.5): the stream's pin is 0.3×,
and it measures 0.58–0.67× (§8.4).
Track all three per commit through the §3.14 counters.

The floor has since run on the target hardware itself (`docs/pi-cluster.md`,
2026-09-26): on two Raspberry Pi 5 nodes over one gigabit switch, the echo's
fastest round trip is 153-156 µs, the library's get 1.09×, put and quiet 1.08×
and fetch-add 1.12-1.13× of it, and the library streams at 0.999× the echo's
111-112 MiB/s. The Pi's round trip is the Pi's network stack, not the library.

### Release point

*(Added 2026-09-26, with the author.)* The project is released when all of the
following hold, and RoCE work waits until after it:

1. OpenSHMEM 1.5 complete: contexts and `SHMEM_THREAD_MULTIPLE` (phase 1 step
   8, three slices).
2. OpenSHMEM 1.6 complete: the scans (§9.10.10) built, beside the
   block-strided RMA and the sessions already there, and with them the two
   limits §7.4 once recorded (item 6).
3. The SOS test suite passes every program except those using SOS's own
   extensions (§8.6, the runbook's §8 names them).
4. The Pi runbook green end to end, with the floor ratios, `osoe-bench` and the
   collective sweeps recorded in `docs/pi-cluster.md`.
5. A license chosen (§10), and `cmake --install` with the README enough for a
   stranger to build and run a program on a Slurm cluster like the author's.
6. A version tag and a release note listing the known limits: whatever the
   last Pi session measured short of the floor. *(The two §7.4 limits once
   listed here, the deprecated routines' profiling names and partial active
   sets, were closed on 2026-09-26 at the author's direction.)*

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

*As built since v0.2 step 4's Part 2* (`docs/design/progress-helpers-proposal.md`
§3.1): **two connections a node pair, a control wire and a data wire** - and the
second is not a data socket. The data wire is the stream this section is about:
every put, get, atomic and reply, and every collective's data, which are puts
and adds, on one ordered stream a pair, so fence keeps its property. The control
wire carries the control plane alone: the internode tree's ARRIVE and RELEASE,
the deadlock detector's messages, and each wire's goodbye or abortive goodbye -
nothing with a payload, and nothing a data message's order depends on (a
barrier's quiet has its acknowledgements before its arrival is posted; a
release promises nothing about data; the detector needs every message it
counts acknowledged, on whichever wire, not ordered against the others). Its
segments are acknowledged as the data wire's are (§3.4, invariant 22): the
tree's two messages are application work their receiver counts, and the
detector's proof needs a node whose message is not yet applied to look busy
(§3.15) - which, as first built, unacknowledged, it did not (the adversarial
read of Part 2, F1). It is the lead's, sealed from a private buffer no PE
writes, so a barrier's messages never queue behind a put's bytes in a buffer
or a socket - they share the link, and the progress thread, whose pass takes
the control wires first - and the control plane's structures have one owner
however many threads move the data (§3.12). It costs a second socket a pair,
and a mesh twice as long to make (§6.2).

### 3.2 Chunking

Application-level fragmentation at ~64 KB. A 4 MB payload holds the stream for
~32 ms on 1 GbE; a barrier queued behind it waits the full time. Chunking bounds
head-of-line blocking while keeping a single ordered stream. The bound for a
control message is roughly **two segments** — the one in flight and the one
sealed behind it — plus any publication-window stall (§3.6); an earlier draft's
"one chunk time" understated it.

*Measured in v0.2 step 4, the bound left out the kernel.* Nothing limited a
socket's unsent queue, and at a gigabit the kernel held some 2.6 MB of a put
stream: a fetch-add behind it waited 14 to 22 ms on the Pi, whatever the
connection buffer. Each data wire now limits it (`TCP_NOTSENT_LOWAT`, §3.3) to
the link's bandwidth times 128 µs, at least 16 KiB, once the link is measured
at the end of `shmem_init`, so the bound is two segments, the limit and what is in
flight - 3.3 to 3.9 ms for that fetch-add, the stream at line rate
(`docs/decisions/2026-10-07-one-thread-profile.md`). The control wire (v0.2
step 4, Part 2) takes control messages off the data's stream altogether.

Each chunk is a self-contained AM carrying `(region, offset, length)`. The
receiver lands each chunk at its own offset — no reassembly, no fragment
tracking. Header overhead is ~32 bytes per 64 KB.

**SHMEM chunks above AM's advertised maximum payload; AM never fragments
internally.** AM keeps one invariant: every message fits in a segment.

*Since v0.2 step 4 a put's part may be shorter than a chunk:* it takes the room
the open buffer has left, down to a sixteenth of the largest payload a put's
message carries - 4,094 bytes at the default chunk - and the rest goes on in the
next (§3.5, "A put's part takes the room there is"). Each part is still a
self-contained AM landing at its own offset.

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

**`TCP_NOTSENT_LOWAT`, once the link is known** (v0.2 step 4). The kernel may
hold about the link's bandwidth times 128 µs of a data wire's bytes unsent -
about, since it takes a send whole while it holds less - and at least 16 KiB
(`wire-settings.h`; `OSOE_WIRE_UNSENT_BYTES` sets it by hand, taken as given,
0 leaving the system's own): what the link sends while the wire's owner comes
round to give it more, and no more for a small message to wait behind (§3.2). A
control wire keeps the system's own. The lead's application thread asks for it
at the end of `shmem_init`, after step 2 has measured the link without it, so
that the measurement sees the link as it is; each owner of the node's data
wires - the agent, and each progress helper (§3.12), since each alone owns its
wires' sockets (invariant 19) - sets it on its next pass, the request a word
taken as a dirty bit is, at a pass and again after the owner's `sleeping` flag
is raised; and an aborting owner lifts it, so that its last bytes leave with the
kernel even toward a peer that has stopped reading. Under the shape `fixed`,
which measures no link (§5.3), the limit is the 16 KiB floor on any link unless
`OSOE_LINK_*` give the bandwidth (`docs/decisions/2026-10-07.md`, item 11). A
send the kernel cannot then take whole is a short write, offered again
(invariant 2).

**Off by default:** `IORING_OP_SEND_ZC`. Two CQEs, buffer stability requirement,
and only pays above ~10–16 KB. Revisit only on a fast NIC.
*The author, 2026-10-07: built in v0.2 step 4 behind a setting, off by
default, for large segments, so that it is debugged before a fast NIC exists
and its default decided on one (`docs/decisions/2026-10-07.md` item 7). Over
TCP the buffer is released by the notification once the peer has acknowledged
its bytes, so with it on a connection's two buffers cap one peer's stream at
about 512 KiB a round trip - some 100 Gb/s at 40 µs (256 KiB and some 50 Gb/s
while the default buffer was 128 KiB; `docs/decisions/2026-10-07.md` item 12).*
*As built (v0.2 step 4):* `OSOE_WIRE_ZERO_COPY=on` sends a connection buffer
by reference wherever a send carries at least `OSOE_WIRE_ZERO_COPY_BYTES`
(16 KiB by default), with `IORING_SEND_ZC_REPORT_USAGE`; the completion frees the
wire for the other buffer at once, and the buffer it sent is **lent** - each
send's notification counted owed per buffer, a short send's remainder a send of
its own - until the last notification has come, when it is released as a copied
buffer is at its last completion. `reservation_model.py`'s `zero_copy` cases
hold it, and a release at the last completion and one at the first of several
notifications fail. A buffer about to be written with notifications still owed
ends the job by name - checked where it is first written, not where it was
released. **Only one buffer of a connection is lent at a time**: a send goes by
reference only while the other buffer owes nothing, and copies otherwise, so
that an acknowledgement or a reply always finds a buffer a copied send away -
with both lent, the Pi's `osoe-bench` stalled for seconds at 1 MiB, the
acknowledgement a quiet waited for queued behind two buffers that waited for
the peer's TCP acknowledgement, which the peer delays while it has nothing to
send. `reservation_model.py` checks it (BOTH LENT), and the engine ends the job
if both are ever lent. A wire owes at most 16 notifications, past which a send
copies, so that the completion queue keeps its bound: 16 more completions a
wire.

*As built* (phase 1 step 3 slice 1; `ring.[ch]`, `wire.[ch]`, `listener.[ch]`).
One ring per progress thread, created `IORING_SETUP_SINGLE_ISSUER |
IORING_SETUP_R_DISABLED | IORING_SETUP_CQSIZE` on the thread that makes the
node's connections and **enabled by the lead's agent on its first pass**, which
makes the agent the one task the kernel lets submit: invariant 19 enforced by the
kernel, not by convention (another thread's submission fails `-EEXIST`, measured
on 6.12). Since v0.2 step 4 each owner of the node's wires, the agent and every
progress helper, has a ring of its own, made so on the same thread and enabled
on its owner's first pass (§3.12). Both bounds are computed and asserted at open: the SQ holds `2 × wires +
1`, a re-arm and a send per wire and the futex wait, and the CQ `buffers + 2 ×
wires + 1` — one completion per provided buffer, because a buffer is refilled
only after the pass that emptied it; a terminal completion per armed receive; a
send per wire; the futex wait — and with the zero-copy send on (v0.2 step 4,
below) 16 more a data wire, the notifications a wire that lends may owe: `buffers
+ 2 × wires + 16 × data wires + 1`, on owner 0's ring at no helpers `buffers +
20 × (n - 1) + 1` with the control wires (§3.1), which never lend - which
io_uring's 65,536 completions bound at 3,274 nodes with the default buffers
(3,638 on one wire a pair), past which the setting is refused by name. A
submission that finds the SQ full is a fatal naming the broken bound, not a
retry.

**One provided-buffer ring for every wire**, `OSOE_WIRE_BUFFERS` of
`OSOE_WIRE_BYTES` (default 64 × 16 KiB), so the memory a node holds for
receiving does not grow with its peers; a multishot recv per wire selects from
it. The reap queues each filled buffer on its wire, oldest first, and marks the
wire readable; the pass then parses every byte of every readable wire where the
kernel put it - `osoe_wire_peek` shows the oldest buffer's unread bytes, and
`osoe_wire_consume` gives them back - recycling each buffer the moment it is
emptied. *(Until v0.2 step 4 the pass copied a wire's bytes into a chunk of
scratch and parsed that: a quarter of the receiving agent's time once a put
stream had made it the busiest thread, `docs/decisions/2026-10-07-one-thread-profile.md`.)*
So a buffer is
held from one reap to the end of that pass and never across the next reap, and
the next reap **checks**: a buffer still queued is a fatal naming invariant 23,
which is how "never hold a provided buffer" became assertable rather than
remembered. Every completion without `IORING_CQE_F_MORE` re-arms, `-ENOBUFS`
included; the re-arm is submitted at the end of the pass, after the buffers the
reap queued have been recycled.

**Send references the sealed connection buffer itself** — no staging copy, since
a sealed buffer is already stable until released. That is the one change to the
wire's contract: `osoe_wire_send`'s first offer starts the send and returns 0, and
an offer after its completion has been reaped returns what the kernel took, which
the caller then offers the rest after. The engine already behaved that way — it
releases a buffer only once every byte has been accepted, and re-offers from where
the count ended — so `progress.c` did not change for it; the wire checks that
bytes offered while a send is in flight are the ones in flight. `MSG_NOSIGNAL` on
every send: a peer that has gone is a completion with `-EPIPE`, not a signal.
Such a failure — `-EPIPE` or `-ECONNRESET`, the peer's reset — is the send
side's alone (phase 1 step 4, third review): the wire sends nothing more, and
its receive side goes on, since what the peer sent ahead of the reset is still
there to read; any other failure is the whole connection's.

**Split injection survives the socket.** `OSOE_WIRE_SPLIT` and `OSOE_WIRE_SEED`
truncate sends (to at least one byte — a send truncated to nothing would wait for
a completion that is not coming) and receives, hash-derived as before (§3.14), and
a small `OSOE_WIRE_BYTES` now makes the kernel itself split a segment across
provided buffers. `shmem.wire` asserts that short sends, short receives and
`-ENOBUFS` re-arms all happened (64-byte buffers, four of them).

### 3.4 Segment structure

**The trust boundary.** All PEs of a job trust each other completely — a
one-sided API cannot do otherwise, since any PE may write any heap. They are the
same job and the same build, **assumed buggy but not hostile**. Other local users
are untrusted, which mode 0600 and `O_EXCL` on every object enforce (§6.2). The
network is trusted at the level Slurm's own traffic is; §10 records how a secret
in the step-3 handshake would change that. *(Built in phase 1 step 3: the
optional `OSOE_JOB_SECRET` keeps hosts that do not know it out of the job's
hellos, §6.2; it does not encrypt the traffic that follows.)* What follows from
"buggy": **a peer's bytes must never be able to write outside a heap or a
landing slot**. The
receiving node validates every length and index a segment carries before
applying it — a message's length against what is left of its segment, the
message count against the segment's size, a get reply's token, offset and total
against the landing slot, an atomic's width, operation and alignment — and a
failure is a fatal on the receiving node naming the peer. That turns a peer's bug
into a named abort where it lands, instead of corruption where it lands. Before
phase 1 step 2 slice 2 none of this was checked or stated, and a forged get reply
wrote 64 bytes past a landing zone with the test passing (second review, D4).

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

Each message within a segment is **padded to 8-byte alignment**, so an AMO payload
is aligned as the specification requires of an AMO target. An earlier draft said an
unaligned 64-bit atomic faults on aarch64; *measured*, it does not on Linux, which
leaves EL0 alignment checking off - it silently performs a misaligned, possibly
line-straddling update (second review, portability F13). The rule stands; its
reason is that nothing would catch the mistake, so alignment is checked on receipt
rather than left to a trap.

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
| `OSOE_AM_BUFFER` | `4 × chunk` | Bytes per connection buffer, 256 KiB at the default chunk; a power of two, refused below the floor, `2 × chunk` |
| `OSOE_AM_BUFFER_BUDGET` | 1 GiB | Per-node ceiling on `2 × (nodes − 1) × buffer` |
| `OSOE_AM_HUGEPAGES` | 0 | Opt in to huge pages for the area - no effect as built: the area is a `/dev/shm` object, on which Linux refuses `MAP_HUGETLB` |

**The floor is the default, because everything except aggregation pushes down.**
§3.2 bounds head-of-line blocking at roughly two segments — about 2 ms on 1 GbE
at 128 KiB, doubling with the buffer — and §3.9's self-clocking already grows the
batch under load without needing headroom, since the buffer only has to hold
what accumulates during one in-flight send. Going above the floor is an opt-in
for links fast enough that a millisecond of wire time stops mattering.

*Since v0.2 step 4 the default is four chunks, 256 KiB; the floor stays two.*
With a put filling the room a buffer has (§3.5), a stream over loopback ran 1.4
to 1.5 times further at 256 KiB than at the floor (8.8 to 10.0 GiB/s against 6.4
to 6.5); at a gigabit, with the kernel's
unsent queue limited (§3.3), the larger buffer costs a fetch-add behind a stream
1.7 ms at the 99th percentile and nothing at the median
(`docs/decisions/2026-10-07.md` item 12).

**The budget is a guard rail, not a routine mechanism.** At these defaults it is
not reached until 2,050 nodes, past what §1 targets (the default buffer holds to
2,049 nodes: 2,048 peers × 2 × 256 KiB is 1 GiB), so inside the supported range
the behaviour is a constant buffer size with no heuristic to reason about:
7.5 MB per node at 16 nodes, 250 MB at 500. When it is exceeded (from 2,050
nodes at the defaults) the buffer halves — staying a power of two, which the
ring arithmetic needs — and keeps the *largest* size that fits; the floor is a
limit, not a target. If the floor itself does not fit (4,098 nodes at the
defaults; 4,097 fit) that is an error naming both knobs, not a silent drop below
it, because below the floor aggregation fails at the top of the range and
surfaces as a performance mystery rather than a message.

**The case that actually needs tuning is testing, not production.** §8.1's
logical node map puts every node on one machine, so the whole-host cost is
quadratic: 120 MB at 16 logical nodes, about 2 GB at 64, 32 GB at 256. Lowering
`OSOE_AM_CHUNK` is the knob for it, since it lowers the floor and the default
with it — 64 logical nodes cost about 2 GB at the default chunk and 250 MB at
8 KiB. Tier-2 tests set the sizes explicitly rather than inheriting the
production default.

**Huge pages are off by default**, and heaps do not use them either (§6.2).
Connection buffers are touched sparsely, because a halo exchange faults six
peers' worth of the whole area; `hugetlbfs` pages are reserved up front, so
opting in trades away exactly the demand paging that makes a large node count
affordable. *As built, the opt-in has no effect*: it adds `MAP_HUGETLB` to the
mapping of the area's `/dev/shm` object, and Linux refuses that flag for a file
that is not on hugetlbfs, before it looks for a page - so every PE warns and
maps ordinary pages, whatever huge pages are reserved (the step-2 review's F9,
and the release fixes' review found it again). A `/dev/shm` mounted `huge=always`
or `within_size` gives the area transparent huge pages regardless. A hugetlbfs
area is designed and not built.

**Known and not addressed: NUMA first touch.** On a multi-socket node the lead
creates the area, so first touch places every buffer on the lead's NUMA node and
PEs on the other socket pay for it on every `memcpy`. The target hardware is
single-socket, and fixing it means a first-touch policy per PE or per peer.
Recorded rather than built.

**Ack-only segments.** Piggybacking alone never acknowledges one-way traffic. If
node A only puts to B, no B→A segment exists to carry the ack, and A's `quiet`,
`barrier_all`, and finalize wait forever: the safety sweep submits only sealed
buffers, and B has none.

- A `fence`, `quiet`, or barrier flush asks the peer to **acknowledge through
  S**, where S is the highest segment sequence the caller wrote into toward it.
  The request is a sequence number in a shared word, `ack_wanted_through`, raised
  as a monotone maximum by CAS and never cleared; the lead reads it once a pass.
- A segment carries *ack requested* if its own sequence is above what the peer
  has acknowledged and at or below S — or, the **late request**, if S is still
  unacknowledged and nothing this node has sent since asked for it. Then the
  lead seals one header-only segment carrying the flag; its sequence is past S,
  and since an acknowledgement is cumulative, answering it covers S. The lead
  remembers the highest sequence it has sent flagged, so a late request is asked
  once, not every pass.
- On applying a flagged segment, B's progress thread owes an acknowledgement. Its
  next segment toward A pays it — every segment carries the cumulative `ack` —
  and if none is due it sends a **header-only segment**: acknowledgement, zero
  messages. **A segment sealed only to pay that debt never carries the flag.**
- Header-only segments reserve nothing, so §3.7's handler rule is untouched. They
  use the connection's single in-flight send like any other segment.

*Why a sequence and not a flag (phase 1 step 2 slice 2).* The first version had
one `owe_ack` bool meaning both "the peer asked" and "my writer asked", and a
request word the lead took with an exchange. Two defects hid each other (second
review, C1 and C2). Every acknowledgement was itself a request, so a quiesced
pair exchanged ~2,500 header-only segments a second per direction forever. And
the request rode whichever segment was finalized next — possibly one sealed
before the write the writer was waiting for — so with the echo removed the
obvious way, `quiet` hung (4 in 5 runs under timing injection). The echo was
what kept `quiet` live. `docs/design/evidence/am-backend/ack_model.py` has all
three: the old protocol fails QUIESCES, the naive split fails TERMINATES, this
one passes both under SC and TSO and needs at most three header-only seals per
node. A quiesced pair may still exchange one: a lead that receives a request
must answer it, and cannot know the asker's `quiet` was already satisfied by
some other segment's ack. What makes it the last is that the answer carries no
flag.

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

**A put's part takes the room there is** (v0.2 step 4,
`docs/decisions/2026-10-07-one-thread-profile.md`). The loop above is
`osoe_connection_reserve_up_to`'s whole case: given `most` and `least`, a writer
takes as much of `most` as the room after the head it loaded holds, 8-byte
aligned, and asks for a seal only when that is under `least`. The length is
decided from the head the CAS then compares, never from another load, or a
writer could reserve past the buffer's end. A put asks for its part whole and at
least a sixteenth of its largest payload, cuts its message to what it got, and sends the
rest as its next part - a successive message of the same writer, as a chunk
always was, so fence, signal-after-payload and quiet hold unchanged; every other
message asks for itself whole. Before it a buffer of twice the chunk carried one
64 KiB message, the headers leaving no room for a second, so a stream waited for
a turnover every 64 KiB; over loopback it moved a fifth of a bare stream with no
thread of the job more than 56% busy, and filling the room took it to 0.44.
`reservation_model.py`'s `fill` cases check it - messages of several units, a
put's signal after its parts, the pre-open drain, x86-TSO - and three breakages
fail: a stale head (OVERFLOW), a part that does not advance (ONCE), a floor not
lowered for a short remainder (STUCK).

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

*As built since v0.2 step 4's Part 2:* the collective forwards and the
detector's messages queue on the control wire toward their node (§3.1), which
carries nothing else, so no get reply is ever ahead of them; a data connection
keeps the AMO results, its *value* queue, drained fully first, and the bulk
queue. `osoe-stat`'s connection table shows the three (`ctrlq`, `valq`,
`bulkq`).

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

*As built (phase 1 step 2 slice 2).* The guard runs on every open: `flush`
drains pending replies — control queue first, then the bulk rotation — into the
freed buffer while it still carries the seal bit, up to half its capacity, and
`osoe_connection_open` then publishes `filled` and `head` at the drained extent,
seal bit last, instead of at the header. The opportunistic drain into an open
buffer stays: it is what keeps reply latency low when there is room.
`docs/design/evidence/am-backend/reservation_model.py` checks it, with breakages
for an open that resets `head` over the drained replies, a drain whose `filled`
misses the header (the accounting defect the second review measured, head 96
against filled 64), and two that make ONCE and ORDER each the first check to
fail.

Before that slice the guard had no caller: `drain()` competed with writers
through the ordinary reservation, and an open would have written over anything
drained. **The starvation it guards against was not reproduced** on the
shared-memory wire: one getter against 2-16 writers at the smallest legal
buffer, with puts sized to leave no room for a reply, under CPU load, and with
the writers' 20 µs backoff removed, never kept a get from completing; the worst
cost was a 2-6 ms get against 0.3 ms idle. The lead wins the race for a freed
buffer often enough. The guard is built because this section specifies it as the
guarantee, and step 3's wake-on-free and real sockets change the race; it is not
evidence of an observed hang.

#### Queue bounds — three separate arguments, each independently breakable

1. **AMO results:** bounded by the origin's outstanding-get cap. **Enforced at
   the origin, not locally** — a distributed invariant. A peer's accounting bug
   overflows *my* queue and the symptom appears on the wrong node.
2. **Barrier forwards:** bounded by tree degree × concurrent teams, per *node*.
   The epoch argument: if X holds an unforwarded epoch-N arrival, the root has not
   released, so no PE entered N+1, so no N+1 arrival can exist. On the *k*-ary
   tree (§5.3) it is concrete: per team a node queues either one ARRIVE toward its
   parent or up to *k* RELEASEs toward its children, never both, because its next
   ARRIVE needs every child's next arrival and a child has that only once the
   RELEASE to it has left the queue. So *k* + 1 per team node-wide is a bound with
   margin, and one per team per connection. **This rests on
   collectives being blocking.** Nonblocking collectives would dissolve it —
   write this into the code, because a future contributor adding
   `shmem_barrier_nbi` would not know to check.
3. **Quiet acknowledgements:** coalescable to one outstanding per connection
   (a high-water mark subsumes older ones); piggybacked or header-only (§3.4), so
   not really a queue entry.

Size the node-wide pool as `connections × (C + slack) + degree × max_teams`:
argument 1 is per connection, argument 2 is per node.

*As built (phase 1 step 2 slice 3):* `degree` is `OSOE_COLLECTIVE_DEGREE(k)` =
*k* + 1 for the tree's fan-out *k*, `max_teams` is `OSOE_TEAMS` (the two
predefined teams and the finalize team). *Phase 1 step 7* made `OSOE_MAX_TEAMS`
a run-time value (§5.7) and sized everything indexed by team number by it; its
first slice left this term at the three teams that could then exist, and its
second, whose splits create teams, made it `OSOE_MAX_TEAMS`.
`shmem.reply-pool.overflow` changed with it: at *C* = 1, *k* = 2 and the
fewest teams the variable allows, four, the pool is 1 + 3 × 4 = 13, and fifteen
origins overflow it where eleven overflowed three teams' 10. *Since the team
redesign's step D (part 2)* the term is `degree × OSOE_TEAM_BLOCKS`, the teams
the node can hold at once (§5.7): a number is each PE's own, so a node may hold
more teams than any one PE's numbers. The bound a block needs the old team's
forwards gone before the block is claimed again, and a team's members leave
its block only after its local release - which `release_local` applies only
once the team's RELEASEs have left the children's control queues. It flushes
each child's control wire first (since v0.2 step 4's Part 2; until then it
drained and flushed each child's connection), and that seals them unless a
segment is still on the wire, the wire's one buffer; then the local release
waits a pass, or as many as the send takes (`transport.am.releases_held`
counts the times it is held back, twice a pass). The
adversarial read of step D found the first build applied the release whatever
the drain left, which made the bound hold only when the drain emptied the
queues. 5,120 descriptors at the defaults,
about 240 KiB; `shmem.reply-pool.overflow` overflows `1 + 1 + 3 × 8` with
thirty-one origins and the smallest pool. Slice 2 sized argument
2 per connection at one per team, which was the star's shape — its root released
every peer at once — and cost N − 1 descriptors per team on every node for a need
only the root had; the node-wide term is O(*k*·*T*). `slack` stays 0 — every
descriptor belongs to one of the arguments above, and a
margin would only postpone the fatal that names the one that broke. The first
build sized the pool at `connections × C`, with no term for the forwards, and
dropped the request that found it empty: a node holding C gets toward a peer
lost its own barrier arrival or a get, and the origin timed out blaming the
wrong node (second review, D1). An empty pool is now a fatal on the node whose
pool it is, naming the peer, the kind of request, the pool and queue depths, and
which argument bounds it. Only a broken invariant reaches it; the test that does
gives the target a smaller `OSOE_AM_CREDITS` than the origin.

**The control queue is multi-producer** once a node runs more than one progress
thread: a collective forward received on one shard's connection is enqueued
toward a peer owned by another shard, with a wake to that shard.
*Not so since v0.2 step 4's Part 2:* a collective forward arrives on a control
wire and is queued toward a control wire, both the lead's, so the control queue
has one producer whatever the threads - the progress helpers proposal's reason
for the control wire. Pre-allocate at maximum. **Assert
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
lead (§3.12), and it asks the peer to acknowledge through the caller's highest
segment so a one-way peer still acknowledges (§3.4).

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

> **§7.1 step 2 polled, and that was a stand-in rather than a change of mind.**
> The loop above waits on *one* submission that carries both the socket
> completions and the futex — that is the whole trick, and `IORING_OP_FUTEX_WAIT`
> is in `tools/probe` because of it. Step 2 had no ring: its wire was shared
> memory, and a plain `futex_wait` takes one address, so the lead's agent could
> wait on its wake word or on one peer's wire, but not on both, and with several
> peers not on all the wires. Arriving bytes were found by looking, every 250 µs.

*As built* (phase 1 step 3 slice 1; `osoe_ring_wait_wake`). The loop above, with
three facts it did not state. **The backend owns the wait**: `agent.c` calls
`osoe_transport_agent_wait(wake, seen)`, and the lead's implementation arms the
ring's futex wait and calls `io_uring_submit_and_wait_timeout` with a sweep
interval; every other agent keeps a plain `futex_wait` (§4.10; the seam's first
shape, `idle_ns`, could not carry this). **The claimed wake of slice 3 is kept
unchanged**: the lead raises `lead_sleeping`, rechecks the dirty bits, and only
then waits; a writer that claims the flag bumps the word and wakes it, which
completes the armed wait. **A wait armed in an earlier idle period may be the one
slept on**: a socket completion ends a wait without completing the futex, so the
armed flag survives into the next idle period with an older value, and that is
correct because every bump of the word — the claimed wake, a posted arrival, the
agent's stop — is followed by a `futex_wake`, which completes a registered wait
whatever its value. `lead_wake_model.py` checks it: the `ring` design passes under
SC and TSO with the trapped-state check, including a lead woken twice by sockets
with its wait still armed; arming on every idle pass breaks "at most one armed"
(the 1,881); a wake without its bump is still a lost wake, ring or not. The
250 µs interval (`OSOE_PROGRESS_SLEEP_NS`) is deleted.

**How the lead finds its work** (phase 1 step 2 slice 3; second review, D12). The
loop above says `submit_sealed_buffers()` and `work_pending()` without saying how
the lead learns *which* connections gained bytes; as first built, every writer
did a locked add on the lead's one wake word and a `futex_wake` syscall, and
every pass drained and flushed every connection — O(*N*) per wake, and *L*-way
contention on one line. Two structures replace that, in the connection area's
header, so every local PE sees them:

- **A dirty-connection bitmap**, one bit per peer slot, 64 to a word. A writer
  that commits into connection *s* — or asks it for an acknowledgement, or waits
  for room in it — sets bit *s* with a `seq_cst` `fetch_or`, after every commit
  (not once per operation: a multi-chunk put whose next chunk waits for room
  hung until it did). The lead reads each word and exchanges it to zero only if
  it is nonzero, and drains and flushes only the connections whose bits it took,
  those that received something this pass, and those it privately knows are busy
  — a buffer to open, one sealed or on the wire, replies queued, an
  acknowledgement owed, or bytes its own pre-open drain left in the open buffer
  (the last found by a hang: the pre-open drain runs after the pass's chance to
  seal). Cost per pass: *N*/64 cached loads and O(active) connections visited.
  Measured with 16 logical nodes and traffic on one connection: 0.78–0.85
  connections drained and flushed per pass, against 15 before
  (`shmem.work-discovery`, which exports and asserts it through
  `transport.am.passes` and `transport.am.send_visits`).
- **A `lead_sleeping` flag.** The lead raises it before its last check, then
  looks at the bits again, then sleeps on its wake word at the value it loaded
  before the pass; it lowers the flag on its next pass. A writer loads the flag
  after setting its bit and wakes the lead only if it can **claim** the wake —
  a CAS from 1 to 0 — so one writer makes the syscall per sleep. A two-sided
  protocol (§3.16), modelled in `docs/design/evidence/lead-wake/`: SC and TSO,
  claimed and unclaimed, and four broken variants that each lose a wake
  (a `release` store on either side under TSO, no recheck, no bump). Measured,
  the same burst of 20,000 puts made 110–334 `futex_wake` calls instead of one per
  put; without the claim it still made 93% of them, because every put landing
  during the lead's wake-up latency saw the flag raised.

*As built since v0.2 step 4's owners* (`owner-map.h`; the progress helpers
proposal's §3.2 and §3.3): the bitmap, the flag and the wake word are an
**owner's** - a thread with a ring and the wires attached to it. The lead's agent
is owner 0, and with no helper threads, which is all that is built, it holds
every wire: one bitmap, one flag (`lead_sleeping` until then) and one word, as
above. With H helpers, peer slot *s*'s wire would belong to owner 1 + (*s* mod
*H*) as its (*s* div *H*)th, its *index*, which numbers the owner's bits, its
ring's tags and its engine's state. The connection area (layout 5) records H,
which every PE of a node must open it with; each owner's flag and wake word sit
on a line of their own, and each owner's bitmap starts one, since each is
another thread's. Owner 0 sleeps on its PE slot's `agent_wake` still, which
stop, abort and the posted arrivals bump too. A writer marks the bit of its
connection's index in that connection's owner's bitmap and claims that owner's
wake (`osoe_progress_host_wake`), the owner named once, for both
(`osoe_connection_area_owner`). A wake `OSOE_TRANSPORT_MORE` holds back is
remembered by the thread, an owner a bit, and made with its next wake whichever
owner that is for; a wait for what the PE held back toward any number of peers
wakes every owner of a data wire, each only if it sleeps.
`lead_wake_model.py` takes the owner as a parameter: its earlier cases are
unchanged, cases with one and two helpers hold, and two breakages lose a wake -
a writer whose map is wrong for its mark and its wake alike (`wrong_owner`),
and one that marks its connection's owner and wakes another (`mark_wake_apart`,
the adversarial read of Part 1's N2). `shmem.owners` runs the engine at two and
three helpers, every owner of a node turned by one thread, since with none an
owner's index of a wire is its peer slot and no other test could tell them
apart.

**Receiving polled every wire** in step 2: the shared-memory wire said nothing
when bytes arrived, so the lead looked at all *N* − 1 on every pass, O(*N*) per
pass. *Replaced in phase 1 step 3*: the ring's reap marks the wires something
arrived on, and a pass receives from those and no others — the receive side's
equal of the dirty bitmap. The send side's "privately known busy" half was still
asked of every connection (O(1) each, but O(*N*) per pass), so it is now a
private bitmap too, set where the work appears (a reply or collective forward
queued toward a connection) and settled after each visit. A pass costs *N*/64
loads of each bitmap and O(active) connections visited. Nothing added touches
every connection's shared state periodically, so lazy connection establishment
is no harder.

**A pass also reads the arrival handoffs** (§5.3), a word per team number in
the node header, for arrivals the node's members have posted. *As built (phase 1
step 7):* only those below `team_numbers_published`, the node's high-water mark
of published team records, which each record's writer raises after publishing
it and the lead's agent loads once a pass: team numbers are never reused
(§5.7), so a pass costs the numbers issued so far, up to the highest published
on the node, and not O(`OSOE_MAX_TEAMS`), 256 by default (64 until the team
redesign's step E) and 1,024 at most.
*As built since the team redesign's step D (layout 29):* a word per block of
the node's pool of teams, read for the predefined teams' three and for the
blocks at and above `team_blocks_published_from`, the lowest block of a created
team whose record has been published - blocks being claimed from the top of the
pool down, the highest free first - so a pass costs the most blocks the node has
held at once, not O(`OSOE_TEAM_BLOCKS`). Since layout 30 the word carries the
block's generation beside the epoch, and the agent compares the whole word with
the one it took last: a block given back and claimed for another team starts its
epochs again, never its generation (§5.7). And since layout 30 the agent no longer
walks the blocks: a member that posts sets its block's bit in a **bitmap of posted
blocks** and that bitmap word's bit in a **summary** (`team_posted_summary`,
`node.h`), after its handoff and before the wake, and the agent takes the
summary and the words it names by exchange (`tools/modelcheck/posted_bitmap_model.py`)
- a pass with nothing posted is one load at the default pool, sixteen at the
largest, however many teams the node holds. The walk it replaced cost the most
blocks the node had held at once: in the VM, 500 teams held made a barrier of the
world about 8 to 11 µs slower, some 10% (`docs/decisions/2026-10-03-team-allocation.md`).

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

*As built (phase 1 step 3 slice 2).* Two of the classes have their words: a
connection's acknowledged sequence, which quiet waits on, and its buffer
turnover, which a writer with no room waits on (`base/wait-word.h`,
`wait_word_model.py`). Both are divisible and wake every waiter. Beside each
word is a count of registered waiters, so the agent — which moves both values on
every acknowledgement and every open — makes the wake syscall only when someone
waits. The count is what makes each wait two-sided (§3.16), and the model's
breakages each lose a wake. Step 2 polled both with a 20 µs sleep (§8.4). The
third class, reply credits, is indivisible, and has the handoff below.

**Wake count tracks divisibility:**

- Buffer becomes `OPEN` (divisible: 192 KB free, 30 waiters needing 200 B each)
  → **wake many.** Waking one creates a serial chain with a scheduling delay per
  link; at 30 deep on a Pi that is milliseconds of pointless latency.
  *As built until v0.2 step 4 the room was divisible for small messages only*: a
  buffer reopened at the default sizes, twice `OSOE_AM_CHUNK` less its header,
  had room for one 64 KiB message, so writers of messages that large - a node's
  PEs putting to one node at once, an alltoall's step - woke together and all
  but one slept again (the release fixes' review, S1). A put's part now takes
  the room it finds (§3.5), so they each get some of it; a whole message larger
  than the room left still sleeps again. Measured in the VM: the wakes as
  counted, a few microseconds of the agent's a reopening at 32 writers, no fall
  in the node's rate, and a bare wake-one 10-25% slower at 4 and 16 writers -
  this paragraph's own chain. The fix is a hand-on wake - a writer that reserves
  and leaves room wakes the next, and a woken writer that finds none passes the
  wake on - after release, `wait_word_model.py` first.
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

*As built, for reply credits* (phase 1 step 3 slice 2; `reply.[ch]`,
`credit_wait_model.py`). A bit alone does not do it. With a flag and a wake of
one, a thread that registers just after the release still reaches the free
credit before the woken waiter runs. What makes acquisition "mediated by the
queue", as the note above asks, is to put the queue in the resource's own word:
one 64-bit word packs the free credits, the next ticket and the tickets served.
Whether a PE takes a free credit or a ticket is one CAS on that word, and a give
with a ticket outstanding serves the oldest ticket — the credit passes to it
without ever being free, which is the handoff. The free count is non-zero only
while no ticket is outstanding. A queued PE sleeps on the credit's futex word
with its ticket's bit (a futex bitset, one class in 32), so a give wakes only the
served ticket's class: exactly the waiter it serves while 32 or fewer wait, and
one in 32 of them beyond that, not a herd — with no memory per PE and no
syscall when nobody waits. The model's breakages: the handoff without the queue
barges, a plain wake of one or a bump before the serve loses a wake, and a give
that also frees the credit over-subscribes it. Step 2 polled here too.

**No spinning before blocking, in phase 1.** A useful spin loop needs a CPU relax
hint, which is an intrinsic or assembly, and §3.16 allows neither. A spin without
one burns power and, on x86 with SMT, slows the sibling thread. Waits therefore go
straight to the futex. Under oversubscription — the Pi's normal case — a spin
would be wrong anyway, since it takes a core a progress thread or mid-copy writer
needs. Revisit only with measurements.

*Measured, and kept* (phase 1 step 3 slice 2). With the waits on their words, a
put and quiet costs one library round trip, the same four wakes as a get: 2.06×
the floor idle. A spin of up to 50 µs of `sched_yield` before the futex took it
to 1.14×, and a get with it, and on a lightly loaded node a get fell from 30 µs
to 8. Beside 16 busy loops on four cores — oversubscribed, the Pi's normal case
— the same spin held a put and quiet at 4–7 ms in two runs of three and a
1 GiB stream at about 10 MiB/s, against 9–26 µs and 0.5–1.1 GiB/s without it:
the spinner took the core the agent needed, as this paragraph said it would. A
spin is `sched_yield`, a syscall and not an intrinsic, so §3.16 would have
allowed it. The measurement is what rules it out.

**No real-time priority anywhere.** It is tempting on an oversubscribed Pi and
would introduce unbounded priority inversion. Where isolation is needed, place
the progress thread on its own core with cpusets — isolation without a priority
relationship — bearing in mind that it runs inside the lead task's Slurm cpuset
(§3.12), so the job's CPU binding must leave room for it.

*As built, where Slurm binds each task to one core* (the author's questions of
2026-09-27; `agent.c`). A job whose tasks fill a node gets each bound to one
core by default, and the node's progress thread, the lead's agent, was bound
with its PE: on the Pi it was runnable and not running 29% of the time while
three cores idled, and a pipelined mailbox ran at a sixth of its unbound rate.
Two remedies, neither a priority and neither touching the system. Every agent
asks for a short EEVDF slice (`sched_attr`'s `sched_runtime`, 100 µs by
default, `OSOE_AGENT_SLICE_US`; Linux 6.12, no privilege): the same fair share,
but a sooner preemption when it wakes. And, asked for with
`OSOE_AGENT_CPUS=node`, every agent may run on any CPU of its node's tasks - the
union of the bindings the PEs publish in their slots (layout 27), so the job's
own cores and no other's. Bound, 16 PEs, a mailbox: 42,900 messages a second
with neither, 51,300 widened, 57,500 with the slice, 58,500 with both;
unbound, 259,000 either way. Beside the slice the widening bought nothing and
cost every small collective, in both of two alternating `osoe-bench` runs: an
8-byte reduce 223 against 200 µs, a barrier 191 against 178, a 64 KiB reduce
2,819 against 2,385. So the slice is the default and the widening is not: it
is for a kernel before 6.12, which ignores the slice, and a program bound by
point-to-point messages.

Keep futex words few (one per resource class per connection, not one per PE per
connection) and cache-line aligned with `_Alignas`. Futex waits hash into kernel buckets by address; unrelated
words can collide on the same bucket spinlock.

### 3.12 Progress threads and where they run

> **Every PE starts one agent thread in `shmem_init`. The local lead's agent is
> the node's control plane and, by default, its whole progress engine; with
> `OSOE_PROGRESS_HELPERS`, helper threads of the lead's process own the data
> wires beside it. Every other agent is a service thread that does only work
> which must happen inside its own process.**

An earlier draft said "a small fixed pool per node" without saying which process
the pool lives in. PEs are processes, so that choice decides how every
cross-process mechanism works: the pool is threads of the lead's process, and
nowhere else.

**The node's network state is held an owner at a time** (`struct
osoe_progress_owner`, `progress.h`; v0.2 step 4,
`docs/decisions/2026-10-08-owners.md`): an io_uring ring, the wires on it, their
connections, busy bits, reply pool (§3.8, sized by its argument over the owner's
own wires) and counters, which only the owner's thread touches. Each owner is
the sole issuer of its ring (§3.3); it seals, submits, reaps, applies incoming
operations to every local PE's heap (all local heaps are mapped in every
process, §4.1), and drains its deferred replies. Every node pair has two wires
at every helper count, a control wire and a data wire (§3.1,
`docs/decisions/2026-10-08-control-wire.md`).

- **Owner 0 is the lead's agent.** It holds every control wire - the internode
  tree's ARRIVE and RELEASE and the deadlock detector's messages, and nothing
  else but the goodbyes every wire carries, its own and the abortive one - and
  the control plane's state, the tree's and the detector's. With no
  helpers, the default, it holds every data wire too, and is the node's one
  progress thread.
- **Owners 1 to H are helper threads**, `OSOE_PROGRESS_HELPERS` of them, at
  most 64 (`progress-host.c`, `docs/decisions/2026-10-08-helpers.md`). The data
  wire to the peer in slot p is owner 1 + (p mod H)'s (`owner-map.h`), so a
  peer's traffic is always one thread's and keeps a single stream's order; a
  helper with no peer is idle. A node's PEs must agree on H in a job of more
  than one node; nodes may differ.
- **Writers wake the owner of their wire**, not the lead: the connection area
  holds a sleeping flag and a dirty bitmap an owner, and a helper's wake word,
  while the lead's agent sleeps on its PE slot's in the node segment, which stop
  and abort bump too (§3.10; `connection-area.h`), so a put to a peer wakes that
  peer's owner alone.
- **The agent starts the helpers at its first pass**, so that each begins with
  its signal mask and CPUs; each places itself as the agents are placed and
  asks the agents' slice (`OSOE_AGENT_CPUS`, `OSOE_AGENT_SLICE_US`, §3.11),
  names itself `osoe-helper-N`, publishes its tid and enables its own ring.
- **The agent asks and the helpers answer**, through a request word the agent
  alone writes and an answer word each helper alone writes
  (`helper_request_model.py`): the deadlock detector's idle and quiet views,
  each answered after a whole pass (§3.15, `deadlock_owners_model.py`), and at
  the end the stop, acknowledged at once and done once the helper has closed
  its own wires. A helper that ignores its stop is found by its own count of
  turns - two since the request without an answer ends the job naming it -
  and no wait gives up because time has passed.
- **An abort reaches every owner.** Each owner reads the node's abort word at
  every turn, as the agents do; the agent nudges the helpers, aborts its own
  wires, and waits for their done words within the grace before its process
  ends (`abort_owners_model.py`). A reader excuses an end of stream once its
  node's abort is declared, and sends nothing more on a connection that has
  failed; the carrier's wait judges the peers by their control wires alone
  (`carrier_model.py`, `control_only`). An abort found at a sweep of the
  agent's own close at `shmem_finalize` ends the process there, telling no
  peer and waiting for no helper, as it did with no helpers.
- **Each owner's counters are a record of its own** in the node segment
  (layout 36, the owner scope), which `osoe-stat` shows owner by owner and
  every reader sums.

**Non-lead agents never touch a ring or a socket.** Each sleeps on a futex word in
the node segment and is woken only for work that must run in its own address
space:

- operations on that PE's **symmetric statics** while they are private memory
  (§4.3), applied in FIFO order per PE *(not built: phase 1.5 was skipped, and
  statics are shared memory from `shmem_init` - §4.3, as built)*;
- copying completed **nonblocking get** replies from the landing zone into a
  private destination, so landing-zone space is released without waiting for the
  application to call `quiet` *(not built: the issuing thread copies them at its
  next wait, or when the zone is full - §4.6, as built)*;
- **`shmem_global_exit` and job abort**: the agent ends its own process with the
  status - with `exit(status)` on the caller's PE, and `fflush` and
  `_exit(status)` on every other (§3.15, as built).

**Application threads never submit.** They reserve and copy into connection
buffers (§3.5), and ask the owner of the peer's wire - the lead's agent, or with
helpers the helper that holds it - for everything else, a seal-and-flush for
`fence` and `quiet` and a wake for new work, with a dirty bit plus a futex wake;
the lead's agent alone for the control plane's arrivals.

**Replies land in shared memory, never at a private address.** A get's `dest`
may be a stack or `malloc` buffer the lead's process cannot see. Each PE
therefore has a **landing zone** in the node segment, sized by the outstanding-get
resource (§4.6).

It is in the **control segment**, not the AM connection area (§3.4), because the
reason it exists is the address-space boundary rather than the network: every
backend needs it, and an RDMA backend writing a reply straight into the origin's
memory still cannot write a private `dest`. It arrived at control-segment
**layout 6**, one slot per local PE, each holding one AM chunk — a landing zone
smaller than the wire's largest message would make that message undeliverable,
which is a hang rather than a short read. One slot per PE is enough while gets
and fetching AMOs are blocking, since `SERIALIZED` allows one outstanding (§4.6),
so the zone is sized by `L` and not by node count; nonblocking gets change that,
and the `bytes_issued`/`bytes_landed` pair beside it is already the shape §4.6
specifies for them. *(They did, at layout 16: each zone now also holds its PE's
nonblocking gets in flight, records and an arena, §4.6 as built.)* The progress thread writes reply chunks and fetching-AMO results
there, and the *origin* copies into `dest` *(as built, a fetching AMO's result is
a value written into the calling thread's reply slot, and takes no extent of the
zone: `transport-am.c`, `OSOE_LANDING_NO_EXTENT`)*:

- blocking get or fetching AMO: the calling thread, already waiting in the
  library;
- nonblocking get: that PE's agent, or the application thread at `quiet`,
  whichever comes first *(as built, the thread at its next wait, or at an issue
  that finds the zone full; §4.6)*.

This corrects the earlier claim that "the origin side never needs a helper." It
never needs one *for progress*; it does need its private memory written by a
thread inside it.

**How many progress threads a node wants** is set by memory bandwidth and the
link, not by the PE count: one thread on the Pi, perhaps four at 256 PEs on a
fast link, all in the lead's process. Neither count is measured: no link on
which one thread is the limit has been available (README, Limits), so the
default is none and a count comes only from the setting
(`docs/decisions/2026-10-07.md`, items 4 to 6). On the Pi's gigabit, under
Slurm's binding, two helpers made a 64 KiB broadcast and a 4 KiB alltoall 6 to
35% slower and a 64 KiB reduction 15 to 37% faster, and the mailbox a fifth
faster; unbound they cost the broadcast and alltoall 2 to 9% and bought the
reduction nothing (the README's Limits; the review of v0.2 step 4's merge point,
scale S4).

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
  network progress, its helpers' with it; debug with non-stop mode, or read its
  state with `osoe-stat` (§3.14), which needs no cooperation from the stopped
  process. A crashed lead cuts the node off. Under §3.15 the job aborts either
  way, but the other PEs must *notice*: every wait carries a deadline (§3.10),
  and on expiry the waiter checks the lead's liveness.
- **The lead's cores carry the progress work.** Under Slurm CPU binding the agent
  and the helpers run in the lead task's cpuset, competing with the lead's
  application thread; `OSOE_AGENT_CPUS=node` lets them run on any CPU of the
  node's tasks. On a Pi with one core per task this is the oversubscription
  case §3.11 plans for; measure it (§8.4) rather than assume.
- **Internal threads in an application process.** Agents block all asynchronous
  signals before they are created, and helpers inherit the agent's mask, so
  application handlers keep running on application threads. A child created
  by `fork()` after `shmem_init` has no agents and no helpers and must neither
  call OpenSHMEM nor run teardown (§6.4).

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

- **In the node's control segment**, `/osoe.<job>.<step>.<node>.<session>` (§6.2).
  **Its name is removed during init**, like every other object's (§6.2), and
  **`/proc` is the rendezvous instead**: every PE keeps the segment's file
  descriptor open for the session, and `osoe-stat` finds a job by scanning
  `/proc/<pid>/fd` for `/dev/shm/osoe.<job>.… (deleted)` and opening *that path*
  read-only — any live PE of the node serves, so a dead lead does not blind it.
  The kernel opens the same object, and updates show live. A reader outside the
  job's user sees nothing: another user's `/proc/<pid>/fd` is not readable, and
  with `hidepid` not even listable; `osoe-stat` says which of "no such job", "not
  visible to this user" and "/proc is restricted" it met. `/proc/<pid>/map_files/`
  is not used: it needs `CAP_SYS_ADMIN`. This route also works where Slurm's
  `job_container/tmpfs` gives each job a private `/dev/shm`, in which a name-based
  reader outside the job would see nothing at all.
- **Nothing survives a crash** — no death of any process can strand a name. The
  decision it replaces kept the control segment's name until the last finalize,
  so that a crash left the state readable for a post-mortem, and accepted the leak
  (§10). But what that wanted was uncooperative diagnosis of a *live* job from
  outside, and the name was only the rendezvous; `/proc` is a rendezvous that
  needs no name. A name that survives a crash is at once the leak and the
  post-mortem record — the same bytes — so only one can be the default, and the
  default is that nothing leaks: about eight cancelled jobs used to fill a 4 GiB
  Pi's `/dev/shm` (second review, D13). A site may clean `/dev/shm` between jobs
  on exclusive nodes; the library does not depend on it.
  **`OSOE_KEEP_CONTROL_SEGMENT=1`** opts back in: the name is kept until the last
  finalize, a crash leaves the segment for post-mortem reading (`osoe-stat` takes a
  path as well as a job), and `osoe-stat --gc` removes it once every recorded PE is
  gone and no live process maps it. Only in this mode can anything be stranded.
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

  **`struct osoe_metric_descriptor` is frozen with it, and for the same reason.** The
  table is what makes every other field readable, so its own element cannot be
  described by the table; something has to be the floor. The directory carries
  `metric_descriptor_bytes` so that a reader *checks* this assumption rather than
  making it, and a reader that meets a descriptor size it does not know prints
  the directory and stops. An unknown `layout` is not that case: the table still
  describes every field, so a reader finds everything by name as usual, and the
  layout decides only whether the watch word is decoded (`osoe-stat`, below).
  Two structures, both frozen, both checked: that is the whole contract between
  a job and a tool from another commit.

  Sections, each starting on a cache line: the header, the PE slots, the
  metric table, the counter blocks, and since layouts 6, 12 and 18 the landing
  zones, the connection records and each PE's row of published epochs. Since
  layout 18 the header ends with the team blocks, one per team number until
  layout 29 and since a pool of `OSOE_TEAM_BLOCKS` (§5.7). A metric names its
  **scope** — node, PE, thread, connection, and since layout 18 **team** (a
  block of the pool, indexed by its place there, which was the team's number
  until layout 29; the block's name says the number) and **epochs** (a PE's row,
  a word per team number) — which says which record its offset is into. The predefined teams' fields keep their node-scope names,
  `team.world.*` and the like, since the blocks are the header's tail; a reader
  finds every team by walking the team scope, and `osoe-stat` shows each whose
  record is published as `team.<name>.*`.

- **Counters are bounded per PE, and exhaustion ends the job, naming
  `OSOE_MAX_THREADS`.** `OSOE_MAX_THREADS` blocks per PE and the agent's one
  more, claimed by CAS on first use. A thread that finds none ends the job
  (below), and the *fact* is first recorded as `invariant.blocks_exhausted` **in
  the PE's slot, not in a counter block**. That distinction is the whole point
  and an earlier version of this line got it wrong: a thread with no block
  writes to memory nobody outside the process can read, so a counter kept there
  is exactly the one counter that can never be seen — including by the §8.2
  assertion that exists to see it. It is written by whichever thread ran out, so
  it is a `seq_cst` read-modify-write rather than this PE's single-writer
  publication, on a path taken at most once per thread. *As first designed,
  exhaustion was visible rather than fatal:* §4.11 proposed aborting when a PE's
  thread table fills, because there correctness depends on having a record, but
  here only the metrics did — and a job that dies because it made a ninth thread
  would be a worse failure than a job whose ninth thread is not counted. So a
  thread that found none got a **process-private block**, not a shared one,
  which would break §3.16's single-writer rule on every counter; the private
  block made the fallback path identical to the normal one, so nothing had to
  test whether it was initialized.

  *Changed in phase 1 step 8 slice 2.* A block now also indexes the thread's
  record of §4.11 items 3 and 5, and a thread without one would have nowhere to
  wait or take a reply, so **a full table ends the job, naming
  `OSOE_MAX_THREADS`**, at every thread level (decided 2026-09-25); the PE's
  slot still counts the event first, for `osoe-stat`. The private block is left
  for before init and after finalize. A thread gives its block back at exit, by
  a `pthread_key_create` destructor that also forgets it, so a destructor of
  the program's that runs later claims afresh; the give-back and finalize's
  release of the pool share a mutex, so a thread exiting during a finalize
  never stores into a segment being unmapped. `OSOE_MAX_THREADS` counts the
  application's threads, default 16 and at most 1,023, and the agent's block
  is one more (review of the slice, B1, by its arithmetic and not run: with the
  agent inside a default of 8, SOS's eight-thread programs would have ended the
  job).
- **On abort**, the library prints the control segment's name to standard error, so
  whoever reads the message knows what to point `osoe-stat` at.

#### `osoe-stat`

An **installed** tool that maps the control segment read-only and reads without
locks and without the job's cooperation. It is the only tool under `tools/` that
is installed, because the thing it is for happens on a machine that has no build
tree.

It is written **against the segment, not the library**: it includes nothing from
`shmem/src`, and from `base/` only `atomic-name.h` - how a name the library
rewrites is loaded, as the atomic words it is kept in, which says nothing of
where anything is (the "blocked in" record, above) - beside libc and the two
frozen structures above; and, for `--gc` alone, what two other objects open
with (below). That constraint is the mechanism. A reader with access to the library's
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
- **`--gc`** for what a job leaves in `/dev/shm`: segments left by a start that
  died, or kept by `OSOE_KEEP_CONTROL_SEGMENT`, and the two objects a start of
  a job of more than one node can leave. A segment is collectable when
  every PE that ever joined it is gone. One where *no* PE has joined is a job
  in bootstrap while the lead that made it lives, and a corpse once it is gone:
  collectable then, by `node.creator_pid`, if no live process maps it - a
  library from before that metric does not say, and its segment is left alone.
  A *tombstone* (§6.2), which has a directory and nothing after it, is told by
  its directory alone and collectable once no live process maps it, since its
  writer maps it until it ends. Heap objects are
  removed first, by the name each slot records, since removing the control
  segment first would strand them. A recycled pid can only make a dead job look
  alive, which costs a few megabytes of `/dev/shm` rather than removing a
  running job's segment. A segment still mapped by a live process is never
  removed, whatever its slots say.

  **Two objects besides the control segment.** A job of more than one node
  names two more while it starts, a node's job tables and its connection area
  (§6.2), and each loses its name when the last of the node's PEs has opened it.
  A start that fails or is cancelled before that leaves them, with nothing to
  say whose they are: by then the lead has removed the control segment's name,
  and the heaps', once every PE had mapped the others' (the round-three review's
  R5a). `--gc` collects them by the same rule, in terms of what they have, which
  is no PE slots but a creator: an object is collectable when the creator
  recorded in its header is gone and no live process maps it, and left alone
  while the creator has not yet recorded itself, which is a start in bootstrap.
  As with a control segment no PE has joined, an object whose creator died in
  that moment stays, until a later job with the same ids reclaims it (§6.2) or a
  site's epilog removes it. These objects are not self-describing, so this is
  the one place `osoe-stat` knows a structure by value beyond the two above:
  their magic numbers and the byte offset of the creator's pid. The library
  asserts the offsets where it declares the headers (`job.c`,
  `connection-area.c`), so that moving one fails the build and not `--gc`; the
  numbers are in `tools/stat/segment.c`.

Readers load with `memory_order_acquire` and accept values a moment stale.

**One field is decoded, not merely printed: the §4.9 watch word.** Its encoding
is part of what `layout` covers (§10), so the tool carries its own small copy of
the decode and uses it only for the layouts known to share that encoding: from
the first layout that had it, 4, to the newest the tool was built against. Any
other layout gets the raw word and a note, and nothing else changes — every
other field is still found by name, and no segment is refused for its layout.
The range has a top because a newer layout may change the encoding, and a raw
word is better than a confident wrong range; raising the top is a check that
the new layout did not. That is what a layout version is *for*, and it is the
only place the tool knows an encoding rather than an offset.

It was a single layout rather than a range until layouts 11 to 13 each left it
behind, and every job printed its watch word raw with nothing failing.
`shmem.stat` now checks the decode against a PE that is waiting, so a layout
bump that leaves the tool behind fails a test instead, and `stat.segment` runs
the tool on a hand-written segment either side of each end of the range, which
no job of this tree's own layout can show.

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
- **One file descriptor per PE** for the whole session: the control segment's,
  which is what keeps it findable through `/proc` once its name is gone. (The
  agents' pidfds, §3.15, are one more on a PE that is not a lead and L − 1 on a
  lead.)
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
- **Per team, in the internode tree** (layout 12, §5.3): the epoch this node has
  open and the last it closed, whether its own members have arrived, how many
  children have, and **the lowest child node that has not**. With the tree that
  last field turns "which node has not arrived" from a visit to every node into a
  walk down from the root, O(log N) visits, and `node.hostname` — also layout 12 —
  says which machine to ask next. `osoe-stat --summary` prints one line per node
  and team for exactly this walk - of a barrier or a sync, whose epochs the
  progress thread keeps: the data collectives' generic phase runs on the
  calling thread and publishes none, so a team hung in one reads "nothing
  open" there, and the PE lines say where its PEs wait (round six's Q5-1; its
  own walk is after the release). *Measured* (`shmem.hang-walk`, and the
  transcript in the commit): one PE of eight logical nodes never calls the second
  barrier, and three visits find it — node 0 names child 2, node 2 names child 6,
  node 6 has no epoch open and its own members not arrived, so its PE lines say
  which PE and in what.
- **Per PE:** a "blocked in" record (routine, target, and since when), its watch
  slot (§4.9), outstanding get bytes, bootstrap phase, and the abort word.
  The target carries a kind since layout 9 — a peer node, an epoch, a heap
  offset or a PE — so `osoe-stat` prints `shmem_quiet (node 1)` rather than
  `shmem_quiet(+0x1)`; the kinds are part of the segment's encoding. Every
  internode wait publishes one naming the peer node, which none did before phase
  1 step 2 slice 2: a PE stuck in a quiet toward a dead node showed as not
  blocked at all (second review, D2). Since layout 14 (phase 1 step 3 slice 2)
  the kind also says which of the connection's words the wait sleeps on
  (§3.11) — its acknowledgement, its buffer turnover, a reply credit, or the
  reply itself on the PE's landing slot — so `osoe-stat` prints `shmem_quiet
  (acknowledgement from node 1)`, and counts for each connection the PEs asleep
  on each word: the per-connection "waiters per wake word" below, read from
  records the job writes anyway, so the job keeps no count and pays nothing for
  it. `shmem.unswept-wait` checks both against a PE held in a quiet toward a
  stopped node. A reader built for an older layout prints the new kinds raw,
  and still reads everything it finds by name.

  It is the first record here with more than one word in it, so it is the first
  that can be read torn while the PE publishes its next wait. A **generation
  word bumped with two `seq_cst` read-modify-writes** around the field writes,
  and a reader that loads it, reads the fields, and loads it again, accepting
  only an unchanged even value. Two locked operations rather than a textbook
  seqlock, because that needs relaxed stores and a release fence and §3.16
  permits neither, while `fetch_add` naming `seq_cst` is a rule the design
  already has. **Every field is an atomic word**, stored with release and
  loaded with acquire, the routine's name as four 64-bit words holding its
  characters; `shmem/src/blocked.c` gives the argument in C11's terms, and
  `base/atomic-name.h` the packing the writer and both readers share. The name
  was a plain character array until the round-three review (K1): on aarch64 a
  reader's plain loads may be satisfied after its second load of the
  generation, and a litmus accepted 28,000 to 218,000 torn copies in 60 to 90
  million reads. Its bytes stayed where they were, so the layout did not
  change. Model-checked (`tools/modelcheck/blocked_record_model.py`, first in
  `docs/design/evidence/blocked-record/`), which also shows the four ways to
  get it wrong: no generation, bumping after the writes, letting a field write
  escape above the opening bump, and a reader that does not re-read - under
  sequential consistency and x86-TSO, which keep loads in order and so could
  not show K1. `shmem.blocked-record` runs the code's writer against both
  readers on the machine. It fails, in an optimized build, when the library
  reader's loads of the name are made plain again; it cannot see a writer
  whose stores are plain, on cores whose read-modify-writes are single
  instructions, nor `osoe-stat`'s reader made plain, which looks its fields up
  between its loads - the C11 argument in `blocked.c` is what holds those.

  **Published only when a wait is about to sleep**, never on entry. A wait whose
  predicate is already true touches no locked instruction and no clock — which
  is exactly what §8.6's U1 and U3 measure, "wait with nothing pending", so the
  record must not appear in that path's cost. The consequence to accept is that
  a PE spinning through satisfied waits shows no record; it is also not blocked.
- **Per connection:** sequence numbers sent, acknowledged as applied, and received
  and applied; the age of the in-flight send; each buffer's reserved and filled
  counts and seal bit; reply queue depths; credits left; waiters per wake word
  (counted from the per-PE records, below); the armed-futex flag.

**2. Is the network the bottleneck?**

- **Per peer:** bytes, segments, and messages in each direction; messages per
  segment and bytes per segment, as histograms; send latency from submit to
  completion, as a histogram; short sends; `-ENOBUFS` re-arms; multishot
  terminations; header-only ack segments sent.
- **Per connection, at read time:** TCP round-trip time, retransmits, and congestion
  window. These separate "the network is slow" from "osoe is slow".

*As built (layout 8):* node totals, not yet per peer — `transport.am.segments_sent`,
`header_only_sent`, `segments_applied`, `messages_applied`,
`reply_reservation_failures`, and the high-water marks of the deepest control
and bulk queue of any connection and of the node's descriptor pool. They are
fields of the node header written by the lead's progress thread alone, the
single-writer case of §3.16. They exist first because two protocols could not
otherwise be seen working: an acknowledgement echo (§3.4) and an exhausted reply
pool (§3.8) have no effect a test can observe except these counts.

*As built (layout 13, phase 1 step 3 slice 1):* the ring's own counts, node
totals written by the progress thread once a pass - `transport.am.completions`,
`receive_buffers` (provided buffers filled), `receive_enobufs`,
`receive_rearms` (every multishot end, `-ENOBUFS` or not), `sends` and
`short_sends`; since layout 34, the zero-copy send's `zero_copy_sends` (sends by
reference), `zero_copy_notifications` and `zero_copy_copied` (notifications
that said the bytes did not go by reference after all). `-ENOBUFS` and short
sends are normal under load; a rate far
above the traffic's is a provided-buffer ring too small for the peers sending
at once (§3.3). Per peer they wait, as the rest of this list does. An
`osoe-stat` from before layout 13 finds every metric by name through the
segment's own table, so it prints these too; it decodes nothing whose encoding
it does not know.

*As built (layout 12, phase 1 step 2 slice 3):* the **fourth scope** this
question asked for. One record per connection toward a peer node, in peer-slot
order, in the control segment after the landing zones, described in the metric
table like everything else and read by index: `connection.peer`,
`segments_sent`, `bytes_sent`, `header_only_sent`, `segments_applied`,
`bytes_applied`, `messages_applied`, the two queue depths and their high-water
marks, and the three sequence numbers a stuck connection is read by -
`sent_through`, `acked_through`, `wanted_through`. `osoe-stat` prints them as a
line per peer under the PE lines. The gauges are refreshed when the lead visits
the connection, which since slice 3 is only when it has work (§3.10), so an idle
connection's are as stale as it is idle - which is what a hung job wants: the
last state of the connection that stopped. Sized by node count, never PE count:
128 bytes per peer, 512 KiB per node at 4,096 nodes. A test reads them by name
through the same reader (`shmem.work-discovery` checks that the per-connection
counts add up to the node's).

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
  sweep is how such a wait is *meant* to end. Those go to `transport.watch.late_wake`,
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
so does `transport.wait.dirty_peers`; both land with the AM backend, which is also when
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

**No wait gives up because time has passed** (decided 2026-09-20). A wait may
end the job only because what it watches has *stopped moving*: a progress word —
a connection's acknowledged sequence, a buffer's turnover, a landing slot's wake
word, a rendezvous count — unchanged for `OSOE_NO_PROGRESS_TIMEOUT` seconds.
The default is 0, never. A large machine is slow in ways no bound anticipates,
and failing a job early under contention is worse than waiting: the launcher can
always tear a waiting job down, and nothing can bring a failed one back. This
applies to the init waits too. Every wait still sweeps (§4.10) — the abort word
and local liveness are checked every `OSOE_SWEEP_NS` — so a dead sibling still
ends a wait promptly; what goes is only the fatal deadline. (`base/stall-watch.h`
is the one helper; §3.10's absolute futex deadlines are clock instants for one
sweep-length sleep, and stay.)

**A start-up wait says what it waits for** (the round-three review's R8 and
N1; the author's D2, 2026-10-02). A job whose start had gone wrong - a peer
unreachable, a PE that never called `shmem_init`, a node that refused - waited
in silence until the launcher ended it, with nothing on stderr. Now a wait of
`shmem_init` that has lasted 30 s is said, by a notice, and again every minute,
and the notice changes nothing: it reads how long the wait has lasted, not
whether anything moved, so it is no exception to the rule above. One line a
node: the lead prints for the wait it is in - the local PEs that have not
joined or mapped, with their ranks; the node it calls, its host-list entry, the
address and ports dialled, and the last error and when; the nodes it waits to
be called by - and of the others only local id 1, for the waits the lead is not
in: while the lead has no segment to speak through, or a leftover of another
launch by the same names still stands in its place, and while the lead makes
the node's connection area. The others wait on the lead, and end if its process
ends: during the node's join by its pid in the header (`lead_ended`), and after
it by liveness (§3.15). `base/stall-watch.h`'s noticed watches carry the timing,
`osoe_notice` the line; `OSOE_TEST_NOTICE_SECONDS` shortens the 30 s for tests,
and is no setting.

**A program's own deadlock ends the job, once it is proved** (the author's
decision of 2026-09-27, "build it"; `deadlock.h`,
`tools/modelcheck/deadlock_model.py`). A job is deadlocked when every PE waits
inside the library, nothing any node sent is still in flight, and nothing waits
to be sent: only a library operation satisfies a SHMEM wait, and none is left to
happen. That is a state to prove, not a time to guess, so this is no exception
to the rule above - time only paces the looking.

- *What a node can see.* Each PE counts, in its slot's `waits` word (layout 26),
  its threads asleep in library waits and every change in that number: a thread
  counts from just before its wait first sleeps until the wait ends, and in
  between does nothing another thread or node could see - a routine that posts
  and then waits, as a barrier does, begins counting after the post
  (`base/sweep.h`'s contract, which every sleeping loop keeps). `/proc` lists the
  PE's threads, its agent's, the lead's progress helpers' and io_uring's
  workers' aside, and whether each sleeps; a thread woken and not yet run does
  not. The engine says whether the node owes the network anything, and whether
  every segment it sent that carried application work has been applied by its
  peer - asking the peer, where it has not said so, with the late
  acknowledgement request §3.4 already has; with helpers each owner says so of
  its own wires, a helper in answer to the agent's request (§3.12). The node's
  *activity* is the changes in its PEs' words and the application messages its
  engine applied, every owner's counted.
- *A sample* is taken by the node's lead's agent in two halves with a pass of
  its engine between them (with helpers, a pass of each owner, a helper's answer
  naming the request the first half posted or a later one): the words, then
  `/proc`, which must show as many
  threads as the words count in waits, all asleep; the pass, which takes what
  the PEs posted before they began to wait and the engine cannot see until it
  has (on AM, a collective's arrivals); then `/proc` again, the engine quiet, and
  the words, unmoved. A half that sees a thread awake - most likely a sweep's -
  or an acknowledgement still coming looks again a millisecond later, for up to
  5 ms, while nothing moves: longer would only hold up the node's engine when
  the thread is one that computes.
- *Rounds.* Once the root's own node - node 0's lead looks at its PEs' words
  each sweep interval - has had every PE waiting and nothing to send for a
  second, the root samples itself and asks every other node for a sample. If
  every node was quiet, it waits `OSOE_DEADLOCK_SECONDS` (default 10; 0 turns
  detection off), giving up as soon as its own node moves, then samples itself
  again and asks again. If every node was quiet again, its activity unmoved
  since its first sample began, the job is deadlocked: every node prints its
  PEs, those waiting at the same place on one line, with the routine, the
  detail and how long, and the job ends with status 1. The root asks every
  node directly, not up the collective tree: a round is O(N) small messages at
  the root, at most one a second and only while its node looks quiet, where
  the tree would make it O(log N) - the refinement once a job of hundreds of
  nodes runs.
- *Why that is proof.* With t* the end of the first collection, every node's two
  samples bracket t*, and nothing happened at any node between them. A message
  in flight at t* was sent before its sender's first sample ended, since the
  sender sent nothing after (a send needs a thread that runs, a message applied,
  or a post, and the pass took the posts), so it was acknowledged by then:
  applied before t*, a contradiction. So at t* every PE waited, nothing was in
  flight and nothing was queued - a state no later event can leave. A condition
  that came true with no wake - a store through `shmem_ptr`, a lost wake - is
  found by its waiter within a sweep interval, far inside the wait between the
  collections, which ends its wait and moves the activity; and that premise is
  checked, not assumed: each PE's sweep count must have risen, between a
  node's two samples, by at least the threads it has waiting - a timer so
  slack, or a PE so frozen, that a waiter never sweeps leaves nothing
  declared. The model checks the argument, and that each ingredient is needed: activity without the applied
  messages, quiet without acknowledgements or without the idle check, halves
  with no pass between, waits counted from the start of a routine that posts,
  one collection, or quiet without the PEs' waits each declares a job that could
  still move.
- *What it cannot see, and so never ends*: a PE with a thread the library does
  not know waiting - one computing, one in a condition variable, an idle OpenMP
  pool - or a stopped process; and, outside what OpenSHMEM defines, a
  thread that leaves a wait by `siglongjmp` or cancellation, or a forked child
  that waits in the library, either of which leaves the count wrong. A wait
  the library has not taught the count - init's rendezvous - leaves its PE
  never blocked, too. io_uring's workers are left out by the kernel's
  `PF_IO_WORKER` flag, not by their `iou-` names, which a program's own
  thread may share.
- *What reaches a PE from outside the library, which it cannot see either*: a
  signal handler's store, another program writing memory it shares with a PE,
  which no SHMEM program relies on. No sample reads a timer: a pending `alarm`
  lives in the kernel. A program woken only from outside the library should turn
  detection off, `OSOE_DEADLOCK_SECONDS=0`, as the README says. The round-three
  review ran one (K2): a PE waiting on a flag that its own `SIGALRM` handler
  sets, the others in a barrier, `OSOE_DEADLOCK_SECONDS=1`. In both of the
  handler's modes, a plain store and a library call, the detector reported the
  job deadlocked, each PE's wait 2 s old by its own report, and the job ended
  with status 1.
- *Probes are data in flight.* A round's probe toward a node that has vanished
  is data TCP keepalive does not probe, so the detector depends on the check
  that finds such a node by its kernel's silence within keepalive's budget
  (below, "As built: keepalive"). Before that check existed, a probe hid a
  vanished node for the kernel's fifteen minutes; tier 3's vanished-node tests
  found it.
- *Cost.* Nothing on any path that does not sleep. A wait that sleeps pays one
  locked add as it first sleeps and one as it ends. The root's lead reads L
  words each sweep interval; a round, once the root's node has looked quiet for
  a second, costs a message to and from each node and a sample of each, whose
  `/proc` reads are made only where every PE has a thread waiting, and which
  holds the node's engine up for 5 ms at most. After each round that fails the
  root waits twice as long before the next, up to `OSOE_DEADLOCK_SECONDS`,
  until its node is seen to move - so a threaded program whose PEs each keep a
  thread waiting while another computes, which a count of waits alone takes
  for quiet, pays a round every ten seconds at most. The pool of
  reply descriptors holds one more a peer for the detector's messages (§3.8), of
  which a connection has at most one queued.

**A fatal writes its reason first.** The message goes to stderr, in one write,
before the fatal publishes anything — a rendezvous's `failed` word, the node's
abort — because anything published lets the other PEs stop, the launcher then
sends the rest SIGTERM, and a reason not yet written is lost. It was lost once in
30 runs of a refused rendezvous under load; `osoe_fatal_then` (`fatal.h`) is the
form for a fatal with something to publish, and `shmem.fatal-order` and
`shmem.job.duplicate-rank.slow-message` hold the order.

**A fatal declares the node's abort**, and every wait reads the abort word at its
next sweep in every build. So the other PEs of a node stop within a sweep of the
first fatal. A
remote node cannot see another node's abort word: it learns of the abort from
the abortive goodbye on its connection to that node (as built, below), or, from
a node that died, as an end of stream without one.

The first build had a fatal deadline instead: `OSOE_BOOTSTRAP_TIMEOUT`, 60 s,
meant for rendezvous but also bounding every internode wait. A legitimate 150 s
quiet on 1 GbE died at 60 s, and a lead stopped for 20 s killed every PE talking
to its node (second review, D3). The variable is retired; a job that sets it is
told once that it is ignored.

A connection dying mid-message leaves part of a payload written into the
symmetric heap, unrecoverably. For a library where connection loss means job
loss this is acceptable, but it is a **documented invariant**: AM's error path
is "abort the job loudly," not "recover."

The policy is right; an earlier draft lacked **detection and propagation**:

- **`shmem_global_exit(status)`** (§9.1.6) must terminate every PE "regardless of
  their current execution state" — including PEs deep in computation that never
  call the library.
  1. The calling PE's lead sends the abort to every node — as built, as an
     abortive goodbye on every open connection, not along the tree (below).
  2. Each lead sets an **abort word** in its node segment and wakes every local
     agent.
  3. Each agent calls `exit(status)` in its own process (§3.12), so the status
     survives and standard I/O is flushed as §9.1.6 requires. *(As built:
     `exit` on the caller's PE only; every other PE flushes standard I/O and
     `_exit`s, below.)*

  Every library wait also checks the abort word. `exit` from a non-main thread can
  block if an application thread holds a stdio lock, so an agent falls back to
  `_exit(status)` after a bounded delay.
- **A crashed local PE** can die mid-reservation, leaving a segment that never
  becomes sendable while every TCP connection stays healthy — so nothing "aborts
  loudly". The lead's agent holds a **pidfd for every local PE**, polled on its
  ring, using the pids recorded at local rendezvous (§6.2). A pidfd completion for
  a PE that has not finalized aborts the job. Non-lead PEs detect a dead lead when
  a wait deadline expires (§3.12). *(As built, every agent watches with pidfds,
  polled at its sweep, and init's waits do until the agents run: below.)*
- **A vanished peer node** surfaces through TCP keepalive on every node-pair
  connection, as a connection error — which aborts. Not `TCP_USER_TIMEOUT`, which
  would declare a merely stopped peer dead (as built, below).
- **Slurm's `KillOnBadExit` defaults to 0**, so one PE exiting nonzero does not by
  itself stop the others. Document `srun -K1`. The spec does not define crash
  behaviour, so this is operability, not conformance.

*As built: every PE ends when its node aborts* (phase 1 step 4 slice 1, part B;
`abort.[ch]`). The abort word says how the job is ending — a fatal, or a global
exit — with the exit status and the rank that ended it, packed in 64 bits, and
the first declarer wins it: one CAS from zero installs all three, so a fatal
that follows a global exit cannot overwrite its status, and no reader sees one
without the others or waits for them (layout 15; `osoe-stat` names the kind and
the status, and one built before it still finds the rank by the name it reads it
by). The status kept is its low eight bits, which is all `exit` passes on.
Declaring wakes every local agent in local-id order, the lead's first, and every
agent reads the word at each turn of its loop, after its wake word — the stop
protocol's order — so a PE ends whatever its main thread is doing, including
computing outside the library, where nothing else of it runs. The lead's agent,
finding the word, first wakes every local agent again: a declarer's own agent
can end its process before the declarer has woken the rest, and the lead's
agent, woken first, closes that gap for all of them (O(L) once per node;
`abort_exit_model.py`, `no_rewake`) - on a global exit a second line now,
since the carrier's process outlives its node (below). The agent ends the
process. On a global
exit, the caller's PE — the PE its node's word names, whose declaration won:
the *carrier* — ends with `exit(status)`, its handlers and then standard I/O
as §9.1.6 asks, and a caller whose declaration lost with `exit(0)`,
with the library's own atexit handlers and `shmem_finalize` standing aside,
since the one is a barrier and the other would declare an abort that already
is. Every other PE ends with `fflush` and then `_exit(0)`, as a fatal does but
with 0. With `_exit`, because its main thread may still be running the
program, and a handler or a
C++ destructor run beside it would pull state from under it; and `_exit` takes
no lock, where `exit` since glibc 2.41 (BZ 31997) takes one it never gives
back, so that a second thread's `exit` waits for good on a main thread already
inside one — returned from `main` into start_pes's finalize, say (the author's
decision after the third review, which had found that wait and fixed it with a
hand-off to the thread inside `exit`, now gone; `abort_exit_model.py`,
`exit_everywhere`; `shmem.global-exit.handlers`, `.start-pes` and
`.returned`). What a non-caller loses is what only its handlers would have
flushed: output buffered outside stdio - a C++ stream not synchronized with it,
a logger's own buffer. With 0, because the status is the job's and the caller's,
not each task's: a launcher which ends the step at the first bad exit —
`srun -K1`, `KillOnBadExit` — then sees none before the caller's own, which
comes after its handlers and its flush, and `srun`, which returns the highest
code of its tasks, returns the caller's (the author's decision after the
round-three review, whose C2 had the others' `_exit(status)` come first and
Slurm kill the caller inside `exit`; measured under the VM's Slurm with
handlers of 0 to 5 s; `abort_exit_model.py`'s STATUS,
`shmem.global-exit.kills.*`). Callers racing each other leave the job one of
their statuses, as §9.1.6 asks: the first declaration on a node fixes the
status there, and every other caller leaves with 0.

**The carrier leaves last.** Such a launcher kills every task still running at
the first bad exit, and a killed task counts 143, which `srun`'s status would
then be: measured, before this, on two logical nodes in 2 and 7 runs of 30 when
the caller was not its node's lead - its goodbye leaving after the caller's own
agent had ended it - and on any node for a PE whose flush waited on a lock or
had megabytes to write, and for a second caller, killed inside its handlers
(Part 2's adversarial read, 2026-10-02). So, the author's decision of that
evening, the carrier's end waits until nothing else of the job runs. The
carrier waits for every other PE of its node, by pidfds opened then
(`osoe_liveness_outlast`), before its own end begins. A lead of a job of more
than one node waits for its node's others but the carrier before its own end,
so that its end of stream - its end makes it - tells the carrier's node that
its node is done; and the carrier's node's lead, after its goodbyes, waits for
every other node's end of stream (`progress-host.c`), but a node whose own
goodbye named another caller (`peer_abort_origin`), which carries that one -
each waiting for the other would only run both to the bound. The waits are
bounded, `OSOE_ABORT_LEAD_WAIT_SECONDS` (20) and
`OSOE_ABORT_CARRIER_WAIT_SECONDS` (30), each above the grace of what it waits
for: a PE whose end is held still ends first, by its own grace. Within a node
graces do not stack - a lead's starts before its wait for its node, and itself
waits for that node before `_exit` - but a lead of the carrier's node starts
its grace only after its goodbyes and its wait for the other nodes, so such a
lead ends within five graces of the abort, any other lead within three, and the
carrier within four; processes held at several places at once, or stopped
outright, can run a bound out, and the order with it. On a global exit a
lead's stream is not ended at its goodbye but by its process's end: with three
nodes, a node told by a third sent the carrier's node a goodbye and ended its
stream at once, which that node took for its PEs having ended (Part 2's second
adversarial read; the model, of two nodes, relays no goodbye) (`abort_exit_model.py`'s
NO_KILL and JOB_STATUS, with srun -K1 as a thread that kills; its variants
`no_carrier_wait`, `no_lead_wait`, `no_remote_wait`, `wait_every_node`,
`no_exclusion`, `called_carries`; `shmem.global-exit.kills.*` require no rank
killed). Two limits stay: a site's `WaitTime`, or `srun -W n`, ends the other
tasks n seconds after the first leaves, whatever its code; and two PEs of two
nodes calling at once, each first on its own node, each carry their node's end,
and the first to leave can still have a PE of the other killed (the model's
`both` with `kills`). A process stopped outright outlasts every bound, and is
left behind. And one narrow case waits out the grace again: on the
caller's PE, another thread already inside `exit` - main returned into the
library's handler while a second thread called `shmem_global_exit` - holds the
lock the agent's `exit` needs (the model's `caller_exiting`). Because an end can still wait on a lock another thread holds —
a stream's, or on the caller's PE `exit`'s own — `_exit` follows after
`OSOE_ABORT_GRACE_SECONDS` (10), with the status the PE would have left with,
flushing stdout first if no thread holds it; a caller whose own handlers
outlast it is ended so too.
That is not a wait that gives up because time passed: it ends a process that is
already ending. On a fatal the agent says so once and `_exit`s with the
declarer's status. On a global exit exactly one thread of a process ends it, and while the
agent runs it is the agent; any other thread that finds the word waits for it —
including one that reaches `shmem_finalize` or the library's atexit handler
meanwhile, which stand aside by returning only on the thread ending the process:
on any other they would return into a second `exit`, main's own beside the
agent's, which C leaves undefined and which could end the process with main's
status (found in review; `abort_exit_model.py`, `finalize_returns`). A fatal
ends at once, on whichever thread finds it, as it did before: its status is a
failure whoever ends the process, and on the eager mesh every node reads the
dying lead's end of stream itself. With on-demand connections a fatal will need
the agent's goodbye too. On the caller's PE, `exit` from the agent runs the
process's atexit handlers beside whatever other threads the program has; a
handler that calls into the library then waits out the grace. No PE returns from `shmem_init` before its
node's lead's agent runs, which the lead publishes in the node's rendezvous
words: a non-lead is otherwise done once the connection area is open, and a
global exit straight after init found its lead still connecting the mesh, with
no agent to tell anyone — the nodes it had reached took its end of stream for a
death, and those still dialling retried forever (found in review;
`abort_exit_model.py`, `no_init_wait` — the tests' immediate exits did not hit
the window, which a lead's mesh setup keeps to about a millisecond).
`shmem_global_exit(status)` declares the word as a global exit and ends through
the same path. A library wait that sweeps into a global exit ends the same way,
not through a fatal that would change the status, and so does a fatal whose own
declaration loses to a global exit, since what went wrong may be that exit's own
doing. (The development watchdog read the word again before it called a dead
sibling a fatal, since a PE a global exit ended is dead too, until it was
retired: below.) `abort_exit_model.py` checks this ordering: every process ends, each with a status a caller gave, and
each of these rules broken fails it.

*As built: the abort crosses nodes* (phase 1 step 4 slice 1, part A;
`abort_model.py`). A lead whose node aborts sends, on every open connection, an
**abortive goodbye**: a last segment flagged `OSOE_AM_SEGMENT_GOODBYE` that
carries one `OSOE_AM_ABORT` message — the rank, the status and the disposition —
after finishing the segment already on the wire, so the peer's stream stays
whole, and then it ends its stream. A peer reads it before the end of stream it
precedes, so that end of stream is the job ending, not a death; the peer
declares its own node's abort with the same rank, status and disposition — the
first declarer wins — and its lead does the same in turn. The model put two
designs to it. Sent along the world team's tree — O(k) a node, O(log N) hops —
the abort loses the status: the eager mesh already carries a death in one hop,
as an end of stream without a goodbye, and a node that reads a dying node's end
of stream before the tree's message reaches it ends with a failure where
`shmem_global_exit`'s caller gave a status. The abortive goodbye is read first
by construction. It costs O(open connections) a node, once: the eager mesh's N,
which is what a normal goodbye costs too, and O(k + active peers) with on-demand
connections (§6.2), which still reach every node through the tree's own
connections. At 4,096 nodes that is 4,095 segments of 56 bytes from each lead. A
connection with nobody to tell is skipped: one whose normal goodbye has gone,
whose peer died, or whose peer's own abortive goodbye has arrived — and a peer
whose abortive goodbye has arrived is sent nothing more of the job's, since its
exit may already have reset the connection. From the moment the node's abort is
declared - before its owners have begun their own, with helpers, since the
goodbye that declared it may have come on a wire another owner holds - a failed
connection or an end of stream without a goodbye is a peer that is ending too,
not a fatal. A goodbye counts as told once the peer's kernel has
acknowledged it (`SIOCOUTQ` at 0), the engine receiving meanwhile: this
process's end with input unread is a reset, and a reset drops whatever is still
queued — the goodbye among it, behind a stream or waiting on a retransmission —
so that the peer would read a reset without a goodbye and end with a failure
(found in review; `shmem.global-exit.stream`). The wait is bounded by
`OSOE_ABORT_GRACE_SECONDS`: a peer stopped with a full receive window could hold
the bytes forever, and this process is ending either way; a peer told nothing
takes its end of stream for a death. A reset ends it at once — it leaves
`SIOCOUTQ` above 0 for good, so a closed connection counts as told — and the
grace of `exit` above follows it, so a process ends within twice the grace of
its abort. The wait makes sure of the peer's kernel, and the peer's engine does
the rest: a peer still sending to this node finds the reset as a failed send,
and one that took that for the connection's failure ended on it at once, with
the goodbye unread in its kernel whenever its ring had had no buffer free for it
(found in the third review; `abort_model.py`, `send_error_ends`). A failed send
on a reset now stops the sends and nothing else: the peer goes on receiving,
reads the goodbye ahead of the reset, and is judged at its stream's end — by the
goodbye, or, without one, as a peer that died (`shmem.global-exit.stream` with
two wire buffers). The close follows the same rule: a connection whose sends met
the peer's reset is owed nothing more, and needs no goodbye of this side's once
the peer's own has arrived and its stream has ended - a peer that crashed during
the close, which would otherwise leave this node waiting to send a goodbye
nobody will read (the model's crash cases). The one new message kind is `OSOE_AM_ABORT`
(§3.13's table, and so its checksum). An abort during §6.4's close needs no
abortive goodbye, the model found: after the final barrier no PE can call
`shmem_global_exit`, and a fatal's status is a failure anyway.

*As built: liveness* (phase 1 step 4 slice 1, part C;
`liveness.[ch]`). Every PE's agent watches the deaths it can see: the lead's
agent holds a pidfd for every other local PE, and every other agent one for its
lead. At most once a sweep interval each agent polls the pidfds it holds, with
no wait — one `poll(2)` every `OSOE_SWEEP_NS`, O(L) descriptors on the lead and
one elsewhere — and a PE whose process has ended before its last finalize is a
death: the agent writes one line naming it, then declares the node's abort as a
fatal naming it, which the abort carries to every PE of the job (above). Not if
the word is already set: a PE that an abort ended is dead too, and its death is
the abort's own (`abort_exit_model.py`). A pidfd names a process rather than a
number, so, unlike `kill(pid, 0)`, a recycled pid cannot hide a death; only a
pid recycled before its pidfd opened could, and they open at the first check
after the node's rendezvous, which every PE's pid is published before. Until the
agent runs, the sweeps of init's waits after that rendezvous do the checking, so
a death during init is found in every build: first built, a lead that died in
the job's rendezvous left its non-leads waiting there for the launcher (found in
review; `shmem.dead-pe.lead-in-init`), and the connection area's first wait did
not sweep until the third review. A lead that dies inside the node's rendezvous
itself is still the launcher's to find. Where no pidfd can be had — out of descriptors,
or a kernel or sandbox refusing the call — the PE is watched by `kill(pid, 0)`
instead, after one line saying so, rather than the job ending for it; and init
raises the soft `RLIMIT_NOFILE` to the hard limit, as §6.2 always said it did
and the code had not. Like the reclaim of a dead job's objects (§6.2), this
assumes a node's PEs share a pid namespace. The design put the lead's pidfds on
its ring, which a single-node job does not have, and had non-leads check the
lead with `kill(pid, 0)` at a library wait's sweep, which a computing PE never
reaches; the agent's own sweep serves both, the same way on every job. It is up
to one sweep interval late, and a completion on the ring would not be — a
refinement the multi-node lead can take later, at the price of a seam function.
The model's `pidfd` option is that ring version, a completion that wakes the
lead at once; as built, the lead's own sweep wakes it within a sweep interval,
and that is what the model's Part B case checks. `shmem.dead-pe` kills a lead or
a sibling, on one node and on two: every survivor leaves with the failure
status, the computing ones included.

*As built: keepalive* (phase 1 step 4 slice 1, part D; `wire.c`). Every
node-pair socket sets `SO_KEEPALIVE` with `TCP_KEEPIDLE` 60 s, `TCP_KEEPINTVL`
10 s and `TCP_KEEPCNT` 6: a connection idle for a minute is probed every ten
seconds, and six probes nobody answers end it with `ETIMEDOUT`, which ends the
job with a fatal saying which node stopped answering. A node that vanishes
without a FIN — power lost, a cable cut, a partition — is found one to two
minutes after its connections go quiet. Keepalive asks whether the peer's
*kernel* is there, which contention between nodes has not been seen to stop -
not tested at scale - though on one
overloaded machine running many logical nodes, whose loopback carries every
probe and answer, the backlog drops both, and a healthy job can end so (the
review of v0.2 step 4's merge point, correctness Q2-1; the README says so for
`osoe-launch`, where `OSOE_KEEPALIVE=0` loses nothing): a stopped node's kernel still
acknowledges, so a stopped node is waited for, as the no-fatal-timeouts decision
requires. A host that answers nothing at all is declared dead: powered off,
partitioned, or a virtual machine paused for longer than keepalive's bound.
`shmem.stopped-node.keepalive` stops one for 6 s against keepalive's tightest
setting, 4 s, and the quiet completes. `TCP_USER_TIMEOUT` is not set. It bounds
how long sent data may go unacknowledged, and Linux applies it to zero-window
probing too, so a peer merely stopped with a full receive buffer would be
declared dead after it: a fatal timeout by another name. With data in flight to
a vanished node - which keepalive does not probe - the kernel's own
retransmission limit ended the connection instead, `tcp_retries2`, about
fifteen minutes by default. *Since 2026-09-28* the lead asks the kernel the
same question keepalive asks, of the data: once a keepalive interval,
`TCP_INFO` on each connection, and a peer whose kernel has acknowledged
nothing for keepalive's whole budget (idle, then count probes an interval
apart) while segments wait for acknowledgement ends the job, naming it, as an
idle one would have (`osoe_progress_check_silent`). A peer merely stopped is
not found: its kernel acknowledges what arrives, and one whose receive buffer
is full is probed through a zero window, which leaves nothing outstanding.
`shmem.stopped-bulk` holds that second half: 64 MiB put toward a node stopped
for 15 s, keepalive at its tightest. Without the condition that something is
unacknowledged the check ends that job at about 12 s, and no other test notices.
The deadlock detector made this needed, since its probe is data in flight
toward a node that may have vanished in an otherwise idle job;
`tier3.*.vanished-node` found it. `OSOE_KEEPALIVE=IDLE,INTERVAL,COUNT` overrides
the values, and 0 turns keepalive, and this check, off; tests make it short.
`tier3.*.vanished-node` cuts a node's link in both directions once the job is
ready and quiet — no FIN, no reset — while the other node waits on it idle, and
the job ends within keepalive's bound, every rank with the failure status. Quiet
matters: the first run cut the lossy link while one side still had a
retransmission outstanding, and that side, correctly, waited on `tcp_retries2`
rather than keepalive (TCP probes only a connection with nothing
unacknowledged); the cut now comes two seconds after the job is ready.
`tier3.*.vanished-node-inflight` puts data in flight on purpose after the cut,
and the job ends within the same bound.

#### The development watchdog, retired

Until §7.1 step 4, a PE blocked in `wait_until` or a barrier whose sibling had
died waited for the launcher's timeout, so the development presets carried a
watchdog: at each sweep of a library wait (§3.10), a `kill(pid, 0)` of every
local PE, and on a dead one a loud failure naming the PE, the routine the waiter
was in, and the control segment to point `osoe-stat` at. Off in `release`, so
that production could not come to depend on it.

Step 4 made detection the library's, in every build: the agents' pidfds find
any local death, and the abort ends every PE (above). The watchdog then added
only its message, mostly unseen, since the agents usually found a death first,
and the author retired it (2026-09-23): the ending agent now names the routine
its PE was waiting in (abort.c), from a pointer each wait publishes beside its
blocked record (§3.14; `blocked.h`) - the innermost wait's, the outer one's again
once an inner one ends, as a barrier's quiet does - so the message is in every
build (`shmem.death-in-wait.*`, on one node and on two). One
thing went with it: every waiting PE checked every sibling, so a lead alive but
wedged still had its siblings' deaths found; now only the lead's agent watches
them. `OSOE_WATCHDOG` and `OSOE_LIVENESS` are retired, and a job that sets
either is told so once. What a wait's sweep does is what remains of it
(`sweep.c`): it counts, so that a blocked PE is seen to move (§3.14); polls the
pidfds while the agent does not run yet, during init; and reads the abort word.

### 3.16 Memory-order discipline

> **ISO C11 `<stdatomic.h>` only, with a named memory order at every access to a
> shared protocol word.** No compiler intrinsics, no assembly, no `__atomic_*` or
> `__sync_*` builtins.

Rules:

- **Loads** name `memory_order_acquire`, except the load half of a two-sided
  protocol (below), which names `memory_order_seq_cst`.
- **CAS and `fetch_add`/`fetch_sub`** name `memory_order_seq_cst`. They compile to
  locked instructions on every target whatever order is named — `lock cmpxchg`
  and `lock xadd` on x86-64, LSE instructions or libgcc's outline helpers on
  aarch64 — so naming `seq_cst` keeps the text honest.
- **Plain stores** name `memory_order_release` for single-writer publication,
  where one side stores and the other only loads.
- **Two-sided ("Dekker-shaped") protocols** — each side stores its own word and
  then loads the other's — name `memory_order_seq_cst` on **both** stores, or use a
  locked operation, **and on both loads**. The §4.9 watch-table insert against the
  `ivar` store is one; `shmem_signal_set` against a signal wait is another. It is
  the only one in the tree today: its loads are `osoe_transport_load` (the
  waiter's predicate) and `notify`'s load of the watch (the writer's). Message
  passing — publish data, then a flag; read the flag, then the data — is not
  two-sided, and release and acquire are exactly right for it.
- Implicit-order calls (`atomic_load`, `atomic_store`, operator forms) and
  `memory_order_relaxed` are forbidden on protocol words, so every access shows its
  order.
- **Three of the six orders.** ISO C11 (§7.17.3) defines `relaxed`, `consume`,
  `acquire`, `release`, `acq_rel` and `seq_cst`, all standard. The project uses
  `acquire`, `release` and `seq_cst` as above, and `relaxed` nowhere on a protocol
  word. `consume` is promoted to `acquire` by every compiler and buys nothing;
  `acq_rel` is what a CAS or `fetch_add` would otherwise name, and naming `seq_cst`
  there keeps one order for every locked operation. (An earlier draft called the
  last two GCC extensions. They are not; the rule stands, that reason did not.)
- **A futex word that is its own predicate** — `ready` in every segment header,
  the agent's `ready` — is a third shape, stored **`release`**. One side stores
  the word and wakes; the other loads it and sleeps on the value it saw. It is not
  a Dekker pair: the waiter's second half is the kernel's compare, which futex(2)
  guarantees is "totally ordered with respect to concurrent operations performed
  by other threads on the same futex word". C11 has no say in that; what it does
  govern is the header written before the word, and publishing that is exactly a
  release. The same holds for `failed` (checked by waiters sleeping on another
  word, so a wake that races it is caught by the next sweep whatever the order)
  and for a landing slot's `ready`, whose waiter sleeps on a separate wake word
  bumped afterwards by a `seq_cst` RMW. (The tree had used both orders for these,
  second review atomics F4; slice 2 unified them on `seq_cst`, and the follow-up
  set them to the minimum, below.)
- Configure fails if the compiler defines `__STDC_NO_ATOMICS__`.

**Why two-sided protocols need `seq_cst` on both sides.** GCC 14 compiles both `release` and
`seq_cst` stores to `stlr` on aarch64, but on x86-64 a `release` store is a plain
`mov`, which sits in the store buffer where another core cannot see it, while a
`seq_cst` store is `xchg`, which forces it out. In a two-sided protocol, both
sides can then miss each other's store and a wake is lost — shown by the review's
model (variants V4, V4b, V4c lose wakes and V5 does not) and confirmed by
disassembly in the development VM. §3.5's earlier claim that missing orders cause
corruption "never on x86" is true for single-writer publication and false for
this shape.

The loads need it too, and that is the language's rule before it is any
machine's: C11 puts only `seq_cst` operations in its single total order, so a
`seq_cst` store followed by an `acquire` load of another word may be reordered.
An earlier draft named the loads `acquire` and was right by accident: on aarch64
GCC emitted `ldar`, which the architecture does not let pass an earlier `stlr`
(the two are "RCsc"). Built for Armv8.3 or later — `-mcpu=cortex-a76`, a Pi 5, or
`-mcpu=native` there — GCC 14 emits an acquire load as `ldapr` ("RCpc"), which may
pass it. *Measured* on the development VM, whose cores have `lrcpc`
(`docs/design/evidence/fence-litmus/store-buffer-litmus`, 2 million rounds, three
runs): `stlr; ldapr` showed both sides missing each other's store 289, 4,449 and
278 times; `stlr; ldar`, 0. A `seq_cst` load is `ldar` on every aarch64 target
and a plain `mov` on x86-64, so naming it costs nothing.

**Every choice above, derived from the C11 model** (2026-09-21). The rules were
first written from compiler output and measurement; this is the check that each
is what the language requires and no more. The question for each shape is which
outcome C11 forbids, and with what.

| Shape | C11's requirement | Order | Where |
|---|---|---|---|
| Single-writer publication (message passing): data, then a flag; the reader loads the flag, then the data | A release store that an acquire load reads makes everything before the store happen-before everything after the load | **`release`** store, **`acquire`** load | counters, `blocked`, `armed_token`, a buffer's `filled` and `seq`; the wire's `produced` (whose wake word is bumped after it by a locked add) and `consumed`; the agent's `running` (phase 1 step 4); `called`, a caller of `shmem_global_exit` recorded before its declaration's CAS, which the agent's acquire of the word then carries; `waiting_in`, the routine a wait publishes for its agent (the watchdog's retirement). *`acked` and the buffers' `head` were here until phase 1 step 3 slice 2, when waiters began to register on them (next row)* |
| A futex word that is its own predicate | Only the header written before it, which is a release; the kernel orders the compare (futex(2)) | **`release`** | `ready` in the node, job, connection-area and wire headers; the agent's `ready`; `failed`; a landing slot's `ready`; the node's `lead_agent_running` (phase 1 step 4), whose store also follows the lead's removal of the node's heap and control-segment names (§6.2) |
| Two-sided (store buffering): each side stores its own word, then loads the other's | Both-miss is forbidden only among `seq_cst` operations, which alone are in the single total order *S*; an `acquire` load is not in *S* | **`seq_cst`** on **both stores and both loads** (an RMW counts as the store) | §4.9's watch: `osoe_transport_watch`/`unwatch` and the rmw write; `osoe_transport_load` and `notify`'s load of the watch. The wait words (`base/wait-word.h`, `wait_word_model.py`, slice 2 of step 3): a waiter's registration and its check of the value — `acked`, or a reservation's loads of the buffers' `head` — against the agent's store of the value — `acked`, or open's hand-over of `head` — and its load of the count. |
| CAS, `fetch_add` and the other RMWs | Any order is atomic; `seq_cst` puts them in *S*, which the two-sided rule needs of the RMW that is its store | **`seq_cst`** | everywhere; among them the abort word, installed whole by one CAS, and `ending`, the claim on ending a process (phase 1 step 4) |
| A queue in one word, then a wake word: a waiter's ticket and a giver's serve are RMWs on the same word; the giver then bumps a futex word; the waiter loads that word, then checks its ticket (slice 2 of step 3) | Not store buffering: two RMWs on one word are ordered by its modification order, so a give always sees every ticket taken before it. The waiter's check needs only to follow the giver's serve when it has seen the giver's bump — a release RMW read by an `acquire` load; a bump it has not seen is the futex compare's to catch | RMWs **`seq_cst`** (the row above); the waiter's loads of the wake word and of the state **`acquire`** | `osoe_credit_take`, `_give`, `_served` (`credit_wait_model.py`) |
| Two-sided, with a claim: the writer's store is an RMW on a bitmap and the lead's a flag, and a writer that reads the flag raised takes it down with a CAS before waking (slice 3) | The store-buffering outcome - the lead's recheck misses the bit and the writer misses the flag - is forbidden only among `seq_cst` operations; the claim CAS is an RMW in *S*, and a writer that loses it read a later value of the flag, so the lead's recheck ordered after that value sees its bit | **`seq_cst`** `fetch_or`, flag store, flag load and CAS; `seq_cst` recheck loads | `osoe_connection_area_mark`, `osoe_progress_host_wake` (an owner's, `owner-map.h`), `osoe_progress_host_poll` (`lead_wake_model.py`) |
| `shmem_fence` over plain stores (W→W) | Nothing, for a race-free program: its flag is an AMO, a `seq_cst` store and so a release. Table 14's plain stores are a data race C11 gives no meaning; the weakest construct ordering earlier stores before later ones is a release fence | **`atomic_thread_fence(release)`** | `osoe_transport_fence` |
| `shmem_quiet`, `pe_quiet`, the barriers (W→R) | "Visible to all PEs" on return is the store-buffering shape again; C11 forbids its bad outcome with a `seq_cst` fence between each side's store and load | **`atomic_thread_fence(seq_cst)`** | `osoe_transport_wait` |

And what changed when the tree was held to that table:

| Was | Required by C11? | Now | Cost |
|---|---|---|---|
| Two-sided loads named `acquire` | **Yes, `seq_cst`**: an `acquire` load is outside *S*. Right by accident while GCC emitted `ldar`; built for Armv8.3+ it emits `ldapr` and lost 278–4,449 wakes in 2 M rounds (`evidence/fence-litmus/`) | `seq_cst` | none: `ldar` on aarch64, `mov` on x86-64 |
| Quiet's point a `seq_cst` store to a thread-local word | **No, it guaranteed nothing**: C11 relates that store to no other location. On aarch64 (`stlr`) loads after quiet missed another thread's store ~3.6 M times in 6 M rounds, as often as with nothing there | `seq_cst` fence | `dmb ish`; `mfence` for `xchg` on x86-64 |
| Fence's point the same store | **Too weak in C11, and a full fence was too strong**: fence needs W→W only | release fence | none on x86-64; `dmb ish` on aarch64 |
| Futex words that are their own predicate at `seq_cst` (slice 2), and the landing slot's `ready` | **No**: the kernel orders the compare; C11 governs only the header before it | `release` | an `xchg` fewer per word on x86-64 |
| The wire's `produced` and `consumed` at `seq_cst` | **No**: message passing into a separate wake word, not two-sided. `wire_model.py` now runs every case under SC and x86-TSO with both as release stores: the design holds, each broken variant is still caught | `release` | an `xchg` fewer per send and per receive on x86-64 |

The measurements are evidence that the gaps are real on a shipping target, not
the argument; the argument is the C11 column. §4.6 has the fence and quiet
derivation in full.

**Consequences:**

- **Futex words** are `_Atomic uint32_t`, with a `_Static_assert` that its size
  and alignment equal `uint32_t`'s (true for GCC on aarch64 and x86-64).
- **Outline atomics** on ARMv8.0 cores without LSE, such as the Pi 4's Cortex-A72,
  link statically from `libgcc.a`; a test binary's `NEEDED` list is libc only
  (review, lead-verified), so invariant 15 holds.
- **No spin-before-block** (§3.11): there is no ISO C relax hint.
- **Two fences** put the calling thread's plain stores under `fence` and `quiet`
  (§4.6 rule 2, `base/order.h`): `fence` begins with
  `atomic_thread_fence(memory_order_release)` — no instruction on x86-64, a
  `dmb ish` on aarch64 — and `quiet`, `pe_quiet` and the barriers with
  `atomic_thread_fence(memory_order_seq_cst)` — an `mfence`, a `dmb ish`. §4.6
  derives each from the C11 model, and says why the `seq_cst` store to a
  thread-local word they replace guaranteed neither.

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
space — 256 GiB for 256 PEs at 1 GiB, comfortable in 48-bit aarch64. Heap pages
are committed only when touched. They are ordinary pages: a hugetlbfs option,
reserved up front with a fallback when there are none, was designed and is not
built (§6.2). `SHMEM_SYMMETRIC_SIZE` must agree across PEs and is checked at
rendezvous.

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
to all three, but it does not exist in a 39-bit or x86-64 address space. GCC
11's ThreadSanitizer (Rocky Linux 9; v0.2 step 1) has an older layout of its own,
which allows only below `0x2_0000_0000`, the band
`[0xaaaa_0000_0000, 0xaaaf_0000_0000)` and above `0xffff_0000_0000`. So:
- **The lead chooses `B`** by mapping its own heap at the first of a short
  candidate list that succeeds: `0x1000_0000_0000`, `0x3000_0000_0000`,
  `0x100_0000_0000`, `0x10_0000_0000`, `0x1_0000_0000`. That is a plain build,
  ASan, TSan, a 39-bit address space and GCC 11's TSan, in order; the last
  holds a heap of up to 4 GiB.
- **It publishes `B` in the control segment** before any sibling reads it.
- **Every other PE maps its own heap at the published `B`**, and fails loudly if it
  cannot. All local PEs run the same binary, so the lead's choice suits them.
- **`OSOE_HEAP_BASE` replaces the list** for systems where none of the candidates
  is free. It must be a multiple of the page, which the mapping needs, and one
  that is not is refused by name - a Pi 5's page is 16 KiB, so a base chosen on
  a 4 KiB machine may not do. Beside it, `OSOE_HEAP_SHIFT` is §8.2's heap-base-shift test mode: each
  PE's own heap past `B` by its rank times the shift.

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

**As built (phase 1 step 6, slice 1; `docs/prompts/done/phase1-step6-slice1-prompt.md`).**
Phase 2 directly: phase 1.5 would have been slow by design and thrown away, and
the wrapper it waited for exists. The decisions are Claude's, for the author's
review; the adversarial review of the slice corrected several claims below.

- **The script gathers all of the executable's writable data, not the user's
  objects.** `tools/wrapper/osoe-statics.ld`, which `oshcc` passes to every
  link of a program, is an `INSERT` augmentation of the linker's default
  script: every `.data` and `.bss` input section (and x86-64's large-model
  `.ldata` and `.lbss`), from any object or archive member, goes into
  `.osoe_statics_data` or `.osoe_statics_bss`, each starting and ending on
  `MAXPAGESIZE` and bracketed by `__osoe_statics_{data,bss}_{start,end}`.
  Gathering by name rather than by object makes the script independent of how
  the program was built - one command or many, archives or not - and the
  wrapper need not know which objects are the user's. What else is gathered is
  harmless, because the region is not shared the way the rejected alternative
  shared it: **each PE maps its own object** over its own ranges, and
  node-mates map it only to serve SHMEM operations that name an offset in it.
  Since an INSERT script's statements are matched before the default script's,
  `.data.*` also takes `.data.rel.ro` - read-only data with relocations, and
  GNU ld's copies of shared libraries' read-only objects - which so loses
  RELRO's protection: hardening, not correctness. `.tdata` and `.tbss`, the
  GOT, the constructor tables and GNU ld's copies of shared libraries' writable
  variables (`.dynbss`: `stdout`, `environ`) stay out. Tested with GNU ld; lld
  puts those copies in `.bss`, where they are gathered, and gold and mold do
  not take INSERT. Proved in the VM before anything depended on it: gathered
  sections, the brackets, PIE, a static archive, one-command builds, TLS left
  out, and a 64 MiB `.bss` that adds nothing to the file.
- **At `shmem_init`**, if the brackets are there (weak, default visibility -
  a hidden weak reference would be settled when the library is linked), each
  PE creates `<control>.statics.<local id>` beside its heap, writes into it the
  ranges' pages that hold a nonzero byte - an untouched `.bss` page is the
  kernel's zero page, so a large `.bss` costs neither a copy nor `/dev/shm`
  until the program touches it (on tmpfs a read allocates a page too) - and
  maps it `MAP_SHARED | MAP_FIXED` over the two ranges, the data at offset 0
  and the bss after it, with an alias of the whole object for the transport.
  **The window between the copy and the mapping** must write nothing in the
  ranges, or the write lands in the pages the mapping hides: the application
  is inside `shmem_init`, the library's threads start later, signals are
  blocked across it so a handler's write cannot be lost, and with a static
  `libosoe` - whose own globals are then in the ranges - the code in it keeps
  its results in locals. The two mappings are made by the system call itself,
  since a sanitizer intercepting `mmap` would reset the shadow of the
  program's globals. A PE's lead publishes `ready` before its own copy, so no
  PE waits on another's. Node-mates map each other's objects as they map the
  heaps; the lead removes the names with the heaps' (§6.2); the capacity check
  counts them, and its refusal names them; and the rendezvous refuses local PEs
  whose statics differ in size, which is PEs running different programs.
- **`OSOE_REGION_STATICS`** at the boundary (§4.10): `symmetric.c` finds the
  region an address is in, heap first, and every API path passes `(region,
  offset)` - RMA, AMOs, signals, waits, locks, the collectives' intranode phase (a
  member's source read through its statics mapping, as through its heap) - so
  the wire and both transport paths carry statics exactly as they carry the
  heap. A PE's own statics are addressed at the program's own addresses, never
  the alias, so that a transfer between overlapping parts of one static is one
  `memmove` over one range - through the alias, `memmove` saw two ranges and
  copied forward (the review's first finding). `shmem_ptr` and
  `shmem_addr_accessible` answer for statics as for the heap. The control
  segment (layout 17) has each PE's `pe.statics_bytes` and `pe.statics_name`,
  which `osoe-stat --gc` removes with the heaps, and a wait on a static records
  `OSOE_BLOCKED_DETAIL_STATICS_OFFSET`.
- **`-no-pie` stays**, for a program that sends a static's address to another
  PE as data - the addresses agree only when the executable does not move.
  Every SHMEM operation is by offset and works under PIE too (a test builds the
  statics test `-fpie -pie`), and a caller's own `-fpie` with `-pie` wins over
  the wrapper's. **`-static` is refused**: it would gather the C library's own
  state - its allocator's arenas, stdio, loader locks - which a `fork()` in the
  child writes before it returns.
- **A program linked without the script has no symmetric statics**, and a
  static passed where a symmetric address is required is refused by name,
  saying it was not linked with `oshcc`.
- **Sessions.** At the last `shmem_finalize` a PE unmaps its node-mates'
  objects and its alias, but not its own mapping over the ranges, which is
  where its globals now live - so the sharing outlives the library. The next
  `shmem_init` copies them into a new object, reading only the parts of the old
  one that hold data (`SEEK_DATA`, `SEEK_HOLE`): reading a hole of a tmpfs
  mapping allocates it, and a scan of every page had made the old object whole
  just before it was freed.
- **Costs.** Initialized `.data` that no PE writes used to be the page cache's,
  one copy a node; now it is one copy a PE, in `/dev/shm`. A 200 MiB table
  written at initialization costs 50 GiB on a node of 256 PEs; the capacity
  check says so.
- **Known limits.** A child made by `fork()` after `shmem_init` shares its
  parent's statics, as it shares the heaps (§6.4): its writes to globals are
  the parent's, and a child that calls `exit()` rather than `_exit()` runs the
  program's `atexit` handlers and destructors on them; `fork` followed by
  `exec` - `system`, `popen`, `posix_spawn` - writes none. The window above is
  closed to signals but not to a thread the application, or a library it
  linked statically, started before `shmem_init` - a thread pool whose
  workers take a lock in their statics could lose the unlock - nor to a later
  session's threads. Pages set up before `shmem_init` lose that setup: a
  static buffer registered for DMA or with io_uring keeps the old pages,
  `mlock`, `madvise` and `mprotect` settings are gone, and a `PROT_NONE` guard
  page in the ranges faults in the copy. A `MAP_FIXED` that fails after
  unmapping, as older kernels may, leaves a static `libosoe` in a hole, where
  the message cannot be printed. Not in this slice: statics of shared
  libraries the program loads, and statics under an RDMA backend's
  registration (they register as the heap does, when there is one). *A
  structural alternative the review offered: remap once at load time, from an
  `.preinit_array` entry `oshcc` adds, into an unnamed object that
  `shmem_init` names - which closes the window and makes every session's copy
  unnecessary, at the price of a remap in every program `oshcc` builds.*

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
  alignment, which every candidate base in §4.1 is up to 4 GiB. Above that,
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
  - **Decimal notation, with an optional exponent.** The spec says "floating
    point value" without defining it, and a C programmer reads `1e3` as one; an
    earlier draft refused it, which the second review found surprised users
    (conformance F9). The exponent is applied in the same exact integer
    arithmetic, never through `strtod`. A sign, surrounding space, an exponent
    with no digits (`1e`, so a typo does not become a size), or any character
    other than a multiplier after the number is an error, which the spec lets
    init report by terminating. Characters after the multiplier are ignored, as
    the spec requires. `SMA_SYMMETRIC_SIZE` is read when `SHMEM_SYMMETRIC_SIZE`
    is unset, like every other variable (Annex F).
  - **A value that does not fit in 64 bits is an error.**
  - The grammar lives in `base/size.c`, with its tier-1 test next to it.

**Default size.** Without `SHMEM_SYMMETRIC_SIZE` a heap is 256 MiB. Heap objects
commit pages only when touched, so the default costs address space: L × 256 MiB
per process, 64 GiB at 256 local PEs. A size of 0 is refused at init by name: a
heap in which every allocation fails is never what was meant (second review,
omissions 11).

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
tree topology, counters. *Verified against each collective by the conformance
reviews of rounds three to six, the spec read the author closed on 2026-10-04;
the deprecated routines never touch their `pSync` or `pWrk` (phase 1 step 5
slice 3).*

**Contexts** are purely local (`shmem_ctx_create` is not collective) and must
require no symmetric memory. A context is a sequence-number space. Confirm this
stays true as contexts are fleshed out; a context needing symmetric scratch
would be genuinely difficult. *It stayed true* (phase 1 step 8 slice 1): a
context is process memory and one cache line of the node segment, nothing
symmetric.

### 4.6 Ordering and completion

Two completion domains that cannot be collapsed:

| Domain | Complete when | Evidence |
|---|---|---|
| Requests (put, signalled put, non-fetching AMO) | Applied at target | Peer's cumulative acknowledgement of applied segments (§3.4) |
| Get replies | Delivered locally | Bytes landed in the landing zone, or directly in a symmetric `dest` (§3.12) |
| Memory stores by the calling thread | Visible to other cores | A serializing fence in the calling thread (below) |

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
   including a local PE under §8.1's force-remote flag. A thread's store can then never race that same thread's queued put to the
   same object: either both are local and ordered by the thread itself, or no
   pointer exists.
2. **`fence` begins with a release fence in the calling thread, and `quiet`,
   `pe_quiet` and the barriers with a `seq_cst` fence** (§3.16). The first orders
   the thread's earlier stores before its later ones; the second orders every
   earlier access before every later one.

In normal mode `barrier_all` already happened to conform, because its arrival
`fetch_add` is a locked instruction. The rule makes that deliberate.

**Rule 2 from the C11 model.** Two shapes, and C11 says different things about
each.

- **Fence is W→W**: `store A; shmem_fence(); store B` must not let a target see B
  without A. In a program C11 calls race-free, B — the word the target waits on —
  is written by an AMO (spec 1.5 change #17 requires it of wait variables), which
  is a `seq_cst` store and so a release: every store before it, A included,
  happens-before the target's acquire load that reads it. *C11 needs nothing from
  fence here.* What is left is Table 14's promise for plain stores — a store
  through `shmem_ptr`, a put's `memcpy` read by a target that did not synchronize
  — which C11 calls a data race and gives no meaning. No C11 construct can
  guarantee that; the weakest one that orders earlier stores before later ones on
  every mapping is **`atomic_thread_fence(memory_order_release)`**, which is what
  fence uses: no instruction on x86-64, whose stores stay in order, and a
  `dmb ish` on aarch64.
- **Quiet is also W→R**: everything is "complete and visible to all PEs when
  shmem_quiet returns" (spec §9.12.2), so a load after quiet — an atomic fetch,
  a wait's predicate — must not be satisfied before a store before it is visible.
  That is the store-buffering shape, and C11 forbids its bad outcome with
  **`atomic_thread_fence(memory_order_seq_cst)`** between each side's store and
  load (the fence rules of C11 §7.17.4 and the `seq_cst` total order). A release
  fence does not. So quiet, `pe_quiet` and the barriers use the `seq_cst` fence:
  an `mfence` on x86-64, a `dmb ish` on aarch64. The barriers would not need it
  in C11 terms — their arrival RMW and release already synchronize — but the spec
  defines `barrier_all` as a quiet and a sync, and one fence is small beside a
  barrier.

Rule 2 was once one `seq_cst` store to a thread-local word nothing reads
(`base/order.c`). C11 relates that store to no other location, so it guaranteed
neither shape; on x86-64 it compiles to `xchg`, which gave both by the hardware's
courtesy, and on aarch64 to `stlr`, which orders earlier accesses before *itself*
and nothing after it. *Measured* on the development VM
(`docs/design/evidence/fence-litmus/`):

| Shape | Nothing between | `stlr` (the old rule 2) | `dmb ish` (either fence now) |
|---|---|---|---|
| W→W, fence (40 M rounds; a third thread contends the data line) | 921 reordered | 0 | 0 |
| W→R, quiet (6 M rounds) | ~3.4 M both-zero | ~3.6 M both-zero | 0 |

So fence held on these cores by their implementation and not by any guarantee,
and quiet did not hold at all for a load the application made directly after it.
`base.serializing-fence` now checks the W→R row on every build. **Run the two
litmus programs on a Pi 4 (Cortex-A72) and a Pi 5 (Cortex-A76)** when they exist.

#### Data structures

**Thread level.** Phase 1 provides up to `SHMEM_THREAD_SERIALIZED`: `SINGLE`,
`FUNNELED`, and `SERIALIZED` are all supported, and a request for `MULTIPLE`
provides `SERIALIZED`, which §9.2.1 permits. *Since phase 1 step 8 slice 2*,
`shmem_init_thread(SHMEM_THREAD_MULTIPLE)` provides `MULTIPLE` (§4.11, as
built); `shmem_init` still provides `SERIALIZED`, so a program that never asks
for threads pays nothing for them. The structures below assume that at
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
  library relies on it rather than adding a serializing fence to every entry point.
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
value that `quiet` waits on follows the same pattern. *As built* (phase 1 step 3
slice 2): `acked` has its word, with a count of registered waiters beside it, so
the agent wakes only when a quiet waits (`base/wait-word.h`, §3.11). Its store
and the waiter's load are `seq_cst`, since the count makes the wait two-sided
(§3.16).

**Blocking gets and fetching AMOs** do need per-operation completion: the caller
waits for *its* reply, not for a byte count. Up to `SERIALIZED` a PE has at most one
such operation outstanding, so its landing zone holds one completion slot with its
own wake word.

*As built: nonblocking gets* (after phase 1 step 4, the author's decision on
the strided forms, 2026-09-24). A PE's landing zone holds, after its blocking
slot, `OSOE_GETS_IN_FLIGHT` request records (default 64) and an arena of
`OSOE_GET_LANDING_BYTES` (default 256 KiB) that their replies share (control
segment layout 16). A nonblocking get asks and returns: each piece - no larger
than a chunk, the wire's limit or the arena - takes a record and a range of the
arena, writes the range into the record and then its token, with release,
adds its bytes to `bytes_issued`, takes a reply credit and goes out. The token
carries the record's index in its low 16 bits and a top bit that no blocking
token has, so **the wire format is unchanged**: the reply echoes the token, and
the lead finds the record by it and refuses a reply whose token is not the
record's or whose bytes would fall outside the record's range, as it refuses an
unarmed reply to the blocking slot (§3.4's trust boundary). The lead lands the
bytes in the arena, then adds the reply's length to `bytes_landed` - seq_cst,
since it is the value half of the two-sided wait on the wake word beside it
(base/wait-word.h: a wake only while the PE waits) - and **returns the credit
as the reply lands**, where a blocking operation's caller returns it after
reading its slot: the PE copies nonblocking replies out only at its next wait,
and holding their credits until then would keep its node-mates' gets queued
behind replies long delivered. That is the return point the credit's own
comment has named since the credits were built (reply.h), and the credit
model's take and give are the same operations whoever gives. Every wait -
quiet, pe_quiet whatever PEs it names, and a barrier's opening quiet - waits
for `bytes_landed == bytes_issued` and copies each reply to its `dst`, and so
does finalize's barrier; an iget ends with a wait for its gets alone
(`osoe_transport_wait_gets`), since a whole wait would also complete, and pay
the acknowledgement of, the writes to that PE's node, which an iget does not
promise. The lead retires a record as its reply lands, so a duplicate reply is
refused, and checks a reply with no bytes as it checks one with some - the
parser hands only replies with bytes to `destination()`, which is where the
check had lived, for the blocking slot too (review of the nonblocking gets). A
count of landed bytes past those issued is fatal rather than a wait that can
never end. **An issue that finds the records or the arena used up completes
what is in flight first**, which bounds the zone without anything failing; the
design's other release, the PE's agent copying replies out early, is not built.
The record's fields and the counters use release, acquire and seq_cst, never
relaxed, as §3.16 asks of every protocol word. *Replaced in phase 1 step 8
slice 2* (§4.11, as built): the records and arena became 1 KiB extents claimed
by compare-and-swap, `OSOE_GETS_IN_FLIGHT` is retired, and the blocking slot
became one per thread.
A get to a node-mate is a `memcpy` before it returns, as ever. The simple
equality is right while one thread issues and completes; `MULTIPLE` needs the
generation scheme below (§4.6, `get_completion_model.py`). Packed replies,
rather than a chunk-sized slot per get, are for the corpus's one CPU user of
`get_nbi`, bale's getput, which issues many one-word gets to random PEs before
a barrier: with slots of a chunk each, 256 KiB would hold four of them.

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
    serializing_fence();                          /* memory stores (above) */
    request_ack_through(pe->dirty, pe->ndirty, pe->high_seg);  /* §3.4; the lead seals */
    for (i = 0; i < pe->ndirty; i++)
        wait_until(acked[pe->dirty[i]] >= pe->high_seg[pe->dirty[i]]);
    wait_until(pe->bytes_landed == pe->bytes_issued);
    copy_out_completed_gets();                    /* landing zone -> private dest */
    pe->ndirty = 0;
}

void shmem_fence(void) {
    serializing_fence();
    /* peers already acknowledged through high_seg are skipped: asking again
     * would only make the lead send another request */
    request_ack_through(pe->dirty, pe->ndirty, pe->high_seg);
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
  unconditionally is conforming** — with one exception the earlier text here
  missed: spec §7 requires a non-null pointer for every member of
  `SHMEM_TEAM_SHARED`, which is why that team is the singleton `{self}` under
  §8.1's force-remote flag rather than the node (second review, conformance F2).
  The intranode case is essentially free from the shared heap.
- `shmem_ptr` returns `NULL` for any target reached over the network, including a
  local PE under §8.1's force-remote flag, which makes the pointer unavailable
  without changing the route (§4.6, §8.1). It also returns `NULL` for an
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
(`shmem/src/watch-range.h`), which is the same word §4.11 item 3 publishes per
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
`docs/reviews/archive/watch-notification-options.md` has the measurements and the
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
  and the kernel wakes a waiter only when the two intersect; `base/futex-futex2.c`
  (and the single-node build's `base/futex-classic.c`, whose classic calls take
  the same mask) passes `FUTEX_BITSET_MATCH_ANY` today. A waiter could set the bits of its
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
Nothing in §4 should know that. The core-API study (`docs/reviews/archive/core-api.md`
§8.3) proposed naming the boundary in these terms so a future RDMA backend can
replace the handlers without touching this section. It costs one internal header,
`shmem/src/transport.h`, which states the full contract. This section records the
decisions in it. The core-API study named six operations; the seventh, `load`,
closes a gap found against it: without it, `shmem_test` and the `wait_until`
predicate would read the variable directly, which is incoherent on a network whose
NIC atomics the CPU cannot see.

**Its sibling, and what else crosses** (phase 1 step 2, slice 3). The seven
operations are the whole of the *point-to-point* boundary, and they stay
PE-addressed. Beside them `shmem/src/internode.h` carries the internode phase of
a collective (§5.3): a team's node set given once at team creation, an arrival
per `(team, epoch)` going down, and a release upcall per `(team, epoch)` coming
back on the node's progress thread (§10 records why this reversed the step-2
decision) - and, since phase 1 step 5's second slice, the data collectives'
internode phase as a blocking call carrying the operation and the element, with
a generic implementation over this boundary that every backend may call
(§5.8). Two more things cross that the original contract left out, and both
are now declared in `transport.h`: the backend's **lifecycle** —
`osoe_transport_init` starts whatever the backend needs, AM's connection area,
wires and engine included, before the agent thread exists, and
`osoe_transport_fini` frees what is left after that thread is joined — and the
**agent thread**, which the library lends the backend through
`osoe_transport_agent_pass`, `osoe_transport_agent_wait` and
`osoe_transport_agent_exit` and nothing else (§3.12; §10's "boundary progress
call"). The backend owns the agent's idle wait, because only it can wait on its
own events and the wake word in one call, and it gets a last turn on the thread
before the thread returns, because what the thread owns only it can finish
(phase 1 step 3; the §10 entry has why the first shape was wrong). So `barrier.c`, `agent.c` and `init.c`
name no AM symbol, and the sentence "the SHMEM layer calls only what is declared
here" is true of `transport.h` plus `internode.h`. **It is enforced**: the AM
backend's state moved out of `struct osoe_state` into `am-state.h`, and the tier-1
test `shmem.boundary` (`cmake/CheckBoundary.cmake`) walks the includes of every
file above the boundary and fails if one reaches a header below it. The layers
are five lists in that one script - the API, the runtime, the files on the
boundary, the backend, and the one header only a backend includes - and every
library source file and header must be in exactly one of them. An include is
judged by the file it resolves to, quoted or angled, so no spelling of a
forbidden header gets past. Three files on the boundary reached an AM header,
each known: `node.c` sized the landing zones from AM's chunk, `rendezvous.c`
checks AM's handler checksum, and `transport-local.c` shares a header with AM;
every other file on the boundary is held to the same rule as the files above
it. *(Since v0.2 step 1 `node.c` reads the AM layer's sizes from a file of their
own on the boundary, so two remain; the backend-facing headers are two,
`internode-generic.h` and `internode-backend.h`; and a third check holds every
file outside the internode backend to reaching it through `internode-backend.h`
alone: below, "As built, v0.2 step 1".)*
*(Since phase 1 step 5 slice 2 a second walk checks that nothing outside the API
includes an API header or a public one: the includes run from the OpenSHMEM
layer down to the runtime and never back - the backend's one call up is the
release upcall `internode.h` declares - which was already true and is what a
second API beside this one on the same runtime would need.)* The backend is chosen
by the CMake option `OSOE_BACKEND`, whose only value is `am`. The last two small
gaps are closed as well: `osoe_transport_get` takes `flags`, with none defined
yet, so that `OSOE_TRANSPORT_MORE` and the nonblocking flag arrive without a
signature change *(both did, with the nonblocking gets: §4.6, as built)*; and `osoe_transport_load` and `osoe_transport_set_range` live in
`transport-am.c`, the file a second backend replaces, rather than in the
intranode path's file, which it keeps.

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

Consequently `osoe_transport_direct` returning non-NULL says that *stores* through the
pointer are ordered as described; it does not say that rmw to that PE executes
locally. Nothing above the boundary changes, because the spec already makes a
plain store racing an AMO on the same word undefined (spec §3.2).
`osoe_transport_direct(pe, region, offset)` *is* that function. It returns the target's
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
the call takes a deadline. `osoe_transport_wait`, a blocking `osoe_transport_get` and any
future blocking call sweep on the same schedule as `osoe_transport_watch_wait`. Neither
blocks on the intranode backend, so nothing has forced the question yet; on the
AM backend `osoe_transport_wait` blocks on acknowledgements and on an RDMA backend it is
a completion-queue spin, and the rule is the difference between a hang and a
clean abort.

**A batching hint, `OSOE_TRANSPORT_MORE`.** One flag on put and rmw saying more
operations follow on this context, so a backend may hold one back and submit it
with the next.

*What it defers on the AM backend is the wake, not the seal.* §3.9's aggregation
self-clocks off the send completion, so an operation on an idle connection is
sealed and submitted at once — batch of one — and nothing ever waits for a
buffer to fill. The only thing an application thread controls is whether it asks
the lead for a wake (§3.12), so `OSOE_TRANSPORT_MORE` means "do not wake the lead yet";
the next operation without it, or any fence, wait or get, does. The source-bytes
rule below is satisfied unconditionally here, because reservation copies into
the connection buffer, so the flag is safe and its only saving is futex wakes on
a burst. **Ignoring it is correct and nearly free**: §3.10 requires the safety
sweep to submit anything sealed regardless of signalling, so the worst case is
one sweep interval of added latency, and in practice the next operation wakes
the lead anyway. Its first *acting* caller is a backend that posts work requests
referencing the caller's `src` rather than a copy — an RDMA one — which is the
case the rule was written for.

**Its first caller is `put_signal`**, and §7.1 step 2 is where it arrives. That
routine is the one place in the step that can set the flag *honestly*: the
signal follows the put on the same context and to the same PE, so "more
follows" is a fact about the next statement rather than a guess about the
application. The step's other candidate drivers — the strided and block RMA
loops, and the 1.6 session routines — are all out of that step's scope, so the
item was scheduled for a step in which every driver but this one is absent;
`put_signal` is what keeps it from being deferred again.

The AM path acts on it and the intranode path still ignores it, correctly,
since there is nothing to defer when every put is a `memcpy` applied before the
call returns. `am.wakes_deferred` counts what was held back, because a flag
whose whole effect is to change nothing observable needs a counter to be
testable at all, and the test asserts it **per PE and not per job**: the flag is
only reached when the signalled put actually crosses a node, so on a map with
several PEs to a node the partner is usually a node-mate and the count is
rightly zero. Purely a hint: it changes nothing about completion, ordering or
when `src` may be reused, and a backend that ignores it is correct — which is how
the intranode backend implements it. The rule that makes it safe is that **a
backend may defer an operation only if it has taken its own copy of the source
bytes**; put's local-completion contract is unconditional, so deferring a large
zero-copy put is wrong however the flag is set. It is satisfied unconditionally
on the AM path, where reserving copies into the connection buffer before the
call returns, so what is deferred there is only the wake. It exists because an RDMA backend can post a list
of work requests under one doorbell instead of ringing one per put, which is
where the small-message rate is won, and because §3.9's aggregation wants to be
told when not to seal a buffer early. Its drivers are the 1.6 session routines
(§7.6) and the strided, block and collective loops.

**Nonblocking completion is counters, never per-operation handles.** A
nonblocking operation is a flag on the existing call, completed by the existing
`osoe_transport_wait`. The spec defines `_nbi` completion only at `quiet`, so there is
no per-operation wait call to serve; §4.6's `bytes_issued`/`bytes_landed` pair
already *is* this design; and the core-API study found counters sufficient in all
six production transports it surveyed, with per-operation handles an open
question nobody needed. Recorded before a benchmark applies pressure for them.

| Operation | Guarantee | Intranode backend | AM backend |
|---|---|---|---|
| put | Returns at local completion. Applied later; complete and visible once a covering wait returns. No ordering between puts to one PE except across a fence (spec §9.6.1.1). Notify best effort | `memcpy` into the target's mapping, applied before return, then notify | Reserve and copy into the node-pair buffer (§3.5); the dirty set records the node |
| get | Blocking. Complete per operation: `dst` holds the bytes on return. With `OSOE_TRANSPORT_NONBLOCKING`, complete at the PE's next wait instead. Never fenced | `memcpy` out of the target's mapping, nonblocking or not | Request; a blocking get's reply lands in the landing slot and the calling thread copies it out (§3.12); a nonblocking one's in the landing arena, copied out by the wait (§4.6, as built) |
| rmw | One 4- or 8-byte word. Exactly once. Exclusive against same-width rmw on the word within one atomicity domain, and against waiters and tests (spec §3.2). Fetching: complete on return. Non-fetching: as a put. Notify exact | One C11 atomic on the shared mapping (§3.16 orders), then notify | Handler-time CPU atomic at the target; a fetched value is a value descriptor (§3.8) |
| fence | Writes issued on the context before the call are placed at each target before later ones (spec §9.12.1). Unless the context is `NOSTORE`, the calling thread's stores become visible. Does not wait, and need not send anything | Serializing store only; every write is already placed | Serializing store, then ask each dirty peer not yet acknowledged to acknowledge through its highest segment, and flush (§3.4, §4.6) |
| wait | Every write on the context — to listed PEs, or to all — and every nonblocking get the PE has issued is complete and visible, and unless the context is `NOSTORE` so are the calling thread's stores (spec §9.12.2, §9.12.3). May over-wait | Serializing store only; nothing is ever outstanding | The §4.6 `shmem_quiet`, restricted to the listed PEs' nodes, after the nonblocking gets |
| wait for gets | Every nonblocking get the PE has issued is complete; writes are left for the next wait. An iget's end | Nothing: none is ever in flight | The gets half of the wait (§4.6, as built) |
| load | A coherent load of a word of the caller's own region, 2, 4 or 8 bytes. Exclusive against rmw on it (spec §3.2). For `shmem_test*`, the `wait_until` predicate, and `shmem_signal_fetch` | One `acquire` load of the word | The same: every writer is a CPU atomic on the shared mapping |
| notify | Wakes a PE's waiters whose watched range a write overlaps (§4.9). Exact for rmw, best effort for puts; wakes may be spurious | §4.9 watch slot | The same function, called by the progress thread after it applies a write |

**Ordering is the weak contract, with a per-operation fence flag.** The boundary
promises only what the spec promises: writes to one PE are ordered across a
fence. An operation may also carry `OSOE_TRANSPORT_FENCED`, which places it after every
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
sound would take a serializing fence on every put, which spec 1.5 change #17 does
not require (§4.9). A backend whose puts the CPU never sees need not notify for them.

**Atomicity is the spec's, not osoe's.** osoe gets exclusivity across atomicity
domains for free, because every path is the same CPU atomic on the same mapping
(§4.1). The boundary does not promise it. A backend whose NIC atomics are not
coherent with the CPU can then run one domain on the NIC and another on the CPU,
instead of pushing every on-node rmw through NIC loopback.

**Outside the boundary:**

- **Collectives (§5).** The intranode phase runs on node-segment counters, which
  are the same under any backend. The internode phase crosses `internode.h`:
  the barrier's as an arrival and a release upcall, the data collectives' as a
  blocking call carrying the operation and the element (phase 1 step 5 slice
  2). A backend implements either natively or, for the data half, hands it to
  the generic implementation over this boundary, which uses
  `OSOE_REGION_INTERNAL` for scratch and control words (§5.8). The core-API
  study keeps collectives above its core; osoe keeps their internode phase
  below, because that is where a network's offload is.
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
  *As built:* the nonblocking gets are a flag on get (above), and contexts
  (phase 1 step 8 slice 1) two calls, `osoe_transport_context_create(options)`,
  which may return NULL, and `osoe_transport_context_destroy`, whose implicit
  wait is spec §9.5.3's quiet. A context's PE is a world rank at the boundary,
  as every PE is: the SHMEM layer translates a team context's (`context.c`).

*As built, v0.2 step 1 (`docs/decisions/2026-10-06-single-node-seam.md`): the
internode backend behind one file boundary.* Below the boundary the transport
is in two parts. The part every job uses is `transport-am.c`: the dispatch on
locality, the intranode path it hands node-mates to, the pool of contexts and
their modes, and the fence and wait of a job of one node, which are the calling
thread's fences alone, since every operation there is applied before its call
returns. Everything only a job of more than one node uses is the *internode
backend*, reached through `internode-backend.h` alone - each of its entry
points one of the calls above, or its share of one, for such a job - and on AM
it is the far half (`transport-internode.c`, where the landing zone, the dirty
set, the credits and the waits that the as-built notes of §3, §4.6 and §4.11
place in `transport-am.c` now are), the engine, the wires and the ring, the
connection area, the replies and the messages, the rendezvous of the node
leads, and `internode.h` over AM. `shmem.boundary` holds every other file to
reaching it through that header (`cmake/CheckBoundary.cmake`, its third
check). The transport's settings check now also refuses what the backend needs
of the kernel for a job of this many nodes - on AM io_uring's checks, which
`init.c` made itself before - telling no one, as the futex check does; and the
AM layer's sizes and the wire's settings, which every PE checks as values,
have files of their own on the boundary (`am-configuration.c`,
`wire-settings.c`).

*And the second internode backend* (`docs/decisions/2026-10-06-single-node-build.md`):
the single-node build compiles none of the internode backend's files and
`transport-single-node.c` in their place - every entry point of
`internode-backend.h` and of `internode.h`, of which only the start-up check
and the four `internode.h` calls a job of one node makes are reachable; the
check refuses a job of more than one node by name, and every other entry point
ends the job naming itself as a defect of the part every job uses. It is the
seam's first exercise by a second backend, the RoCE constraint's kind of test,
and it changed nothing above `transport.h`.

### 4.11 `SHMEM_THREAD_MULTIPLE`

> **Designed and model-checked; being built in three slices** (phase 1 step 8,
> `docs/prompts/done/phase1-step8-slice1-prompt.md`). Slice 1, contexts in serial
> mode, is built (below, "Context modes"); slice 2 provides `MULTIPLE`, with
> an interim mutex on shareable contexts (below, "As built, slice 2"); slice 3
> builds items 1, 2 and 4, lock-free issue with the mutex on waits alone
> (below, "As built, slice 3"). Under `MULTIPLE`, a context that promises serialization
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
  and fence and wait are a serializing fence. Only notify changes (item 3).
- **Memory stores.** Fence and quiet order only the calling thread's stores (spec
  §9.12.1, §9.12.2 Notes), so the per-thread serializing fence of §4.6 is already
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
(below), so a blocking operation skips the thread-local lookup. `NOSTORE` keeps the
serializing fence in either mode, where an earlier draft skipped it: the fence that orders the calling
thread's stores is also what orders the context's own puts on the intranode
path, each a `memcpy` in the calling thread (`base/order.h`), and those spec
§9.5 does not exempt. A backend whose fences order nothing but the
application's stores may skip them (`transport.h`). *(Corrected by the review
of phase 1 step 8 slice 1, B1, which found the skip as first built.)*

Operations branch once on the mode. Programs that never request `MULTIPLE` run only
serial mode, so phase-1 costs do not move.

*As built, serial mode* (phase 1 step 8 slice 1; `context.c`,
`transport-am.c`, `node.h`). A context is two parts. In the node segment, a
cache line from its PE's pool of `OSOE_MAX_CONTEXTS` (default 64, 1 to 4,096,
agreed between a node's PEs, layout 21): a state word claimed by
compare-and-swap at creation and released at destroy, and the context's
`bytes_issued`, `bytes_landed` and their wake word, which until then were one
pair per PE in the landing slot; block 0 is `SHMEM_CTX_DEFAULT`'s. In process
memory, the dirty set, allocated at creation. A nonblocking get's token names
its context's block in bits 48 to 59, so the lead credits the right counters
and the wire is unchanged. The landing zone stays the PE's: its records are
threaded on per-context lists, and its arena is handed out from one end and
reused once no context has a get in flight; an issue that finds no room
completes its own context's gets and then every other context's until there
is, an early completion the boundary allows. A wait on a context completes
that context's gets and no other's - `am.gets_completed` pins it. A handle is
the address of an element of a process pool of `OSOE_MAX_CONTEXTS`, so a
destroyed or foreign handle ends the job by name. A team context translates
each PE through its team, `shmem_ctx_pe_quiet`'s targets too (spec §9.5: every
point-to-point routine on the context). `NOSTORE` is recorded and changes
nothing (above); `PRIVATE` records its creator, which alone may destroy it. A
team's destroy destroys the shareable contexts made from it before its sync
(spec §9.4.8) and ends the job naming a live private one, which the spec
leaves undefined; the last finalize destroys every context left, private ones
too, silently - also undefined for private ones (§9.1.4), and the one moment
nothing can use them again. **Contexts still share one landing zone**: while
any context has a get in flight the arena is not rewound, so a loop of gets
and quiets on one context can fill it and complete another's gets early -
correct, and a coupling in time between contexts that slice 2's extents,
reused one by one, removes.

*As built, slice 2* (`docs/prompts/done/phase1-step8-slice2-prompt.md`;
`metrics.c`, `watch.c`, `transport-am.c`, `progress-host.c`, `node.h` layout
23). `shmem_init_thread` provides `MULTIPLE` when asked; `shmem_init` stays at
`SERIALIZED` (§4.6). What exists:

- **Thread records** (items 3 and 5): a PE's `OSOE_MAX_THREADS` records, one
  per counter block (§3.14), each a watch range and sequence and a completion
  slot (wake, ready, length, value, token, armed token); an `active` bitmap of
  16 words a PE marks the records holding a range, and a notify bumps every
  overlapping one. A blocking get or fetching AMO lands in its thread's slot:
  the token names the thread in the owner bits, with a per-thread sequence.
- **Landing extents** (§10's decision of 2026-09-25): the zone is 1 KiB
  extents, claimed as runs by compare-and-swap over a bitmap; a record goes
  FREE, ISSUED, LANDED, then COPYING by compare-and-swap, so exactly one of the
  PE's threads copies a landed piece to its `dst`, credits `bytes_copied` in
  its context's block and frees the extents; a wait is over when
  `bytes_copied` reaches `bytes_issued`. An issue that finds no room copies
  out every landed piece it can claim, of any context, then sleeps on the
  zone's `freed` word. The claim's check and the landing's state store are
  `seq_cst`, as the value of a wait whose wake is skipped with nobody
  registered (base/wait-word.h; the review, S1).
- **The interim**: a shareable context, the default included, holds a
  process mutex for the whole of each call on it under `MULTIPLE`; serial
  contexts and every context below `MULTIPLE` take none. The allocator, the
  lock list, the context pool and, under `MULTIPLE`, the blocked record's
  owner take mutexes of their own. Slice 3 removes the context mutex from
  issue.
- **No collective borrows the heap**: a split's number travels in the parent
  team's spare internal-region words, and collect's per-member lengths
  through the reduction tree's fixed slots into private memory (the author's
  C4).
- **The blocked record** stays one per PE: the first of its threads to wait
  owns it until its outermost wait ends, so `osoe-stat` shows one waiting
  thread whole; the other threads' waits show in their watch records.

*As built, slice 3* (`docs/prompts/done/phase1-step8-slice3-prompt.md`;
`transport-am.c`, `node.h` layout 24). Items 1, 2 and 4 as designed, over
slice 2's structures:

- **Get completion**: the context block holds `generation`, `full` and two
  pairs, `bytes_issued[2]` and `bytes_copied[2]`. A shared context's
  nonblocking get counts in the pair of the generation it loaded and loads it
  again, taking the add back and retrying on a change (`am.issue_retries`);
  the pair travels in the token's bit 47, so the copier credits it. A serial
  context uses pair 0 and its pending list, unchanged.
- **The dirty set**: per peer slot, a highest segment raised by
  compare-and-swap and then a bit, both in process memory. A wait loads the
  slot's number, asks the peer through it, waits for that number - not for
  the slot's current one, which another thread's put may raise after the ask
  (the first build waited for that and hung) - then clears the bit and looks
  again. Fence reads the bits without a lock.
- **The mutex** is taken by waits alone - quiet, `pe_quiet`, a wait for gets,
  destroy's quiet - and counted (`am.context_locks`). A wait loads the
  generation, takes the mutex, and piggybacks when `full` is past it;
  otherwise it bumps the generation, completes the closed pair, waits for the
  dirty set, and a full quiet stores `full`.
- **How a quiet finds its landed pieces** (the brief's decision 5): it scans
  the PE's landing records for its context's owner bits and claims each by
  slice 2's compare-and-swap. No word of its own, so nothing new to model.

**The timing** (`osoe-bench threads`, one PE's threads on the default context
toward another node; `docs/pi-cluster-results.md`). On the Pis, four threads
went from 0.89 of one thread's rate to about 1.4; one thread is unchanged.
In the VM, four threads on four cores beside the agents are oversubscribed
and show nothing; issue alone, without quiets, rose 10-25% at two and four
threads. What bounds the rest lies below the context - the node's shared
connection buffer or its one progress thread, not yet told apart - and is
the next limit, not this slice's.

Its test, `test-multiple.c`, runs four threads a PE in two waves on one node,
on block and interleaved maps and, under tsan, on one PE a node - where every
writer of a PE's memory is a thread of its own process, so tsan sees them
all - with a landing zone of 8 extents that the threads' pieces overfill
(the test pins that the zone filled).

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
    serializing_fence();                                   /* §4.6 memory stores */
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
  load `high_seg[n]`, ask for an acknowledgement through it (§3.4), and wait for
  `acked[n] >= high_seg`.
  Clear the bit, re-load `high_seg[n]` and `acked[n]`, and if the node is still
  behind, set the bit again and move on. *(Changed 2026-09-26, approved by the
  author: the design said "and repeat", and a quiet that waits again can be
  held for ever, with the mutex, by a thread that keeps putting to the node -
  the review of step 8 slice 3, S1. The one wait already covers every put
  that returned before the call, since each raised `high_seg[n]` before the
  quiet loaded it; the re-set hands a later put to the next quiet.
  `dirty_set_model.py`'s variant `resetstop` passes every design case.)*
- **Fence** reads the bits and flushes, and never clears, so it takes no lock.
- **`shmem_ctx_pe_quiet`** clears only its listed nodes' bits, under the same mutex.
  It still bumps the generation for gets, since it waits on every get (§4.6). It
  must never satisfy a full quiet's piggyback, so the piggyback test in item 1 reads
  a second word, `full`: the generation the last *full* quiet opened, which only a
  full quiet stores, under the mutex. A quiet that loaded `e` at its call
  piggybacks when `full` is past `e`, compared as a wrapping difference: a full
  quiet then bumped after the call, and finished its wait and its flush of every
  node before releasing the mutex. A `pe_quiet` piggybacks on the same test, since
  a full quiet covers every node. `tools/modelcheck/pe_quiet_model.py` checks
  this (§10).

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
    if (predicate(osoe_transport_load(...))) break;
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
  serializing fence.
- A `pthread_key_create` destructor releases the block at thread exit. A thread
  cannot exit inside a call, so the block's bit and slot are idle then.
- `PRIVATE` contexts cache the creator's block.

**How many.** `T` blocks per PE, `OSOE_MAX_THREADS` of them - 16 by default, at
most 1,023 - and the agent's one more, read at the first init as `max_teams` is
(§5.7). The default is not the node's online CPU count, as this section first
said: a four-core Pi would get 4, threads oversubscribe cores freely, and a
default that moves with the machine is one a program cannot rely on (phase 1
step 8 slice 2, review B1, by its arithmetic and not run: at 8 with the agent
inside, SOS's eight-thread programs would have ended the job). A block is a
counter block and a thread record, about 768 bytes on aarch64, so 256 PEs × 17
blocks is 3.2 MiB of node segment. A PE whose table is full ends the job, naming
the variable (§10, decided 2026-09-25); a thread gives its block back when it
exits, not when its call returns.

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
 * OSOE_TRANSPORT_FENCED orders an operation after every write on the same (ctx, pe)
 * whose call returned before this one began. A thread holds at most one watch.
 * osoe_transport_notify may run in any number of threads and processes at once.
```

Two declarations follow from it:
- `osoe_transport_context_create(options)`, which may fail and records the mode;
- a per-thread handle behind the watch and blocking calls, obtained by
  `osoe_transport_watch` and `osoe_transport_get` themselves, so callers do not change.

"Returned before it was called" is exactly spec §9.12.2's "posted prior to calling
shmem_quiet". The models check it in that form.

---

### 4.12 Locks

`shmem_set_lock`, `shmem_test_lock` and `shmem_clear_lock` (spec §9.13), built
at phase 1 so that the corpus's lock users run: `dtrand`, and hclib's `uts`,
whose work stealing takes one lock per victim PE.

**First come, first served, so a queue lock.** The spec is explicit - "Waiting
PEs are guaranteed to set the lock in a first-come, first-served manner" - and
no test-and-set with back-off gives that, however it is tuned. osoe's is the
MCS queue lock (`lock.c`), as Sandia OpenSHMEM's is.

**The application's `long` is the implementation's, on every PE.** The spec
makes any other access to an in-use lock undefined, and requires the variable
zeroed on every PE before first use, so the lock may use all of its copies. It
uses them as two 32-bit words:
- on the lock's **home** PE, the queue's **tail**: the last waiter's PE + 1, or
  0 when the lock is free;
- on **every** PE, that PE's **node** in the queue: its successor's PE + 1,
  and a `GRANTED` bit its predecessor sets to hand the lock over.

An acquirer zeroes its node, swaps itself into the tail, and - if there was a
predecessor - links itself into the predecessor's node and waits on its own
node for `GRANTED`. A releaser quiets (the spec's "quiet operation on the
default context" before release), then grants its successor, or, having none,
frees the lock with a compare-and-swap of the tail - and if that fails, someone
swapped in behind it and has not linked yet, so it waits for the link. A tester
compares-and-swaps the tail from 0. Every operation on another PE's word is one
four-byte rmw; every wait is on the PE's own word, through the region wait
(`region-wait.h`), so a waiter sleeps rather than spins and the remote traffic
per acquire is O(1) - a swap, a link and, later, a grant. **Model-checked**
(`tools/modelcheck/lock_model.py`): mutual exclusion, the order of arrival, and
no state from which a PE cannot finish, on two to four PEs with a tester among
them; eight breakages - the node zeroed after the swap, a release that does not
wait for a link, `GRANTED` cleared by a store, a test that swaps, a waiter that
never links, a grant to the tail, a waiter that enters without its grant, a
waiter that enters leaving `GRANTED` set - each fail one of the three. The
model's release waits as the code's does, for the whole node word to be
nonzero, and grants from the value that ended the wait; that is the link only
because an entering waiter clears `GRANTED`, which the eighth breakage shows the
model checking rather than assuming. *(The first model reloaded the successor
after its wait, which the code does not; the adversarial review of the lock
found the difference.)*

**The home is spread by a hash of the address**: the lock's offset in `long`s,
mixed (splitmix64's finalizer), modulo the PEs. Locks at nearby offsets - an
array of them, `uts`'s one per PE, or several allocated one after another,
which the allocator aligns to 16 bytes - put their tails on PEs spread over the
job and so over its nodes. A plain modulo put them on the first PEs, which a
block map places all on node 0, whose lead would then apply every swap in the
job; and, at 16-byte alignment, on even PEs only.

**Misuse is named.** Each PE keeps the offsets of the locks it holds, so a
clear of a lock it does not hold, and a set or test of one it holds - undefined
in the spec, which calls the lock non-reentrant - end the job by name instead
of hanging in a queue nobody will grant. Only the home's tail need be zero when
a lock is first used, since each PE zeroes its own node before anyone can find
it; a tail holding what no lock routine writes - no PE of the job, or the
caller itself - is the mark of a lock not zeroed on its home, and is fatal by
name rather than a link sent to no waiter. A last `shmem_finalize` begun with
locks held says so on stderr: a PE waiting for one never reaches the barrier
that finalize waits in.

Costs: an uncontended acquire is two rmw (the zeroing of the node is local) and
a release one; a contended acquire adds a link and a sleep, and a release a
grant. With *P* PEs contending, the queue is *O(P)* long but each PE's work is
*O(1)*, and the home sees one swap per acquire rather than one poll per waiter
per back-off. Not built: lock operations on contexts other than the default
(the spec has none), and a lock whose home follows its holders - the tail's PE
is fixed per lock, so one very hot lock loads one PE.

**What an RDMA backend would pay.** The link and the grant land in memory the
application allocated, in words its owner waits on; §4.10's hint does not reach
them, since `osoe_transport_set_range` belongs to the allocator. A backend whose
remote atomics do not wake the target (§4.10) would see a contended handoff at
the waiter's next sweep - `OSOE_SWEEP_NS`, 100 ms - unless it notifies by some
means of its own. The interface change that would let the lock say which rmw
wakes someone is a flag on the rmw, in the spirit of `OSOE_TRANSPORT_MORE`; it
waits for the backend that needs it. And standard verbs carry only 64-bit
fetch-and-add and compare-and-swap: the 4-byte swap and OR become masked
atomics, or compare-and-swap loops on the 8-byte `long`, where on the home PE
the tail shares the word with the home's own node.

## 5. Collectives

### 5.1 Two-level structure

Reduce or gather within the node in shared memory, then one internode operation
between node leads. Internode cost becomes a function of **node count, not PE
count** — this is the answer to the PE-scaling concern. The internode operation
is a *k*-ary tree over the participating nodes (§5.3), reached through
`internode.h` so that a backend with something better replaces it without
touching the intranode half.

### 5.2 Team accounting

Computed once at team creation, stored in the private team descriptor:

- **Local member count** — team members *on this node*, not local PE count. A
  node with eight PEs of which three are in team T has arrival target three.
- **Node participation** — the internode tree spans only nodes with at least one
  member. Topology is per team. *As built (phase 1 step 7):* each member node's
  block for the team - found by the team's name since the team redesign's step
  D (§5.7) - holds the team's **record** (`node.h`) - its local
  member count, and the O(*k*) of the tree over its sorted node list that the
  node needs, each neighbour named by its lead's rank - published before the
  team's first arrival anywhere; the progress thread and the internode backend
  learn the team from it the first time they meet it, never from a descriptor,
  since the node lead's process need not belong to the team. A team a split
  creates (phase 1 step 7 slice 2) finds its node set once, at creation, and
  only on each node's local lead of it, the one PE per node that needs it: it
  walks the team's *S* ranks, mapping each to its node, and takes per node its
  member of lowest team rank as the lead - O(*S*) node lookups when the walk
  meets the nodes in order, as a block distribution's does (backwards for a
  negative stride), and otherwise, for a cyclic distribution's or an
  interleaved map's, the members gathered as (node, rank) pairs and sorted,
  O(*S* log *S*); O(*S*) memory either way, and nothing in the job's node
  count. It is the one step of team creation that is not logarithmic. Every
  other member needs only whether the team spans nodes, which its node's
  members answer in O(1): some member is not here. The O(*N*) list of node
  numbers is dropped
  once the record is written, and so are its leads - **except by each node's
  local lead of a created team, which keeps every node's lead by its position
  in the set**, O(the team's nodes) on one PE per node: the generic internode
  phase re-roots a broadcast's tree at any node and must address that node's
  lead by position (§5.5), which for the predefined teams, spanning every node,
  is the job's table of leads, and for a created team this list. (Until step 7
  the team handed the list to `osoe_internode_team`.) The world and finalize
  teams span every node; a split team's list is sparse, and the topology
  function is tested on sparse lists (`shmem.collective-tree`), as the teams
  the splits create are (`shmem.team-split.*`).
- **Local lead** — lowest team rank among local members. Must be a member, since
  its `dest` is the accumulator and it is the only PE that writes that `dest`
  before release (§5.5). *As built (phase 1 step 7):* the rule for teams a split
  creates - their members on a node listed in team-rank order, the first their
  lead - which the other nodes' leads address through the team's records and
  its list of leads above, whatever local id the lead has. The
  predefined teams' lead is local id 0, the node's lead, which the generic
  internode phase and the job's table of leads address on every node; the two
  agree under every distribution Slurm and `osoe-launch` produce, and a local
  order given by hand, whose local id 0 is not the lowest rank, runs as it
  always did. *Since the team redesign's step D* the local lead's local id is
  half of the team's **name** on the node, the number the other half (§5.7):
  the lead claims the node's block for the team, names it, maps it under the
  name and, at destroy, clears that entry; the rule is unchanged, so a PE leads
  no two live teams under one number on its node, and a name is one team's for
  as long as the team lives there.

### 5.3 Intranode phase

**Flat counter to start.**

> At `L` local members all performing `fetch_add` on one cache line, the line
> ping-pongs between cores and arrival cost is roughly `L` serialized cache-line
> transfers. Negligible at four. At 64 or 256 it dominates the intranode phase
> entirely. Fix: two-level intranode tree with fanout near core-cluster size,
> chosen at init from local member count.

*Still flat, deliberately* (phase 1 step 2 slice 3). The arrival counter was not
sharded: the development machines have four cores, and there is no measurement at
large *L* to size a fan-out from. Nothing in the control segment's layout prevents
it — each team's counters are whole cache lines found by name (§3.14), and a
sharded arrival is more lines per team and a layout bump. What would trigger it:
on a host with 64 or more cores, `sync_all` on one node growing faster than linearly
in *L*, or the arrival (time from the first member's `fetch_add` to the last's)
exceeding the release at *L* ≥ 64; `shmem.barrier_ns` against *L* is the
measurement.

Each §5.4 counter must be **alone on its cache line** — false sharing between
counters, or with the team descriptor, hurts even at four PEs, and one alignment
attribute prevents it.

**Barrier and sync: arrival does not wait for a designated PE.** Whichever local
member's increment reaches the epoch's arrival target posts the node's internode
arrival and returns to waiting. One comparison on the `fetch_add` result; no
dependency on the lead being scheduled.

**It posts the arrival; it does not send it.** An earlier draft said the arriver
"enqueues the internode arrival AM", which §3.12 does allow of an application
thread in general - reserving and copying into a connection buffer is what every
writer does (§3.5). It is wrong here for a different reason: the state the
arrival moves is the progress thread's, not the connection's. The root's arrival
counts, the participating-node count and the per-connection queues are private
to the local lead's *agent thread*, in the lead's process, and the arriver is an
application thread of any local PE - usually another process, which cannot see
them at all. So the arrival crosses that boundary by §3.12's stated mechanism, a
flag plus a futex wake: **a per-team word in the node header** (control segment
layout 7) in which the arriver leaves the epoch, and a bump of the lead's
`agent_wake`. The lead's agent consumes it and performs the arrival.

Nothing is lost by the extra hop. The bytes could not leave before the lead's
agent ran in either arrangement, since sending is its alone (§3.12), so posting
costs the wake that had to happen anyway and buys one mechanism instead of two -
the root node, where there is no message to send at all, takes the same path.

**One word per team is enough, and that is the bounded-skew argument of §5.4
again.** Epoch *k+2*'s arrival cannot be posted until every local member has
been released from *k+1*, and that release cannot happen until the lead has
already consumed and forwarded *k+1*. So no post can overwrite one the lead has
not read, and the word needs no queue, no sequence number and no second slot.
Two rules make it hold, both easy to get wrong and both model-checked in
`docs/design/evidence/am-backend/arrival_model.py`: the agent loads `agent_wake`
**before** the handoff word (§4.9), and it records the epoch consumed **from the
value it forwarded**, not by re-reading the word afterwards - on the root node
the forward can release the team locally at once, so a member can re-enter and
post the next epoch into that word before the re-read would happen.

**Data-carrying collectives do wait for the lead**, which combines every local
contribution itself (§5.5). That dependency is the price of a single writer, and
the lead is blocked in the same call regardless.

**Release** arrives at the progress thread, which advances the team's `released`
counter and wakes the local waiters — a divisible event, so wake-many per §3.11.
*As built* (phase 1 step 2 slice 3) it wakes them as a **chain**: the progress
thread wakes two, and every waiter that slept and finds its target reached wakes
two more before returning (`osoe_epoch_release_wake`,
`osoe_epoch_wait_eq_chained`). Depth O(log *L*), and the progress thread — the
node's network — does O(1) instead of a kernel wake of all *L*. No waiter is
stranded: a waiter that arrives late loads the counter before sleeping and never
sleeps, one whose load raced the release is refused by the kernel's compare, the
counter cannot move past *e* while any member still waits for *e* (the next release
needs that member's own next arrival), and every waiter a chain wakes passes it
on — so the chain's frontier is non-empty for as long as a waiter for *e* sleeps.
The sweep bounds any residue, and slice 2's `invariant.missed_wake` makes residue
visible.

**One premise of that argument, as first stated, was false**, and the model is
where it was found (`docs/design/evidence/chained-wake/`). It was that every waiter
on a `released` counter waits for the same value. A member woken early returns,
runs to the next barrier, arrives, and sleeps on the *same* counter waiting for
*e* + 1 while members still asleep wait for *e*; a wake meant for *e* can pick it,
it finds nothing to do and passes nothing on, and if both of a link's two picks are
such members the chain ends with a member stranded (`unmasked`: a lost wake at two
epochs, none at one). Bounded skew limits the targets to two, so waiters sleep with
futex2 bitset 1 << (target & 1) and a chained wake carries its epoch's parity: a
next-epoch waiter is never picked by this epoch's chain. The tests cannot see the
unmasked defect — the kernel wakes waiters of equal priority in FIFO order, and a
next-epoch waiter slept last — but futex2 promises no order, and waiters of
different nice values are ordered by priority, so the mask is kept for the
contract rather than the observation.

**The single-node release keeps one wake-all**, by measurement (4-core VM,
`sync_all`, one node, median of 7 × 2,000 rounds, before and after interleaved):
a chain of width 2, 4 or 8 was slower at every *L* from 8 to 32 — 34 against
20 µs at 8, 61 against 41 at 32 — because a chain only pays when the woken threads
can run in parallel, and four cores cannot. There the releaser is the last
arriving member, not the network, so wake-all is the right trade until a
many-core host says otherwise; with it the single-node path is unchanged (10.9
against 11.0 µs at *L* = 2, 42.1 against 41.6 at 32). The internode release,
which runs on the progress thread, chains, and is not slower: 460 against 481 µs
at 2 nodes × 4 PEs, 455 against 480 at 2 × 8 (decided with the author,
2026-09-22). At *L* = 256 on as many cores the arithmetic reverses — a wake-all is
roughly 256 × 1–2 µs of serial kernel work on one thread, a width-2 chain eight
levels — and the single-node path should be measured there before it is changed.

**The arrival and the release cross `internode.h`, not §4.10.** The phase is
reached through its own build-time-bound interface, beside the point-to-point
boundary: `osoe_internode_arrive(team, epoch)` down, called by whichever member's
increment reached the target and never blocking, and `osoe_internode_released(team,
epoch)` up, implemented in `barrier.c` and called by the backend on the node's
progress thread. On AM they are messages, and the AM implementation
(`internode-am.c`) is the handoff above. §10 once closed this the other way, on
two reasons the code contradicted; the entry there keeps the history.

**The count is the node's, not the PE's.** Whichever local member's increment
reaches the arrival target posts one arrival for the whole node, so a node of
eight members arrives once. The root releases only when every participating node
has arrived — which at *two* nodes is indistinguishable from releasing on any
arrival, since the single message it receives is also the last one. A test of
this phase needs three (§8.2), and a test of the tree needs a partly filled last
level and more than one interior node: `shmem.tree.*` runs 7, 8 and 13 nodes at
*k* = 2 and 4, with one PE per node and uneven maps.

**The internode phase is a k-ary tree over the participating nodes** (phase 1
step 2 slice 3; `collective-tree.h`, `progress.c`). §5.2's node set is a sorted
list; node index *i* has parent (*i* − 1)/*k* and children *k·i* + 1 … *k·i* + *k*,
over the *index* in the list rather than the node number, so a sparse split team
gets the same shape as the world team. *k* is `OSOE_COLLECTIVE_FANOUT`, between 2
and 32, and its default is the **backend's** (`osoe_internode_fanout_default`, 4 on
AM; below). Every node lead must agree on it — the job rendezvous
checks it beside `SHMEM_SYMMETRIC_SIZE` (§6.2), because a node that disagrees
waits for children that send elsewhere and the job hangs without a message.

*As built (v0.2 step 2; `collective-shape.c`, `link-measure.c`,
`internode-generic.c`; `docs/decisions/2026-10-06-tree-shape.md` and its Part 2
record):* for the large reductions and broadcasts of §5.8, *k* is the call's,
not the setting's. `OSOE_COLLECTIVE_FANOUT` is the **ceiling** - default 8 on
both backends, still agreed at the rendezvous, since it sizes the internal
region's rows - and each call takes the *k* from 2 to the ceiling that
`osoe_shape_choose` gives for its bytes and node count on the job's link: three
integers - a hop *h*, a message cost *o* and a bandwidth *B* - measured at the
end of `shmem_init` by the root's lead against its first children's, through the
data path with every progress thread running, and handed to every PE by an AND
over the finalize team's split words, so that the world team's epochs stay the
application's; or set by hand (`OSOE_LINK_HOP_NS`, `_MESSAGE_NS`,
`_BYTES_PER_SECOND`). The chooser is integer arithmetic over those numbers and
the call's alone, so every node builds the same tree for the same call; its
closed form is `tools/modelcheck/shape_timing.py`'s, which a grid of 3,024
inputs holds the C to. The barrier, the small class and the gathers keep the
ceiling's tree, and the scans 0.1.0's shape (§5.8, "Pipelined pieces", as built
at v0.2); `OSOE_COLLECTIVE_SHAPE=fixed` keeps the ceiling's for every call -
with `OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2`, 0.1.0's trees exactly.

Per team per node the progress thread keeps the tree (O(*k*)) and the epoch in
progress (O(1)): which epoch is open, whether the node's own members are in, and
a bit per child that has arrived. A node **opens** an epoch at its first
arrival, its own or a child's, and **closes** it when both are complete: it
queues ARRIVE to its parent, or at the root starts the release. Closing resets
the counts **before** anything is released. A node that receives RELEASE, and
the root when it closes, queues RELEASE to every child **first**; the node's own
release is applied at the end of that pass, after the children's connections
have been drained and flushed, so the message is in the wire (or in the open
buffer behind a send already in flight) before this level's O(*L*) local wake.
Forward-first is what makes the reset-first rule load-bearing: a child released
by the forward can be back with the next epoch before this node's members have
been woken, and must find the epoch closed and clean. Both rules, and the
refusals a trust boundary needs, are model-checked
(`docs/design/evidence/am-backend/tree_barrier_model.py`): five nodes at
*k* = 2 (depth 2), two epochs, a node with two members of which one re-enters
early, SC and TSO. Each broken variant fails its own property first:
forwarding before the last child and not resetting the child count release a
node before every node has arrived; releasing locally before forwarding and
resetting after leaves two epochs open at a node; applying a child's arrival to
the epoch the node's own counter implies books it against a closed one.

*As built (phase 1 step 7):* the progress thread learns each team from its
record in the node's block for the team's number (§5.2), never from a
descriptor. The node's lead writes the predefined teams' records before its
agent starts, and the agent meets every published team at its first pass, so
that `osoe-stat` shows each one's place in the tree from the start; a team
published later is met at its first arrival or message. The releases a pass
forwards are applied at its end in the order they were forwarded - a list,
O(*k*) each, rather than a look at every team number - which no team can
observe, each team's release being its own.

The wire message did not change: `struct osoe_am_barrier {team, epoch,
from_node}` carries everything, and `from_node` is now checked — an ARRIVE must
come from a child and a RELEASE from the parent, or the node ends the job naming
the tree it disagrees about.

*Costs*, with *N* participating nodes, *L* local members, *T* teams: per node per
barrier, *k* + 1 messages handled and O(*k*) work on the progress thread (the
star's root handled 2(*N* − 1)); depth ⌈log_*k* *N*⌉, so the critical path is
2·⌈log_*k* *N*⌉ hops; state O(*k*·*T*) per node; setup O(*N*) once per team (the
sortedness check of the node list, which is then dropped).

*Choosing k.* The critical path is about depth × (hop latency *h* + *k* × per-message
cost *o*) with depth ⌈log_*k* *N*⌉, so the best *k* grows with *h*/*o*; on RDMA the
up-sweep's *k* arrivals are fetch-adds the parent's NIC applies rather than its CPU,
which favours a wider tree still. The minimum is broad (*recalled* costs; one
direction shown, the other is the same):

| Fabric | *h*/*o* | *k* = 4 | *k* = 8 | *k* = 16 | *k* = 32 |
|---|---|---|---|---|---|
| RoCE, *N* = 1,000 (*h* ≈ 2 µs, *o* ≈ 0.15 µs) | ≈ 13 | *d* = 5: 13 µs | *d* = 4: 12.8 µs | *d* = 3: 13.2 µs | *d* = 2: 13.6 µs |
| Pi TCP, *N* = 500 (*h* ≈ 100 µs, *o* ≈ 13 µs) | ≈ 8 | *d* = 5: 0.76 ms | *d* = 3: 0.61 ms | *d* = 3: 0.92 ms | *d* = 2: 1.03 ms |

So *k* is a default and not a knob that must be right, and it belongs to the
backend, which knows its *h* and *o*: AM over Ethernet uses 4 (server TCP's
*h*/*o* ≈ 4), and an RDMA backend would use 8–16. What must never happen at
thousands of nodes is the flat case — *N* arrivals serialized through one thread,
or *N* atomics on one NIC. Measuring *h* and *o* at init and computing *k* is on
§10's measurement list (decided with the author, 2026-09-22).

*Measured on the step-2 wire, and why it is slower there* (phase 1 step 2 slice
3; release build, 4-core VM, `sync_all` at one PE per logical node, median of 7
means of 2,000 rounds, before and after interleaved). Up to five nodes, where
*k* = 4 is depth 1, the tree equals the star (531 against 529 µs at 2 nodes, 592
against 590 at 5). From six nodes it is slower: 932 against 613 µs at 6, 1,061
against 636 at 8, 1,222 against 655 at 16. **Latency follows depth, not node
count**: at 16 nodes *k* = 16 (depth 1) measures 641 µs, *k* = 4 (depth 2) 1,220
and *k* = 2 (depth 4) 1,930, about 170–200 µs per level each way. That is the
step-2 lead's idle poll (§3.10's stand-in, `OSOE_PROGRESS_SLEEP_NS` = 250 µs): a
hop's bytes wait until the receiving lead next looks, because the shared-memory
wire cannot wake it. The star paid it twice; the tree pays it 2*d* times. Step 3
blocks in io_uring on socket completions and the futex together and deletes the
poll, and an RDMA backend polls a completion queue on a core of its own, so no
production path depends on a tuned sleep; the regression was recorded and
accepted with the author on 2026-09-22 rather than engineered around in code step
3 deletes.

*Measured on sockets* (phase 1 step 3 slice 1, part C; the same program, build
type and VM, with the step-2 tree at `3926a60` and this one interleaved per
repetition, median of 7 means of 2,000 rounds). The poll is gone and so is the
regression. At *k* = 4 the barrier measures 60 µs at 2 nodes (516 on the step-2
wire), 63 at 5 (592), 76 at 6 (929), 86 at 8 (1,055) and 109 at 16 (1,231): 9 to
12 times faster. The step-2 column reproduces slice 3's numbers within 5%. At 16
nodes, *k* = 16 (depth 1) measures 108 µs, *k* = 4 (depth 2) 109 and *k* = 2
(depth 4) 131. That is about 11 µs a level, up and down together, where the poll
cost 170–200 µs a level each way: **latency now follows hops and per-message work,
not poll intervals.** On four cores running sixteen nodes' agents and PEs, a
one-level tree's fifteen arrivals, serialized through the root, cost as much as a
second level does (108 against 109 µs). The target's trade-off between depth and
fan-out is the arithmetic above, not this machine's.

*Why a tree now, and what the shared-memory wire hides.* The star it replaced —
every node's arrival to node 0, node 0's release to every node — was optimal in
messages and serial in one thread: 2(*N* − 1) message events on node 0's
progress thread per barrier. The step-2 wire is shared memory, where a message
costs little more than a cache line, so that O(*N*) term is invisible below 16
logical nodes and the tree's measured effect is a no-regression check, not proof
of the win (§8.4's barrier row). The arithmetic for the target, per barrier on
TCP, with its assumptions labelled — *recalled*: a small message costs about
*t* = 5 µs of CPU per send or receive on a server core and 13 µs on a Pi 5 core;
one-way latency *h* is about 20 µs on fast Ethernet and 100 µs on the Pi's 1 GbE;
*assumed*: messages to different peers do not batch (§3.9 cannot), and a level's
*k* sends are serial on one thread:

| | Star: 2*h* + 2(*N* − 1)·*t* | Tree, *k* = 4: 2*d*·(*h* + *k*·*t*) | Ratio |
|---|---|---|---|
| Pi 1 GbE, *N* = 500 | 0.2 ms + 998 × 13 µs ≈ **13 ms** | *d* = 5: 10 × (100 + 52) µs ≈ **1.5 ms** | ≈ 9× |
| server TCP, *N* = 500 | 0.04 ms + 998 × 5 µs ≈ **5.0 ms** | *d* = 5: 10 × 40 µs ≈ **0.4 ms** | ≈ 12× |
| server TCP, *N* = 4,096 | 0.04 ms + 8,190 × 5 µs ≈ **41 ms** | *d* = 6: 12 × 40 µs ≈ **0.48 ms** | ≈ 85× |

The star's column is its root's thread doing all 2(*N* − 1) message events; the
tree's is its critical path, where each level costs one hop plus the
parent's *k* sends. At 4,096 nodes the star finishes about 25 barriers per second
job-wide and every `shmem_malloc` is one (§4.5); the tree about 2,000. Unmeasured
until two real hosts exist (§10's measurement list).
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
values are unobservable by any conforming program. For barrier and sync the
epoch number travels in the internode AM and is asserted on receipt — the
collective analogue of §3.13's handler-table checksum. The data collectives'
internode phase does the same: a reduction's and a broadcast's with a header
on every piece - the epoch, the piece, its bytes and which collective it is -
checked by the lead that receives it (`internode-generic.c`,
`shmem.collective-divergence`), and since phase 1 step 5 slice 6 a gather's
with its shape - the epoch, the result's bytes and its block - which each lead
puts beside the READY counter of the lead that writes to it first, and which
that lead compares with its own before it puts a byte
(`shmem.collective-divergence.gather-size`; §5.8). It names a divergence where
the leads meet, and only there: collectives whose phases share no edge or word
wait in different places, a hang whose blocked records name each PE's routine
- a gather against a reduction among them; broadcasts from two roots on one
node carry the same root position; and a node's members are not compared with
each other at all.

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
but a syscall per member per collective. *(As built, step 6: phase 2 directly,
so a member's source in statics is read through the lead's mapping of that
member's statics object, as through its heap.)*

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

Each needs its own design before it is implemented (§7). Scans have theirs as
built (§5.8, phase 1 step 10 slice 1); alltoall and collect have theirs as
built (below, phase 1 step 5 slice 4).

For the collectives it covers, this eliminates scratch sizing within a node — no
chunking loop, no high-water allocation, no per-team data area. Their shared
footprint there is **three counters on three cache lines**, regardless of
reduction size. Between nodes a partial result has to land somewhere that is not
`dest`, and the as-built paragraph below says where: fixed slots of §5.8's
internal region, a vector larger than a slot going in pieces - since the team
redesign's step E, a team number's small slots for a collective of at most
`OSOE_SMALL_COLLECTIVE_BYTES`, and for a larger one the lanes its leads'
threads lend it (§5.8, "Small and large collectives").

*(Fallback for cases where `dest` cannot serve: chunk through fixed scratch. The
loop costs nothing on small reductions and adds pipelining on large ones. §5.8 turns
this into the large-reduction design.)*

*As built: reductions and broadcast* (phase 1 step 5 slice 1; `collectives.c`,
`reductions.c`, `broadcast.c`). Within a node, exactly the scheme above: every
member arrives on the team's `arrived`, the last to arrive wakes the lead, the
lead - local id 0 for the predefined teams, the node's lead, which the generic
internode phase addresses on every node; §5.2 says the lowest team rank, which
that is under every distribution Slurm and the launcher produce, though the
bootstrap accepts orders in which it is not, and any member would do so long as
every member agrees (phase 1 step 7 keeps it so: the descriptor lists the
predefined teams' members in local-id order, and §5.2's rule is a split team's) -
reads each member's source only now and
combines it into its own `dest`, bumps `released`, and leaves after the last
copy-out on `copied`. **Between nodes the lead hands its node's result to
`internode.h`'s data half** and gets the team's back in its `dest` (phase 1
step 5 slice 2; §5.8). The operation goes down as values - the reduction's
operation and its element, a kind and a width - and not as the function the
lead combines with, so a backend can give it to hardware that knows the
operation and cannot call a function. AM's implementation is the generic one,
which any backend may call: the leads meet over put, fenced rmw and a wait -
§5.8's generic route - not through AM messages the agents interpret, as the
first slice built it above the boundary: a child puts its partial result into a slot of its
parent's `OSOE_REGION_INTERNAL` and adds one to the parent's arrival counter
for its position with a fenced rmw, so the add lands after the bytes
(invariant 5); the parent's application thread waits on the counters, combines
the slots into its `dest` and sends up in turn; the root's result comes down by
a put into each child's release slot and an add to its release counter. The
lead's thread is blocked in the call and is `dest`'s only writer regardless
(invariant 16), so an AM design
would still have had to land each partial result where that thread could read
it, and wake it; this does so with operations the transport has, and a RoCE
backend inherits it. A put returns at local completion, so the result sent down
is the lead's again at once: the by-reference hazard of the rule above cannot
arise. The tree is §5.3's k-ary tree over the nodes, with the same fan-out;
a reduction's is rooted at the first node and found once per team, and a
broadcast's is rotated to its root's node, so its data travels the depth and
not up to node 0 first. **A vector larger than a slot, 16 KiB, goes in
pieces** - the fallback above - and since phase 1 step 5 slice 5 each lead
keeps D of them in flight (§5.8, "Pipelined pieces"); a broadcast's pieces go
up empty, a child's arrival for a piece saying its release slot for it is
free. The counters - an arrival counter per child position and a release
counter - are cumulative, never reset, and compared as at least against bases
each lead advances by its own tree at a collective's end; trees differing from
one collective to the next cannot disturb them: the root releases a piece only
when every node has sent it up, and a node sends a piece up only after its own
wait for its children's, so an early arrival for the next collective only ever
counts toward that one (`internode-generic.c` walks it through,
`pipeline_model.py` checks it). *Since the team redesign's step E* the slots
are a team number's small ones for a small collective, in one piece, and for a
large one a lane of each lead's thread, lent by a grant (§5.8, "Small and large
collectives"). *(Until slice 5 each piece made a whole pass up
and down, and one arrival word per node was compared for equality, §5.4's
rule.)* Every PE ends with the same bits, each element combined once along the
tree (§5.8 rule 3). Integer sums and products wrap, through unsigned
arithmetic, where C would make a signed overflow undefined. Costs, with *N*
nodes, *L* local members, *k* the fan-out and *n* bytes: the lead's combine
O(*L n*), a copy-out O(*n*), ⌈*n* / 16 KiB⌉ pieces through a tree of depth
⌈log_*k* *N*⌉, D in flight at each lead - about ⌈*n* / 16 KiB⌉ / D round trips
of the tree where one at a time took ⌈*n* / 16 KiB⌉, while the round trip
rather than the links is the limit - with O(*k n*) combining and *k* puts per
node per piece; a broadcast finds its rotated tree's *k* + 1 neighbouring leads
in O(*k*), from a table of each node's lead that the job object keeps beside
its addresses and blobs (*N* words a node, written at the
rendezvous in O(*R* + *N*)) - scanning the rank map instead made every
broadcast linear in the node count under a block distribution. *As built
(phase 1 step 7 slice 2):* a team a split creates, which spans some of the
nodes and is led on each by its member of lowest team rank, has its own table:
the list of every node's lead by position in its node set that each node's
local lead of the team keeps (§5.2), which the backend asks for through
`internode.h` (`osoe_internode_team_lead`), the root's position found in it by
a binary search, O(log *N*). The tree is rotated from this node's position,
which the team's record holds, rather than from its node number.

*As built: collect, fcollect, alltoall and alltoalls* (phase 1 step 5 slice 4;
`collectives.c`, `collect.c`, `alltoall.c`;
`docs/prompts/done/phase1-step5-slice4-prompt.md`, whose decisions are Claude's for
the author's review; the adversarial review of the slice reshaped the seam and
collect's lengths). **fcollect is the accumulator with places**: once every
member has arrived, the lead copies each local member's block to its rank's
place in its `dest` - O(*L n*) of its own, under the O(*P n*) each member
spends copying the result out - and hands `dest` to the internode phase; then
the release and the copy-outs, as for a broadcast. No member writes the lead's
`dest`, which a slower member may still be reading for the collective before.
**The internode phase is its own operation at the seam, `osoe_internode_collect`**,
given the result's layout - a fixed block, or each member's place - so that a
backend knows which bytes are whose, and bound by one rule: a peer's buffer may
be written only once that peer has entered the collective, since until then
its members may still be copying the last result out of it; before, a backend
stages through the internal region, or waits. The spec's precondition would
allow a put straight into another node's `dest` at once, which SOS uses; this
rule is why osoe does not. **The generic implementation is Bruck's allgather
among the team's node leads** (§5.8, "An allgather for fcollect", as built at
phase 1 step 5 slice 6): ⌈log₂ *N*⌉ steps, at each of which a lead puts the
shares it holds - each node's members' blocks at their places - straight into
the buffer of the lead 2^s positions before it once that lead has said it
entered, so that each lead receives every other node's share once and sends as
many. *(Until then it was the reduction's tree running an or: each node zeroed
the other nodes' places, blocks at disjoint places or'd to their
concatenation, and a piece header marked it a gather. Its cost grew with the
whole result at every level - ⌈*P n* / 16 KiB⌉ · 2⌈log_*k* *N*⌉ hops one piece
at a time, 510 at 400 nodes of 256 PEs and *k* = 4 for one long each, about D
times fewer with the pipeline - which made it the one internode phase whose
latency grew faster than log *N*, the exception §1's goal carried until the
allgather replaced it.)* **collect's places
are the prefix sums of every member's length**: each member leaves its length
in its own word of the team number's line of the internal region, the lead sets
its members' lengths at their ranks in a vector of the team's size in its own
private memory, the internode reduction's tree sums the nodes' vectors, 8-byte
integers, into every lead's, and the lead turns the result into places and the
total, which its members read after the release. Nothing is borrowed from the
symmetric heap (§4.11, the author's C4). The lengths cost O(*P*) bytes a level
of the tree in every call, 8 a PE - at 500 nodes of 256 PEs, 8 × 128,000 = 1 MB
reduced, and about 5 MB out of each interior lead (the release review's P6) -
where `fcollect`'s fixed block needs no such pass; the data itself keeps the
allgather.
A scan (§9.10.10, built since phase 1 step 10) gives each PE its own place in O(1) memory.
**alltoall and alltoalls are direct puts and the team's barrier**: every block
goes from its source PE to its dest PE, which no tree shortens, and every put
but the last says more follows (`OSOE_TRANSPORT_MORE`). alltoall itself reads
no `dest`, so the precondition that it is ready is the application's to keep,
as the spec says - which includes a sync after a data collective into the same
array, since the lead's copy of that result may still be being copied out when
a fast member's put lands in it: back-to-back gathers need none, alltoall after
one does. The puts start at the PE's right neighbour, so no PE is everyone's
first target; a strided block is a put an element, element-strided within each
block as the spec's Example 33 and SOS read §9.10.6.

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
one place to fail loudly. *As built (phase 1 step 7):* `OSOE_MAX_TEAMS`,
default 256 since the team redesign's step E (64 until then, 32 until 2026-09-26), between 4 and 1,024, read at each session's first init and agreed at
the job rendezvous (§6.2) and between a node's PEs; it sizes each PE's
published epochs, the internal region's per-number blocks (§5.8) and a PE's
mask of free numbers. A node's team blocks, the progress thread's per-team
state and the control queue's term count `OSOE_TEAM_BLOCKS`, a node's pool,
since the team redesign's step D (below; §3.8).

**It counts the team numbers one PE holds at once, the predefined teams'
included, and a destroyed team gives its number back (step D, below)**: it
bounds the teams a PE is in at once, 253 of its own at the default, not the teams
a session makes in its life - and a split takes a number free on every member of
the new team, so a PE can meet it holding fewer, when its team-mates hold the
rest. *Until step D it counted numbers job-wide, never reused within a session,
and so bounded the teams a session made in its life, 61 at the default.*
The control-queue bound is per connection: the number of
distinct teams whose trees use that node pair. `shmem_team_split_2d` can put `L`
different teams on one node pair while each PE belongs to only two, so a per-PE
count makes the §3.8 assert fire on conforming programs (review C10). Teams per
edge never exceed teams job-wide, so the job-wide count is a safe bound, and the
node-wide pool (§3.8) keeps it from costing memory per connection.

**The team range allocator and the split protocol**, open here until phase 1
step 7, were designed and approved (`docs/prompts/done/phase1-step7-slice1-prompt.md`,
`docs/decisions/2026-09-25.md`): a team is a number, job-wide and never reused
within a session, claimed by the parent's rank 0 with one fetching add on a
counter in world PE 0's internal page (as built, a compare-and-swap, below);
every node's block for the number holds
the team's counters and, on each member node, its record; the split is
collective over the parent and ends with a sync over it. Step 7's first slice
built the storage, and its second the split.

*As built (phase 1 step 7 slice 2; `team.c`,
`docs/prompts/done/phase1-step7-slice2-prompt.md`).* The counter is the internal
region's last line (§5.8), used on world PE 0 alone. A split runs, over its
parent: the argument checks, alike on every parent PE - an invalid triplet
returns nonzero on all of them, and what the spec leaves undefined, a stride of
0 with a size other than 1 or a non-positive `xrange`, ends the job by name;
the claim, by the parent's rank 0, of a number for every team the call creates,
one for `split_strided` and every row and column for `split_2d`; a broadcast
over the parent of the first number, in the allocator's smallest block, 16
bytes, which each parent PE borrows from the heap for the call; each member's
descriptor - its node's members in team-rank order, O(*L*), whether the team
spans nodes, O(1), and on each node's local lead the node set, O(*S*) or
O(*S* log *S*), §5.2; each member node's record, written by the team's local
lead there; and the sync over the parent, which orders every record before any
collective on the new team (`node.h` argues it, from orders that already
existed). **The claim is a compare-and-swap**, where the approved design had a
fetching add: rank 0 exchanges the counter from where it guesses it stands -
the value it last saw, 0 at first - to that plus the call's count, and again
from the value each failed exchange returns, until one lands. A claim the
numbers left cannot hold takes none, and fails the split on every parent PE
with one diagnostic naming `OSOE_MAX_TEAMS` and the count claimed, so a later
call that fits still gets them; with an add, a `split_2d` of many rows that
failed would have used up the numbers of every split after it (review of the
slice, M2). A call that creates more teams than a session can have at all is
refused before claiming, and a heap without the 16 bytes before claiming too,
each said once by rank 0. A created team's handle is its descriptor's address,
in a per-process pool indexed by number, so a handle never issued or already
destroyed is named without being read through. Destroy is a sync over the
team's members, then the descriptor's arrays are freed; the number, its blocks
and records stay.

*As built (the team redesign's step D, part 1, layout 29;
`docs/design/team-resources-proposal.md` D3, `docs/prompts/done/phase1-step7-slice3-prompt.md`).*
A node's blocks for teams are a **pool**, `OSOE_TEAM_BLOCKS` of them (default
1,024, 8 to 65,536, agreed among a node's PEs and free to differ between nodes),
ending the node header as the per-number blocks did - the first three the
predefined teams', at the index of their number, so the node scope still names
their fields. A created team's block is found by its **name** on the node, the
pair (local id of its local lead there, its number), through the **map**, a word
per name - `OSOE_MAX_TEAMS` names for each local PE - holding the block's index
plus one; which blocks are free is a bitmap. The team's local lead on each node
claims a block by compare-and-swap on the bitmap, from the top of the pool
down, so that a created team's block index is seldom its number - never at
the defaults - and a mix-up of the two shows at once; names it - lead, number, its local members as its
holders, its generation raised; writes the record; lowers the node's low-water
mark of published blocks, which bounds the progress thread's scan of arrival
handoffs above the predefined three (in this part: the second part replaced the
mark by the posted bitmap, a bit a block behind a summary word, §3.10); and
stores the map entry with release,
before the split's sync, after which every member acquires it (`node.h`'s
argument for the record, unchanged). The internode seam and the progress
engine know a team by its block index (`internode.h`), node-local; a barrier
message names it by its name on the node it goes to, the number in `team` and
the receiving node's lead local id in `lead` (`am.h`), which that node's map
resolves, a name with no block being named rather than learnt; and the generic
internode phase places a team's slots and counters in the internal region by
the number its block's name holds, the same on every node. Numbers are still
job-wide and never reused in this part, and no block is given back: a node
whose pool runs dry ends the job at the next split, naming the setting, since
the number is claimed by then and the split cannot fail alike - the second part
makes it such a failure. The control queue's term stays per number (§3.8),
which a node's blocks match one for one while numbers are job-wide.

*As built (the team redesign's step D, part 2, layout 30;
`docs/design/team-resources-proposal.md` D1, D2 and D4 to D8).* **A number is
each PE's own**: no PE is in two live teams under one number, and two teams with
no PE in common may hold one, on one node or on two. `OSOE_MAX_TEAMS` counts the
numbers one PE holds at once, the three predefined included - still 4 to 1,024,
default 64 through step D and 256 since step E, agreed at the rendezvous, since it lays out every PE's internal
region alike - and a destroyed team gives its number back. **A split agrees its
numbers by one AND reduction over the parent** (`team.c`), in place of the claim
on world PE 0 and the broadcast, in two lines of the parent number's words in
the internal region (§5.8): every member of a team the call creates contributes
its *free mask* - a bit for each number it holds no team under and under which
no block is mapped in its own name on its node - and every other parent PE all
ones, beside a word of marks written inverted so that the one AND carries them.
Every parent PE reads the same result: `split_strided` takes the lowest bit,
`split_2d` the lowest for every row and the next for every column, since rows
share no PE, nor columns. Before contributing, the would-be local lead of each
new team on each node claims the team's block from the pool, so that a dry pool
fails the split alike - said by the PE that found it dry, naming
`OSOE_TEAM_BLOCKS` and its node - and so does a mask with no bit left, said once
by the parent's rank 0, naming `OSOE_MAX_TEAMS`; a block claimed for a split that
fails goes back. Each member then takes the number: its descriptor's slot in its
process's pool, and its counters for the number in its internal region zeroed
- since step E its grant counter and the grant's words too, guarding twice
against a grant the team before it under the number left, since epochs start
again at 1: the counter zeroed keeps the count exact, and the words, which a
reader also clears as it takes them, make a count with no grant behind it end
the job by name (§5.8, "Small and large collectives") - (`internode.h`,
`osoe_internode_number_taken`), before the split's sync, after
which alone anybody writes them for the new team - and only for a number it has
held before in the session, the heap object being new each session; and each
node's local lead names its block and writes the record and the map entry, as
in the first part.

**A block's life** (`node.h`; `tools/modelcheck/team_block_model.py`): a block
counts its holders, the team's members on the node. At destroy, after the sync
over the team, the team's local lead on each node clears its name's map entry -
its own, which nothing resolves once any member there has left the team, no
message for the team reaching the node after that (the proposal's "Why giving a
number back is safe") - and every member leaves the block; the last to leave
retracts the record, raises the block's generation and returns it to the
bitmap, in that order. The generation is odd while a team holds the block. The
arrival handoff carries it beside the epoch, in one 64-bit word, and the
progress engine (§3.12) and the generic internode phase (§5.8) learn a block
afresh when they meet it at another generation than the one they learnt it at;
an engine that finds a team it is relearning with an epoch open names the
broken argument rather than overwriting it. **Two departures from the
proposal**, both from runs of the tests: the proposal had the last holder clear
the map entry, and then a lead whose members on its node were slow to leave
found its own name still mapped, its number out of its free mask, so that a
split right after a destroy failed with the numbers tight (the model's
`last_clears` breakage, the first design, fails the property the change gives:
the lead finds its name free as soon as it has left); and a would-be lead that
finds its pool empty while a block may be about to come back marks the round
busy, which every parent PE runs again, rather than failing the split as dry
(`tools/modelcheck/pool_dry_model.py`): in the split's first round, whose claim
may have come before a parent PE's destroy just ahead of the call - which gives
its blocks back before that PE arrives in the round's reduction, and so before
the next round's claim; and while a block is on its way back - a team
destroyed, its members there past the sync and still leaving it, or the last
of them past its give-back but the block not yet in the bitmap. What it cannot
wait for is a team whose members on the node are outside the split's parent and
none of whom has begun to leave: the node cannot tell one past its destroy's
sync from one still waiting in it for a PE inside the split, whose destroy
waits for the split, and waiting for that one would never end - the model's
`remote` and `split_first`; the read of the rebuilt part, which found the
README promising more. A block keeps
its last team's member count after the team has left it and a claim zeroes it,
so that a nonzero count with fewer holders says "on its way back" and a zero
count says "claimed": the first test, fewer holders than members with the count
never zeroed, held for ever of a block given back and claimed again, and a
`split_2d` whose lead held one claimed block and found no second ran its rounds
for ever (the adversarial read of 2026-10-03; the model's `first` breakage).

**Handles** (D6): a created team's handle is its descriptor's address plus the
descriptor's generation - how many times the PE has taken the number - times
the size of the process's pool of descriptors, an integer never dereferenced,
recovered by division, and accepted while the descriptor is live at that
generation; a handle of a destroyed team is named as such, the more so when its
number names another team since. **Under `SHMEM_THREAD_MULTIPLE`** (D7;
`tools/modelcheck/split_numbers_model.py`) a PE's free mask is one resource,
which two splits' reductions can read at once and agree the same number from.
Nothing is held across the split's first reduction, which waits for every
parent PE and so, through the program's other collectives, for anything: the
proposal's mask token, held across it, deadlocked a valid program that way - PE 0's
split of {0, 2} holding the token waiting for PE 2, which waited in a sync of
{1, 2} for PE 1, which ran the busy rounds of {0, 1}'s split for ever, PE 0's
other thread unable to take the token (the adversarial read of 2026-10-03; the
model's `token` case). Each member instead takes its agreed numbers *pending*
on its PE once the reduction has agreed them: a word per number, set from 0 to
the parent number plus one by compare-and-swap, and then the number found not
held. A word another split holds is waited for when that split's parent number
is higher, and lost when it is lower; a second AND reduction over the parent
says whether every member took its numbers, and then each marks them held and
clears its words, or clears them and every parent PE runs the round again. A
split holds words only between its two reductions, with every member past the
first, and waits only for higher parent numbers - a team's number is one
number on all its members - so no waits close a cycle, and the split of the
lowest parent number among those active loses a round only to a number another
has made held. The pending take and the second reduction run when any parent
PE has `SHMEM_THREAD_MULTIPLE` - the levels of a job's PEs may differ (spec
§9.2), so the first reduction's marks carry a bit for it, and every parent PE
runs the same rounds; a PE below that level takes its numbers uncontended.
When none has, one thread of each is in the library at a time, and a split
takes its numbers after its one reduction (the adversarial read of the
rebuilt part found the first rebuild deciding it on each PE's own level, which
mixed levels turned into a split that never finished). Three counters say how often:
`shmem.team_split.calls`, `shmem.team_split.busy_rounds` and
`shmem.team_split.lost_rounds`. Each number costs every PE about 14 KiB of its
internal region's address space since step E - its counters and small slots,
its scans' the same, and its allgather's words, 16 KiB at a thousand nodes -
where it cost 200 KiB before (§5.8, "Small and large collectives"), and the
default is 256.

*Two readings of §9.4.6*, where the specification says two things. A triplet
whose sequence would wrap around the parent - "considered invalid and the
behavior is undefined" - is taken as the invalid triplet it also is, one with
a rank outside the parent, and returns nonzero with `SHMEM_TEAM_INVALID` on
every parent PE, as §9.4.6 says an invalid triplet does; a failure is within
what undefined allows, and it is the answer a program can test. And a negative
stride orders the new team's members as §9.4.6 says, in reverse of their
parent ranks: rank *i* is parent rank `start` + `stride` × *i*, so `start` is
the highest parent rank of the team, its world stride negative (`team.h`).

Runtime rather than build-time, despite the init-ordering cost: the pool cannot
be sized until the bootstrap has supplied local PE count, so team-pool sizing
must happen after §6 completes and before anything that allocates node-level
structures. That ordering is worth getting right once. A build-time constant
would be simpler, but forcing a rebuild to raise a limit is exactly the friction
§1 exists to remove.

*(A build-time ceiling with a runtime value below it was considered — it keeps
every structure statically sized. Rejected because the ceiling reintroduces the
rebuild, which is the whole objection.)*

**Team ranges leaked until `finalize`**, with a counter, until the team
redesign's step D. §9.4.8 calls `shmem_team_destroy` "a collective operation
that destroys the team referenced by the team handle argument team" without
naming a scope in the routine text, but the rest of the spec fixes it: a
parent-team PE that is not in a new team "will receive a value of
SHMEM_TEAM_INVALID" (§9.4), and destroying `SHMEM_TEAM_INVALID` is a no-op. So
destroy is collective over the team's members. An earlier draft called the scope
ambiguous; it is not. The first design concluded that parent PEs outside the
child cannot observe the destroy, so job-wide numbers cannot be reclaimed
without divergence, and that leaking cost nothing because teams are typically
created once during setup - which SOS's `shmem_team_reuse_teams` and every
program that splits in a loop disproved. Step D removes the premise: a number is
each member's own, so a PE outside the team never needs to know it is free, and
each member gives it back at destroy (above).

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

**Pipelined pieces** *(decided 2026-09-25, the Pi cluster's numbers in hand: a
1 MiB broadcast over four nodes took 36.5 ms and a 16 x 1 MiB fcollect 773 ms;
checked by `pipeline_model.py`; built at phase 1 step 5 slice 5, below).* The
generic internode phase keeps D pieces in flight instead of one: each parent
holds D slots per child position and each node D release slots, piece k in
slot k mod D. Four rules make it hold - the model breaks the first, third and
fourth, the tests the second, where an equality wait hangs - and a fifth makes
it fast:
- **One arrival counter per child position**, not one per node. With pieces in
  flight, a fast child's second arrival would stand in for a slow child's first
  in a summed count, and the parent would read a slot not yet written.
- **Counters compared as at least**, cumulative over the team's life. Within a
  collective a child runs up to D pieces ahead, so equality (§5.4) no longer
  holds; the bound that equality protected becomes the window.
- **A node sends piece k up only once it holds piece k - D's release**, which is
  the parent's word that slot k mod D has been read. One piece more and the
  parent reads a piece from the window after.
- **A node copies a release out and forwards it before sending anything that
  release lets through**: the arrival it sends next is what tells its parent
  the release slot is free. Reversing the two lets a parent overwrite a release
  a node has not copied.
- **A lead takes whichever input is ready** - its children's copies of the next
  piece up, while the window allows, or its parent's next release - and waits
  for either when neither is. A lead that waited for its children alone, with a
  release waiting, would hold back the release they need to send what it waits
  for: release k + 1 would leave it no sooner than a round trip to the bottom
  of its subtree after release k, and a tree of depth h would run at about
  h / (h - 1) times one piece at a time's speed, whatever D. *(The slice's first
  build did exactly that, and its review found it with a timing simulation of
  the loop: 1.97, 2.03 and 2.17 times at D = 2, 4 and 8 on a tree two deep,
  1.50 to 1.66 three deep, where a lead taking either gets 2, 4 and 8 at every
  depth.)* The model lets either input go first, so it covers the code's
  order, which takes the children's piece first.

Collectives still follow one another with different trees, a broadcast's rotated
to its root, so a position's counter is fed by different children over the
team's life: each node adds to its per-position bases what its own tree says
each position received, and the argument for early arrivals (§5.5 as built)
carries over, since no node starts the next collective before holding this one's
last release. D pieces cost D times the internal region's slots: at D = 2, 64
teams and a fan-out of 4, 10 MiB a PE, touched only by a team's local leads.
*(Since the team redesign's step E the slots are lanes, one per thread record,
2.7 MiB a PE at the defaults: "Small and large collectives", below.)*

*As built* (phase 1 step 5 slice 5; `internode-generic.c`, `internal-region.h`;
`docs/prompts/done/phase1-step5-slice5-prompt.md`). D is `OSOE_COLLECTIVE_PIPELINE`,
default 2, between 1 and 8, read at init beside the fan-out and `OSOE_MAX_TEAMS`
and agreed at the rendezvous as they are (§6.2), since it fixes the internal
region's offsets - and, with the fan-out, between a node's PEs at the node's
own rendezvous (`node.c`), as `OSOE_MAX_TEAMS` is, since a created team's local
lead may be any of them; D = 1 keeps one piece at a time. A team number's
block of slots holds (fan-out + 1) × D slots, and its words fan-out + 1
counters, each alone on its line. The loop is the model's, taking whichever
input is ready: piece `up`, when `up` < pieces and `up` < `down` + D and its
children's copies are in - combined, and on to the parent, fenced add after
the put - or release `down`: copied out, forwarded to every child, and only
then counted. One watch over the team's words, which are contiguous, waits
for either (`osoe_region_wait_condition`). The root, which sends nothing up,
gathers each piece from its children and forwards it. Every slot's header is
checked as before, so a divergence is still named where the pieces meet. Each
lead publishes the most pieces it had in flight in its last collective in the
team's tree state (`team.tree.most_pieces_in_flight`, layout 19): exactly
min(D, pieces) at a leaf, which fills its window before it looks for a
release, and between 1 and that at a lead with children, as the schedule has
it. `shmem.pipeline.*` asserts both at D = 1, 2 and 4 on four maps, the
deepest three levels, and under the AM stress settings: the count pins the
window; the speed-up is the simulation's, which a test could only time. Timed
once in the VM (one host, loopback, 1 MiB, the median of 31, fan-out 2): on
five nodes a broadcast took 12.0 ms at D = 1, 4.4 and 4.2 ms at D = 2 and 4
with the first build's loop, and 4.0 and 2.2 ms with this one; on eight nodes,
three levels deep, a reduction took 18.8 ms at D = 1, 12.8 and 12.4 ms with
the first loop, 10.7 and 6.3 ms with this one.

*As built at v0.2 (step 2; the fan-out's note in §5.3):* D is the call's
window, the shape's, at most `OSOE_COLLECTIVE_PIPELINE`, which is now the
**capacity** - default 4, the slots of a lane's row - and the chooser gives every
large call the capacity, since more pieces in flight never cost a large
collective time in either of `shape_timing.py`'s simulations of the loop. With
the call's fan-out its tree differs from the last call's, which the counters
already allow: each child position keeps its own base, advanced by what this
call's tree says the position received, so a position unused in one call
neither advances nor stands in for another - the argument a broadcast's rotated
tree needed, unchanged (§5.5 as built). The trees are built per call, in O(*k*)
as a broadcast's always were, not cached. **The scans keep one shape for every
call**: their rows share one base, the pieces the team's scans have moved, which
a row unused in one call would leave behind, so a scan whose fan-out varied
would wait forever on its next call. Under `link` it is 0.1.0's, a fan-out of 4
and two pieces in flight (or the ceiling and the capacity where those are
less), not the ceiling's: on the Pi a scan over 16 PEs on four nodes at the
defaults' fan-out of 8 took 1.5 to 2.0 times as long from 8 KiB up, its root's
node sending six copies over its link where 4 sends three, and at 4 with four
pieces in flight it was 1.30 times faster at 1 MiB and 6.6% slower at 64 KiB. Each lead publishes the shape of its last
reduction or broadcast in the team's tree state (`team.tree.shape_fanout`,
`shape_window`), which `shmem.shape.chosen.*` holds to the chooser call by
call.

**Small and large collectives, lanes and grants** *(the team redesign's step E,
`docs/design/team-resources-proposal.md` §4, decided with the author on
2026-10-02; checked by `lane_model.py`, timed by `grant_timing.py`; built in
step E's first part, below).* Until step E every team number held the slots
above, (fan-out + 1) × D of 16 KiB on every PE, so that a child could put a
piece into its parent's row whatever other team's collective the parent was
in: 200 KiB a number, which kept `OSOE_MAX_TEAMS` at 64. But a thread is in one
collective at a time, so the slots a lead reads during a collective can be its
own, used by one team after another, if a child knows when they are its
team's:
- **Two classes, by size.** A reduction or broadcast whose bytes are at most
  `OSOE_SMALL_COLLECTIVE_BYTES` (default 1,024, from 128 to 16,384; the
  author's answer to the proposal's E2) is *small*, and *large* otherwise.
  Every lead decides alike from the call's bytes - a reduction's count times
  its width, a broadcast's bytes - which rests on every PE passing the same
  count, and on every node holding the same setting, agreed at the rendezvous
  and between a node's PEs, since it lays out the internal region.
- **A small collective** is one piece through its team number's own small
  slots, one per child position and one for releases, and the fenced add: no
  change in hops. The slot is safe for the reason one slot per position is
  safe at D = 1 (`pipeline_model.py`'s D = 1 cases): a child reaches the
  team's next collective only once it holds this one's release, which exists
  only after every lead has read its children's slots.
- **A large collective** goes through *lanes*: each PE's internal region holds
  one per thread record (node.h), (fan-out + 1) × D slots, each a piece's
  16-byte header and its 16 KiB (round six's U1: with the header inside 16
  KiB, a power-of-two size went in one piece more), which the thread lends to the collective it leads from its entry to its return. One
  per thread record and not a pool (the author's answer to E9): two threads of
  a PE may lead two teams' collectives at once, and a thread waiting for a lane
  could close a cycle across PEs with others waiting the same way.
- **The grant.** Where a lane is depends on which thread leads, which may change
  from one collective of a team to the next, so it travels. A lead entering a
  large reduction sends each child of its tree a grant as it enters - before
  it waits for its own node's members, through one more call at the seam
  (`internode.h`, `osoe_internode_reduce_enter`) - the epoch, the collective's
  identity, its bytes and the lane's index put into the child's grant line
  for the team, then a fenced add to the child's grant counter. A child sends
  its first piece up only once its grant counter shows this collective's
  grant, counted cumulatively and as at least, and a grant naming another
  collective ends the job naming both leads, a hop before a piece's header
  would. A large broadcast needs no grant: its pieces go up empty.
- **The lane word.** A child puts its own lane's index and the epoch into its
  row's lane word at its parent before its first add of a large collective,
  and the parent reads it as it takes that child's first piece, keeping it for
  the collective's releases. The proposal had the parent read it when it first
  sends that child a release; the model found that wrong (its
  `release_lane_late`): with no more pieces than the window, a middle lead's
  last piece goes up before its first release comes down, the collective can
  end elsewhere, and in the team's next collective - a broadcast rooted
  elsewhere, whose tree is turned - another node can be the lead's child at
  that position and write the word first. Read with the first piece, the word
  is ordered before anything of the next collective, since that cannot begin
  before this lead's last piece has gone up.
- **Why a lane can be handed on** (E5): every write into a lead's slots, lane
  or small, is one that lead waits for before it returns - its children's
  pieces, each of which it combines, and its parent's releases, each of which
  it copies out - so at return nothing addressed to the lane is still to come,
  and the next collective's children cannot write before the lead's next
  grant, or for a release before their own next arrival. The grant counter is
  zeroed as a PE takes a number again (§5.7): a grant's epoch and identity
  cannot tell the last grant of a team destroyed under a number from the first
  of the next, whose epochs start again at 1. That keeps the count exact; the
  grant's words, which their reader clears as it takes them, are the second
  guard - a stale count meets words that say "nothing" and ends the job by
  name.

What it costs (the proposal's "What it costs", `grant_timing.py`): once, on a
large reduction, a child's wait for its parent's grant. When the leads enter
together, a leaf has its node's partial result when its intranode phase ends,
and waits for a grant that left its parent as the parent entered, so the cost
is one hop less the leaf's intranode time, never below zero: +1.00 hop when
that time is nothing, +0.70 at 0.3 of a hop, nothing at a hop or more, from 4
nodes to 4,096 and from 1 piece to 64 - the proposal's case. When a lead with
children enters after them, its grants leave only then, and their first pieces
arrive a round trip after it enters - the grant down, the piece back - where a
team number's own slots had them waiting: up to two hops less the late lead's
own intranode time, +1.72 at 0.3 of a hop and +1.02 at a hop for the root or a
middle lead late by two hops or more at 21 nodes, less where a deeper subtree
hides the lateness (at 500 nodes +0.70 and nothing, as when the leads enter
together). A late leaf costs nothing. The second case is the second part's
adversarial read's, found for the scans. Barriers, small collectives,
broadcasts and gathers pay nothing.

*As built* (step E, first part; `internal-region.h`, `internode-generic.c`,
`docs/decisions/2026-10-04-team-lanes.md`). The region's order puts scratch
first, as before: every team number's small rows - a lane word and a small
slot each, rounded to the line - then every number's words, its arrival
counters, release counter and grant counter contiguous, so that one watch
covers every word a lead waits on and no put writes inside it -
the lane words and the grant's words, which are put, lie outside it, in the
rows and past the grant counter - then the splits' lines, the allgather's
words, the scans' area, and last the lanes, whose number places nothing else
(`OSOE_MAX_THREADS` is agreed only between a node's PEs). A PE's region was
5.6 MiB at the first part's defaults, where it had been 12.6: the lanes 2.7
MiB, each of 64 numbers about 48 KiB - its counters and small slots 7 KiB, its
scans' slots, still each number's then, 41 KiB. Since the second part
(`docs/decisions/2026-10-04-team-scans.md`), whose scans go through the same
small rows and lanes, a number is about 14 KiB, and the region 6 MiB at the
default of 256 numbers. Each lead publishes its last data
collective's class and, for a large one, its lane in the team's tree state
(`team.tree.collective_class`, `team.tree.lane`, layout 31). `shmem.lanes.*`
runs the model's cases on the code - two teams whose leads coincide with a
child outside its parent's collective, two threads leading two teams at once,
a team's collectives led by its PEs' threads in turn, a tree turned between a
reduction and a broadcast under `OSOE_TEST_PERTURB`, the class boundary a byte
either side, a number taken again, and two leads whose grant names their
difference - and each of the model's breakages, as a mutation of the code,
fails at least one of them.

**An allgather for fcollect** *(decided 2026-09-25; checked by
`allgather_model.py`; built at phase 1 step 5 slice 6, below).* The generic
gather's latency grew with the whole result at every level (§5.5 as built), so
fcollect's internode phase becomes Bruck's allgather among the team's node
leads: at step s, lead i sends every share it holds - nodes i to i + c - 1,
c = min(2^s, N - 2^s) - to lead i - 2^s, written straight to their places in
that lead's buffer. ceil(log2 N) steps, each node's blocks crossing the network
once, any N. Two counters per step and team keep it safe:
- **READY**: at entry a lead adds one at every lead that will write to it, and a
  writer waits for it - internode.h's rule that a peer's buffer is written only
  once the peer has entered, since its members may still be copying out the
  collective before's result.
- **DATA**: a writer adds one after its put, fenced, and the receiver waits for
  it before forwarding at the next step, and before returning.
Both are cumulative and compared as at least: each collective adds one to each.
A node's share is its members' blocks, contiguous under a block distribution and
L runs under a cyclic one, so a step's put is one or two ranges or at most L of
them. The model breaks each rule - a write before READY, a forward before DATA,
and floor rather than ceiling of log2 N steps - and found that sending the last
step's full 2^s shares, past N - 2^s, only rewrites shares the receiver holds.

*As built* (phase 1 step 5 slice 6; `internode-generic.c`, `internal-region.h`;
`docs/prompts/done/phase1-step5-slice6-prompt.md`). Every team the generic
implementation serves takes it, the predefined teams and those a split creates,
over the reduction tree's positions: the team's sorted node set, this node's
position from its record and any other's lead through
`osoe_internode_team_lead`. **A lead enters first of all**: it puts the
gather's shape - the team's epoch, the result's bytes, and whether places lay
it out or a block, the block then - into the READY[0] line of the lead at
i + 1, past the counter, adds one to that READY[0] fenced after it, and adds
one to READY[s] at the lead at i + 2^s for every other s. Then, step by step, it
waits for its own READY[s] - at step 0 comparing the shape its receiver put
there with its own, and ending the job naming both where they differ - puts the
step's shares into the buffer of the lead at i - 2^s, every put saying more
follows and the step's fenced add of that lead's DATA[s] ending it, and waits
for its own DATA[s]. Step 0 joins every lead to the one before it in a ring,
so its comparisons alone cover every pair, and a gather whose size differs
between nodes is named before a byte lands
(`shmem.collective-divergence.gather-size`) - what the piece header's gather
bit, which went with the or-tree, named while it existed; a gather against a
reduction waits on other
words than the reduction's, a hang whose blocked records name each PE's
routine, as for collectives whose trees share no edge (§5.5). The words are
each team number's own: a READY and a DATA for each of the ⌈log₂ *N*⌉ steps
the job's *N* nodes allow, each alone on its line, after every number's words
(the internal region, below; the line that once counted the team numbers
claimed went with the team redesign's step D) - sized by the node count's logarithm and the team count, and
by nothing else; the shape takes 24 bytes of READY[0]'s line. A lead counts
its gathers per team, and its gather *x* waits for *x* + 1.

**Which members' blocks each step sends is found once per team**, at its
first gather, after the lead has entered: it depends on the team and the rank
map alone. A walk of the team's ranks in order - their world ranks as the
progression the team's record carries since layout 22 (the author's decision
E of 2026-09-26: every team with an internode phase is strided in world ranks,
so a backend learns a team from its record alone; from phase 1 step 5 slice 6
until then a third upcall, `osoe_internode_team_world_ranks`, asked the
descriptor above the seam); their nodes from the rank map's
runs, a stretch of one node's members at a time under a block distribution
and one member at a time, by arithmetic, under a cyclic one - puts each
stretch in the list of every step whose shares include its node, merged with
the interval before it when the two touch. A list holds intervals of
consecutive team ranks, which are contiguous in the result whatever its
layout: one or two a step under a block distribution and at most *L* + 1 under
a cyclic one. The walk costs O(*N* + intervals) under a block distribution
and O(*P*) under a cyclic one, and for a team spanning fewer than every node
a binary search of its node set per stretch besides, O(log *N*) lookups of a
lead and its node: O((*N* + intervals) log *N*). It checks that this node is
where the team's record puts it and holds as many members as the team lists
here (`shmem.gather-misuse.local-count`). **A gather after it costs
O(intervals)** beside its puts and its ⌈log₂
*N*⌉ waits: each interval's ends mapped to bytes by the call's block or
through its places, checked there - non-decreasing, from 0 to the result's
end - so that no put leaves the result whatever the places hold, and a layout
overwritten since it was made ends the job by name, as a program's collect
into a dest shorter than the result makes one, the lead's own copy of its
members' blocks running on into the room that holds the places
(`shmem.gather-misuse.short-dest`, SOS's `shmem_team_b2b_collectives`). At
the RoCE target the walk, which ran at every call until the review of the
slice, would have cost more than the gather: a copy of it timed 76 µs at 4,096
nodes under a block distribution and 3.3 ms at 10⁶ PEs under a cyclic one,
against about 58 µs for the whole 13-step gather. The buffer is addressed at
a peer by its region, heap or statics, and offset (`internode.h`).

Each lead publishes the steps its last gather made and the (node, step) pairs
its lists hold, counted as the walk fills them (`team.tree.gather_steps` and
`gather_shares`, layout 20): ⌈log₂ *N*⌉ and *N* - 1, which `shmem.allgather.*`
asserts on the world, its even ranks, the world reversed and a team spanning
three of five nodes, over two to five nodes of a block map, a cyclic one and
an uneven one - with every byte of blocks of 1 byte, 8 bytes and 64 KiB, a
collect's differing blocks, dests in both statics ranges and a collect into
them, and fcollects back to back with the team's rank 0 reading each result
late, which READY is there for. A last step sending the full 2^s shares rewrites only bytes the receiver
holds, and shows in the pairs alone.

Timed in the VM - one host, loopback, release, the default fan-out 4 and D = 2,
one PE a node - as the median of 31 fcollects between barriers, whose own time,
55 to 70 µs, each includes, and the median of three such runs; before is main
at the slice's base (4ec5e5d), the tree running an or with its pieces
pipelined. Runs in other sessions of the same day came out up to a fifth faster
or slower on both sides, the VM's noise:

| Nodes | Per PE | Before | After | |
|---|---|---|---|---|
| 4 | 1 MiB | 14.1 ms | 2.0 ms | 7.1 times as fast |
| 5 | 1 MiB | 19.2 ms | 3.5 ms | 5.5 times as fast |
| 4 | 8 bytes | 176 µs | 280 µs | 1.6 times as long |
| 5 | 8 bytes | 164 µs | 310 µs | 1.9 times as long |

**The bytes predict the large gather.** Before, the whole result went through
the tree in 16 KiB pieces, two at a time, so it took ⌈*N* MiB / 16 KiB⌉ / 2
round trips of a tree one level deep - 129 and 161 - at about 110 and 120 µs
each: bound by round trips, not by bytes. After, each lead puts *N* - 1 shares
out and receives as many, 3 and 4 MiB each way over ⌈log₂ *N*⌉ steps, every
lead at once. At the rate one pair of leads reaches alone - two nodes, one
step, 1 MiB each way in 0.44 ms - that predicts 1.3 and 1.8 ms where 2.0 and
3.5 were measured, and 0.9 ms on three nodes, where 1.0 was. The gap at four
and five nodes is the VM's, not the algorithm's: every lead and its agent work
at once, 8 and 10 busy threads on four CPUs, and the bytes all the leads move
together, *N* (*N* - 1) MiB, go at 6 to 7 GB/s from three nodes on. On a
machine per node each lead would keep the pair's rate.

**The step count predicts the small gather, and the barrier after it the
rest.** Every lead's gather was timed from inside, at 8 bytes - its marks on
one clock, the VM's, which every process reads:

| Per lead, the median of 93 gathers | 4 nodes | 5 nodes |
|---|---|---|
| Before: the tree's gather, entry to exit | 71 µs | 71 µs |
| After: the entry's adds, and the intervals and checks | 2 µs, 0 | 1 µs, 0 |
| Entry to READY[0] held | 35 µs | 37 µs |
| READY[0]'s add, sent to held | 34 µs | 34 µs |
| Each step's DATA wait, steps 0, 1, 2 | 30, 32 µs | 32, 26, 28 µs |
| After: the gather, entry to exit | 105 µs | 142 µs |
| First lead's entry to last lead's exit, before and after | 103, 148 µs | 109, 189 µs |

A hop costs the same in both: a step's DATA wait is one message's crossing,
26 to 32 µs, and the tree's gather, up and down, is two, 71 µs. Nothing at
entry costs what a hop does - the adds take 2 µs and the intervals, found at
the team's first gather, nothing - but every lead enters within about 20 µs
of the others, so READY[0], added by the lead before at its own entry, is held
one crossing after this one's: ⌈log₂ *N*⌉ + 1 hops, 3 and 4 where the tree
made 2, 105 and 142 µs where it took 71, as the count predicts. The rounds
lose more than that, 104 and 146 µs, for two reasons the marks show. The
leads' exits spread wider - the tree's root releases every leaf at once, where
an allgather's lead finishes when its last share arrives - so the last lead
leaves 45 and 80 µs later than the tree's did. And the barrier that ends each
round begins with a quiet, which waits for the acknowledgement of every put:
an allgather's lead puts its last step's shares one hop before it leaves, a
tree's leaf its piece two hops before, acknowledged by then. Rounds between
`shmem_sync_all`, which has no quiet, lose 62 and 96 µs rather than 104 and
146 (230 against 168 µs, 253 against 157). So a small gather on a few nodes is
slower than it was by ⌈log₂ *N*⌉ - 1 hops of its own, the wider spread of its
exits, and the quiet of a barrier after it; on more nodes the tree's
2⌈log_*k* *N*⌉ hops and the allgather's ⌈log₂ *N*⌉ + 1 stay within a hop of
each other at *k* = 4, and no lead takes *k* arrivals a hop. Taking READY off
the critical path - staging step 0 in slots of the internal region, which the
review of the slice simulated at 92 -> 63 µs on four nodes - and keeping the
tree for small gathers, a choice by size and node count like rule 4's below,
are for the author.

With two PEs a node, the same internode phase and each member copying twice
the result out, 1 MiB each took 29.4 against 5.0 ms on four nodes and 40.0
against 8.0 on five (one run each, against ea311cf and the slice's first
build).

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
region is additive and costs nothing until a collective uses it. *(Built at
step 5 as the tail of each PE's heap mapping, past the page-rounded
`SHMEM_SYMMETRIC_SIZE`: no new object or mapping, the allocator never reaches
it, no API check admits it, and a backend that registers the heap registers it.
It holds fan-out + 1 slots of 16 KiB and then a page of control words - eight
bytes each, on lines of their own, since stock verbs have no four-byte atomics;
the slots first, so that a store running off the end of the heap lands in
scratch rather than in a count. Its size is fixed by the fan-out every node
agrees on, and the slot is a constant for the same reason.
`internal-region.h`. *Since phase 1 step 7* a block per team number: first every
team number's fan-out + 1 slots, then every team number's two words, each alone
on a 128-byte line - a constant, not the compiler's cache line, which is 64
bytes on x86-64 and 128 on aarch64, so that nodes of different architectures
compute the same offsets. *Since phase 1 step 5 slice 5* each team number's
slots are (fan-out + 1) × D, D per child position and D for releases, and its
words fan-out + 1, an arrival counter per child position and a release counter
("Pipelined pieces", above). About 10 MiB a PE at the default 64 teams (and,
*since phase 1 step 10*, 1.3 MiB more of the scans' per-team slots at the
region's end, on every member: "Prefix scans", below), a
fan-out of 4 and D = 2, of which a team touches only its own slots and words,
and only on its leads. And from the second slice of step 7 to the team
redesign's step D one line more, whose first word counted the team numbers
claimed, on world PE 0 alone (§5.7); step D gave each number two lines for its
splits' words instead. *Since phase 1 step 5 slice 6*, after the words, each
team number's allgather words: a READY and a DATA counter for each of the
⌈log₂ *N*⌉ steps the job's *N* nodes allow a team, each alone on its line - the
one part of the layout the node count sizes, 2.5 KiB a team number at a
thousand nodes ("An allgather for fcollect", below). *Since the team
redesign's step E* a number keeps counters and small slots, and the large
slots are lanes, one per thread record, at the region's end: every number's
small rows first, still scratch first; then every number's words, with a grant
counter; then the allgather's words and the scans' area; then the lanes -
6 MiB a PE at the defaults, 256 numbers of about 14 KiB and 17 lanes of 160
KiB ("Small and large collectives", above).)*

*(Built at phase 1 step 5 slice 2, once reductions had landed and shown what it
must contain - not as a separate header but as `internode.h`'s data half, beside
the barrier's. `osoe_internode_reduce` and `osoe_internode_broadcast` are
blocking calls on the lead's thread, since rule 2 below keeps the combining
there, and they carry the reduction as values: an operation, and an element
described by kind and width, which is the vocabulary offload interfaces use
(libfabric's, UCC's) where a function pointer can never be offloaded. Every
backend defines them; each may hand any call to the generic implementation,
`internode-generic.c`, which is the tree over put, fenced rmw and wait that
the first slice built, and a backend that offloads some pairs gives it the
rest. That is a mandatory operation with a generic fallback, which the RoCE
review's case against negotiated offload does not reach
(`docs/reviews/archive/roce-transport.md` §8.4). The combine functions are per element
(`combine.c`), thirteen of them, so two types that are one element cannot be
reduced differently.)*

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
   count** - the intranode phase's above the boundary, the internode phase's by
   the backend behind `internode.h`'s data half, the generic implementation's
   ring among them. Neither is a capability query (§4.10): nothing above asks
   what the backend chose. The crossover sizes come from measurement (§10).
   *(Amended at phase 1 step 5 slice 2, when the internode phase went behind
   the seam: an algorithm choice above it would have kept the phase there.)*

The intranode phase is unchanged: the lead combines local members' sources into
its `dest` after they arrive (§5.5). The internode algorithm then runs between
leads, and release and copy-out follow the §5.4 counters as before.

*As built (v0.2 step 3; `2026-10-07-circulant.md`, `2026-10-07-circulant-data-path.md`):*
a large reduction takes Träff's circulant reduce-scatter and allgather (arXiv
2410.14234) where the chooser says it costs less than the best tree (`shape_of`
in `internode-generic.c`, under `OSOE_COLLECTIVE_SHAPE=link`; `tree` turns it
off by name): ceil(log2 N) rounds a half for every node count, every lead
sending N - 1 blocks a half - under two vectors on every lead's link, where a
tree's interior lead sends k + 1 and the fold the step's brief first described
put up to 3.5 on a fold lead's. Rule 1 holds through the lanes' rows, round k
of either half into the k-th block of the receiver's lane - a block being as
many of the lane's rows as the rounds leave a round, all of whose slots it
keeps in flight, 16 pieces on four nodes; rule 2 on the lead's thread,
which combines or copies every piece; rule 3 since each block is combined once,
at its owner, in an order the schedule fixes, and copied to every lead. Its
arithmetic is `collective-circulant.c`, its cost `osoe_shape_circulant_cost`,
its protocol `tools/modelcheck/circulant_model.py`'s: words of its own per team
number - an arrival counter a row, a credit counter, the allgather's grant
count, the reduce-scatter's grant lines with a field a round, and the senders'
lanes - because its allgather ends with no step every lead waits for, so a
finished lead's next collective could add to a tree's counters while another
lead still waits on them; and credits for the pieces a sender's window waits on
alone, so that nothing lands in a lead's words once it has returned. A grant's
field - the epoch, identity and bytes of the collective its receiver entered and
the lane it lends - is that receiver's alone, and the lead checks every field:
a head every receiver put was a word two progress helpers wrote at once, and the
lead checked only the last to land (the review of v0.2 step 4's merge point,
correctness Q2-4). Each lead
publishes the algorithm its last such call took and the rounds Träff's made in
the team's tree state (`team.tree.algorithm`, `circulant_rounds`; layout 33),
which `shmem.shape.chosen.*` and `shmem.circulant.*` hold call by call. Its
crossover is the cost model's on the measured link: on the Pi's four nodes
between 8 and 16 KiB, and it was faster than the trees at every size it took - 1.32 to 1.41
times at 1 MiB - and the trees below it unchanged (`docs/pi-cluster-results.md`,
v0.2 step 3).

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

*As built - not the runs above* (phase 1 step 10 slice 1;
`docs/prompts/done/phase1-step10-slice1-prompt.md`; the author's design A of
2026-09-26). **Every member holds its team rank's place in a pre-order k-ary
tree over the ranks**, k the agreed fan-out: a place covering [a, b) is rank
a, its children split [a + 1, b) into at most k consecutive blocks, so every
subtree is a contiguous range of ranks. Up, a member sums its children's
subtree totals and its own source and puts that to its parent; down, it
receives the sum of every rank before its subtree - its exclusive result -
and sends child j that plus its own source plus the totals of children
before j. Each member runs its own place on its own thread, reads only its
own source and writes only its own `dest`, so in place is safe. A tree over
the ranks rather than the runs: the modeled and timed comparison
(`tools/modelcheck/scan_model.py`, `scan_timing.py`) found the two equal under
a cyclic layout and the runs about two network hops faster under a block one,
where the rank tree's subtrees do not line up with nodes (at a Pi's 4 x 4,
about 320 against 165 µs); the runs would have needed a walk of the layout per
team, run bookkeeping and a lead serving many places, and no program in the
corpus calls a scan. The "rank-level" rejection above was of a flat parallel
prefix over ranks; a tree's levels within a node cost shared-memory hops.

**The open point, how a node holding many runs takes their messages**,
resolved itself: every PE has its own internal region, so every member's place
lands in its own. **The slots are the team number's on every member** - k
up-slots and a down-slot of 4 KiB, and a counter for each, cumulative, each
member expecting one more piece than the team's scans have moved - and not the
reductions' rows, which a node that finished a scan could be writing for a
reduction while its tree parent in the scan still reads them. Consecutive
scans on one team are safe by the tree alone: a child reaches the next piece
only after its parent read its total and sent it the down. *The brief and the
first build shared one set of slots among every team, with a READY per team
number to keep a new team's member from writing into a parent still in
another scan; the review of the slice found that wrong under
`THREAD_MULTIPLE`, where two threads of a PE may scan two teams at once (spec
§9.4), and the per-team slots need no READY. Both designs are in the model,
with a breakage of each rule.* The seam's call is `osoe_internode_scan`, the one data-half
call every member makes (internode.h), over the team's world ranks from its
record (decision E); a team on one node is scanned by its local lead, in rank
order, between the intranode arrival and release. A scan on a team spanning
nodes takes no epoch: nothing of the node's counters is used.

*Pipelined pieces* (the author's decision of 2026-09-27, after the Pi measured
a 1 MiB scan at 134 ms where reduce took 30: its 4 KiB pieces went one at a
time, each all the way up and down the tree). **A member keeps D pieces in
flight**, D the reductions' agreed `OSOE_COLLECTIVE_PIPELINE`: a row of D
up-slots per child position and a row of D down-slots, piece g of the team's
scans in slot g mod D, and its loop is the reductions' - whichever input is
ready, its children's totals for the next piece up while the window allows or
the next piece's down. Five rules, each with a breakage the model shows failing
(`tools/modelcheck/scan_pipeline_model.py`): a member sends piece q up only
once it holds piece q - D's down; copies a down out before its next piece up
can let its parent write the slot again; keeps one counter per child position;
takes a piece down only once it took it up, since the downs it sends carry
the totals of the children before each (at the root the one rule that orders
the two); and keeps its own source and its children's totals for the D pieces
in flight. The scans' area doubles at the default D of 2, to 2.6 MiB of
address space a PE, touched only by scans. Each member publishes the most
pieces it had in flight in its PE slot (layout 25), min(D, pieces) at a leaf,
which the tests pin.

*Since the team redesign's step E* (`docs/decisions/2026-10-04-team-scans.md`;
E6) the scans move as the reductions do: a scan of at most
`OSOE_SMALL_COLLECTIVE_BYTES` is one piece through its team number's own small
rows - a lane word and a small slot per child position and for the down - and
a larger one goes through the members' lanes, D pieces in flight - pieces sized
to the scan in elements, its count's share of D rounded up but at least 8 KiB
of elements and at most a lane's 16 KiB slot of them - one piece to 8 KiB, 8
KiB ones to D × 8 KiB, D to D × 16 KiB and 16 KiB ones past that (sized in
bytes, 2,049 longs
were cut 1,024 + 1,024 + 1, a third piece that waited a round trip of the tree;
the second part's adversarial read): of three rules the Pi measured against
step D, the proposal's 16 KiB at every size (1.17 times slower at 16 KiB,
where four pieces in flight had beaten one), 4 KiB below 64 KiB (slower at 8
and 32 KiB, where fewer messages won) and this, this was the fastest at every
scan size and step D's equal or better from 8 KiB up, 1.5 times faster at 1 MiB
(the author's decision of 2026-10-04 to size them, the rule the measurement's)
- every member granting its tree children as
it enters (the call is already every member's at entry, so the seam needs
nothing new) and each child putting its own lane's word before its first total
up, which its parent reads as it takes that total. A grant names the scan by its
number among the team's - every member's alike, since every member makes every
scan of its team - its identity (inclusive or not, its element) and its bytes,
and a difference ends the job naming both PEs. The scan's tree never turns, a
position's child the same rank in every scan of the team, so the reductions'
reason to read a lane word with the first piece cannot arise here; the rule is
kept for uniformity. A small scan's piece carries no header, and so compares
nothing: members given other counts below the line get wrong results (the
README's limits). A reader clears what it read, as the reductions' do, and a
large scan's lane slot where a header would be, so that a slot a scan left
reads as nothing to the other collectives' header checks. What the grant costs
(`scan_grant_timing.py`, every member its own node): +1.00 hop when the members
enter together, at 16 members to 256 and 1 piece to 16; up to a round trip,
+1.96 to +2.02, when the root or a member with children enters after its
children by enough to be on the scan's longest path - a hop at 16 members,
three at 256 - where its slots had their first totals waiting; nothing when a
leaf is late. On the Pi, a 2 KiB scan against step D's (the second part's
adversarial read's probe): +119 µs with nobody late, +259 with a member with
children a millisecond late, +228 with the root late, -6 with a leaf late, a
collective's hop there - the message and its handling - being 115 to 135 µs. A number's scans' area falls from 40 KiB to 6.4 KiB at the
defaults, and the 2.6 MiB of 64 numbers to 1.6 MiB of 256 (`scan_pipeline_model.py`
with lanes, grants and small scans; `shmem.lanes.scan-*`; the scan's breakages,
each a mutation of the code, failing).

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

*As built* (phase 1 step 4, the second slice's Part B): the bootstrap reads
these names as `srun` sets them, or as `osoe-launch` mirrors them with `OSOE_`
in place of `SLURM_`. `OSOE_PROCID` decides which, and every name is then read
with that prefix, never a mixture — so a test that `osoe-launch` starts inside
a Slurm allocation, whose environment holds both, is the launcher's job and not
the allocation's. A task in a Slurm job but in no step of it — a program run
straight from a batch script — is refused, naming `srun`. The node list is
Slurm's compressed form (`pi-[001-004]`), each entry resolved by
`getaddrinfo` — and a node whose own name its host resolves to a loopback
address, as Debian's `/etc/hosts` maps a host's own name to 127.0.1.1, listens
and is advertised at the address its host would send from toward another node
instead, since its peers could never reach it at loopback; an IP literal is
taken as given (`listener.h`, `shmem.own-address`). `osoe-launch -s` sets
`srun`'s names, so the Slurm path runs at tier 2 on one machine
(`shmem.slurm.*`), and `shmem.bootstrap.*` replays
real and documented `srun` environments (`shmem/tests/slurm-fixtures/`), which
`tools/launch/capture-slurm-fixtures.sh` captures.

### 6.2 Rendezvous

> **Local PEs meet on their node; only each node's lead crosses the network.
> Rendezvous connections scale with node count.**

The rendezvous inherits §3.4's trust boundary: the PEs of a job trust each other,
other local users do not (every object is mode 0600, created `O_EXCL`), and the
network is trusted as Slurm's own traffic is. The step-3 handshake authenticates
nothing beyond the job and step ids, which are public; §10 has the open item.
*(Decided and built in phase 1 step 3: an optional `OSOE_JOB_SECRET`, below.)*

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
1. **The lead creates the control segment** with `O_EXCL`, mode 0600. The
   lead writes the versioned header, maps its own heap (choosing `B`, §4.1), and
   publishes `ready`.

   **A name that already exists is reclaimed or refused, never reused.** Every
   named object records its creator's pid in its header, with the first store after
   sizing it. An existing object is *reclaimed* (unlinked, then created afresh, with a
   line on stderr) only when `fstat` says it belongs to this uid **and** its recorded
   creator is dead or was never recorded (a job killed between `shm_open` and that
   first store leaves exactly this). Otherwise init fails, naming the object and the
   creator's pid: a live creator means another job is using these job and step ids,
   and guessing which one is the leftover would destroy a running job. The same rule
   covers the job segment, the connection areas and the wires (`shm-object.c`).

   **An object whose `st_uid` is not ours is never opened**, by creator or opener.
   On a shared node `/dev/shm` is world-writable, so another user can pre-create a
   name this job will use; mode 0600 on our own objects does not stop that. The fatal
   names the owner's uid.

   An opener can attach to a leftover in the window before its creator reclaims it.
   It then waits on a `ready` that will never be published, so every `ready` wait
   also checks `st_nlink`: an object with no name left has been reclaimed, and the
   opener starts again from the name.

   **A request larger than `/dev/shm` is refused before anything is created.**
   The lead adds up its node's request - the control segment and `L` heaps - and
   compares it with `statvfs`'s free space; the connection area's creator does the
   same for the area. Either fails by name, with both numbers. The check is
   **advisory**: tmpfs allocates a page when it is first touched, so another job can
   take the space after the check passes, and nothing reserves it. Reserving it
   (`posix_fallocate`) is ruled out because it defeats the demand paging every
   sizing decision in §3.4 and §4.1 relies on, and a 4 GiB Raspberry Pi cannot
   afford it. When tmpfs does run out later, the failure is a SIGBUS in whichever
   process first touches an unbacked page - often the **lead's agent applying
   another node's put** to a local heap, so the process that dies is not the one
   whose memory ran out. The check is compared against the nominal size, so it
   also refuses a job that would have fitted by touching little of a large heap;
   `SHMEM_SYMMETRIC_SIZE` is the remedy it names. (Second review, omissions 5.)
   The refusal is the default and stays so. `OSOE_SHM_CAPACITY_CHECK=0` turns it
   into a line on stderr, for testing on a machine whose `/dev/shm` is smaller
   than the nominal request (decided 2026-09-21).

   **A PE's core holds its own heap and nothing of its node-mates'.** Sibling
   heaps and the connection area are mapped `MADV_DONTDUMP`, or every crashing PE
   would write `L` heaps and the node's traffic. The control segment stays in: it is
   the post-mortem state (omissions 4).
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
siblings stop within one safety sweep. Otherwise a rendezvous waits as long as
its counts keep moving, and by default forever (§3.15's no-progress rule): with
`OSOE_NO_PROGRESS_TIMEOUT` set, a count that stands still that long ends the job
naming the local ids that never arrived.

**Nothing refuses before there is a segment to say so in** (the round-three
review's R8; the author's D2). `failed` lives in the control segment, so a
refusal before the lead has made it left the node's other PEs waiting for a
segment that never came - the likeliest first-day failure, a `/dev/shm` too
small for the node's heaps, among them. Three rules close it:

- *Every PE checks every setting that shapes the node or the job* in its own
  bootstrap and init, before its node joins - until the review the lead alone
  checked seven, as it laid out the segment, and the leads alone the wire's -
  and a node's PEs are compared on every one that lays out the segment
  (`OSOE_MAX_TEAMS`, `OSOE_TEAM_BLOCKS`, `OSOE_MAX_CONTEXTS`, `OSOE_MAX_THREADS`,
  `OSOE_COLLECTIVE_FANOUT`, `OSOE_COLLECTIVE_PIPELINE`,
  `OSOE_SMALL_COLLECTIVE_BYTES`, `OSOE_GET_LANDING_BYTES`,
  `SHMEM_SYMMETRIC_SIZE`), through `failed`. The
  agent's and the detector's (`OSOE_AGENT_SLICE_US`, `OSOE_AGENT_CPUS`,
  `OSOE_DEADLOCK_SECONDS`, `OSOE_CHECK_QUIET`, `OSOE_FORCE_REMOTE`, and the test
  hook `OSOE_TEST_PERTURB`) every PE checks after the join, as it starts its
  agent, where a bad one ends the job through the abort word: loud, naming the
  PE, but with "the job is aborting on behalf of PE n" on the others.
- *A launch is told from another by the same names*: a requeued Slurm job keeps
  its job id and starts its steps again, and an `osoe-launch` job's id is its
  launcher's pid, so a failed start's leftover can carry this start's names. The
  header records the launch (`SLURM_RESTART_COUNT`, or `OSOE_LAUNCH`, which
  `osoe-launch` sets afresh each time; layout 28), and a node-mate takes a ready
  or failed segment of another launch for the leftover it is, waiting for this
  launch's lead to reclaim it, as `tell_the_lead` does (the adversarial read of
  2026-10-03: a refusal's leftover had made the next start with the same ids
  fail, four times in ten). The job tables and the connection area, which a
  node-mate also opens by name, carry no launch: it takes them only from the
  lead whose segment it joined, by the creator's pid each header records, and
  waits past one another start left - ready, or of another size - for that lead
  to replace it (`job.c`, `connection-area.c`; the round-four review's
  correctness F1, which found a requeued start binding both and ending as a
  false deadlock). A new lead with the dead lead's pid would defeat the check,
  which on one node and within a requeue is a coincidence of `pid_max` - though
  a launch in its own pid namespace makes equal pids likely, if `/dev/shm`
  outlives the launch.
- *The lead makes the segment before anything that can refuse*: the `/dev/shm`
  check comes after it exists and refuses through `failed`; the pages the lead
  writes first are allocated, not only sized, so a `/dev/shm` too full even for
  those refuses by name rather than by SIGBUS. The name stays, a failed job's
  leftover.
- *A PE that refuses before its node has joined tells the node*: from the
  moment it knows its job, step, node and local id until its node has joined, a
  fatal of its own runs a last word (`fatal.h`, `node.h`) - `failed` in the
  segment if it has it; otherwise, from the lead, a *tombstone*, a control
  segment that is nothing but a header with `failed` set; from any other PE,
  `failed` in the lead's segment, waiting at most the abort grace for it. A PE
  that cannot read its own identity tells nobody, and a task that never reaches
  the library cannot: the notices of §3.15 name both, and the launcher ends
  them. Nor does a refusal of the kernel, which every local PE makes by itself
  before anything is made (§1): a tombstone would tell nobody anything, and its
  wake is the futex2 call that failed. A tombstone's name stays, as a refused
  segment's does: `osoe-stat --gc` removes both (§3.14).

**Removing names: as soon as everyone who must open an object has.** Every object
is opened only during init, and each name is removed the moment the last process
that needs it has it open, so that after init nothing this job created in
`/dev/shm` has a name and no death of any process can strand one (§3.14):

- **Heaps and the control segment**: by the lead, once `mapped` reaches `L` —
  every local PE opened the segment before it could map a sibling's heap.
- **The connection area**: by whichever local PE's open brings its `opened`
  count to `L`.
- **The job segment**: likewise, at `L`: it is per node since phase 1 step 3
  ("The rendezvous as built", below). Step 2's job-wide segment counted to the
  world size.
- **A wire** had a name in step 2, removed by the second of its two leads to
  open it; since step 3 a wire is a socket, and has none.

**For the heaps and the control segment, "after init" holds per PE**: no PE
returns from `shmem_init` while they have names. The lead removes them before it
publishes `lead_agent_running`, and every other local PE waits for that word
before it returns (§3.15). The order is §3.16's for a futex word that is its own
predicate, with the removal where the header would be: each `shm_unlink` has
returned before the lead's `release` store, a PE's `acquire` load that reads the
store comes after it, and a name an `unlink` has removed is gone for every
lookup begun after the call returned. So the removal stays before that store;
after it, a PE could leave init with the names still there. For the connection
area and the job segment, "after init" means after *every* local PE's init:
their last opener removes the name, and a sibling may return from init first. A
barrier after init is past both points, and that is where the tier-2 tests look
(`invariants.h`, `shmem.kill-after-init.*`); `shmem.fork.*` looks as soon as its
own init returns, at the heaps and the control segment. (Until the review of
phase 1 step 4's first slice added the §3.15 wait, `9d9251e`, a non-lead could
return before its lead had acted on `mapped` reaching `L`, and the fork test
failed about one run in four beside 16 busy loops in tsan. With only that wait
removed from today's tree it fails the same way, 6 of 30; 2026-09-23.)

The last opener rather than the creator removes each name, so nothing waits for
anything, and a re-init after finalize is a fresh session with fresh names.
`OSOE_KEEP_CONTROL_SEGMENT=1` keeps the control segment's name until the last
finalize instead: each PE bumps `finalized`, exactly one increment reaches `L`,
and that PE removes it. The session number keeps that safe — without it, a PE
that finished and immediately re-initialized could recreate the name and a slower
sibling would remove the new segment. The window that remains is init itself: a
job killed there can leave names behind. A later job with the same ids reclaims
the ones it meets when it creates its own, a leftover whose creator is gone
(above); `osoe-stat --gc` removes them once nothing uses them (§3.14); and a
site's epilog may remove `/dev/shm/osoe.<job>.*` at the end of every job. Heaps
are ordinary pages of `/dev/shm`, under the same names: no code asks for huge
pages for them (`MAP_HUGETLB` is used only by the connection area, when
`OSOE_AM_HUGEPAGES=1`, and Linux refuses it there, §3.4). Every process maps every node-mate's heap, so a
PE's page tables grow with the bytes it touches in all of them; a hugetlbfs path
was designed and is not built.

**Global.** The step's rendezvous server is the lead of node 0: the task with
`SLURM_NODEID == 0` and `SLURM_LOCALID == 0`, on the first host of
`SLURM_STEP_NODELIST`. That is the same list `SLURM_NODEID` indexes, so every task
computes the same host under any distribution. *(As written, a star; built in
phase 1 step 3 as a tree rooted at that lead - "The rendezvous as built", below.)*

- **Port:** a deterministic sequence of candidate ports hashed from the job and
  step IDs, within a configurable range. The server binds the first free
  candidate; clients try candidates in the same order. The handshake carries the
  job and step IDs, so a client that reaches another job's server is rejected and
  moves on.
- **Who connects:** only node leads — one connection per node. Each sends its
  node's rank list (`SLURM_GTIDS`), `SHMEM_SYMMETRIC_SIZE`, handler-table
  checksum (§3.13), `OSOE_COLLECTIVE_FANOUT` (§5.3: a node building a
  different tree waits for arrivals sent elsewhere, silently), since phase 1
  step 7 `OSOE_MAX_TEAMS` (§5.7: a node counting to another number lays its
  team state and internal region out differently), and since phase 1 step 5
  slice 5 `OSOE_COLLECTIVE_PIPELINE` (§5.8: a node keeping another number of
  pieces in flight places them in its internal region by it), and since the
  team redesign's step E `OSOE_SMALL_COLLECTIVE_BYTES` (§5.8: a node taking a
  collective as large where its parent takes it as small waits for a grant
  that never comes). The server checks
  agreement, then broadcasts the rank→node map and node addresses, and each
  node's fixed-size backend blob (§4.10; zeros on AM, an RDMA backend's queue-pair
  and key material).
- **Node-pair connections:** the lower node index connects to the higher, so each
  pair opens exactly one stream (§3.1).
  *As built since v0.2 step 4's Part 2:* two, the control wire first and then the
  data wire (§3.1), which the hello tells apart by its purpose
  (`OSOE_HELLO_CONTROL_WIRE`, `OSOE_HELLO_DATA_WIRE`), a lead accepting either
  while it waits for its lower nodes. The chain of connection times is about 1.8
  times as long - 1.7 ms at 4 nodes where it was 0.95, 583 ms at 1,024 where it
  was 322 (`mesh_timing.py`'s two wires) - which `docs/design/mesh-schedule-proposal.md`
  is the answer to either way. The control wires are owner 0's, on its ring
  after its data wires (`owner-map.h`).

*Node-pair connections as built* (phase 1 step 3 slice 1; `listener.[ch]`,
`osoe_wires_open`). **Every lead listens, not only node 0's**, on the first free
port of a candidate sequence hashed from (job, step, **node**) inside
`OSOE_PORT_RANGE` (default 17000–26999, below Linux's ephemeral range), and
another lead finds it by trying the same sequence and asking whoever answers who
it is. With the node in the hash every lead has a listener another can find —
what the node-pair mesh needs, and what a tree-shaped rendezvous needs (below) —
and on one host, where §8.1's logical nodes all live, two leads' sequences start
in different places. No `SO_REUSEADDR`: a candidate in `TIME_WAIT` is skipped
like one in use.

**The hello is three steps**: the caller's hello (job, step, session, both node
numbers, and what the connection is for), the answer, and the caller's
confirmation. A caller that gets no answer within a second — a stranger's port
that accepted and says nothing — closes and tries the next candidate, and the
whole sequence again after a backoff; with two steps the answering lead could not
tell that caller from one it now owns, and would count a peer whose socket is
already closed. It counts a connection only on the confirmation. The second's
wait is not a time bound on the job (§3.15): giving up on a candidate retries.

**Each lead accepts every lower node, then connects to every higher one.** A lead
answers its callers the whole time it waits for them, so no lead waits on one that
is itself waiting to be answered; the last lead is connected to after about *N*
connection times, not after a chain of them. The listen backlog is `min(N, 4096)`,
every lower node at once, capped by `net.core.somaxconn`; past 4,096 nodes a
connection waits in SYN retries rather than failing. The listener is closed once
the lower nodes are in, so a stranger probing a candidate afterwards is refused
rather than left unanswered. In a job the mesh does not probe at all: each lead
listens on a port the kernel chooses, **advertises it in its backend blob**
(`osoe_transport_advertise`, the call the blob was waiting for), and the others
connect to the address and port the rendezvous carried (below). The port rule's
probing remains for the rendezvous's own parent connection, and for the tests
below the library that make a mesh without a rendezvous.

**What the map holds, and what it costs** (phase 1 step 2 slice 3; second review,
D7). Per rank: its node (4 bytes) and its **local index on that node** (2 bytes;
16 bits suffice at 256 PEs per node, and `OSOE_GTIDS` longer than 65,535 is
refused; *since the map became runs (`rank-map.h`) its fields are 32 bits, and
the bound on `OSOE_GTIDS` stays, far above any node*). The local index is the one fact about a *remote* PE that an RDMA
backend cannot do without — it forms a remote heap address as the node's
registered base plus index × the heap mapping's size plus offset (the mapping
being the heap and, past it, §5.8's internal region at `internal_offset`), and
under Slurm's cyclic or
arbitrary distributions the index is not derivable from the node map. The map is
shared, one copy per node: 6 × 10⁶ bytes ≈ 6 MiB per node at 10⁶ PEs, accepted for
now. A PE's own node is answered privately and in O(*L*): which local id a rank
has is a subtraction when the node's ranks are one contiguous block in local-id
order, and otherwise a binary search over the node's *L* ranks sorted — replacing
a world-sized `local_of_rank` array in every PE, which was 4 × *P* bytes per PE and
1 GiB per node at 4,096 × 256. `gtids` is sized by *L*, not *P*.

**The known next step was a range-compressed map.** Under block and cyclic
distributions the map is O(*N*) runs, not O(*P*) entries, and so is the broadcast
that fills it — which as designed sent 4·*P* bytes to each of *N* − 1 leads from
one NIC, O(*N*·*P*) bytes through node 0: 255 MB at 500 × 256 (about 2 s on 1 GbE),
16 GB at 4,096 × 256 (second review, algorithmic-scaling 9). Built with the socket
rendezvous (below).

`SLURM_STEP_NODELIST` is a compressed hostlist (`pi[01-04,07]`). Expanding it is a
small parser — call it fifty lines — **written with unit tests in tier 1**, because
it is pure string handling that would otherwise be debugged on a cluster.
*Built* (`base/hostlist.c`, `base.hostlist`): prefix, one bracketed list of
numbers and ranges keeping the lower bound's width, suffix; Slurm's multi-bracket
names are refused by name rather than guessed. Nothing expands the list: a lead
asks for its own entry and its parent's, O(length of the string) each.

**The rendezvous as built** (phase 1 step 3 slice 1; `rendezvous.[ch]`, `job.[ch]`;
decided with the author 2026-09-22). **A tree, not a star.** Node *i*'s lead
connects to its parent, (*i* − 1)/8, in a tree of fan-out 8 over the node indices
rooted at node 0, found by the port rule on the address its host-list entry names;
each lead listens on its own entry's address. §6.2's star put every connection
into one listener and every lead's copy of the job's tables through one NIC —
O(*N*) connections and O(*N*²) bytes out of node 0: 2.2 GB at 4,096 nodes for
the addresses and blobs alone, even with the map compressed (2.6 GB with a
block map's runs). In the tree a lead has one connection up and at most eight down, and sends each child
the tables once: O(*N*) bytes per edge, depth ⌈log₈ *N*⌉ — 4 at 4,096 nodes. The
fan-out is a constant, so no setting can make two leads disagree about who is
whose parent.

**The node count is the value that shapes the waits**, because a lead expects
children by its own count: one given too large a count waits for children that
do not exist. The order first written — accept the children, read their
reports, only then call the parent — let it do that before its parent had heard
from it, and then nobody could ever say why: its parent waited for it, the root
for the parent, every other lead for the root's tables, and no lead refused
anything (`tools/modelcheck/rendezvous_model.py`, STUCK; and
`shmem.job.disagree.node-count` hung until its timeout, written to show it). So
**a lead calls its parent before it accepts its children**, and **the hello
carries the node count and the job's size**, checked by the lead that answers:
a disagreement about the tree's shape is refused on the first edge it crosses,
before either end waits on it. The model shows both rules are needed — the
count in the hello without the order is STUCK too — and that with them every
case in which one lead differs ends in a refusal, with the trapped-state check
on. What no rendezvous can catch is two *groups* of leads that each agree
among themselves: the root's group completes a smaller job while the other
waits for a listener that has closed. That is two launches sharing one job's
ids, indistinguishable from one slow launch without a time limit, which §3.15
rules out; the no-progress rule is the answer there.

**A lead serves callers part-way through the hello side by side, and acts on a
hello only once it may** (the round-three review's N2). Read one at a time, a
hello followed by silence held the listener for good, and a hello's counts,
judged as it arrived, let a host that knew no secret end a lead's start with a
wrong one. Now an accepted connection's hello is read within 0.1 s - safe,
since an unanswered caller retries - and an answered caller waits, for its
confirmation and with no time limit, in a set of sixteen polled with the
listening socket: a caller that is answered writes its confirmation and
carries on, and one dropped for time would end the job. A full set accepts no
more until a place frees; the rest wait in the backlog, as a busy lead's
callers do. With `OSOE_JOB_SECRET`, a hello's counts and node claim are judged
only after its caller has proved the secret; without one nothing can be
proved, and the hello is judged as it arrives. `rendezvous_model.py` abstracts
a connection as the parent's accept, which none of this changes: the counts
are still judged before the parent counts the child.

Every read and write of the rendezvous waits as long as its peer takes,
sweeping, and ends the job naming the peer only by the no-progress rule
(§3.15) — a write too, since the tables are larger than a socket buffer. A lead
that refuses the job ends it by a fatal, which the launcher turns into the
job's end (`srun -K1`, §3.15; `osoe-launch` does the same), and a neighbour
waiting on its socket names it ("the rendezvous lost node 9, its child…").

*Up the tree* goes each subtree's report: per node its address, its ranks as
arithmetic progressions in local-id order (one under block, one under cyclic;
`rank-map.h`) and its blob; and the values every node must agree on — the job's
size, `SHMEM_SYMMETRIC_SIZE`, the handler checksum, `OSOE_COLLECTIVE_FANOUT`,
`OSOE_MAX_TEAMS`, `OSOE_COLLECTIVE_PIPELINE`, `OSOE_SMALL_COLLECTIVE_BYTES` —
which each parent checks against
its own, so a disagreement is refused by the
lead that sees it, naming both nodes (`shmem.job.disagree.*`). *At the root* the
map is built: the progressions are expanded once, into O(*P*) scratch on that one
node, to check that every rank is claimed exactly once (`shmem.job.duplicate-rank`,
`unclaimed-rank` keep their messages), then compressed into **runs** — stretches
of consecutive ranks whose node and local id advance by constant steps: one run
per node under block, one per round under cyclic. *Down the tree* goes the runs,
every node's address and every node's blob; each lead writes them into a
**node-local** object, `/osoe.<job>.<step>.<node>.job.<session>`, which its PEs
open like the control segment and the last of them unlinks. The step-2 job-wide
segment, which only worked on one host, is gone. A job of one node meets no one
and opens no socket: a machine with no network runs it.

*What it costs*, per node: O(*R* + *N*) shared — *R* runs (4,096 under block at
4,096 × 256, 256 under cyclic; 24 bytes each), and *N* addresses and blobs (132
bytes each, 540 KB at 4,096 nodes) — where the per-rank map was 6 MiB at 10⁶ PEs.
Which node a rank is on is a binary search, O(log *R*), and only the network path
asks it; whether a rank is on *this* node — the question every put and get asks
first (`osoe_routes_in_process`) — is this node's own rank set, O(1) when its
ranks are one progression: a subtraction for block, a division for cyclic, a
binary search over *L* otherwise. The root's O(*P*) scratch is once per job, as
the step-2 check of the whole map was. `shmem.rank-map` checks lookups against the
uncompressed map on §8.1's four poles, and the run counts at 4,096 × 256.

**`OSOE_JOB_SECRET`: decided, and built** (§10's open item). Optional, 128 bits as
32 hexadecimal digits, which a wrapper sets before `srun` and Slurm carries to every
task. With it, every hello — the rendezvous's and the mesh's — carries a fresh
nonce from each side, and each side proves it knows the secret with a SipHash-2-4
digest (`base/siphash.c`, checked against the published vectors) over the hello
and both nonces, without sending it: so a host that can reach a listener but does
not know the secret cannot join the job, and a proof overheard on one connection
answers no other. A caller that cannot prove it is refused and not counted; a lead
whose peer has no secret, or another one, ends the job naming both
(`shmem.job.secret.*`). What it does not do is protect a connection from a host
that can see and inject its packets after the hello: that would take encrypting
the traffic, and the network is still trusted that far, as Slurm's own is (§3.4).
Off by default, since §3.4's trust boundary is unchanged.

**The mesh is still eager, and what on-demand would need.** Every lead connects
to every higher node at init: *N*(*N* − 1)/2 connections job-wide, 8.4 M at 4,096
nodes, and a listen backlog of *N* − 1 (capped by `somaxconn`), which at 4,096
nodes turns the last connections into SYN retries rather than failures. Nothing
in this slice made laziness harder. On-demand would need: a connection opened at
the first operation toward a node, which the per-connection state already allows
— a wire, a ring slot and the connection area's buffers are all per peer slot, and
the dirty set and the busy bitmap only visit what has work; the lead's listener
kept open for the life of the job, served by a multishot accept on the ring
rather than closed after init; a writer that finds no connection toward a node
posting a request to the lead rather than reserving, since only the lead
connects (§3.12); and a rule for two leads that connect to each other at once —
the lower node's connection wins, as §6.2's direction already says. The ring's
bounds would then follow the active peers rather than *N*.

**`set_range` is not acted on here** (§4.10, §7.1 step 3). A TCP backend has
nothing to prepare before the network touches memory: it receives into its own
provided buffers and copies, and sends from connection buffers, so it neither
registers memory nor needs to know which words take atomics. The claim that step
3's backend is the first to act on it was wrong for this backend; the first that
must is an RDMA one.

**Failure handling:**

- Clients retry with jittered backoff for as long as it takes, or until
  `OSOE_NO_PROGRESS_TIMEOUT` if set, then abort naming what they were waiting
  for (§3.15).
- `shmem_init` raises the soft `RLIMIT_NOFILE` to the hard limit. Slurm propagates
  the submitting shell's limits, commonly a soft limit of 1,024, and a lead on a
  500-node job holds two sockets per peer node, its control wire and its data
  wire (§3.1; one until v0.2 step 4).
- **Bare `sbatch` launch.** If `SLURM_JOB_ID` is set but `SLURM_STEP_ID` is not,
  the program was started directly by a batch script rather than by `srun`.
  Fail at once with that message rather than waiting for peers that will never
  start. *(As built, keyed on the step id: Slurm 24.11's batch script has
  `SLURM_PROCID` and `SLURM_GTIDS` but no step id, captured in
  `shmem/tests/slurm-fixtures/`. Slurm's own steps, ids of 0xfffffff0 and up,
  and heterogeneous jobs are refused too, until captured.)*

*(Rejected: every PE connecting to rank 0 — about 77,000 connections into one
listener at 256 PEs × 300 nodes, violating §1, and failing at the 1,021st
connection under a 1,024 file limit. Rejected: rank 0 "publishing" its address, as
an earlier draft proposed — there is no channel to publish on without a KVS or a
shared filesystem.)*

**Non-Slurm launch** (plain `ssh`, a test harness) sets the same variables under
the `OSOE_` prefix. `OSOE_PROCID` decides which set is read, and then all of it:
the names are never mixed (§6.1, as built). The `SHMEM_` prefix is the
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
| `OSOE_STEP_NODELIST` | `SLURM_STEP_NODELIST` | No: one node needs no host list. *Since phase 1 step 3, required when there is more than one node*: the leads' addresses |
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
`OSOE_NO_PROGRESS_TIMEOUT` (§3.15) is osoe's own, read by the library; it
replaced `OSOE_BOOTSTRAP_TIMEOUT`, which is retired. So, since phase 1 step 3,
are `OSOE_PORT_RANGE` and `OSOE_JOB_SECRET` (§6.2), both optional, and since
step 4 `OSOE_KEEPALIVE` (§3.15); `OSOE_LIVENESS`, from the same step, is
retired with the development watchdog (§3.15).
`OSOE_NODE_MAP` (§8.1) is osoe's own too but is read by `osoe-launch`, which
turns it into the `OSOE_NODEID`, `OSOE_LOCALID` and `OSOE_GTIDS` above; the
library never sees it.

The test launcher, `tools/launch/osoe-launch`, starts L processes of a one-node
job with these variables set. Its job id is its own pid, so concurrent launches
never share a name. It fails on a nonzero exit, a signal, a timeout, or a clean
job that left `/dev/shm/osoe.<job>.*` behind, which it then removes (kept control
segments excepted, for `--gc`). And every tier-2 test checks, *while the job
runs*, that nothing of its job has a name once init has completed
(`invariants.h`) — the naming rules above, on every multi-process test.

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
  performs its implicit barrier, then tears down. **Finalize's barrier runs on a
  team of its own** — the world's members on counters no application collective
  touches (`OSOE_TEAM_FINALIZE`, layout 10). On the world's counters, a PE
  finalizing matched another PE's `shmem_barrier_all`: under `start_pes`, a PE
  returning from `main` ran the implicit finalize and let the others out of a
  barrier the application never matched (second review, D14). Now the two wait
  for each other, which is a hang in a program whose collectives do not match —
  that program's — rather than a barrier returning that nobody matched.
- **Leaving without finalizing ends the job.** An `shmem_init` program ends its
  use of the library with `shmem_finalize` or `shmem_global_exit` (spec §4); the
  specification finalizes implicitly only under `start_pes` (§9.1, Annex F.2.3).
  An `atexit` handler registered at the first init finds a process leaving with
  the library still initialized, prints so, declares the node's abort, and makes
  the status nonzero, since `srun -K1` acts only on a nonzero one — an `exit(0)`
  used to hang a release-build job until its timeout. A global exit already
  declared it joins instead, and the process ends with that exit's status
  (§3.15). `start_pes` registers its
  implicit finalize after that handler, so it runs first. **No exit path runs a
  collective the application did not call**: `osoe_fatal` leaves through `_exit`,
  not `exit`, so it never runs `start_pes`'s finalize — which is a barrier — on its
  way out (second review, conformance F1).
- **The count is per process, not per thread**, and must be updated atomically.
- **Concurrent first `shmem_init`** from two application threads passes through a
  once-gate, so bootstrap runs exactly once.
- **A node must not stop its progress engine while it owes a peer bytes.** The
  final barrier does not establish that it does not, and the gap is §5.3's own
  shape: the root releases *its own* node the instant the last node arrives, and
  only enqueues the releases that free the others, which later drain and flush
  passes put on the wire. So the root's PEs can leave that barrier, reach
  `shmem_finalize` and stop the agent while the message every other node is
  waiting for is still in a buffer only that agent sends — and every other node
  then waits for a node that has already exited. The lead therefore drains the
  engine to idle before its wires close — since phase 1 step 3 on the agent
  thread itself, as its last turn (`osoe_transport_agent_exit`), because the
  engine's ring is that thread's (§4.10). Measured
  at 3 hangs in 12 runs of the suite without it and 0 in 25 with it; the
  signature is that the root's non-lead PEs exit cleanly while every other rank
  is killed at the launcher's timeout.
- **A wire is unmapped only after both ends have stopped writing.** Same reason
  from the other side: a lead that has applied its release may still owe an
  acknowledgement for the segment that carried it (§3.4), so a peer can write
  after this side has run its last collective, and unmapping first turns that
  into a SIGSEGV on the node that did nothing wrong. Announcing and waiting are
  separate calls, so a lead tells every peer before waiting on any.

  *On a socket* (phase 1 step 3 slice 1; `tools/modelcheck/close_model.py`) the
  same rule has three parts, each a way to lose something. **A lead drains, then
  sends a goodbye, then ends its stream**: the goodbye is a header-only segment
  flagged `OSOE_AM_SEGMENT_GOODBYE`, sealed only once the connection owes the peer
  nothing, and the last segment the lead sends there; `shutdown(SHUT_WR)` follows
  its send. **An end of stream without a goodbye is a peer that died**, since a
  crashed process's kernel closes its sockets with the same FIN, so it ends the
  job naming the peer — a remote death that step 2 could only wait out. **A lead
  closes a socket only once the peer's end of stream has arrived**: closing while
  the peer still sends makes the kernel reset the connection, and the peer's send
  fails on the node that did nothing wrong. Nothing is lost by refusing to send
  after the goodbye: past the final barrier no lead needs anything from another
  but the barrier's releases, which drain before it. The model checks all of it
  over three nodes with an interior one, trapped-state check on; a goodbye that
  does not wait for the drain strands a child, a FIN without a goodbye trips the
  dead-peer check, and a close before the peer's FIN sends into a reset.
  **A lead whose node aborts ends each stream with an abortive goodbye instead**
  (§3.15, as built; `abort_model.py`): the same flag on a segment that carries
  one `OSOE_AM_ABORT`, sent without waiting for the drain, since the job's own
  traffic has stopped. A peer takes the end of stream after it for the job
  ending, not a death, and ends with the status it carried.
  *As built since v0.2 step 4's Part 2:* **each of a pair's two wires closes on
  its own** - its goodbye after its own last bytes, its end of stream after its
  goodbye, its socket closed once the peer's end of stream has arrived on it -
  and a reset is judged per wire, by that wire's goodbye and end of stream.
  `close_model.py`'s two-wire cases check it on a pair of nodes, with liveness:
  a goodbye on one wire alone makes the other's end of stream a death, and a
  close on either wire's end resets the other. The abortive goodbye goes on
  every wire that has someone to tell - none where the peer's own has arrived -
  so two that cross can arrive on different wires: a peer's abort is the
  peer's, not its wire's, and with no helpers, where one owner holds both of a
  peer's wires, its abort read on either excuses the other's reset and, naming
  another caller, stops a carrier waiting on either (Part 2's second
  adversarial read, S1 and N1; `carrier_model.py`). With helpers the two wires
  are two owners', and Part 3 carries the word between them.
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
  but do not leave it accidental. *As built (phase 1 step 7):* re-read, like
  everything the bootstrap reads, at each session's first init, since a new
  session re-runs the whole of §6: a changed environment gives it its own count,
  agreed afresh at its rendezvous.

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
   noticing: it is the second backend behind §4.10, so §8.1's node map can run
   the same program over both paths and diff. Two open items are timed
   to land with it, because this step writes the code they would otherwise have
   to restructure: **decide `osoe_transport_sync`** (§10 — whether a collective's
   internode phase crosses the boundary) while writing the internode phase that
   is its only caller, and **`OSOE_TRANSPORT_MORE`'s first caller**, since this is the
   first backend for which deferring a submission means anything.

   **`osoe_transport_set_range` also lands here** (§4.10), with the allocator call site
   it needs, for the same reason: the call site is in the allocator, which this
   step is already working around.
3. **Real transport.** Start with the `echo-uring` floor harness (§8.4), the
   transport's smallest reproducer. Then io_uring with the repaired reservation
   protocol, stream-parsed receive, the armed-flag idle loop, per-class wake words,
   reply credits and DRAINING caps, and the global rendezvous with its timeout.
   *(Done, in two slices. Slice 1 built the harness, the ring, the idle loop
   and the rendezvous, and measured what was left: quiet's wait and a writer's
   wait for a full buffer still polled the step-2 way, waking the lead and
   sleeping 20 µs. Slice 2 built the per-class wake words (§3.11): quiet and a
   writer with no room sleep on words the agent bumps only when someone waits,
   and a PE waiting for a reply credit takes a ticket in the credit's own word.
   Then it pinned the library to its floor (§8.4), and ran the multi-node
   tests over a link that delays, drops and fragments (tier 3, §8.2). The
   rendezvous's timeout is the no-progress rule, never by default (§3.15).
   Carried forward, deliberately: the mesh is eager, and §6.2 says what
   on-demand connections would need.)*

   The first backend that *acts* on `osoe_transport_set_range` is here — it is the
   first that must prepare memory before the network can touch it — but the hook
   and its allocator call site landed at step 2 (§4.10), because the call site is
   in the allocator rather than in a backend. *(Wrong for this backend, found
   building it: TCP receives into its own buffers and sends from connection
   buffers, so it has nothing to prepare. The first backend that must act on
   the call is an RDMA one; §6.2.)* The allocator half — recording
   hints and growing hinted allocations from the opposite end — is older still,
   from step 1, because retrofitting hint plumbing into a finished allocator is
   the expensive version.
4. **Robustness.** Pidfd liveness, abort propagation, `shmem_global_exit`,
   keepalive; Slurm environment fixtures and the heap-base-shift test mode (§8.2).
   *The first half is done (phase 1 step 4 slice 1; §3.15's four as-built
   paragraphs): the abort crosses nodes as an abortive goodbye, every PE ends
   when its node aborts, `shmem_global_exit`, pidfd liveness on every agent, and
   TCP keepalive. The next slice has the Slurm fixtures and the heap-base shift,
   and two refinements this one left open: the multi-node lead's pidfds on its
   ring, where a completion would find a death at once rather than within a
   sweep interval, and whether the development watchdog stays - retired
   (2026-09-23), and the ring not built for now.*

5. **Collectives** (added 2026-09-24 at the author's direction, ahead of
   statics: every code in the corpus reduces, and not every one needs a static,
   §8.6's U7). The data-carrying collectives of §9.10 over §5.5's accumulator.
   *The first slice has reductions and broadcast on the teams that exist, with
   `OSOE_REGION_INTERNAL` built for their internode phase (§5.5, §5.8, as
   built). The second puts that phase behind `internode.h`, with the generic
   implementation every backend may call (§5.8). The third gives the deprecated
   active-set routines - `_to_all`, `broadcast32`/`64`, `shmem_barrier` and
   `shmem_sync` - over the whole job, the only active set they then accepted;
   the seventh (2026-09-26) runs them on any active set (§7.4).
   The fourth adds collect, fcollect, alltoall and alltoalls, their active-set
   forms under the same rule, and the seam's gather (§5.5, as built). The fifth
   keeps D pieces in flight at each lead of the generic internode phase (§5.8,
   "Pipelined pieces"). The sixth makes the generic gather Bruck's allgather
   among the node leads, ⌈log₂ *N*⌉ steps in place of the tree's or over the
   whole result (§5.8, "An allgather for fcollect").*

6. **Symmetric statics and the compiler wrapper** (§4.3). *The first slice:
   `oshcc`, a POSIX `sh` wrapper configured by the build, installed with the
   library - the name every corpus build system looks for - which adds the
   headers, and for a link the library and its run path, `-fno-pie`, `-no-pie`
   and the statics linker script; and the statics themselves, as a region
   (§4.3, as built).*

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
collective, implementing at first only `SHMEM_CTX_DEFAULT`, `SHMEM_TEAM_WORLD`
and `SHMEM_TEAM_SHARED`. Retrofitting either parameter later touches every function
written. `shmem_ctx_create` and `shmem_team_create_ctx` existed and returned
nonzero until phase 1 step 8 implemented them, which the spec permits: §9.5.1 says an unsuccessful
context creation "is not treated as an error" — so a program written against
the spec already has the branch these take. The team split routines did too
until phase 1 step 7, which creates teams (§5.7); §9.4 gives a PE that is not a
member of a new team `SHMEM_TEAM_INVALID`, so the same branch serves there.

**The context form is the definition; the plain form calls it with
`SHMEM_CTX_DEFAULT`.** The other way round — a plain form that does the work and
a context form that discards its argument — passes every test that can be
written while one context exists, and is the shape that breaks when a second
one appears. Every AMO, `putmem`, `getmem`, `putmem_signal`, the signal updates,
`fence`, `quiet` and `pe_quiet` are generated this way from one body.

**Handles are opaque pointers with macro constants, not exported objects.**
`shmem_team_t` and `shmem_ctx_t` are pointers to incomplete types, so passing a
team where a context is expected is a compile error, and the predefined handles
are small integers cast to those types. A created team's handle is the address
of the library's descriptor for it plus the descriptor's generation - how many
times the PE has taken the team's number - times the size of the process's pool
of descriptors, so that a destroyed team's handle is told from a later team's
under the same number by arithmetic alone (§5.7; the team redesign's step D,
D6). A library that exported
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
| Deprecated routines and headers (Annex F) | `start_pes` — "Implementations must include it" — and the other deprecated names; the `mpp/shmem.h` header location | Step 1. Each is the modern routine under its old name, so the two spellings cannot drift. **`start_pes` registers a finalize at exit**, because a program written against it never calls one — finalization used to be implicit (F.2.3) — and without it every such program leaves its segment and heaps in `/dev/shm`, which is the state `osoe-stat --gc` exists to clean up after a *crash*. A program that exits normally must not look like one that crashed. A second `start_pes` is a no-op: it predates the reference counting that makes a repeated `shmem_init` meaningful, and a second implicit finalize would tear down a library the first call owns. `SHMEM_CACHE` (F.2.7) is six no-ops, which is the *correct* implementation on a cache-coherent machine rather than an unimplemented one — §4.1 already rests on coherence. `mpp/shmem.h` is a redirection to `shmem.h` and nothing else. The unprefixed names get no `pshmem_` spelling: §10's profiling interface is defined over the `shmem_` namespace and a `pstart_pes` would be an invention. **The deprecated atomic names of F.2.12 exist too** — `shmem_TYPENAME_fetch`, `_set`, `_swap`, `_cswap`, `_finc`, `_inc`, `_fadd` and `_add`, C11 and C/C++ alike — each the modern atomic under its old name, for the subset of Tables 6 and 7 the deprecated names ever covered |
| Environment variables (§8) | `SHMEM_SYMMETRIC_SIZE`, `SHMEM_DEBUG`, `SHMEM_VERSION`, `SHMEM_INFO`, and their deprecated `SMA_*` forms | Step 1. The `SHMEM_`-prefixed spelling controls when both are set, written as that rule rather than as the shortcut the three flag variables would allow, since `SHMEM_SYMMETRIC_SIZE` has a value and will need it. `SHMEM_VERSION` prints before the rendezvous, so a job that fails to start still says which library it was; `SHMEM_DEBUG` prints after it, because the two things worth knowing are the ones it produces — the control segment to point `osoe-stat` at, and the heap base every PE agreed on. `SHMEM_INFO` documents the `OSOE_` variables too, which a user would otherwise have to read the source to find |
| C11 type-generic interfaces | Part of the API | Step 1, for the types implemented. `_Generic` selects on *type*, and the exact-width types of Tables 6–8 are typedefs of the canonical ones on LP64 Linux, so the selection lists one association per distinct type and the library's build asserts that each typedef is the type assumed. Each list emits a *leading* comma per entry, so there is no `default:` association and a type outside the table is a compile error naming the call rather than a silent route to another type's routine |
| `shmem_pe_accessible`, `shmem_addr_accessible` | Part of the API | Step 1, and not stubs. Every PE of an osoe job runs the same executable by construction, so `pe_accessible` is whether `pe` names a PE of this job. `addr_accessible` additionally requires the address to be symmetric: in the heap, or since step 6 among the statics of a program linked with `oshcc` (§4.3). An automatic address answers 0, and so does a static in a program built without the wrapper - stated by the query rather than discovered at the first put |
| `shmem_signal_fetch`, `shmem_signal_wait_until` | Part of §9.8 and §9.11 | Step 1, with the other signal routines |
| Name-shifted deprecated routines | §10 item 1 wants a `pshmem_` entry point for every function | **Both readings, since 2026-09-26** (below): Sandia OpenSHMEM's `pstart_pes`, `p_my_pe`, `pshmalloc` and the rest as the definitions, the literal `pshmem_start_pes`, `pshmem__my_pe` and the rest as second strong names, the old names weak aliases |
| Deprecated active-set constants (F.2.8) and routines | `SHMEM_SYNC_VALUE` and the `pSync` sizes; `shmem_barrier`, `shmem_sync`, `shmem_broadcast32`/`64`, the `_to_all` reductions of Table 11, `shmem_collect32`/`64`, `shmem_fcollect32`/`64`, `shmem_alltoall32`/`64`, `shmem_alltoalls32`/`64` | The constants at step 1. **The routines at step 5, over the whole job and nothing else** - at the author's direction of 2026-09-24 to implement them so that corpus programs can be tested against osoe, with the whole-job rule and its scope Claude's decisions for the author's review (`docs/prompts/done/phase1-step5-slice3-prompt.md`): an active set of every PE - `PE_start` 0, `logPE_stride` 0, `PE_size` the job's - is the world team, and any other ends the job by name. Step 1 had refused the routines at the link, on the grounds that a wrong answer is worse than a missing symbol; that still holds, and a named refusal at run time is neither. It is the active set every corpus program that still calls these passes (ISx, PRK, bale and hclib's copies) - of which PRK's and bale's run on heap data, where ISx and hclib's reduce symmetric statics, which osoe has for a program linked with `oshcc` since step 6 (§4.3) - and forming a group for an arbitrary set of PEs is machinery osoe does not have. `pSync` and `pWrk` are neither read nor written, which leaves `pSync` as the spec requires it to be found afterwards and makes the reuse rules hold trivially. `broadcast32`/`64` leave the root's own `dest` alone, as the spec says; when the root is its node's lead, the node's copy is the root's `source`, read by the internode phase and the copy-outs and never written (`collectives.c`). The active-set forms of collect, fcollect, alltoall and alltoalls came with their team forms (step 5 slice 4), under the same rule. **Any active set since 2026-09-26** (step 5 slice 7, `active-set.c`, at the author's direction): the whole job keeps the world team's path; any other set within the job runs member-only algorithms over the program's `pSync` - a dissemination barrier, binomial-tree broadcast, fcollect, collect and reductions whose every signal is a counted add taken back by its receiver, so `pSync` is found at `SHMEM_SYNC_VALUE` afterwards and can be reused at once, and direct-put alltoall(s) - log2 of the set's size deep, touching nothing in the node segment. A set past the job, or a caller outside it, still ends the job by name |
| `shmem_ctx_session_start`, `_stop` (§9.9) | Part of the API | Step 1, as no-ops. The spec says sessions "do not affect the completion or ordering semantics of any OpenSHMEM routines", so doing nothing conforms. They are worth having now because the thing a session would inform is §3.9's aggregation through `OSOE_TRANSPORT_MORE`, and §4.10 already records them as that flag's first driver: the backend that can act on them should not also have to introduce them |

**The deprecated routines' profiling names.** §10 item 1 requires "an
alternate entry point name, with the prefix `pshmem_` for each OpenSHMEM
function", and does not say how to shift the seven unprefixed deprecated
routines — `start_pes`, `_my_pe`, `_num_pes`, `shmalloc`, `shfree`,
`shrealloc`, `shmemalign`. Until 2026-09-26 osoe gave them none, a known
non-conformance, rather than guess. Now it gives both readings (`deprecated.c`):
`p` and the name, which Sandia OpenSHMEM defines (`#pragma weak start_pes =
pstart_pes`, `_my_pe = p_my_pe`, `shmalloc = pshmalloc`, in its sources) and
so what existing tools call, as the definition; and the literal `pshmem_` and
the name - `pshmem_start_pes`, `pshmem__my_pe` - as a second strong name for
it. The old name is a weak alias, as every `shmem_` routine is.
`shmem.profiling` checks the three names of each.

Two of §10's other requirements are satisfied and worth recording as such, since
both look like omissions otherwise. Item 3 states that the C11 type-generic
interfaces "cannot have any equivalent `pshmem_` interfaces because [they] are
implemented as macros", so their absence is required rather than missing. Item 6
requires the types and constants the `pshmem_` interfaces need to be defined in
`pshmem.h`, which it satisfies by including `shmem.h`.

**Symmetric statics need `oshcc`** (step 6; §4.3, as built). Most of the
spec's examples use global or static variables as symmetric objects, and in a
program linked with the wrapper they are. A program linked some other way has
none: its statics are refused by name, the diagnostic saying the program was not
linked with `oshcc`, and `shmem_addr_accessible` returns 0 for them. The spec
leaves how an implementation makes statics symmetric to the implementation,
and every one of them has a wrapper for it.

### 7.5 Deferred (mechanical)

Nothing of the collectives: the scans (§9.10.10) are built since phase 1
step 10 (§5.8). *(Locks are built: §4.12. Reductions, broadcast, collect,
fcollect, alltoall and alltoalls are built, and the deprecated active-set
routines on any active set: §7.1 step 5, §5.5 as built, §7.4.)*

**RMA's full type/operation cross product, strided transfers and nonblocking
variants are implemented** (§9.6, §9.8.3-§9.8.4): every standard RMA type of
Table 5 has `put`/`get`/`p`/`g`/`iput`/`iget` and their nonblocking forms, the
sized `put8..128`/`get8..128` have the same six forms, `putmem_nbi`/
`getmem_nbi` sit beside `putmem`/`getmem`, and the typed and sized
put-with-signal and put-with-signal-nbi families sit beside `putmem_signal`
(rma-typed.c, signal.c). Every one is a thin wrapper over `putmem`/`getmem`/
`putmem_signal`, generated from the same type lists `shmem.h`'s C11 selection
uses, on the same "a type appears once" principle as the AMO tables (§7.3).
The C11 generic macros cover `put`/`get`/`p`/`g`/`iput`/`iget`/`put_nbi`/
`get_nbi`/`put_signal`/`put_signal_nbi`, each with and without a leading
context, dispatched on arity first as the AMO generics are (§7.3).
`signal_set` and `signal_add` (§9.8.7, §9.8.5) take a leading context in C11
the same way, by arity alone, since their pointer is a plain `uint64_t *` and
there is no type to select on; a C11 program that declares or defines either by
name - a profiling wrapper - writes `(shmem_signal_add)`, as it does
`(shmem_sync)`. C++ has no context-first form of them in the specification and
gets none: an overload would make a program that takes the routine's address
ambiguous (the adversarial read of 2026-10-02). osoe's C++ `shmem_sync(team)`
is an extension of that kind - the C/C++ synopsis gives the team form only as
`shmem_team_sync` - which predates the read and stays. Every
routine reaches the transport through the body of `putmem` or `getmem` with its
element width and its own name as parameters (`rma.h`), so a bad PE or a
non-symmetric address is reported as `shmem_double_put`'s, not as the byte
form's beneath it.

The nonblocking forms call exactly the blocking code. A conforming
implementation may complete a nonblocking operation before it returns, and the
blocking forms give no less than the nonblocking ones promise on either
backend: a put returns at local completion and completes remotely at quiet,
which is `put_nbi`'s contract already, and a get completes before it returns,
which `get_nbi` permits. Nonblocking gets across nodes are real: several in
flight before one wait, through `OSOE_TRANSPORT_NONBLOCKING` on
`osoe_transport_get` (§4.6, as built).
The strided forms are a loop of single-element transfers, and stay one: no
transport osoe targets has a remote-strided operation to map a whole `iput`
onto - RDMA's scatter/gather list describes the local side only, and a vendor's
strided memory key must be built by the memory's owner, the target, which a
one-sided call cannot arrange - and none of the corpus's applications calls
them, only conformance tests, tutorial examples and one microbenchmark (the
author's decision, 2026-09-24: no strided operation at the boundary). What the
loop can use is batching. An `iput`'s elements but the last carry
`OSOE_TRANSPORT_MORE` (§4.10), its second caller, so across nodes they cost one
wake of the lead rather than one each, and an RDMA backend would post them
under one doorbell. An `iget`'s elements are nonblocking gets, all but the last
with `OSOE_TRANSPORT_MORE`, completed by one wait on its PE that names the
routine: in flight together, up to the landing zone's records.

**Block-strided `ibput`/`ibget` are built** (§9.6.1.4, §9.6.1.8; phase 1 step 9
slice 1): the same generated families, the same `strided_put`/`strided_get`
loop generalized from one element to one block of `bsize` elements, so
`ibput`/`ibget` cost `O(nblocks)` transport calls rather than `O(nblocks *
bsize)` - each one a single contiguous transfer, not a loop over `bsize`
elements. Batched the same way: every block but the last carries
`OSOE_TRANSPORT_MORE`, and an `ibget`'s blocks are nonblocking gets completed
by one wait for the gets alone, so a block size of 1 costs exactly what
`iput`/`iget` costs (the spec's own Note: `shmem_ibput` with `bsize` 1 is
`shmem_iput`). `dst` and `sst` must each be at least `bsize` (spec's
Arguments); below that is undefined, and ends the job by name, as
`shmem_team_split_strided`'s stride of 0 does (§5.7). Both strides equal to
`bsize` lay the blocks end to end, and move as one transfer of the whole.

**The nonblocking fetching AMOs complete at issue** (§9.7.2): `fetch`, `swap`,
`compare_swap`, `fetch_inc`, `fetch_add` and the three bitwise fetches, each
`_nbi` over its AMO table, with context forms and C11 generics
(`compare_swap_nbi` is the one AMO with six arguments). Unlike the nonblocking
gets, each runs its blocking counterpart's fetching rmw and stores `*fetch`
before it returns, which the spec permits: the value is promised by the next
quiet, a deadline rather than a floor. Across nodes each call is therefore a
round trip where a batch could overlap. The upgrade is the rmw taking
`OSOE_TRANSPORT_NONBLOCKING` with a result destination the PE's next wait
copies out, as the gets' does (§4.6), revisiting §4.6's one outstanding
fetching AMO; nothing in the corpus calls them but the SOS test suite.

The collectives are mechanical *given* §5.5's rules, which they must inherit: the
local lead is the single writer before release and leaves last, and a peer's
`source` is read only after that peer has called. A pull-based collect or
alltoall is non-conforming.

### 7.6 Deferred 1.6 additions

| Routine | Why it waits |
|---|---|
| `shmem_ctx_session_start` / `_stop` (§9.9) | Hints only. A conforming implementation may ignore them entirely, so the no-op is the whole phase-1 implementation |
| `shmem_sum_inscan`, `shmem_sum_exscan` (§9.10.10) | Inherits the §5.1 two-level skeleton, but **not** §5.5's accumulator: each PE's result depends on team rank order, so it needs its own design - built at phase 1 step 10, a tree over the team's ranks (§5.8) |

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

A third, since phase 1 step 3, holds the same way: **an address per logical
node.** `osoe-launch` sets `OSOE_STEP_NODELIST=127.0.0.[1-N]`, so logical node
*i*'s lead listens on 127.0.0.*i*+1 — Linux routes all of 127/8 to `lo` with no
setup — and the rendezvous and the mesh find each other by address and port, as
they do between hosts, rather than by a probe that only a single host could
answer. A lead that bound or dialled the wrong entry would meet nobody, so the
tests catch it. The launcher refuses more than 254 nodes, which one bracketed
octet cannot name.

**What a logical node is now, and what that tests** (phase 1 step 3 slice 1).
Until step 3 two logical nodes on one host talked through a shared-memory
stand-in for a wire. Now they talk over TCP on loopback, each lead on its own
address, and everything between two leads is the code a cluster runs: the
rendezvous tree and its hellos, the mesh on advertised ports, the ring with
multishot receive over provided buffers, one send in flight per connection,
the goodbye and the end of stream. Short sends, receives that split a segment,
`-ENOBUFS` and its re-arm, and a peer's FIN are real events on this path, not
simulated ones. The fault injection that makes loopback produce them on every
run (`OSOE_WIRE_BYTES`, `OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED`) is kept, since
loopback's large buffers would otherwise hide them, and the tests that use it
are unchanged. **The four poles stand as they were**, and so do the tests this
step was told not to touch — the differential test, `shmem.internode.*`,
`shmem.tree.*`, `shmem.hang-walk`: their programs and registrations are the
same as when the step began, and only `osoe-launch` changed, to give each
logical node its address.

What loopback does not test, each named so no test is mistaken for it:

- **The link's time.** A loopback round trip is about 25 µs in the development
  VM, and nearly all of it is system calls and wakeups (§8.4). A real link adds
  serialization and a switch; a Pi adds slower cores. No test here checks a
  latency or bandwidth claim about a link. §8.4's ratios are measured over veth
  with `netem`, and its pins compare two ways of using the *same* link.
- **Loss, delay and a small MTU.** Loopback drops nothing, delays nothing and
  segments at 64 KiB, and TCP hides reordering from an application anyway.
  Tier 3 is where the transport meets these, and since phase 1 step 3 slice 2
  it runs the multi-node tests there, each logical node in a network namespace
  of its own (§8.2, "Tier 3 as built").
- **Separate kernels.** Every logical node shares one kernel, one clock, one
  `/dev/shm` and one page cache. The library is not meant to rely on any of
  them: objects are named by logical node (above), and addresses come from the
  host list. But no test on one host can show that it does not; only tier 4
  can.
- **Names.** `osoe-launch`'s host list is numeric, so only a cluster resolves
  host names. The same parser and one `getaddrinfo` per entry a lead needs
  handle a list of names.
- **A host with several addresses.** A lead listens on the address its entry
  names, and refuses by name one that is not local (`EADDRNOTAVAIL`). Which
  interface a cluster's host list names is the site's configuration.
- **IPv6**, which is not supported: an entry that resolves only to IPv6 is
  refused when it is resolved.

Plus a **force-remote flag**. An earlier draft had it route even
same-logical-node traffic over the network, and called that the strongest
differential test available. Implementing it showed the claim was built on a
mechanism that does not exist and a comparison the node map already provides.

**The node map is how a job says what is local and what is remote**, and it is
the only thing that needs to. `osoe-launch -n 6 -m 0,0,1,1,2,2` starts six
processes on one machine and gives each its logical node; PEs rendezvous only
with the PEs named in their own node's `OSOE_GTIDS`, so a PE never maps a heap
outside its logical node and the locality function has no address it could
return. Every arrangement is expressible — all local, all remote, and the four
poles above — so running a program "both ways and diffing" is `-m 0,0,0,0`
against `-m 0,1,2,3`, with no flag involved.

The one arrangement the map cannot express is two PEs that *are* on one logical
node, and so *have* mapped each other's heaps, communicating over the network
anyway. That is what the flag was for, and it is exactly the case with no route:
connections exist per node **pair** (§3.4), so there is none from a node to
itself and such an operation has nowhere to go. Giving it one means a self slot
in the connection area, a wire from a node to itself, and peer-slot arithmetic
that no longer excludes this node — real structure, in the data path, for a
feature no production run will ever use.

**So the flag keeps only its locality half.** `OSOE_FORCE_REMOTE=1` makes
`shmem_ptr` and `shmem_team_ptr` answer `NULL` for a node-mate, while the traffic
itself still takes the intranode path. That costs nothing, and it keeps the
property worth having: a program cannot be written against the pointer shortcut
and then quietly depend on it, and a boundary that stopped consulting the
locality function fails a test. What it no longer claims is to exercise the AM
path for same-node pairs — the node map does that by putting the pair on
different nodes, which is both simpler and closer to the job a real cluster runs.

**It took two predicates to be true** (phase 1 step 2 slice 3; second review,
roce-seam F4, premises P2). Until then one function answered both "which path
carries this operation" and "may I hand out a pointer", so the flag changed the
route as well: `OSOE_FORCE_REMOTE=1` with a put to a node-mate aborted with *no
connection toward node N*, 4 runs in 4, while this section, `job.h` and commit
`56dedff` all said the route was untouched. The split is
`osoe_routes_in_process` (the node map, and nothing else) and
`osoe_direct_access` (the route, minus the flag). It is also what makes §4.10's
"dispatch is per (pe, operation class)" a thing the code can express rather than
a claim: `direct` answering non-NULL while a class's route is elsewhere is
exactly what a NIC with CPU-incoherent atomics needs. `shmem_addr_accessible`
consults neither: accessibility is reachability through OpenSHMEM, which every
PE of the job has.

Two further boundaries on the flag, both consequences rather than concessions:

- **A PE is always local to itself.** Connections exist per node *pair* (§3.4),
  so there is none from a node to itself and an operation addressed to a PE's
  own rank has nowhere to be routed. The flag routes traffic *between* PEs; a PE
  reading and writing its own symmetric memory is not traffic. This is not
  cosmetic: `osoe_transport_load` — the coherent load behind every `shmem_test`
  and every `wait_until` (§4.10) — resolves through the same locality function
  on the caller's own rank, so a rule that made a PE remote from itself is a
  null dereference on the most-travelled path in the library.
- **It needs at least two logical nodes**, and is refused at init otherwise. On
  one node the flag would make every PE unreachable by pointer while no remote
  path exists to replace it, which is a job that can allocate and synchronize
  but whose `shmem_ptr` lies about a machine with nowhere else to go. A job that
  wants the remote path asks the node map for it.

**`SHMEM_TEAM_SHARED` is the singleton `{self}` under the flag**, and the
earlier claim here — that it keeps the node's membership, since returning `NULL`
unconditionally is conforming — was wrong. Spec §7 defines that team as the PEs
whose memory the caller can reach with loads and stores *and* requires
`shmem_ptr` to answer non-NULL for every one of its members; a node-sized SHARED
whose members answer `NULL` violates it (second review, conformance F2; commit
`de540da` recorded the mistaken reading, and §4.8 says what is actually
conforming: `shmem_ptr` may return `NULL` for PEs that are **not** members of
SHARED). The cost is the one the old text was avoiding — the team's size differs
between a run with the flag and one without — so a program whose transcript
depends on SHARED's size is not a differential candidate. That is a smaller
price than a team whose defining property does not hold, and it is stated here
because it is a difference a reader of a diff has to know about.

**What the differential test compares, and what it cannot** (premises 11). The
four maps run one program over every arrangement of the node map, so every
*pair* of PEs is intranode in one run and AM in another, and the transcripts are
diffed. What no arrangement can produce is an **all-AM run with more than one PE
per node**: a PE's node-mates are reached in its own process, and the flag does
not change that, because connections exist per node pair and there is none from
a node to itself. So the AM path is never exercised with several PEs of one node
writing into the same connection *from the origin's side* of the diff — that
combination appears only in the mixed maps, and only for the pairs that cross
nodes (`shmem.internode.*` and `shmem.tree.*` cover it, arrival table and all,
but as assertions rather than as a diff). The other two gaps the review named
stand: every compared observation is fenced by a quiet and a barrier, so the
diff cannot see code that works only because the intranode path is stronger than
the contract; and `put_signal`'s two halves always share a segment at the sizes
used.

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
| 2 | Logical nodes over loopback — since phase 1 step 3 over real sockets, each node on its own address (§8.1) | Integration, threading, the socket path |
| 3 | Logical nodes in network namespaces over veth with netem — the floor harness's sweep, and the multi-node tests (the `tier3` preset; below) | Most transport bugs |
| 4 | Real cluster | Everything else |

> **ThreadSanitizer cannot see a happens-before that runs through another
> process, and the AM data path has one.** The lead's agent writes a local PE's
> heap while that PE's own main thread touches the same bytes; what separates
> them is the barrier, whose chain is this PE's arrival, then whichever local
> member was last posting the §5.3 handoff, then the root node, then the
> release. Every link is a `seq_cst` operation on shared memory and the middle
> ones are in other processes, so tsan reports a race it has no way to
> disprove. The multi-node data-path tests therefore do not run under it; the
> single-node map does, and there the intranode path applies every put in the
> calling thread, which is exactly the case tsan can check. Suppressing the
> report by name would hide real races in the same parser, and adding loads for
> the tool to follow would make it green without making anything safer.

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

*Tier 3 as built* (phase 1 step 3 slice 2). `osoe-launch -N PREFIX` starts
logical node *i*'s ranks in the network namespace `PREFIX`*i*, and `-H` gives
them the host list those namespaces' addresses make, in place of
`127.0.0.[1-N]`. `tools/launch/tier3.sh` makes the namespaces, one per node and
one more holding a bridge, joins each to the bridge by a veth pair, and shapes
each node's egress with `netem`. There are three shapes: a Pi-like link (1 Gbit/s
and 100 µs each way, §5.3), the same dropping 0.1% of packets, and a 1,280-byte
MTU at 25 µs each way. Segmentation offloads are off, so `netem` delays, paces
and drops packets of the MTU's size, as a wire would, and not the 64 KiB bursts a
veth pair otherwise carries whole. Entering a namespace needs root, so the
script runs under `sudo -n`. Each rank enters its namespace between fork and
exec, then drops to the invoking user with that user's groups, so the job runs
as it would anywhere else. Namespaces are named by the script's pid, so runs
never meet. They are removed on exit, on failure and on a signal, and
`tier3.sh clean` removes any a killed run left. What stays behind is what
`netem-sweep.sh` leaves: the `/run/netns` mount point and the loaded modules.

The tests are not a default `ctest`: they change the machine's network state
while they run and need passwordless `sudo`, which §8.5's VM has. The `tier3`
preset registers them (`cmake --workflow --preset tier3`). The subset is every
tier-2 test whose traffic crosses a node boundary through the transport, each
with the environment tier 2 gives it. That is the node-pair wire and the
progress engine below the library; the internode maps, where all three of
§3.11's waits run through two reply credits and a 2 KiB buffer; the AM barrier;
the reply-starvation guard; the tree at depth three and at thirteen uneven
nodes; the rendezvous; the differential over §8.1's four poles; the floor; a
global exit, whose status crosses as the abortive goodbye; and, since step 4, a
node whose link is cut, which only keepalive can find (`TIER3_CUT`). Left out
are the tests that never reach a socket, and those whose subject is a PE that
dies or stops rather than the link.

*What it found.* One thing loopback hid, and it was not in the transport. The
tests below the library open their mesh without a rendezvous, and that path
dialled `127.0.0.1` for every peer. On one host that reaches every lead; in a
namespace it reaches only the node's own, so the AM barrier test retried
forever. That path now dials each peer's host-list address, as the rendezvous
does (`osoe_listener_node_address`), and no library code assumes one host.
Everything else passed on all three shapes the first time. The library's ratios
to its floor there are in §8.4.

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
  heap at the same base. A test mode shifts `B` per PE. *Done* (phase 1 step 4
  slice 2): `OSOE_HEAP_SHIFT=BYTES`, a whole number of pages, maps each PE's
  own heap at `B + rank × BYTES`, so that no two PEs of the job hold a
  symmetric object at the same address; by world rank, so one setting moves
  the intranode case and the internode one at once. The lead still searches
  its candidates for `B` and publishes it unshifted. A sample runs shifted in
  every preset (`shmem.heap-shift.*`), and the whole of tier 2 passes with
  `OSOE_HEAP_SHIFT=2m` in ctest's environment. It found two places that
  assumed one base, neither on the wire: `shmem.rma`'s symmetry check compared
  addresses between PEs, where the specification promises only corresponding
  objects (it now compares each object's offset from another), and
  `shmem_align` decided whether a PE's base met an alignment from that PE's
  own base, so that under a 2 MiB shift a 4 MiB alignment succeeded on even
  ranks and failed on odd ones (it now decides from `B` and the shift, which
  every PE shares). No wire format depended on identical addresses.
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
- **The probe's receive check.** *Done* (test review, 2026-09-23). It asserted
  only more than one completion; it now requires the verified shape, one
  completion per provided buffer, each saying more follows, then one terminal
  `-ENOBUFS`.

### 8.3 Without Pi hardware

Tiers 1–3 run unchanged on x86. **x86 loopback is the adversarial case, not the
easy one:** every decision justified by "wire latency dominates by two orders of
magnitude" — copy into staging, futex over spin, generous chunking — is least
defensible when network and cores are fast. Holding on modern x86 loopback means
holding on a Pi with room to spare. Sweep netem delay from zero to Pi-like and
watch where the constants stop working; a better experiment than one point
measurement on real hardware. (`tools/echo-uring/netem-sweep.sh` does it for the
floor harness, and §8.4 has the result: on aarch64 loopback too, bandwidth is the
half of the assumption that fails.)

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

*As built* (phase 1 step 3 slice 1). `tools/echo-uring` runs both ends on
loopback by default, or one end each with `--server` and `--client` across two
hosts. It uses exactly §3.3's shapes — a `SINGLE_ISSUER` ring whose CQ is sized
from its provided-buffer ring, multishot recv with every provided buffer recycled
as soon as its bytes are consumed, a re-arm on every CQE without
`IORING_CQE_F_MORE`, one send in flight with a short send resubmitted,
`TCP_NODELAY` — and times the same two exchanges over blocking `send` and `recv`
on the same kind of connection. Its first act is a verified stream: every 8-byte
word carries its own offset, through a 4 KiB send buffer and four 1 KiB provided
buffers, which forces about 250 `-ENOBUFS` terminations and re-arms and a few
dozen short sends per MiB. The ctest `env.echo-uring` pins two **io_uring-to-blocking
ratios on the same link**, each the median over seven pairs of trials measured
side by side (a pair shares whatever the machine was doing), and within a pair
the two kinds of round trip alternate exchange by exchange, on two connections
open at once that one thread on each end serves in turn. Timed as two blocks, a
virtual CPU asleep through one block and awake through the other failed the
round-trip pin at 3–4.7× in about one run in ten; interleaving is what §8.4's
library floor showed holds it still. Served by a process per connection, the
two kinds did not share the server's scheduling, and under 16 busy loops one of
a pair's servers ran badly placed through whole trials: 2.3× once in 30 runs
(test review, 2026-09-23). The pins: the **fastest**
round trip, at most 1.5× — measured 1.02–1.25× idle, under 16 busy loops and
beside a `ctest -j4` run — and the stream, at least 0.5× — 0.85–1.06×. Trials
are 1 GiB, since a 256 MiB stream lasts 13 ms, shorter than a scheduler slice
among 16 busy loops.

**The median round trip is reported and not pinned, and why is a result.** The
fastest round trip is the length of the path, both processes on a CPU, and the
two kinds agree on it everywhere in the median of seven side-by-side pairs, which
is what the pin takes. A single pair can differ 4–5×: measured idle in phase 1
step 3 slice 1, part C, one kind's fastest trip in a pair was 15 µs where the
other's was 3, a virtual CPU asleep for one set of trials and not the other.
The median is where the woken process gets to
run, and there they differ under one kind of load: beside a `ctest -j4` run - many
processes sleeping and waking, CPUs partly busy - blocking `recv` measured a
3.6 µs median and the io_uring wait 15–28 µs, 3–5×, with `IORING_SETUP_COOP_TASKRUN`
and `IORING_SETUP_DEFER_TASKRUN` alike; idle (25 µs, a sleeping virtual CPU woken)
and under 16 busy loops (5 µs) the two agree within 1.2×. That is consistent with a
blocking socket's wakeup being a synchronous wake the scheduler may place on the
waker's CPU, which io_uring's task-work wakeup is not - *inferred*, not traced. It
is what the progress thread's ring wait will pay per hop on a partly loaded node,
which is the Pi's normal state (§1), so it is measured again against the library
(§7.1 step 3, part C). The pins were checked by breaking the harness: a 50 µs pause
in its pump measured 13× and 0.05×, and a missing re-arm after `-ENOBUFS` hung into
the timeout. They are checked in the optimized, uninstrumented build only: under
asan and tsan the io_uring round trip measured 6.4× and 3.9×, the instrumentation
of the harness's own loop rather than the link, and every preset still runs the
verified stream. The ratio of
the *library* to this floor is §1's actual target; it is pinned once the library
runs on the ring (§7.1 step 3, below).

*Measured* (4-core aarch64 VM on Apple silicon, kernel 6.12, median of 5,000 or
20,000 round trips; `tools/echo-uring/netem-sweep.sh`, which runs the two ends in
two network namespaces over a veth pair shaped by `netem`, §8.3's sweep):

| Link | 8-byte round trip, io_uring / blocking | Stream | One-way latency ÷ intranode put | Copy ÷ stream bandwidth |
|---|---|---|---|---|
| loopback | 26.3 / 25.0 µs | 19.0 / 20.2 GiB/s | ≈ 1,200 | **1.7** |
| veth at 1 Gbit/s | 32.9 / 31.9 µs | 114 / 114 MiB/s | ≈ 1,500 | ≈ 170 |
| … plus 25 µs each way | 96.3 / 95.6 µs | 114 MiB/s | ≈ 4,400 | ≈ 170 |
| … plus 50 µs each way | 162.8 / 161.6 µs | 114 MiB/s | ≈ 7,400 | ≈ 175 |
| … plus 100 µs each way, about a Pi on 1 GbE (§5.3) | 289.3 / 286.5 µs | 114 MiB/s | ≈ 13,000 | ≈ 175 |

"Intranode put" is the library's `shmem_putmem` of 8 bytes, 11.0 ns in the same
VM (§8.4's third floor, measured in phase 1 step 2 slice 3); the harness's own
proxy, an 8-byte store and a `seq_cst` fence, is 0.5–1.0 ns and makes every
latency ratio 10–20× larger still. "Copy" is `memcpy` in 64 KiB chunks over a
64 MiB working set, measured in the same run: 19–33 GiB/s, the spread being which
of the host's core types ran the VM's CPU, which is also the noise in every
absolute number here. And the idle loopback round trip is mostly the VM waking a
sleeping virtual CPU: with every CPU kept busy (the load loop), the same exchange
measures about 5 µs, which puts loopback's latency ratio nearer 230 than 1,200 —
still two orders.

**What it says about §10's assumption.** Wire *latency* dominates by three to
four orders of magnitude at every point of the sweep, loopback included, so
nothing decided on it — futex over spin (§3.11), deferring the wake
(`OSOE_TRANSPORT_MORE`), the sweep interval — is in question. Wire *bandwidth*
dominates by about 170× at 1 Gbit/s on this host, and not at all on loopback,
where a stream is itself a kernel `memcpy` and an extra copy costs about 60% of
the transfer. That is the adversarial case §8.3 predicted, not a surprise, and it
is the one the copy decisions (§3.5's copy into the connection buffer, §3.3's
copy out of a provided buffer) are measured against when the library runs on
the ring. A Pi's `memcpy` is slower than this host's, so its copy-to-wire ratio on
1 GbE is lower — tens rather than hundreds, *recalled*, unmeasured until the
harness runs there.

**The library against its floor** (phase 1 step 3 slice 1, part C; release
build, idle VM, two logical nodes of one PE each on loopback, `echo-uring` and
the library interleaved, the median over 7 repetitions of 20,000 operations;
`build/scratch`'s `internode-bench`, not yet a test).

| Operation | Library | ÷ `echo-uring` (25.3 µs, 17.7 GiB/s) |
|---|---|---|
| 8-byte get | 51.9 µs | **2.05×** (2.03–2.06) |
| 8-byte fetch-add | 51.9 µs | **2.05×** |
| 8-byte put, then quiet | 101 µs | 4.0× |
| 1 GiB of 64 KiB puts, then quiet | 547 MiB/s | 0.031× |

**A round trip is two wakes dearer than the harness's**, and that is the whole
factor for a get. The harness's round trip wakes two threads, the server and
then the client. The library's wakes four: the caller wakes its lead, the lead
at the other end wakes to the request, the caller's lead wakes to the reply, and
the lead wakes the caller. On loopback a wake is most of what a round trip costs:
the harness's fastest trips take 3–4 µs against its 25 µs median.

**The other two rows measure a step-2 stand-in, not the ring.** Quiet waiting for
an acknowledgement, and a writer waiting for a full connection buffer to turn
over, still wake the lead and then sleep 20 µs before looking again (`poll_lead`
in `transport-am.c`). Step 2 had no word for the lead to bump, and the code says
the stand-in must not survive step 3. The counters show one segment each way per
put and quiet, so nothing extra is sent: the time is the sleep, which Linux's
default 50 µs timer slack can stretch to 70 µs. *Measured*: with a yield in
place of the sleep, in a scratch build, a put and quiet takes 27.5 µs (1.1×)
and the stream runs at 6.0–10.5 GiB/s (0.34–0.6×). The waits get their words
with §7.1 step 3's per-class wake words, and the library's ratios are pinned
after that, not before: a pin now would pin the stand-in.

**Under load, the library's round trip is its wake count.** The wakeup finding
above promised this measurement. Beside a `ctest -j4` of the debug suite (load
average 6–16 on four CPUs, the models left out), the harness's median round
trip falls to about 4 µs, since no CPU sleeps, and io_uring and blocking agree
within 1.1× this time. So the 3–5× depends on the mix of load, and this mix did
not reproduce it. The library's get does not fall with the harness: about 30 µs
in six repetitions of seven, whether the harness's median was 4 µs (four
repetitions, 8–9×) or 16 µs (three, about 2×), because each of its four wakes
now waits for a CPU. *(Corrected in slice 2: this said "about 7×", a ratio of
medians taken across repetitions whose load differed.)* A partly loaded node is
the Pi's normal state (§1), so its round trips there will cost the wakes they
need, not the wire.

**With the waits' words** (phase 1 step 3 slice 2, part A; the same method).
A put and quiet takes 52.2 µs idle, 2.06× the floor: one library round trip,
the same four wakes as a get, where `poll_lead` cost 101 µs. The stream runs at
3.1 GiB/s (0.18×) at the default buffer, and the rest of its shortfall is the
buffer. Each 64 KiB put fills half of a 128 KiB buffer, so a writer waits for a
turnover on every other put, and a wake is what each wait costs. With
`OSOE_AM_BUFFER` at 256 KiB the stream runs at 8.6 GiB/s (0.49×), and at 512 KiB
to 2 MiB at 9.5–10.1 GiB/s (about 0.56×). The default stays the floor (§3.4): a
larger buffer is memory per connection, which the eager mesh multiplies by *N*.
Beside a `ctest -j4`, a get, a fetch-add and a put and quiet each take about
30 µs. Spinning before the futex would take the idle round trip to 1.14×, and
§3.11 says why it is not done.

*Since v0.2 step 4* the default is no longer the floor but four chunks, 256 KiB
(§3.4), and a put fills the room a buffer has (§3.5): the stream runs at
0.58–0.67× (the note after the pin table, below).

**Pinned** (phase 1 step 3 slice 2, part B; `shmem.floor`, checked in the
release preset beside `env.echo-uring`). One job of two logical nodes, one PE
each, times the floor itself: a trivial io_uring echo and stream on a raw TCP
connection between its two processes, dialled at node 1's host-list address,
so it crosses the link the library does. In each of seven pairs, 2,000 rounds
each time one floor round trip, one get, one put and quiet and one fetch-add,
in an order that rotates. Then a 1 GiB stream of 64 KiB pieces on each. Each
pin is the median over the pairs of a ratio taken within a pair. Twelve runs
unbroken (four idle, four beside a `ctest -j4`, four under 16 busy loops), and
nine with step 2's poll put back into quiet's and a writer's waits:

| Pin | Bound | Measured | With the poll |
|---|---|---|---|
| Fastest get, fetch-add, put and quiet ÷ the floor's fastest | ≤ 3 | 1.4–2.2 | put and quiet 2.1–2.7 idle, 3.0–5.7 loaded |
| Put and quiet, and fetch-add, median ÷ a get's | ≤ 1.5 | 0.98–1.00 | put and quiet 1.8–2.1 idle, 2.7–14 loaded |
| Stream ÷ the floor's, default buffer | ≥ 0.1 | 0.21–0.43 | 0.037–0.055 |

Reported, not pinned: a get's median trip is 1.8–1.9× the floor's idle and
beside the build, 1.3–1.9× under the busy loops (idle, 55 µs against 29 µs).
The library's stream, 3.0 GiB/s idle against the floor's 14, is 0.21×. With
a 1 MiB buffer it is 0.52–0.69×.

*Since v0.2 step 4* a put fills the room a buffer has (§3.5) and the default
buffer is 256 KiB (§3.4), and the stream row reads, under this section's
protocol at that step's tree: bound ≥ 0.3, measured 0.72–0.77 idle, 0.43–0.54
beside a `ctest -j4`, 0.43–0.65 under 16 busy loops; with the poll 0.145–0.148
idle, and 0.53–0.62 under the loops, where the scheduler's delay to every woken
waiter hides it - the pin raised from 0.10, by way of 0.15 at the first of the
two, so that an idle poll fails it twice over and no healthy run came within
1.4 times of it (`docs/decisions/2026-10-07-one-thread-profile.md`,
`pin-protocol.txt`).

*What the fastest trip can see, and what it cannot.* It is the length of a
path, and between `echo-uring`'s two kinds — one path of the same length — it
is what holds still. Between the library and the floor it compares four wakes
with two. Timed one kind after another, 2,000 trips each, it moved between
1.7× and 4.1× in the median of pairs over eighteen runs, and a single pair
from 0.5× to 12×: one kind's trips met a sleeping CPU and the next kind's a
waking one. Interleaving
the kinds within each round is what made it hold still, at 1.4–2.2×, since a
round's four trips share whatever the CPUs are doing. It still cannot see
step 2's poll. That poll slept 20 µs, about as long as an idle round trip, so
a put and quiet's fastest trip with it measured what one without it did. The
pin that sees the poll compares two library operations that need the same
wakes: a put and quiet against a get, 1.00× with the words and 1.8× or more
with the poll, whatever the load.

*Why the stream is pinned at the default buffer.* The stream exercises a
writer's turnover wait, and at 128 KiB every other 64 KiB put makes that
wait: 0.21–0.43× with the word, 0.037–0.055× with the poll. At 1 MiB a writer
seldom waits, and the poll measured 0.35–0.64× against the word's 0.52–0.69×,
inside the noise. A stream pin at a large buffer would not have caught the
stand-in this slice removed.

*Since v0.2 step 4* the stream is pinned at the default 256 KiB, where a put
fills the room a buffer has: the pin is 0.3×, the word measuring 0.58–0.67× and
the poll 0.161–0.164×.

*On a shaped link* (tier 3, §8.2: the same test, one logical node per network
namespace, 64 MiB streams). There the wire dominates the wakes, as §10's
assumption says it should:

| Link | Fastest trip ÷ floor | Median ÷ floor | Stream ÷ floor |
|---|---|---|---|
| Loopback (tier 2, above) | 1.4–2.2 | 1.8–1.9 | 0.21–0.43 |
| 1 Gbit/s, 100 µs each way (Pi-like) | 1.02–1.06 | 1.01–1.05 | 1.00, at 114 MiB/s |
| The same, dropping 0.1% | 1.03–1.05 | 1.08 | 1.00, at about 108 MiB/s |
| 1,280-byte MTU, 25 µs each way | 1.28–1.33 | 1.25 | 1.00, at 113 MiB/s |

A library round trip needs two wakes more than the floor's. On 200 µs of wire
they add 2–6% (5–9 µs); at 25 µs each way, about 25 µs, 30%; on loopback, where
the wire is nothing, they double the trip. The default buffer's stream, 0.21×
on loopback, runs at line rate on every shaped link, since a 1 Gbit/s link
takes a millisecond to carry one 128 KiB buffer and a turnover's wake is far
shorter.

*Since v0.2 step 4* the default buffer is 256 KiB, four chunks (§3.4), which a
gigabit takes about two milliseconds to carry, and the loopback stream is
0.58–0.67×; the rows above were measured at 128 KiB.

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
what CI can run anywhere. The model checker's tests (`tools/modelcheck`, which
needs Python 3) carry `model` beside `tier1`. They take about half an hour (29
minutes in the development VM on 2026-10-03) and run in
the release build only, so the `release` test preset leaves that label out and
the `models` preset runs it alone: `ctest --preset models`, or `cmake --workflow
--preset models` from a clean tree. The project's own verification is the four
presets and `models`.

**Page size.** The VM runs 4K pages; Raspberry Pi OS on a Pi 5 defaults to 16K.
Nothing may hardcode 4096 — including the §4.3 linker script, which aligns to
`CONSTANT(MAXPAGESIZE)`. If page-size bugs surface before a Pi is available, a
16K kernel boots in the VM (Apple Silicon supports 4K and 16K granules, not 64K).

**`tools/probe`** checks every kernel assumption this document makes and prints
a pass/fail line for each: single-issuer rings with an explicit CQ size,
`IORING_OP_FUTEX_WAIT` returning `-EAGAIN` on a stale value, multishot recv
producing one CQE per provided buffer plus a terminal `-ENOBUFS`, and the absence
of 64-bit futexes. Its futex checks use the forms the library uses: a word in a
**named** shared-memory object, opened by name by a process that inherited nothing
of it, woken by `futex_wake` from another process, both by `futex_wait` and by an
`IORING_OP_FUTEX_WAIT` without `FUTEX2_PRIVATE` (§3.10's wake word). **Run it first on
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
built or copied: corpus licenses vary, and none is this repository's (LGPLv3).
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
3. **A single ordered data stream per node pair.** (§3.1) A second data socket
   destroys free fence; the control wire beside it carries no data (§3.1).
4. **The progress thread never waits for `filled == head`.** (§3.5) Violating it
   hangs under oversubscription.
5. **Signals land after every payload byte**, guaranteed by in-order application
   of a single stream. (§3.2)
6. **The allocator is a pure deterministic function of its call sequence.**
   (§4.4) Violating it desynchronizes heaps silently.
7. **The wire format never depends on identical virtual addresses.** (§4.2)
   Testable since phase 1 step 4: §8.2's heap-base shift gives every PE's heap
   a base of its own, and tier 2 passes under it.
8. **The wakeup mechanism affects only how soon a sealed buffer is submitted,
   never whether.** (§3.10)
9. **Barrier queue bounds rest on collectives being blocking.** (§3.8) A future
   `shmem_barrier_nbi` dissolves the argument.
10. **Collective epoch counters are compared for equality, which assumes bounded
    skew.** (§5.4) Guaranteed by blocking collectives. Nonblocking collectives
    break it and must move to 64-bit `>=` predicates with a separate wake word.
    The generic internode phase's piece counters are not epoch counters: since
    phase 1 step 5 slice 5 they are 64-bit and compared as at least, since a
    child runs up to D pieces ahead of its parent's wait (§5.8). Nor are its
    allgather's READY and DATA (phase 1 step 5 slice 6): each gather adds one
    to each, and a lead waits for at least its count of gathers plus one.
11. **AMO result descriptors carry values; get descriptors carry references.**
    (§3.8) Unifying them reorders AMOs.
12. **The completion queue is sized from the provided-buffer ring, not from the
    submission-queue bound, and each ring arms at most one futex wait.** (§3.3,
    §3.10) One multishot SQE yields many CQEs, and every extra armed wait adds one
    more; either mistake reintroduces an overflow path under load only. With the
    zero-copy send on, the notifications a wire may owe count too, and a wire
    copies past 16 of them.
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
    The one other writer is the internode phase, into the places of other nodes'
    blocks, once the lead has handed `dest` over (`internode.h`): since phase 1
    step 5 slice 6 the generic gather's puts from the other nodes' leads, which
    READY holds back until then (§5.8).
17. **A data-carrying collective reads a peer's `source` only after that peer has
    called.** (§5.5) Pulling before arrival is non-conforming under 1.6 §9.10.
18. **Futex words are 32-bit, and `futex_wait` deadlines are absolute.** (§3.10,
    §4.9) The first is a kernel limit (`FUTEX2_SIZE_U64` returns `EINVAL`); the
    second fails silently, as a busy loop.
19. **Only an owner of the node's wires submits - the local lead's agent, or one
    of its progress helpers - and each alone owns its ring and its wires'
    sockets.** (§3.12) Application threads and other agents request; they never
    submit. With no helpers, the default, the agent is the one owner.
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
    returns. On a control wire too (§3.1): its acknowledgements are what the
    deadlock detector's proof needs of the tree's messages (§3.15; the
    adversarial read of v0.2 step 4's Part 2, F1).
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
32. **A node of the internode tree closes an epoch - resets its counts - before it
    releases anything, and forwards a release to its children before its local
    wake.** (§5.3) Forwarding first lets a child return with the next epoch before
    this node's members are woken; without the reset first that arrival lands in
    an epoch still open (`tree_barrier_model.py`, `release_first`, `wrong_epoch`).
    No invariant above names the collective root or node 0: 25's "node 0" is the
    rendezvous server of §6.2, not the tree's root, and the two need not coincide
    for a split team.
33. **A release by chain is waited for and passed on by every waiter of that
    counter, and waits and wakes carry the target's parity as a futex bitset.**
    (§5.3) A waiter that does not pass the chain on strands those behind it; a
    wake without the parity can be absorbed by a member already waiting for the
    next epoch. Both lose wakes in `chained_wake_model.py`.
34. **A writer sets its connection's dirty bit, in its owner's bitmap, after every
    commit and before it loads that owner's `sleeping` flag; the owner raises the
    flag before it rechecks its bits; all of it `seq_cst`; and the owner visits
    every connection it privately knows is busy.** (§3.10; the owner is the lead's
    agent while a node runs no helper threads, and the flag was `lead_sleeping`.) Each rule dropped loses the lead's attention for committed
    bytes: a lost wake in `lead_wake_model.py`, or - the private half - a leaf's
    barrier arrival left unsealed, measured one run in eight.
35. **A lead seals its goodbye only once the connection owes the peer nothing,
    ends its stream only after the goodbye has gone, and closes a socket only
    after the peer's stream has ended - each wire of a pair on its own.** (§6.4)
    Dropping each strands a child waiting for its release, makes a finished
    peer look dead, or resets a connection the peer is still sending on
    (`close_model.py`).
36. **A lead whose node aborts has its abortive goodbye acknowledged on every
    connection that still has someone to tell before its process ends; on a
    global exit, while its agent runs, nothing but the agent ends that process,
    and with `exit` only on the caller's PE; and no PE returns from
    `shmem_init` before its lead's agent runs.** (§3.15)
    Sent any slower - along the tree - or after the process has ended, the
    goodbye loses the race with the end of stream it should precede, and a node
    that reads the end of stream first ends with a failure where a caller of
    `shmem_global_exit` gave a status (`abort_model.py`, `abort_exit_model.py`);
    sent but unacknowledged, it dies in the reset of a process that ends with
    input unread; and an `exit` on any other PE would wait for good on a main
    thread already inside one (glibc 2.41).
37. **A connection buffer sent by reference is written again only once every
    notification of its sends has come; at most one of a connection's two
    buffers is lent at a time; and a connection that owes a seal stays busy.**
    (§3.3; v0.2 step 4's zero-copy send) Writing it sooner changes bytes the
    kernel may still transmit or retransmit (`reservation_model.py`'s
    `zc_release_on_send`, `zc_first_notification`); lending both leaves an
    acknowledgement no buffer until the peer's delayed TCP acknowledgement frees
    one, seconds on the Pi (BOTH LENT); and an unmarked seal waits for whatever
    next visits the connection, the deadlock detector's sweep or nothing
    (`visit_model.py`).

## 10. Open Questions

### Closed since the previous draft

Recorded rather than deleted, so they are not reopened from memory.

| Question | Resolution |
|---|---|
| Which SOS configuration is the baseline? | **Withdrawn.** libfabric does not build usably on the target hardware, which is the project's premise, not an obstacle to measuring it. Replaced by the §1 floors |
| `shmem_ptr` context-variant signature | **No such thing.** The variant is team-scoped: `shmem_team_ptr` (1.6 §9.1.10). §4.8 corrected |
| `barrier_all` versus `sync` quiet semantics | **Confirmed** (§9.10.1 Notes): `barrier_all` ≡ `shmem_ctx_quiet` on the default context, then `shmem_team_sync` on world |
| `shmem_team_destroy` scoping | **Collective over the team's members**: non-members hold `SHMEM_TEAM_INVALID`, on which destroy is a no-op. An earlier resolution called the scope ambiguous. Team ranges leaked until finalize until the team redesign's step D, since which each member gives its number back (§5.7) |
| Team-based allocation | **Does not exist through 1.6.** 1.6 change #10 states malloc and friends are world-team collective (§4.5) |
| PMIx on the actual nodes | **Moot.** Nothing links against PMIx or PMI-2 on the default path (§6) |
| `max_teams` runtime or build-time | **Runtime**, read at first init, bounding three structures (§5.7). *As built (phase 1 step 7):* `OSOE_MAX_TEAMS`, read at each session's first init and agreed at the rendezvous, sizing everything indexed by team number, the control queue's term too since the split creates teams. *Since the team redesign's step D:* the numbers one PE holds at once; the control queue's term counts the node's pool of team blocks, `OSOE_TEAM_BLOCKS` |
| Team range allocator and split protocol | **Built** (approved 2026-09-25; §5.7, `docs/prompts/done/phase1-step7-slice1-prompt.md`, `docs/prompts/done/phase1-step7-slice2-prompt.md`): team numbers claimed from a counter on world PE 0 and never reused within a session, storage per number on every node, a record per member node that the progress thread learns a team from, the split collective over its parent and ending with a sync over it. Phase 1 step 7's first slice built the storage, its second creation: `shmem_team_split_strided`, `shmem_team_split_2d`, `shmem_team_destroy`. *Replaced by the team redesign's step D* (next row) |
| Team numbers for `split_2d`, and numbers never reused | **Closed by the team redesign's step D** (`docs/design/team-resources-proposal.md` §3; §5.7). The question (the review of phase 1 step 7 slice 2): a `split_2d` claimed a number for each of its rows and columns from a job-wide space never reused, so a square grid over *P* PEs took 2√*P* numbers, and at a default of 32 SOS's `shmem_team_split_2d` lost splits from six PEs; a number needs to be unique only among the teams a PE is in. Since step D a number is each PE's own, agreed by an AND reduction of free masks over the parent, given back at destroy, and a node's teams are a pool of blocks found by name: a whole-job `split_2d` takes two numbers, and the colouring the question proposed is not needed |
| How a PE blocks in `wait_until` | **One watch slot per local PE with a 32-bit futex sequence word** (§4.9). Futexing on the `ivar` itself was withdrawn, since 64-bit futexes do not exist, and the hash table was replaced once waits were seen to be on the caller's own variable |
| io_uring ring bound | **Two bounds, two rings.** CQ is sized from the provided-buffer ring (§3.3). Verified by `tools/probe` |
| Reduction `dest` versus 1.6 change #17 | **Not a conflict** — same arrays means same symmetric offset. The surrounding text exposed three real bugs, fixed by a single-writer lead that leaves last (§5.5) |
| Collective epoch identification | **Cumulative 32-bit counters with equality waits** (§5.4). Sense reversal retired |
| 64-bit futex support | **Not implemented.** `EINVAL` on 6.12 aarch64 (`tools/probe`) |
| Development environment | **Lima + Debian 13 on Apple Silicon**; CI deferred; a physical Pi for the kernel fork, NIC, and page size (§8.5) |
| Which process hosts progress | **An agent thread in every PE; the local lead's runs the transport** (§3.12). A helper process was rejected on setup complexity, supervision, statics access, and exit status |
| Phase-1 thread level | **Up to `SHMEM_THREAD_SERIALIZED`** (§4.6). Revised from `SINGLE`: nothing in phase 1 depends on which thread calls, only on one caller at a time. `MULTIPLE` is designed but not scheduled (below) |
| `SHMEM_THREAD_MULTIPLE` design | **Designed, model-checked and built** in three slices: contexts (serial), `MULTIPLE` with an interim mutex, lock-free issue (§4.11, phase 1 step 8). Serial mode for `PRIVATE` and `SERIALIZED` contexts. On shareable contexts, lock-free issue and a per-context quiet mutex, with a generation word and two get-counter pairs; dirty bits cleared under the mutex; one block per thread holding its watch record and completion slot. Review D6 item 2's `landed >= snapshot` rule was found unsound and not adopted. Open points below |
| Atomics | **ISO C11 `<stdatomic.h>` with named orders only; no intrinsics or assembly** (§3.16). Consequence: no spin-before-block |
| Introspection export | **Shared-memory control segment read by an installed `osoe-stat`** (§3.14). No HTTP server and no metrics files in the library; Prometheus through node_exporter's textfile collector, deferred until there is a reason for a third formatter. **Revised in phase 1 step 2 slice 2:** the control segment's name is removed during init like every other object's, and `osoe-stat` finds a live job through `/proc/<pid>/fd` of any of its PEs; `OSOE_KEEP_CONTROL_SEGMENT=1` restores the old behaviour for post-mortem work. The original entry kept the name until finalize, accepting a small leak on crash — on the reasoning that uncooperative reading needed a name. It did not: it needed a rendezvous, and `/proc` is one that no crash can strand (§3.14). The reader shares only the frozen directory and the metric descriptor with the writer and finds everything else by name, which a tier-1 test enforces by handing it a segment this build did not write. The flight recorder is deferred to step 2, with its design standing: its events do not exist yet |
| Vendor liburing or use the distribution's? | **Distribution's, ≥ 2.6.** Older OS releases are unsupported rather than accommodated with a bundled copy (§1) |
| Build tool | **CMake ≥ 3.25** (§1) |
| Project name | **osoe** (OpenSHMEM Over Ethernet). Library `libosoe`, built with hidden visibility so only spec API symbols are exported (enforced by the `shmem.exports` test); internal non-static symbols `osoe_`; implementation-specific environment variables `OSOE_`, since the spec reserves `SHMEM_`. Chosen over `eoshmem`, one letter from Open MPI's `oshmem` |
| Public header name | **`shmem.h`**, as OpenSHMEM programs expect. Coexistence with another implementation is by install prefix, as with MPI |
| What `OSOE_NODE_LAYOUT` covers | **The encoding of every word in the control segment, not only the offsets of fields.** A word written by one process and interpreted by another is part of the inter-process contract even when its width and position do not move; changing what its bits mean without bumping the version gives mixed builds silent lost wakes rather than the loud refusal `open_control` exists to produce. Recorded because a proposal got it wrong, arguing the §4.9 watch word could change encoding without a bump since it is "an opaque filter" — it is opaque to its *reader*, which is another process |
| Where a data collective's internode phase meets the backend | **Behind `internode.h`, as a blocking call on the lead's thread carrying the operation and the element** (phase 1 step 5 slice 2; §5.8). The first slice built it above the boundary, over put, fenced rmw and wait, where no backend could replace it; the header §5.8 deferred until reductions landed is `internode.h`'s data half, and that tree is now the generic implementation every backend may call. Not the arrival-with-a-payload `internode.h` had forecast: §5.8 rule 2 keeps O(*n*) combining on the lead's thread, which a progress-thread upcall would only have handed back |
| Capability bits at the boundary | **Rejected twice, from two directions.** §4.10 rejected them on design grounds; the RoCE exercise supports the rejection empirically — every place a RoCE backend differs, it differs *underneath* the boundary (emulation, poller, loopback, size classes, queue-pair choice). The one case where the layer above might have wanted to know whether to block or poll is already answered by `osoe_transport_watch_wait` being the backend's own function |
| A boundary progress call | **Needed, and declared** (phase 1 step 2 slice 3): `osoe_transport_agent_pass` and `osoe_transport_agent_idle_ns` in `transport.h`, the only way the library lends the agent thread to a backend (§3.12, §4.10). *Revised.* The first answer was "not needed, because every place the SHMEM layer can block is already a backend function — `osoe_transport_wait`, `osoe_transport_get`, `osoe_transport_watch_wait` — so a backend pumps its own progress there". That was true of application threads and false of the node: the barrier blocks on an epoch counter outside any backend (`barrier.c`, `base/epoch.h`), and what moves it is the progress thread, which `agent.c` drove by calling the AM engine by name (second review, roce-seam F5). Application threads still need no progress call — the blocking calls pump their own — but the node's thread is the library's, and a backend gets it only through these calls. *Revised again* (phase 1 step 3 slice 1, decided with the author 2026-09-22): the two calls were `pass` and `idle_ns`, how long `agent.c`'s own `futex_wait` might sleep — the review's fix shape ("same code, boundary-named"), and exactly the step-2 stand-in's poll. They could not carry the backend they were declared for: §3.10's lead waits in one io_uring submission on its sockets *and* the wake word, which a futex_wait in `agent.c` cannot, and a `SINGLE_ISSUER` ring refuses every thread but its submitter (`-EEXIST`), while `session_end` joined the agent and only then drained the engine on the main thread. So `idle_ns` became `osoe_transport_agent_wait(wake, seen)`, the backend's own wait, and `osoe_transport_agent_exit()` gives the backend a last turn on the thread for its drain and close. An RDMA backend sleeping on a completion channel needs the first for the same reason. Both were edits above the boundary forced by the seam, not by the backend |
| `osoe_transport_put_word` | **Rejected.** `shmem_p` is the hottest pattern in the corpus, but here the backend branches on `len` and the caller spills a value it usually has in a register: single-digit nanoseconds, visible only in a microbenchmark of the boundary itself |
| Should the `/dev/shm` capacity check (§6.2) warn rather than refuse, since heaps are demand-paged? | **No: it refuses.** `OSOE_SHM_CAPACITY_CHECK=0` makes it a report, for testing on a small `/dev/shm` (2026-09-21). |
| Authenticating the step-3 handshake | **An optional `OSOE_JOB_SECRET`, proved and never sent** (§6.2; phase 1 step 3 slice 1). The open item: the handshake carried only the job and step ids, which are sequential and public (`squeue`), and the port is a hash of them, so any host that could reach it could join as a node and then read and write every heap by design; the second review's portability F2 proposed a wrapper setting `OSOE_JOB_SECRET=$(head -c16 /dev/urandom \| xxd -p)` before `srun`, which Slurm propagates, and the handshake carrying it. Built so, except that the secret is *proved* rather than carried: a SipHash-2-4 digest over the hello and a fresh nonce from each end, so neither an eavesdropper nor a listener the caller reached by mistake learns it, and a proof replays nowhere. Unset, nothing changes; set on some nodes and not others, the job ends naming both. It authenticates the hello, not the traffic after it |
| Pin the §1 floor ratios | **Pinned** (phase 1 step 3 slice 2; §8.4, `shmem.floor`). Only once quiet and a full buffer stopped polling, since until then two of the three ratios measured step 2's stand-in. Three pins, each the median of seven side-by-side pairs: a get's, a fetch-add's and a put and quiet's fastest trip within 3× the floor's; a put and quiet and a fetch-add within 1.5× a get; a stream at the default buffer at least 0.1× the floor's. Checked by putting the stand-in back, which the second and third fail and the first cannot see. *Since v0.2 step 4 the stream's pin is 0.3× (§8.4).* |
| What orders fence and quiet, where a `seq_cst` store to a thread-local word was the answer? | **Derived from the C11 model:** a release fence for fence (W→W), a `seq_cst` fence for quiet and the barriers (W→R), and `seq_cst` on both loads of a two-sided protocol. Measured, the old store did not order quiet's W→R on aarch64, and an acquire load built as `ldapr` lost wakes (§3.16, §4.6; 2026-09-21). |

### Still open — blocking

None. The last, a thorough read of the 1.6 specification, was closed by the
author on 2026-10-04 (`docs/decisions/2026-10-04.md` item 11): the review's
omissions pass covered the phase-1 routines and the general sections and closed
"does any team query return remotely accessible memory" (none does), and four
rounds' conformance reviews then read the C interface against the
specification - round six checking 514,710 collective calls against values the
specification alone gives. What it had left, the 1.6 session routines'
semantics should they ever be more than no-ops, is on the release checklist's
after-release list.

### Still open — measurement, once hardware exists

- **8-byte round trip and small-message rate, two nodes** (§8.4). Everything
  above assumes wire latency dominates memory bandwidth by two orders of
  magnitude. **This assumption is load-bearing** for copy decisions, futex
  decisions, and chunk sizing. *Measured in part* (phase 1 step 3 slice 1,
  `echo-uring`, §8.4): on loopback and on a veth pair shaped to 1 Gbit/s with 0–100
  µs of delay each way, the latency half holds by three to four orders of
  magnitude everywhere, and the bandwidth half holds at 1 Gbit/s (≈ 170× on the
  development host) and fails on loopback (1.7×), as §8.3 predicted. Still owed:
  two real hosts, and a Pi, whose slower `memcpy` puts its copy-to-wire ratio in
  the tens, *recalled*. The small-message *rate* is not measured yet.
  *Measured on the Pi cluster, 2026-09-26 and 27 (`docs/pi-cluster-results.md`):*
  an 8-byte put and quiet between two nodes 169-175 µs against a bare io_uring
  echo's 153-156 (1.08× the floor, a get 1.09×), and a stream at 111 of the
  echo's 112 MiB/s, so the latency half holds on real hosts as on the veth
  pairs. The rate: `osoe-pacer mailbox`, 16 PEs on four nodes, 243,000 to
  257,000 messages a second with every thread free to run anywhere, and 38,900
  where Slurm binds each task to one core and the node's progress thread waits
  behind its PE - 62,900 since the agent asks for a short scheduler slice
  (§3.11). What is left is refinement: the small-message rate bound, and
  x86-64 hosts, which nothing here has yet run on.
- **The internode tree's fan-out from measured costs** (§5.3). The default is the
  backend's constant today; measuring the hop latency *h* and per-message cost *o*
  at init and choosing *k* from their ratio is the next step, once a real fabric
  exists to measure. The optimum is broad, so this is refinement, not a fix.
  *Measured on the Pi cluster, 2026-09-26 (`docs/pi-cluster-results.md`):* on four
  nodes over gigabit Ethernet the large-message optimum is not broad. A 1 MiB
  broadcast at the default fan-out of 4 (one level) takes 36.6 ms at D = 1 and
  27.2 ms at any D ≥ 2, because the root sends three copies over one link and
  3 MiB at 111 MiB/s is 27 ms; at a fan-out of 2 (two levels) it takes 38.8,
  22.3, 18.4 and 18.2 ms at D = 1, 2, 4 and 8, the root's two copies at line
  rate. A 1 MiB reduction goes 48.5 → 28.2 ms at k = 4 and 58.4 → 19.4 ms at
  k = 2, D = 8. So over Ethernet a tree's fan-out is a bandwidth decision for
  large messages - the root's link carries k - 1 copies whatever the pipelining -
  and a latency decision for small ones, where each level costs a hop. The
  refinement is a fan-out chosen per message size, k = 2 above a threshold the
  small-message sweep at k = 2 has yet to set (the sweep ran from 64 KiB up).
  *Measured and built (v0.2 step 2, §5.3 as built;
  `docs/decisions/2026-10-06-tree-shape.md` and
  `docs/decisions/2026-10-07-shape-data-path.md`):* the link's hop, message cost
  and bandwidth are measured at the end of `shmem_init`, and each large
  reduction and broadcast takes the fan-out a closed-form cost model gives its
  size on them, under a ceiling of 8 and a capacity of 4. On the Pi's four
  nodes, against 0.1.0's fixed tree, alternating three times: a 1 MiB
  broadcast 1.43 to 1.47 times faster, a 1 MiB reduction 1.20 to 1.26, every
  size from 64 KiB up faster, and none slower beyond the noise; the sweep of 1
  to 16 KiB the item asked for found the star fastest at every size on four
  nodes, which the chooser takes. Open: the scans' own shape per call (§5.8),
  and a fabric whose links differ, which one link's numbers still shape.

### Still open — the RoCE exercise of 2026-09-17

*Stance since 2026-09-26 (§1): RoCE is a constraint the seams must keep
satisfying, not work this project does before its release point; no hardware
is coming soon. Every item below stays open on those terms, and a Soft-RoCE
functional backend is the post-release road to the ones a device is not
strictly needed for.*

`docs/reviews/archive/roce-transport.md` planned a RoCEv2 backend behind §4.10 to test
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

Folded in already: the sweep bound on blocking calls, `OSOE_TRANSPORT_MORE` with
the source-ownership rule that makes it safe, nonblocking-as-counters,
`set_range`'s contract, §4.1's registration trap, and §6.2's out-of-band
bootstrap. Two entries of this list were *documented rather than built* until
slice 3, and the review said so (premises P2-F3): **dispatch per `(pe, operation
class)`** is now expressible, because the route and the pointer are two
predicates rather than one (§8.1, `job.h`) — a backend can answer `direct`
non-NULL for a PE whose rmw it sends elsewhere — though no backend does it yet;
and **§8.1's "three-way switch for free"** was never true and is withdrawn: with
the flag reduced to a pointer denial, the only routing input is the node map,
which has two states per pair, so a third backend needs a selector of its own
(`OSOE_BACKEND`, §4.10).

- **`osoe_transport_sync`: does a collective's internode phase cross the boundary?**
  **Reversed on 2026-09-20 and built in phase 1 step 2 slice 3: it crosses a
  boundary of its own.** `osoe_transport_sync` stays out of `transport.h`, which
  remains point-to-point and PE-addressed; the internode phase gets a sibling
  interface, `internode.h`, bound at build time exactly as `transport.h` is, with
  AM as one implementation (§4.10, §5.3). Addressed by `(team, epoch)`: the
  node set is given once at team creation, the arrival goes down, and the
  release comes back as an upcall on the node's progress thread.

  *The history, kept so the decision is not reopened from memory.* §2.2 put
  collectives on AM, decided when AM was the only backend; in an RDMA build that
  sends every barrier over TCP while the data path runs on the NIC. The proposal
  was a mandatory `osoe_transport_sync(ctx, group)` over a group of *nodes*,
  called after the intranode phase by one PE per node with a member. At step 2
  it was **closed "no"** on two reasons, both of which the code written that day
  already contradicted (second review, D5; roce-seam F1; premises P1):

  - *"Addressing: §4.10's model is `(pe, region, offset)`, while this phase is
    addressed to a node — a second addressing model."* **Wrong about the code.**
    What crossed at `barrier.c` was already `(team, epoch)`, and the node set was
    computed *below* that call. Nothing node-addressed leaked upward; the
    interface the code had was team-addressed, and the SHMEM layer already owns
    teams.
  - *"The release: expressing it as an rmw would need the backend to know which
    words have futex waiters, which notify does not mean."* **A refutation of a
    design nobody needed.** The release was never an rmw; it was one upcall,
    `release(context, team, epoch)`, in which the host bumped the team's counter.
    An upcall needs no notify and no watch, and leaves §5.4's equality wait alone.

  And **the price was quoted against the wrong baseline.** The entry said "the
  4–6× estimate at 10^6 PEs is unchanged", comparing an RDMA tree with SHARP —
  which RoCE switches do not have (*documented*). What was built was a TCP
  *star* on node 0: against an RDMA tree that is about 500× at 4,096 nodes, or
  about 12× once the AM phase is itself a tree (roce-seam §4.3, *arithmetic*).
  So the AM implementation became a k-ary tree in the same slice (§5.3), and a
  backend with a better way — a tree over RDMA put and atomics, or an offload
  engine — implements `internode.h` without touching `barrier.c`. The data
  collectives followed at phase 1 step 5 slice 2, behind the same header, as a
  blocking call carrying the operation and the element, with a generic
  implementation every backend may call (§5.8).
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
- **`SHMEM_THREAD_MULTIPLE`: points §4.11 leaves open.** None blocks phase 1.
  - **A full thread table.** A PE's `OSOE_MAX_THREADS` blocks can all be taken by
    live threads. Aborting with a diagnostic naming the variable is loud and simple.
    A shared overflow block needs its own locking and wake-all rules, which only
    that rare case would exercise. *Decided 2026-09-25: the job ends, naming
    `OSOE_MAX_THREADS`.*
  - **Landing-zone space under concurrent blocking gets.** §3.12 sizes the landing
    zone per PE, and phase 1 allocates from it with one caller at a time. Several
    threads claiming reply space at once need a concurrent allocator, such as
    fixed extents claimed by CAS over a bitmap. *Decided 2026-09-25: exactly that.
    A thread that finds every bit set sleeps on the word with the value it read;
    the owner copies its reply out, clears its bit, then wakes the word.
    `landing_extent_model.py` checks it, and fails without the CAS, with the
    wake before the clear, with no wake, or with the clear before the copy.*
  - **Copy-out claims.** The agent and any quiet may both copy a landed
    nonblocking get to its destination. A CAS on the record's state gives exactly
    one copier. The idea is standard. *Decided 2026-09-25: the CAS is LANDED to
    COPYING; the winner copies, credits the landed count, then frees the
    record. `copy_out_claim_model.py` checks it, and fails with a plain store for
    the claim, with the credit before the copy, or with the record freed at the
    claim.*
  - **`pe_quiet` and piggyback.** The rule that a full quiet never piggybacks on a
    `pe_quiet` is stated in §4.11 item 2. *Modeled 2026-09-26, in a model of its
    own, `pe_quiet_model.py`, rather than in `get_completion_model.py`: the rule
    is about puts, which that model does not carry. The design passes; it fails
    with item 1's test on the generation word, with a `pe_quiet` storing `full`,
    and with `full >= e`.*
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
  lead's-`dest` accumulator does not cover them. *Closed (v0.2 steps 2 and 3):*
  each call chooses between the trees and Träff's circulant reduce-scatter and
  allgather - Rabenseifner's halving with a fold modelled and set aside - by the
  cost model's closed forms (`collective-shape.c`) on the link measured at the
  end of `shmem_init`; on the Pi's four nodes Träff's from between 8 and 16 KiB, faster than
  the trees at every size it takes, 1.32 to 1.41 times at 1 MiB, and the trees
  below (`docs/pi-cluster-results.md`, v0.2 step 3).

### Review of 2026-09-16

`docs/reviews/archive/design-soundness.md`, an adversarial review with a refutation pass,
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

- **License.** *Decided 2026-09-26 by the author: the GNU LGPL, version 3*
  (`COPYING.LESSER`, with the GPL it supplements in `COPYING`). A library
  under the LGPL may be linked into programs of any license, which is what an
  OpenSHMEM implementation is for. *Version 3 or any later version, the
  author decided on 2026-09-26*: every source file carries
  `SPDX-License-Identifier: LGPL-3.0-or-later`.
