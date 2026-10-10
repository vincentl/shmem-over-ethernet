# Progress helpers: a control plane on the lead's agent, data planes on helper threads

**Status: a proposal, written on 2026-10-05 for v0.2's fourth step
(`docs/decisions/2026-10-05-v0.2.md` item 8; the brief is
`docs/prompts/done/v0.2-step4-progress-helpers-prompt.md`). The author's premise
is settled: one thread is not enough to drive an HPC link - "full stop". What
this document proposes is *which* threads, owning *what*, talking *how*, so
that the count can grow without a lock. §8 lists what it asked the author to
decide; the author answered it on 2026-10-07 (`docs/decisions/2026-10-07.md`,
items 4 to 7): the control wire, two wires a pair always, no helpers by
default and a count only from the environment, and the zero-copy send built
behind a setting that is off by default.**

## 0. What is wrong today

One thread a node moves every message between nodes (design §3.12; README,
Limits). The lead PE's agent thread owns the node's one io_uring ring, every
peer connection and every provided buffer; it seals and sends the node's PEs'
aggregated traffic, receives, parses and applies every incoming operation,
generates every get reply, forwards every collective message, runs the barrier
tree's state machine, the deadlock detector, team publication, abort, close
and the silent-peer check. On the Pi this thread runs a gigabit link at line
rate with four PEs behind it, and the README says truthfully that nothing
above a gigabit has been measured. Over loopback in the four-core VM, where
only the machine limits a stream, it moves a fifth of what a bare io_uring
stream between the same two processes moves.

Three costs land on that one thread, and they are different costs:

- **The kernel's copies.** With io_uring over TCP, the copy from the
  connection buffer into socket buffers on send and from socket buffers into
  provided buffers on receive both run in the submitting task's context, so
  every byte through the link is memcpy'd twice by this thread's CPU time
  before the library touches it.
- **The library's apply.** Every incoming put is parsed and memcpy'd into
  the target PE's heap or statics; every AMO applied; every get read and its
  reply staged.
- **The orchestration.** The barrier tree, the control queue, the detector,
  team state, abort and close: little data, but state that reasons about the
  node as a whole and that today is single-threaded with no locks
  (`deadlock.c:6-10`).

The first two scale with bytes and messages; the third does not. The slide's
"progress threads sharded by peer node" (§3.12: "at 256 PEs perhaps four, all
in the lead process, sharded by peer node so each connection has exactly one
owner") treated all three as one thread's work to be replicated. An audit of
the code against that reading found fourteen node-wide structures that would
need per-shard copies or multi-producer protocols: the ring and its wire
slotting, the wake word and sleeping flag, the dirty-bitmap exchange, the
reply pool, the control queue's enqueue and forward across shards, the receive
scratch, the counters, the barrier's team state and arrival handoff, the
deadlock detector, and the abort, close and silent-check paths. Half of them
are the orchestration, and replicating the orchestration is what makes
sharding hard.

## 1. The proposal in three sentences

**The lead's agent keeps the control plane and nothing else when helpers
exist**: the barrier tree, the control messages, the detector, team
publication, abort, close and the silent-peer check - and it keeps a wire of
its own to every peer for them, so that a control message never waits behind
a 64 KiB put and never crosses a thread. **Helpers own disjoint sets of the
data connections, each with its own ring, and move the bytes**: receive,
parse, apply, reply, seal, send; every structure a helper needs is its own,
and nothing between the threads is a queue - a PE wakes the helper that owns
its connection, the lead tells a helper to flush and close with a word and a
wake, and the detector reads the helpers' counters. **A count of zero helpers
is today's thread with today's work**, so the Pi and every verified tree pay
nothing, and the count grows where a link demands it.

## 2. Read first

Design §3.3 (the sole issuer), §3.4 (connections and the connection area),
§3.8 (the control queue, and its note that "the control queue is
multi-producer once a node runs more than one progress thread ... Pre-allocate
at maximum" - this proposal makes it single-producer instead), §3.9
(aggregation), §3.10 (idle and wakeup: the ring's one armed futex wait, the
dirty bitmap, `lead_sleeping` and the claimed wake, the posted bitmap of team
arrivals), §3.11 (waiter fairness: nothing may spin), §3.12 (progress
threads), §3.15 (the error model), §3.16 (atomics), §4.10 (the boundary),
§6.2 (the rendezvous and the mesh), §6.4 (how a job ends).

The code the audit named, by heading:

- **Ring and wires.** `am-state.h:36` (`osoe_am.ring`, one `struct io_uring`
  by value), `progress.h:260`, `ring.c:41-67` (created `SINGLE_ISSUER`,
  `R_DISABLED`, `CQSIZE`, sized from all peers; the provided-buffer ring and
  its `next`, `filled`, `readable`), `wire.c:175-235` (`osoe_wires_open`
  attaching every wire to one ring; a wire's `slot` both its index into
  `ring->wires` and its CQE tag), `wire.c:262-265`, `progress-host.c:488`,
  `:551`, `:587`, `:644`, `:660`, `:716`, `:775`.
- **Ownership and wake.** `connection-area.h` (`osoe_connection_to(area,
  peer)`, the dirty bitmap and `osoe_connection_area_mark`, `lead_sleeping` at
  `:152`), `progress.h:264-267` (`p->wire`, `p->connection`, `p->busy` by
  peer slot), `progress-host.c:324-346` (the claimed wake into
  `slots[0].agent_wake`), `:351` (`flag_raised`), `:708-738` (the poll, the
  dirty-word exchange), `progress.c:1273-1290`, `internode-am.c:124-126` (the
  barrier arrival's wake).
- **Queues and passes.** `reply.h:45`, `:139`, `progress.h:277` (the reply
  pool, one free list private to the progress thread), `progress-host.c:456`
  (its size), `progress.c:179-184` (`mark_busy`, `enqueue`: writing another
  connection's control queue and the non-atomic `busy` bitmap), `:240`
  (`forward`), `progress.h:279` and `progress-host.c:462` (`p->scratch`),
  `progress.c:655`; `osoe_progress_connection` (`progress.h:~187`), whose
  `receive`, `drain`, `flush` and `finalize` touch only their own slot.
- **Counters.** `base/counters.h:60` (`osoe_counter_word_add`, a load and a
  store, declared single-writer), `progress.h:257` (`application_applied`),
  `connection.h` (`acked`, per connection).
- **Collectives on the progress thread.** `progress.h:149`, `:298-300`
  (`p->team[]`, `p->pending`), `progress.c:209` (`publish_team`), `:271`
  (`release_down`), `:288` (`book_arrival`), `:349` (`known_team`), `:527-560`
  (the ARRIVE and RELEASE handlers), `:1211` (`release_local`),
  `internode-am.c:49`, `:169-200` (`osoe_internode_am_take_arrivals`,
  `consumed[]`).
- **Detector, abort, close.** `deadlock.c:6-10`, `agent.c:239`, `progress.c:1307`
  (`osoe_progress_idle`), `:1401`, `:1425`, `:1437`, `:1451`
  (`osoe_progress_check_silent`), `:1465` (`osoe_progress_quiet`),
  `progress-host.c:359-375` (the silent-check statics), `:536-690`
  (`flush_before_close`, `close_connections`, `osoe_progress_host_abort`,
  `_exit`).
- **Threads.** `agent.c:29` (one static `pthread_t agent`), `agent.c:154`
  (`sched_setaffinity`, `OSOE_AGENT_CPUS`), `transport-am.c:370-387`
  (`osoe_transport_agent_pass`, `_wait`, `_exit`).
- **What already works per owner**: the per-connection state and functions;
  wire send and receive; the shared buffer's reserve, commit and seal; ack
  publish and wait; user-level quiet and fence (`transport-am.c:1510`,
  `:1536`, `:1614-1637`, driven by `osoe_progress_host_acked(peer)`); the
  landing zone and reply slots (atomics per token); the data collectives,
  which run on the lead's *application* thread with puts and fenced adds
  (`internode-generic.c:9-24`); `osoe_internode_released` (`barrier.c:116`);
  liveness and sweep.

## 3. The design

### 3.1 Two planes, two kinds of wire

Every pair of nodes is joined by **one control wire and D data wires** where
today it is joined by one wire. The control wire is the lead's: its socket
lives on the lead's ring, its send buffer is private to the lead (no PE ever
writes a control message: a barrier arrival is a handoff word the lead's agent
reads, §3.10, and a release or a detector probe is the lead's own), and it
carries what the control queue carries today - barrier ARRIVE and RELEASE,
team control, detector probes and answers, the goodbye - and nothing with a
payload. A data wire is a helper's: it carries puts, AMOs, gets and their
replies, signals, acknowledgements, the collectives' pieces and grants
(ordinary puts and fenced adds), and the goodbye too, since each wire ends on
its own.

D is 1: one data wire a peer pair, owned by one helper. (More than one data
wire a pair would let two helpers share a peer, but a peer's operations must
apply in order - invariant 5 - and one wire is what makes that free. A single
helper's wire to a peer runs at the link's rate; the gain from helpers is
across peers, not within one.)

**Why a wire of its own for control, rather than a mailbox from the helper
that receives a control message to the lead.** A mailbox costs every barrier
two thread handoffs - a wake into the lead when the ARRIVE lands on a
helper's wire, and a wake into the helper that owns the parent's wire when the
lead forwards it - some 10 to 20 µs on a loaded node against the Pi's 85 µs
hop; and it leaves the control message behind whatever bulk is ahead of it on
the shared wire, which at a gigabit is 0.6 ms for one 64 KiB put. The control
wire removes both: a barrier's path is lead to lead, ring to ring, as today
with zero helpers, and it is never behind a put. It also makes every structure
single-owner without a queue between threads: the lead's control queue has
one producer and goes out on wires the lead alone owns, which closes §3.8's
"multi-producer once a node runs more than one progress thread". The RoCE
constraint is kept: an RDMA backend with a control queue pair beside its data
queue pairs is the common shape.

**What it costs.** Two sockets a peer pair instead of one - at 500 nodes 998
a node, within a default `ulimit -n` and nothing the kernel minds; the
start-up mesh makes two connections a pair, so the chain's time doubles
(`mesh_timing.py`: about 0.64 s at 1,024 nodes where it is 0.32 today, which
`docs/design/mesh-schedule-proposal.md` is the answer to either way); the
job handshake (§6.2's SipHash proof) runs on both; two keepalives; the "reset
connection is judged only once the goodbye and end of stream have both
arrived" rule holds per wire. The control wire's buffers are small: a page of
send buffer and the ring's provided buffers, no connection-area slot, no
dirty bit. (*Amended on 2026-10-08:* 4 KiB of send buffer, which is a page on
x86-64 and a quarter of one on the Pi 5, in the lead's own memory rather than
the connection area (Part 2's record).)

**Ordering between the planes.** Nothing a control message says depends on a
data message having been applied, and the protocols already make that so: a
barrier implies a quiet, and the quiet waits for acknowledgements on the data
wires before the arrival is posted; a bare `shmem_sync` orders no data; a
release carries no data promise; the detector reasons about counters, not
messages. (*Amended on 2026-10-08:* the detector's counters are
acknowledgements, and its proof needs every message its receiver counts as
activity acknowledged before its sender can look quiet - the tree's ARRIVE and
RELEASE among them - so a control wire's segments are acknowledged as a data
wire's are; built first without, a barrier's message held in the network could
have a job that would finish declared deadlocked (Part 2's adversarial read,
F1). Not an ordering between the planes: each wire's acknowledgements are its
own.) Fence and quiet are entirely within the data plane. Collective
pieces, grants and releases of the data collectives are puts and adds on the
data wires. The one message that must be ordered against data is the
goodbye, and it is sent on every wire the sender owns, each after that wire's
own last bytes.

### 3.2 Owners

An **owner** is a thread with a ring and the wires attached to it. The lead's
agent is owner 0 and owns every control wire. With H helpers, helper h is
owner h and owns the data wires of the peers that map to it; **with H = 0
owner 0 owns the data wires too**, and the lead's agent does today's work on
today's structures plus a second wire a peer. Everything an owner needs is in
one `struct osoe_progress_owner`: its ring and provided-buffer ring, its wires
and their slots, its reply pool, its wake word and sleeping flag, its dirty
bitmap, its counters, its per-connection state. (*Amended on 2026-10-08:* no
receive scratch - since Part 0's fourth rung a pass parses its provided buffers
where the kernel put them, `docs/decisions/2026-10-07-one-thread-profile.md`.)
`osoe_progress_host_poll` becomes a function of an owner; the code that runs
it is the same for owner 0 and for a helper, and what differs is which wires
it holds.

**The map from peer to owner is arithmetic**: peer slot p's data wire belongs
to owner 1 + (p mod H) when H > 0 and to owner 0 when H = 0. A PE needs no
table to find the helper to wake. A node's H is agreed among its PEs at
start-up (it shapes the connection area) and free to differ between nodes: a
desktop with a fast link may run 4 where a Pi runs 0, and the peer never
knows.

### 3.3 Waking an owner

Today's wake protocol (§3.10, the dirty bitmap, `lead_sleeping` and the
claimed wake, model-checked in `docs/design/evidence/lead-wake/`) becomes
**the same protocol per owner**: each owner has its own dirty bitmap over
*its* connections, indexed by (p div H) for the peers it owns, its own
`sleeping` flag and its own wake word, in the connection area's header. A PE
that commits into connection p sets bit (p div H) in owner 1 + (p mod H)'s
bitmap, loads that owner's flag, and claims that owner's wake. The lead's
agent is woken by its ring (control wires), by the posted bitmap of team
arrivals (§3.10, unchanged) and by its own wake word (abort, stop). With
H = 0 there is one bitmap, one flag, one word: today's. The model is the
existing `lead_wake_model.py` with the owner as a parameter; its broken
variants stay broken.

### 3.4 Memory

Per owner: a ring (its SQ, CQ and provided-buffer ring, sized from the wires
it holds, so the total is about today's plus H rings' fixed cost), a reply
pool sized by §3.8's argument over *its* connections (the sum is today's
pool), and the bitmap, flag and word (a cache line each); no scratch buffer
since Part 0's fourth rung, which parses in place (*amended on 2026-10-08*).
The connection area grows by H bitmaps and the lead's control
wires' private buffers. Nothing per PE changes.

### 3.5 Counters and the detector

`osoe_counter_word_add` stays single-writer: every counter the progress code
writes becomes the owner's, and a reader - the detector, `osoe-stat`, the
metrics - sums the owners' with acquire loads. The deadlock detector stays on
the lead's agent, lockless, as `deadlock.c` says; its "activity unchanged
over two halves" check reads the owners' sums, and its probes travel on the
control wires. `osoe_progress_idle` and `osoe_progress_quiet` today walk every
connection from the lead's thread and write `detector_wanted` and `mark_busy`
there; they become a request word per owner the lead sets and the owner
answers on its next pass, so no owner's state is written by another thread.

### 3.6 Abort, close, stop

The lead orchestrates; each owner executes on its own wires. The lead sets a
per-owner request word - flush and close, or abort - and wakes the owner; the
owner flushes its wires as `flush_before_close` does today, sends its
goodbyes, closes, and raises a done word the lead waits on with a deadline,
as every wait has (§3.10). The abort word (§3.15) is unchanged. The
silent-peer check is the lead's, over the control wires, which every peer
answers on. `osoe_progress_host_exit` joins the helpers before the lead's
agent ends. A model of the stop protocol - request, done, deadline, an owner
that never answers - in the form of the existing abort models.

### 3.7 Threads and CPUs

Helpers are threads of the lead PE, created in `shmem_init` after the agent,
with signals blocked as the agent's are, placed by `OSOE_AGENT_CPUS`'s rule
(the lead task's CPUs, or the node's), each asking the same short scheduler
slice (`OSOE_AGENT_SLICE_US`). A count above the lead task's CPUs is allowed
- it is how correctness is tested on a four-core VM - and the README says
what it costs.

### 3.8 The setting

`OSOE_PROGRESS_HELPERS`: default 0, at most 64, agreed among a node's PEs
(it shapes the connection area), free to differ between nodes, shown by
`osoe-stat` in the node header with each owner's wires and counters. A value
above the peer count wastes threads and is allowed.

## 4. What it costs, what it buys

**Buys**, on a link where one thread's copies bound the rate: H + 1 CPUs'
worth of kernel and library copies in place of one, spread across peers, with
every peer's stream still on one thread so ordering is free. On the Pi at a
gigabit, nothing: 0 helpers. On the VM's loopback, where the one thread moves
a fifth of a bare stream: the profile in the brief's Part 0 says how much of
the fifth is this thread and how much is the protocol.

**Buys regardless of H**: a control plane that never waits behind bulk. A
barrier during a put stream is the measurement.

**Costs**: two sockets a peer pair; H rings and pools; a doubled start-up
mesh; the connection area's H bitmaps; and the code - the owner structure
threaded through `progress-host.c`, `progress.c`, `ring.c`, `wire.c`,
`connection-area.c`, `am-state.h`, `reply.c`, `agent.c`, `deadlock.c`, the
rendezvous's second connection, the metrics and `osoe-stat`. The audit's
fourteen become, under this design: ring and slotting per owner (1), the
owner's wake protocol (2), the pools (3; the scratch is gone since Part 0's
fourth rung), the counters (4), the
detector's request words (5), stop and abort per owner (6), the control
wires (7), threads (8). Six of the fourteen - the control queue's producers,
the team state, the arrival handoff, `enqueue` and `forward` across shards,
the barrier handlers on another thread, the silent check - do not change at
all, because the lead keeps them.

## 5. What changes, by file

- `connection-area.h/.c`: per-owner bitmaps, flags and words; the map from
  peer to owner; the layout bump.
- `am-state.h`, `progress.h`: `struct osoe_progress_owner`; the engine holds
  owners, not one ring.
- `ring.c/.h`, `wire.c/.h`: a ring per owner, wires attached to their owner's
  ring, the slot-to-tag mapping per ring; the control wire's private buffer.
- `progress-host.c`: the poll, wake and sleep per owner; the lead's pass over
  control wires; stop and abort through request words.
- `progress.c`: the control plane's handlers unchanged, now reached only
  from owner 0's wires; `enqueue` and `forward` write owner 0's queue only;
  counters per owner.
- `reply.c/.h`: a pool per owner.
- `deadlock.c`: sums over owners; request words.
- `agent.c`: helper threads, placement, join.
- `rendezvous.c`, `listener.c`, `connection.c`: two connections a pair, the
  handshake on both, the control wire's identity.
- `metrics.c`, `osoe-stat`: owners in the node header.
- `bootstrap.*`, `node.c`: `OSOE_PROGRESS_HELPERS`, agreed among a node's
  PEs.
- The README: Limits ("one thread a node moves every message" goes), the
  settings table, the memory rows, `osoe-stat`'s view.

## 6. Tuning the one thread first

The author's aim is the most a link can give through io_uring. Adding threads
to an untuned path multiplies its waste, so the brief's Part 0 is a profile
and a ladder, each rung a measured change on the one thread, before any
helper exists:

1. `perf` on the lead's agent in the VM at loopback and, when a pair exists,
   on a fast link: how the fifth divides between kernel copies, the library's
   parse and apply, the ring's submit and reap, and waits.
2. Provided buffers: count and size against the segment size
   (`OSOE_AM_CHUNK`, 64 KiB, and the connection buffer); multishot receive's
   buffer ring exhaustion counter.
3. `IORING_SETUP_DEFER_TASKRUN` with `COOP_TASKRUN` (Linux 6.1), so the
   kernel's completion work runs when the owner enters the ring rather than
   by interrupting it; `IORING_SETUP_SINGLE_ISSUER` is already set.
4. Registered files for the wires (`io_uring_register_files`), saving a file
   lookup an operation.
5. `IORING_OP_SEND_ZC`, rejected in §3.9 for buffer stability: the
   aggregation clocks off the send CQE and reuses the buffer; with zero copy
   it would clock off the notification CQE instead, which arrives when the
   NIC is done with the pages - later, but the connection buffer's room is
   what the clock governs, and §3.5's protocol already tolerates a later
   reopen. Measure the send-side CPU saved against the later reopen on a
   fast link; on loopback zero copy saves nothing.
   *Corrected on 2026-10-07:* over TCP the notification comes once the
   peer has acknowledged the bytes, since the kernel keeps them to
   retransmit, not when the NIC is done; the next segment can go after the
   send CQE, so a connection's two buffers cap one peer's stream at about
   256 KiB a round trip, some 50 Gb/s at 40 µs (512 KiB and some 100 Gb/s since
   the default buffer became 256 KiB, `docs/decisions/2026-10-07.md` item 12).
   Over loopback the kernel copies anyway and adds the notification. The
   author's answer (§8 item 4) builds it behind a setting, off by default.
6. The library's own: the parse per message, the apply's memcpy alignment,
   the dirty-word exchange per pass.

Only then helpers, with the same measurements.

## 7. Models, tests, measurements

- **Models**: `lead_wake_model.py` per owner; a stop-and-abort model
  (request, done, deadline); the control wire's goodbye against the data
  wire's (a reset judged per wire); the per-owner posted bitmap unchanged.
- **Tests**: every existing multi-node test under `OSOE_PROGRESS_HELPERS=0`
  unchanged; the suite again at 2 and 3 helpers on 3 to 16 logical nodes in
  the VM, under TSan and ASan (the helpers' correctness is testable with no
  fast link: oversubscription is the author's point); a barrier during a put
  stream measured on the Pi at H = 0, before and after the control wire, as
  the control plane's own number; `osoe-pacer mix` and `mailbox` at H = 0
  and 2.
- **Measurements on a fast link**, when a pair exists: a put stream and the
  mailbox rate at H = 0, 1, 2, 4 with 8 to 32 PEs a node; the ladder's rungs
  one at a time at H = 0 first.

## 8. For the author

1. **The control wire.** The proposal's central choice: a second wire a peer
   pair, always, owned by the lead, against a mailbox design that keeps one
   wire and pays two thread handoffs a barrier and the head-of-line wait. The
   recommendation is the control wire; it changes the mesh and the socket
   count, which is why it is the author's.
2. **Always two wires, or two only when H > 0.** Always is one code path and
   gives the control plane its own wire on the Pi too, at the cost of a
   doubled mesh there; only-when-helped keeps the Pi exactly as the tag and
   makes H = 0 a different wire layout from H = 1. The recommendation is
   always.
3. **The default of 0 helpers**, and whether the brief should give a rule
   for raising it (a link above a gigabit, measured by step 2's start-up
   measurement, suggesting a count) or leave it to the user's setting until a
   fast link has been measured. The recommendation is the setting alone until
   there are numbers.
4. **The ladder's `SEND_ZC` rung**: a protocol change to the aggregation's
   clock, worth measuring only on a fast link; include it in the brief as a
   rung, or leave it for the hardware-gated items.

*The author's answers, 2026-10-07* (`docs/decisions/2026-10-07.md`, items 4
to 7): (1) the control wire - "absolutely - mixing control with data can lead
to deadlocks"; (2) always - "it simplifies validation and we want dedicated
control"; (3) no helpers by default, a count set from the environment alone;
(4) built in step 4, behind a setting off by default, for segments above a
threshold, its correctness tested in the VM and its default decided on a fast
link - "will we lose any performance? if no, we should just do it to debug and
get it working", and off by default it loses none.
