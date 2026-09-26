# Phase 1, step 2, slice 1 — performance and scale

Reviewer: `performance-scale` (second adversarial review, question 5). Tree: `main`
at merge `d542c63` (+ review commits). Asymptotic complexity of barrier,
bootstrap, collectives and teams belongs to the sibling report
`docs/reviews/evidence/phase1-step2-slice1/reports/algorithmic-scaling.md`; this
document owns memory/state arithmetic, behaviour at the limits, and the
performance claims.

Evidence labels used throughout: *documented* (design doc / comments),
*observed in source* (file:line read by me), *measured* (run by me in a private
RelWithDebInfo build, `build/scratch/review-performance-scale/tree`), *recalled*
(my memory of Linux/TCP behaviour, unverified here).

**Every timing below was measured under contention, in a 4-core VM shared with
other agents (load average 3–6 during the runs), over the shared-memory wire —
not a socket. Order of magnitude only.**

Evidence directory: `docs/reviews/evidence/phase1-step2-slice1/performance-scale/`
(`struct-sizes.c/.txt`, `bench.c` and `bench-*.txt`, `limit-*.c` and outputs,
build scripts, the instrumentation diff).

---

## 1. Verdict

**Scale.** The headline claim — "state scales with node count, never PE count" —
is *true of the AM layer's shared structures* (connection area, credit,
descriptor pool, progress state: all O(N) per node, no L or P term) and *false of
the process as a whole*. Three per-PE private arrays are sized by **world size**
(`local_of_rank`, `gtids`) or by **node count per PE** (dirty set, connection
handles), so per-node memory has an N·L² term and an N·L term. They are
invisible at 4×4 and at 500×256 cost ~0.27 GiB per node; at 4,096×256 the two
world-sized arrays alone cost **2.1 GiB per node** — and they live in `node.c` /
`bootstrap.c`, which an RoCE build inherits. The connection area itself is O(N)
per node and O(N²) job-wide, is demand-paged, and with default settings
**refuses to start above 1,025 nodes** (loudly, by design).

**Limits.** No deadlock found: §3.7's "enqueue, never reserve" is kept in the C,
and the lead's pass never blocks on outbound space. But the fallible path that
makes that possible is wrong in one place: **the descriptor pool is sized
`peers × C` with no allowance for collective forwards, and a request that finds
it empty is silently dropped, not left in place** — a legal program hangs for
the full timeout and then aborts, and the credit leaks. Reproduced 3/3 with
`OSOE_AM_CREDITS=2`; at the default C=64 it needs ≥64 concurrently outstanding
gets from one node toward one peer, which is exactly the 256-PE-per-node target.
Every other limit degrades to a 20 µs poll and aborts after
`OSOE_BOOTSTRAP_TIMEOUT` (60 s), which is defensible for step 2.

**Performance.** The intranode path is sound: an 8-byte put costs ~13–17 ns
over a raw store, a 4 KiB put is within ~13 % of `memcpy`, 1 MiB is at parity.
The internode path in step 2 is dominated by one number: **the lead polls
arriving bytes every 250 µs**, so every round trip (put+quiet, get, fetching
atomic, barrier) has a median of **300–400 µs** while the *minimum* observed is
20–50 µs. That is a known stand-in (§3.10), but it means nothing measured about
internode latency today says anything about the design. What is measurable and
encouraging: aggregation works (1,000 small puts + one quiet ≈ 1.1–1.5 µs per
put, ~250× better than quiet-per-put), and §3.2's head-of-line bound of "about
two segments" was observed almost exactly. What is not encouraging for the
top-end: the progress pass is O(N) per wake regardless of which connection has
work, and every internode put issues an unconditional `futex_wake` on one
node-wide cache line.

---

## 2. Scale table

Real sizes (*measured*, `struct-sizes.txt`, aarch64): `osoe_connection_shared`
1,152 B; `osoe_connection` (private handle) 24 B; `osoe_reply` 48 B;
`osoe_progress_connection` 224 B; `osoe_wire` (handle) 184 B; `osoe_node_header`
1,536 B; `osoe_pe_slot` 512 B; `osoe_counter_block` 512 B; landing slot 256 B +
chunk; 89 metric descriptors × 64 B; `osoe_team` 64 B. Defaults: chunk 64 KiB,
buffer B = 128 KiB, budget 256 MiB, C = 64, `OSOE_MAX_THREADS` = 8, wire ring
16 KiB/direction, `OSOE_TEAMS` = 2 (compile time).

N = nodes, L = PEs per node, P = N·L, T = threads per PE, C = credits.
"RoCE" = would an RoCE build inherit this structure (yes / no / shape).

| # | Structure (file:line) | Formula | Scope | 4×4 | 500×256 | 4,096×256 | RoCE |
|---|---|---|---|---|---|---|---|
| 1 | Connection area, shared (`connection-area.c:67-75`) | 2(N−1)B + 1,152(N−1) + 128 | per node | 0.75 MiB | **125 MiB** | 1.0 GiB — **fatal at init**: exceeds default budget (`connection-area.c:174`); limit is N ≤ 1,025 | no (AM only), unless §10 keeps AM hot |
| 1j | — same, job-wide | ≈ 2N²B | job | 3 MiB | 61 GiB | 4.0 TiB | — |
| 2 | Connection handles, **private per PE** (`connection-area.c:256`) | 24(N−1) per PE | per PE → ×L per node | 72 B / 288 B | 11.7 KiB / **2.9 MiB** | 96 KiB / **24 MiB** | shape (per-PE peer table) |
| 3 | Dirty set, **private per PE per context** (`transport-am.c:94-96`) | 13(N−1) per ctx per PE | per PE → ×L | 39 B / 156 B | 6.3 KiB / 1.6 MiB | 52 KiB / 13 MiB (×contexts) | shape |
| 4 | Reply descriptor pool, lead only (`progress-host.c:233-236`) | 48·C·(N−1) | per node | 9 KiB | 1.46 MiB | 12 MiB | no |
| 5 | Progress connection state + wire handles, lead only (`progress-host.c:234-235`) | (224+184)(N−1) | per node | 1.2 KiB | 199 KiB | 1.6 MiB | no |
| 6 | Receive scratch (`progress-host.c:237`) | chunk | per node | 64 KiB | 64 KiB | 64 KiB | no |
| 7 | Step-2 wire objects (`wire.c`, stand-in) | ≈ 33 KiB × (N−1) mapped per lead; N(N−1)/2 `/dev/shm` objects job-wide | per node | 100 KiB; 6 objects | 16 MiB; 124,750 objects | 135 MiB; 8.4 M objects | no (deleted in step 3) |
| 8 | Control segment: header + slots + metric table + counter blocks + **landing zones** (`node.c:216-241`) | 1,536 + 512L + 5,696 + 512·L·T + L·(256+chunk) | per node | 284 KiB | **17.2 MiB** (landing zones 16.1 MiB of it) | 17.2 MiB | **yes** |
| 8j | — same, job-wide | ×N | job | 1.1 MiB | 8.4 GiB | 69 GiB | yes |
| 9 | `local_of_rank`, **private per PE, world-sized** (`node.c:391`) | 4P per PE → 4P·L per node = **4·N·L²** | per PE → ×L | 64 B / 256 B | 500 KiB / **125 MiB** | 4.0 MiB / **1.0 GiB** | **yes** |
| 10 | `gtids`, **private per PE, world-sized** though only L entries are used (`bootstrap.c:81`) | 4P per PE → 4·N·L² per node | per PE → ×L | 64 B / 256 B | 500 KiB / **125 MiB** | 4.0 MiB / **1.0 GiB** | **yes** |
| 11 | Job rendezvous map, shared (`job.c:98`) | 4P + 320 | per node (shared pages) | 384 B | 500 KiB | 4.0 MiB | yes (shape: step 3 replaces the object, the map remains) |
| 12 | Heap mappings: every local heap mapped in every local process (`node.c:140,164`) | L² mappings per node; page tables ≤ L² · heap/page · 8 B if fully cross-touched | per node | 16 mappings | 65,536 mappings; worst case 128 GiB of 4 KiB PTEs for 1 GiB heaps (touched pages only in practice; huge pages per §6.2 divide by 512) | same per node | **yes** |
| 13 | Team state: two predefined teams, in the node header and 12 B/team in `osoe_progress` (`node.h:255`, `progress.h:106-108`) | O(1) today; `OSOE_TEAMS` is compile-time | per node | — | — | — | yes; general teams are unbuilt, so not arithmetically checkable |
| 14 | Watch records (`node.h:66-67`, inside the slot) | one cache line per local PE | per node | in row 8 | in row 8 | in row 8 | yes |
| 15 | Allocator metadata, private (`allocator.c:15,39`) | 32 B × live blocks | per PE | — | — | — | yes; independent of N, L |
| 16 | `pe_quiet` scratch (`transport-am.c:582`) | (N−1) bytes `calloc` **per call** | per call | 3 B | 499 B | 4 KiB | shape |

**Per-node totals of the P-dependent private rows (2+3+9+10):** 4×4: ~1 KiB;
500×256: **≈ 255 MiB**; 4,096×256: **≈ 2.1 GiB**. For comparison the structure
the design worries about, the connection area, is 125 MiB at 500 nodes and is
demand-paged; rows 9–10 are `malloc`ed and row 9 is fully written at init
(`node.c:394-395`), so it is resident.

### What grows with what

- **With P (job size), per PE:** rows 9, 10 (world-sized, private), row 11
  (shared). Rows 9–10 are the design's claim failing: per node they are
  4·N·L² each.
- **With N per PE (so N·L per node):** rows 2, 3, 16.
- **With N per node (the intended scaling):** rows 1, 4, 5, 7.
- **With N² job-wide:** row 1j (connection buffers), row 7 (wire objects now;
  TCP connections at step 3: N(N−1)/2 = 124,750 at 500 nodes, 8.4 M at 4,096).
- **With L only:** row 8, dominated by landing zones = L × chunk. At
  `OSOE_AM_CHUNK` = 1 MiB (the permitted maximum) that is 256 MiB per node.
- **With L²:** row 12.

### Step 3 extrapolation (needs step 3; figures *recalled*, unverified)

Per lead: N−1 TCP sockets. At N = 500 that is 499 fds — under the usual soft
`RLIMIT_NOFILE` of 1,024, but N ≥ ~1,000 exceeds it and nothing in the tree
raises it. Idle kernel cost is a few KiB per socket (~2 MiB at 500). Under
all-to-all load, autotuned buffers (defaults up to ~4 MiB send / ~6 MiB receive
per socket) are bounded only by `tcp_mem`, which on a 4 GiB Pi 5 is a few
hundred MiB system-wide: an all-to-all across 499 sockets would reach TCP memory
pressure. §8.2 (line 3901) mentions small `SO_SNDBUF`/`SO_RCVBUF` only as a test
mitigation; see finding F7.

### Are the defaults sane?

- *Pi cluster (4 nodes, 4–8 GiB):* yes. Everything in the table is under 1 MiB
  per node except the heap.
- *500 × 256:* connection area 125 MiB demand-paged, control segment 17 MiB,
  plus the unplanned 255 MiB of private arrays. Acceptable on an HPC node, but
  the last item is pure waste.
- *Top end:* default budget stops the job at N > 1,025 with a clear message
  (*observed*, `struct-sizes.txt`: status `NO_ROOM` at 2,048). Good behaviour,
  but the comment's "not reached until about a thousand nodes" is the *hard
  ceiling*, not a guard rail, because the floor is also the default: there is
  no smaller buffer to shrink to without lowering `OSOE_AM_CHUNK`.
- *No size is checked against reality.* `ftruncate` without `posix_fallocate`
  (*observed*: no `fallocate` anywhere under `shmem/` or `base/`) means an
  over-committed `/dev/shm` (tmpfs defaults to 50 % of RAM, *recalled*) turns
  into `SIGBUS` at first touch in an arbitrary PE rather than an init error.
  `OSOE_AM_BUFFER_BUDGET` accepts up to 64 GiB and `OSOE_AM_BUFFER` up to 1 GiB
  with no comparison to physical memory.
- `OSOE_BOOTSTRAP_TIMEOUT` (60 s) doubles as the timeout of every data-path
  wait (`transport-am.c:181,309,338,601`). One knob, two meanings: raising it
  for a slow launch also delays every hang diagnosis.

---

## 3. At the limits

| Limit | What happens | Where | Degrade / abort | Deadlock? |
|---|---|---|---|---|
| Reply credit exhausted | Issuing PE loops: `futex_wake` the lead, `nanosleep` 20 µs, retry; fatal after 60 s | `transport-am.c:305-321` | degrade → abort | No. The PE holds nothing else while waiting. But see F1: a dropped request never returns its credit, so credits leak. |
| Connection buffer stays full | Writer: same poll loop, counted once in `am.reserve_stalls`; fatal after 60 s. Lead: `drain` and `flush` simply return and try next pass | `transport-am.c:175-193`; `progress.c:338-340, 383-384, 473-503` | degrade → abort; back-pressure lands on the *writer PE only* | No — see below |
| Reply pool empty, get / fetching AMO request | **Request silently dropped.** A private counter is incremented that nothing exports. Origin hangs 60 s, then "no get reply"; its credit is never returned | `progress.c:61-72` | **silent loss → abort on the wrong node** | — (F1) |
| Reply pool empty, collective forward | `osoe_fatal` naming the peer | `progress.c:86-96` | abort | — (reachable inside the credit invariant, F1) |
| Landing zone vs large get | Cannot be exhausted: the origin chunks the request to the slot's capacity and issues chunks **sequentially**, one round trip each, then copies slot → `dst` | `transport-am.c:388-416` | degrade: 1 MiB get = 16 round trips; *measured* 7–25 ms vs 2.3 ms for the put | No |
| Control queue / `max_teams` | `OSOE_TEAMS` = 2 at compile time; a message naming another team is fatal. §3.8's bound 2 (blocking collectives ⇒ ≤ 1 forward per team per connection) holds, but the pool has no room reserved for it (F1) | `progress.c:203-205, 655-657` | abort | No |
| Thread table full | Thread counts into a private orphan block; `blocks_exhausted` bumped in the PE slot | `metrics.c:280-287` | degrade, visibly | No |
| Peer slow / stops reading | `osoe_wire_send` returns 0; lead carries on with other slots, never blocks. Writers toward that peer stall in the poll loop, `quiet` stalls; both fatal at 60 s with a message naming the node | `progress.c:473-487`; `transport-am.c:601-609` | degrade → abort | No |
| Two nodes, all buffers full toward each other | Each lead's pass is `receive → drain → flush` per slot; `receive` needs no outbound resource (puts are applied in place, requests become descriptors, arrivals are enqueued: `progress.c:108-267, 689-691`), so both wires drain and both sends complete | `progress.c:575-584` | — | **No.** §3.7 is kept in the C: no handler path calls `osoe_connection_reserve`. *Observed in source*; I did not model-check it. |

The §3.8 three-bound argument, checked against the C:

1. *AMO/get results bounded by the origin's credit* — kept (`take_credit` is a
   CAS, `reply.h:182-196`), **but the overflow behaviour contradicts
   `reply.h:38-42`** ("a hard assertion naming the peer, not a silent drop"):
   `enqueue()` drops silently.
2. *Barrier forwards bounded by degree × teams* — the bound holds, **the
   allocation does not include it**: §3.8 says size as `C + degree×max_teams +
   slack`; `progress-host.c:233` sizes `peers × C`.
3. *Quiet acks coalesced* — kept; an ack is a header field, not a descriptor
   (`progress.c:439-447, 515-517`).

**§3.8's starvation guard is not wired in.** `osoe_connection_drain_reserve` and
`OSOE_BUFFER_DRAINING` exist (`connection.c:149`, `connection.h:196`) and are
tested in isolation (`test-connection.c:215-229`), but `progress.c` never enters
DRAINING: `drain()` competes with application writers through the ordinary
`osoe_connection_reserve` (`progress.c:338, 383`). I could not provoke
starvation with two streaming writers on 4 cores (F3).

---

## 4. Performance claims, classified

| Claim | Where | Class | Status |
|---|---|---|---|
| Self-clocking aggregation: batch grows under load, one when idle, no tuning | §3.9; `progress.c:505-524` | **testable now** (shape); batch growth against a real send CQE **needs step 3** | *Measured*: 1,000 × put 8 B + one quiet = 1.1–1.5 µs/put vs 315–327 µs for put+quiet each — aggregation is doing its job. "Close to the right policy" is **unfalsifiable as stated**. |
| Head-of-line bound ≈ two segments (~2 ms on 1 GbE at 128 KiB) | §3.2; `connection-area.h:44-52` | **testable now** | *Measured*: 8-byte get latency behind two streaming writers rose from 0.32 ms to 3.1–4.4 ms on a wire moving ~57 MiB/s; 2 × 128 KiB / 57 MiB/s = 4.4 ms. The bound is real **at user level**. With TCP it additionally includes the kernel send buffer — **needs step 3** (F7). |
| Floors: 8-byte RTT vs io_uring echo; bandwidth vs streaming harness | §1, §8.4 | **needs step 3**; and **unfalsifiable as stated** until the constant factors are pinned — §1 says they deliberately are not. The §8.4 harness does not exist in the tree (*observed*: `tools/` has `launch modelcheck probe stat`). |
| Intranode floor: `memcpy` + required synchronisation | §1, §8.4 | **testable now** | *Measured*, below: +13–17 ns fixed, ≤ 13 % at 4 KiB, parity at 1 MiB. Holds. |
| Step-2 progress polling is cheap enough to be a stand-in | §3.10 note; `progress-host.c:28` | **testable now** | *Measured*: 3–4 % of a core per idle lead, ~2,700–3,400 wakeups/s; median internode latency quantised at ~300–400 µs. Acceptable as a stand-in, **fatal to any internode latency measurement taken before step 3**. |
| Copies per put / get | §3.3, §3.12 | **testable now** by reading | Put: `src`→connection buffer (`transport-am.c:242`) → wire ring (`osoe_wire_send`) → scratch (`progress.c:304`) → heap (parser, via `destination`) = **4**. Get adds target heap→buffer (`progress.c:396`) at the front and landing slot→`dst` (`transport-am.c:408`) at the end = **5**. In step 3 the two middle copies become the kernel's. Irrelevant at 1 GbE; relevant to §10's "keep AM hot under RDMA". |
| Syscalls per small put | §3.10, §3.12 | **testable now** | Intranode: 0. Internode: **1 `futex_wake` per put, unconditionally** (`transport-am.c:267` → `progress-host.c:142-149`), even when the lead is awake. §3.10's optional `blocked` hint is not implemented. *Measured*: put alone 212–304 ns median, consistent with one syscall. `quiet` adds 2 syscalls per 20 µs poll iteration. |
| Locality dispatch is one branch | §8.1, §4.10 | **testable now** | Three world-sized table loads per intranode put: `node_of[pe]` (`job.h`), `osoe_is_local` again plus `local_of_rank[pe]` in `osoe_transport_direct` (`transport-am.c:138-140`), `local_of_rank[pe]` again in notify (`watch.c`). Inside the measured 13–17 ns with tiny tables; at 1 M PEs the tables are 4 MiB each and a random internode target costs a cache miss. Minor. |
| `notify` after every intranode put is cheap | §4.9, §4.10 | **testable now** | One acquire load of the target's `watch_rng`, which has its own cache line (`node.h:66`), plus a counter. No syscall unless a waiter's range covers the write. Sound; included in the 13–17 ns. |
| Serializing store in fence/quiet | §4.6; `base/order.c` | **testable now** (cost) | *Measured*: `fence` 7.6–12 ns, `quiet` with nothing outstanding 25–30 ns — the difference is `osoe_now_ns()` taken before the early return (`transport-am.c:559`). On aarch64 a `seq_cst` store compiles to a plain store; whether that still delivers what §4.6 wants from it is a correctness question for the atomics reviewer — flagged, not analysed. |
| Per-op counters obey §3.14's cost rules | §3.14 | **testable now** | Three `osoe_count` calls per intranode put, each an out-of-line `osoe_metrics_block()` with a TLS load (`metrics.c:263`). Total put overhead bounds them at a few ns each. Fine. |
| "Reasonable performance on plain Ethernet" | §1 | **unfalsifiable as stated** | No number, no harness, no socket. |

---

## 5. Measurements

**Method.** Private copy of the tree, `cmake --preset release` (RelWithDebInfo),
built and run only in `build/scratch/review-performance-scale/`. `bench.c` uses
the public API only, under `osoe-launch` with a node map: `-m 0,0` is intranode,
`-m 0,1` puts PE 1 on a second logical node. Batched timings: 41 samples of
20,000–100,000 operations; per-op timings: 1,000 individually timed operations
(30 for 1 MiB). min / median / p90 reported. One run each — no repetition across
runs, so treat run-to-run variation as unknown. `shmem_p`/`shmem_g` are not in
this build (typed RMA deferred, §7.5); 8-byte `putmem`/`getmem` stand in.

**Caveat, again:** measured under contention, VM, shared-memory wire — not a
socket. aarch64, 4 cores, load average 3–6.

### Intranode (`bench-local-run1.txt`)

| Operation | min | median | p90 |
|---|---|---|---|
| raw `memcpy` 8 B (inlined to a store) | 0.3 ns | 0.3 ns | 0.3 ns |
| `shmem_putmem` 8 B | 13.3 ns | 17.3 ns | 27 ns |
| `shmem_getmem` 8 B | 11.0 ns | 15.0 ns | 29 ns |
| raw `memcpy` 4 KiB | 59.9 ns | 71 ns | 139 ns |
| `shmem_putmem` 4 KiB | 66.4 ns | 98 ns | 148 ns |
| raw `memcpy` 1 MiB | 24.5 µs | 28.5 µs | 38.9 µs |
| `shmem_putmem` 1 MiB | 19.2 µs | 42.8 µs | 77.6 µs (noise-dominated; parity) |
| raw `atomic_fetch_add` | 2.3 ns | 2.9 ns | 3.8 ns |
| `shmem_long_atomic_fetch_inc` | 15.6 ns | 18.8 ns | 28 ns |
| `shmem_fence`, nothing outstanding | 7.6 ns | 11.9 ns | 24 ns |
| `shmem_quiet`, nothing outstanding | 24.5 ns | 29.8 ns | 39 ns |
| `putmem` 8 B + `quiet` | 36.8 ns | 45.1 ns | 82 ns |

No surprise. Fixed overhead ≈ 13 ns ≈ 40–50 cycles for argument checks, three
counters, locality and notify.

### Cross-"node" (`bench-remote-default-run1.txt`, `bench-remote-wire4m-run1.txt`)

| Operation | default 16 KiB ring: min / med / p90 | 4 MiB ring: min / med / p90 |
|---|---|---|
| put 8 B + quiet | 145 / **350** / 939 µs | 188 / **324** / 415 µs |
| put 8 B alone (local completion) | 0.22 / 0.30 / 0.30 µs | 0.17 / 0.21 / 0.30 µs |
| 1,000 × put 8 B then one quiet, per put | 0.77 / **1.49** / 2.0 µs | 0.62 / **1.12** / 1.7 µs |
| 200 × (put + quiet), per put | 305 / 316 / 337 µs | 317 / 327 / 337 µs |
| get 8 B | 46 / **356** / 697 µs | 42 / **361** / 408 µs |
| get 4 KiB | 264 / 351 / 595 µs | 289 / 371 / 532 µs |
| `atomic_fetch_inc` | 54 / **336** / 407 µs | 274 / 402 / 1,080 µs |
| put 1 MiB + quiet | 18.0 / 18.9 / 20.1 ms (53 MiB/s) | 1.9 / 2.3 / 3.4 ms (**428 MiB/s**) |
| get 1 MiB | 24.2 / 25.3 / 26.5 ms (40 MiB/s) | 7.2 / 10.5 / 14.4 ms (96 MiB/s) |

Reading: (a) every round trip's median sits at 300–400 µs while minima are
20–50 µs — the 250 µs receive poll (`progress-host.c:28`) plus timer slack, on
one or both sides, *is* the latency. (b) The default test ring throttles bulk to
~53 MiB/s because each 16 KiB needs another pass; with a socket-sized ring the
four-copy path moves 428 MiB/s, 3.7× a 1 GbE link, so copies are not the
bottleneck on the Pi target. (c) A 1 MiB get is 4–5× slower than the put: 16
sequential round trips (`transport-am.c:393-416`), each paying the poll.

### Barrier (`bench-barrier-run1.txt`), 1,000 barriers, seen by PE 0

| Map | min | median | p90 |
|---|---|---|---|
| `0,0` (one node, 2 PEs) | 0.5 µs | 6.3 µs | 34 µs |
| `0,0,0,0` (one node, 4 PEs on 4 busy cores) | 6.0 µs | 33 µs | 44 µs |
| `0,1` | 22 µs | 355 µs | 659 µs |
| `0,1,2` | 43 µs | 364 µs | 671 µs |
| `0,1,2,3` | 34 µs | 390 µs | 662 µs |
| `0,0,1,1` | 25 µs | 357 µs | 660 µs |

Flat in node count at this size; median = one poll interval, p90 = two.

### Idle cost (`bench-idle-run1.txt`, `bench-idle-by-node-count-run1.txt`)

| Job | CPU per process, idle 3 s | voluntary context switches |
|---|---|---|
| 1 node × 2 PEs | 0.04 % of a core | 30 |
| 2 nodes × 1 PE | 3.7–3.8 % | ~8,050 |
| 4 / 8 / 16 / 32 nodes × 1 PE | 3.3 / 3.6 / 2.7 / 3.0 % | 8,400 / 9,100 / 9,850 / 10,150 |

The lead's agent is sleeping on a futex with a 250 µs timeout — not spinning —
and waking ~2,700–3,400 times a second. Cost is flat to 32 nodes, i.e.
dominated by the timed wait, not yet by the O(N) pass.

### Limits experiments

- **Pool exhaustion** (`limit-pool-exhaustion.c`, outputs alongside).
  `OSOE_AM_CREDITS=2`, map `0,0,0,1`: PE 3 puts 1 MiB to PE 0 and enters the
  barrier while PEs 1 and 2 issue 8-byte gets at PE 3. **3/3 runs aborted** with
  "no get reply after 60 s" on node 0. An instrumented copy
  (`limit-pool-exhaustion-instrumentation.diff`, a `fprintf` in `enqueue()`)
  printed `node 1: reply pool empty (in_use 2 of 2), DROPPING request kind 1
  from PE 2`. At default credits 50 iterations completed.
- **Reply starvation** (`limit-reply-starvation.c`). 8-byte gets from PE 0
  while two node-mates of the target stream puts back at PE 0's node. 16 KiB
  ring: median 0.32 ms → 3.1 ms (32 KiB puts), 4.4 ms (65 KB puts), 0.87 ms
  (1 KiB puts). 4 MiB ring: no effect (0.31 ms, p99 0.43 ms). So what was
  observed is §3.2's head-of-line bound, not starvation.

---

## 6. Findings

### F1 — The descriptor pool has no room for collective forwards, and an overflowing request is silently dropped
- **Location:** `shmem/src/progress-host.c:233`; `shmem/src/progress.c:61-72`
  and `86-96`; contradicts `shmem/src/reply.h:38-42` and design §3.8 ("Size as
  `C + degree×max_teams + slack`", "Assert hard on overflow").
- **Failure scenario:** Node X has `peers × C` descriptors. Each peer node may
  legally have C gets outstanding toward X. X's own barrier arrival (non-root)
  or the root's N−1 releases draw from the same pool. With one connection's C
  descriptors held (its outbound buffers busy, so `drain` cannot retire them)
  plus one collective descriptor, the next legal get finds the pool empty and
  `enqueue()` returns without queuing; the parser has already consumed the
  message, so the comment's "leaves the request" is not what happens. The origin
  waits 60 s and aborts with "no get reply"; its credit is never returned. In
  the other order the collective forward hits `osoe_fatal`. At C = 64 this needs
  ≥ 64 PEs of one node with a get outstanding toward the same peer node during a
  barrier — ordinary at 256 PEs per node. With N > 2, the pool is shared, so
  it takes all peers saturated, or fewer descriptors free than forwards needed;
  the root's N−1 releases make the root the likeliest victim.
- **Evidence:** *measured* — reproduced 3/3 with `OSOE_AM_CREDITS=2`; drop
  confirmed by instrumentation. *Observed in source* — sizing and drop path.
  `reply_reservation_failures` and the pool high-water mark are not in
  `metrics.c` (*observed*: grep), so `osoe-stat` cannot show either.
- **Severity:** **blocking** for the get/AMO path at the design's target scale
  (small fix: size `peers × (C + OSOE_TEAMS) + OSOE_TEAMS`, make the drop a
  fatal naming the peer, export the high-water mark).
- **Confidence:** high for the mechanism; medium that C = 64 is reachable in
  practice — a 64+-PE node map run would raise it (not possible on 4 cores).

### F2 — Per-PE private arrays sized by world size: per-node memory grows as N·L²
- **Location:** `shmem/src/node.c:391-397` (`local_of_rank`, 4P bytes, fully
  written); `shmem/src/bootstrap.c:81` (`gtids`, 4P bytes allocated, L used).
- **Failure scenario (arithmetic):** 500×256: 2 × 500 KiB × 256 = 250 MiB per
  node. 4,096×256: 2 × 4 MiB × 256 = **2.0 GiB per node**, 8 TiB job-wide, to
  answer "which local index is this rank" — a question whose answer is
  `rank − first_rank_of_node` under block distribution, or a hash of L entries.
- **Evidence:** *observed in source*; sizes *measured* by arithmetic from
  `struct-sizes.txt`.
- **Severity:** **design change needed** (for goal 4; harmless on the Pi). Both
  files are above the §4.10 seam, so an RoCE build inherits them.
- **Confidence:** high.

### F3 — §3.8's DRAINING starvation guard exists but is never used
- **Location:** `shmem/src/progress.c:320-409` (no DRAINING state, no call to
  `osoe_connection_drain_reserve`); defined at `connection.c:149`; design §3.8
  "Starvation guard", §7.1 line 3606 lists "DRAINING caps" in step 2's scope.
- **Failure scenario:** with many local writers winning every reservation CAS
  between `flush` opening a buffer and the next pass's `drain`, a get reply is
  deferred generation after generation; the origin's `shmem_get` hangs until
  the 60 s abort. The half-buffer cap (review D8) is likewise absent.
- **Evidence:** *observed in source* (grep: only `test-connection.c` calls it).
  *Measured*: **not reproduced** with 2 writers on 4 cores — p99 0.43 ms.
- **Severity:** **clarification needed** — either wire it in or record that
  step 2 does not have it; the model evidence cited for DRAINING does not
  describe the running code.
- **Confidence:** high that it is absent; low on impact. A run with ≥ 32
  writers per node would raise it.

### F4 — The progress pass is O(N) per wake, and every internode put wakes it
- **Location:** `shmem/src/progress.c:575-584` (every slot, every pass);
  `transport-am.c:267`, `progress-host.c:142-149` (unconditional
  `fetch_add` + `futex_wake` on the lead's single `agent_wake` word).
- **Failure scenario (arithmetic):** each slot costs ≥ 4 shared cache lines
  (`ack_requested` exchange — a locked RMW per slot per pass — `head`, wire
  `produced`, `consumed`). At N = 500, ~2,000 line touches ≈ 10–40 µs per pass;
  at 4,096, 100–300 µs. A halo exchange touching 6 peers pays for 499. And 256
  PEs issuing small puts all do a locked add on one cache line and a syscall
  into one futex bucket per put.
- **Evidence:** *observed in source*; per-pass cost *estimated*, not measured
  (flat to N = 32 in the idle measurement, where the timed wait dominates).
- **Severity:** **design change needed** before the 500-node tier: a
  lead-side list of connections with work (the writer already knows the slot),
  and §3.10's `blocked` hint so a running lead is not re-woken.
- **Confidence:** medium; a 256-slot synthetic progress benchmark would settle it.

### F5 — Step-2 internode latency is the poll interval; do not benchmark against it
- **Location:** `shmem/src/progress-host.c:28`, `agent.c:72-73`; all PE-side
  waits are 20 µs `nanosleep` polls (`transport-am.c:184, 312, 604`).
- **Failure scenario:** any latency number taken before step 3 measures
  250 µs + timer slack, not the design; a regression of 50 µs in the real path
  is invisible.
- **Evidence:** *measured* (medians 300–400 µs, minima 20–50 µs); *documented*
  as a stand-in in §3.10.
- **Severity:** **clarification needed** — record in §8.4 that floors are not
  to be pinned on step-2 numbers. Not a defect.
- **Confidence:** high.

### F6 — Large gets are sequential round trips through one 64 KiB landing slot
- **Location:** `shmem/src/transport-am.c:287-295, 388-416`.
- **Failure scenario:** 1 MiB get = 16 request/reply round trips + an extra
  copy; on a 0.2 ms-RTT Ethernet that is ≥ 3.2 ms of pure latency on top of
  9 ms of wire time, and a credit is taken and returned 16 times.
- **Evidence:** *measured* 96 MiB/s get vs 428 MiB/s put; *documented* as
  "slow mode first" in the source comment and §4.7 (accepted asymmetry).
- **Severity:** **minor** (accepted), recorded because the measured factor
  (4–5×) is larger than §4.7 suggests and because landing zones are the
  dominant term of the control segment (16 MiB of 17 MiB at L = 256).
- **Confidence:** high.

### F7 — The two-segment head-of-line bound ignores the kernel's send buffer
- **Location:** design §3.2; `shmem/src/connection-area.h:44-52`.
- **Failure scenario:** with Linux TCP autotuning a socket may hold megabytes
  below the AM layer (*recalled*: `tcp_wmem` max ~4 MiB); a barrier release
  queued behind 4 MiB at 117 MiB/s waits 34 ms, not 2 ms. Bounding it needs
  `SO_SNDBUF` or `TCP_NOTSENT_LOWAT`, which the design mentions only as a test
  mitigation (line 3901).
- **Evidence:** user-level bound *measured* and confirmed; kernel part
  *recalled*, unverified — **needs step 3**.
- **Severity:** **clarification needed**. **Confidence:** medium.

### F8 — Shared-memory objects are sized without reserving the pages
- **Location:** `connection-area.c:198`, `node.c:120, 242`, `wire.c:181`,
  `job.c:110` — `ftruncate` only.
- **Failure scenario:** `/dev/shm` smaller than heaps + areas: `shmem_init`
  succeeds, and some PE takes `SIGBUS` inside a `memcpy` minutes later.
- **Evidence:** *observed in source* (no `fallocate`); tmpfs behaviour
  *recalled*. Demand paging of the connection area is deliberate
  (`connection-area.c:226-232`), so the fix is a capacity check
  (`fstatvfs`) rather than `posix_fallocate` there.
- **Severity:** **minor**. **Confidence:** medium.

### F9 — Small things seen in passing
- `osoe_transport_wait` reads the clock before its nothing-outstanding early
  return (`transport-am.c:559-569`): ~18 ns of a 30 ns `quiet`. *Measured.* Minor.
- `shmem_pe_quiet` allocates and frees an (N−1)-byte array per call
  (`transport-am.c:582`). Minor.
- `finalize()` walks every message of every segment to count them and looks up
  a handler-table entry it then discards (`progress.c:426-438`): O(messages)
  on the lead's critical path for a redundancy check. Minor.
- `receive()` loops until the wire is empty with no per-pass budget
  (`progress.c:303-309`); harmless on a 16 KiB ring, worth a cap before a
  socket with megabyte buffers feeds it. Minor, low confidence.
- For `algorithmic-scaling`: `osoe_node_map_parse` is O(P·N)
  (`base/node-map.c:33-40`), `osoe_node_map_local_id` O(rank); `job.c:236`
  scans the world per PE; the barrier root enqueues N−1 releases (flat tree).

---

## 7. What is sound, and what convinced me

- **The AM layer's shared state really is O(N) per node with no PE term.**
  `osoe_am_area_bytes` has no L in it; 256 PEs share one credit and two buffers
  per peer (`connection.h:143-183`); the descriptor pool is `peers × C`. Checked
  by compiling the sizing functions and printing them.
- **The budget fails loudly, not slowly** — `NO_ROOM` at 2,048 nodes observed,
  and a sub-floor buffer is refused rather than silently accepted.
- **No deadlock at the limits.** Every handler path in `complete()` enqueues or
  applies in place; the lead's `drain`/`flush` return on no-room; the arrival
  path enqueues rather than reserving inline, with the reason written down
  (`reply.h:78-84`). The mutual-full-buffer scenario resolves because receive
  needs no outbound resource.
- **Aggregation works and costs nothing when idle** — 250× between batched and
  quiet-per-put, no timer.
- **§3.2's head-of-line bound was predicted and then observed** within 1 % of
  the arithmetic.
- **The intranode path is at the floor**: ~13 ns fixed, parity at bulk sizes;
  `notify` is one load of a word on its own cache line; no syscall anywhere on
  it (0.04 % idle CPU on a one-node job).
- **The agent sleeps**; the step-2 poll is a documented stand-in with a small,
  flat CPU cost.
- **The dirty set delivers what it claims in time** — a wait scans touched
  peers only (`transport-am.c:594`) — even though its *memory* is O(N) per PE.
- **Thread-table exhaustion degrades visibly**, as §3.14 requires.

---

## 8. Lessons for a successor API

- **`shmem_quiet` / `shmem_fence` with no target force per-context, per-peer
  bookkeeping** (the dirty set: O(N) per context per PE). `shmem_pe_quiet`
  already shows the better shape; a successor should make the peer set
  explicit, or return a completion handle from the put.
- **World-rank addressing of every operation** pushes a rank→(node, local)
  translation onto every call and tempts world-sized tables (F2). A
  (node, local-index) address, or a requirement of block distribution, makes
  the translation arithmetic.
- **Blocking `get` into arbitrary private memory** is what creates landing
  zones, the extra copy, the 16-round-trip large get and the dominant term of
  the control segment. A get whose destination must be symmetric (or
  registered) removes all four.
- **Fetching atomics need a reply and therefore the credit / descriptor
  machinery**; the non-fetching forms complete like puts. A successor could
  make non-fetching the default and price the fetching form honestly.
- **Every local heap mapped into every local process** (needed for `shmem_ptr`
  and intranode puts to any PE) is an L² mapping cost per node; a single
  node-wide heap object with per-PE slices gives the same semantics with one
  mapping per process.
- **Implicit progress** (no `shmem_progress` call) is what obliges an agent
  thread per node and, in step 2, the poll; keep it, but note it is the API that
  demands the thread.

---

## 9. Not investigated

- Anything over a real socket, on a Pi, or on more than one host; all step-3
  figures are recalled, not measured.
- The per-pass cost of `osoe_progress_poll` at N ≥ 64 (F4 is arithmetic).
- F1 at the default C = 64 and F3 with many writers — both need more cores.
- Team state beyond the two predefined teams (§5.7) — unbuilt.
- Whether heaps actually use huge pages (row 12 relies on a source comment).
- Whether the aarch64 "serializing store" does what §4.6 needs — handed to the
  atomics reviewer.
- Repeated benchmark runs: each table is a single run under variable load.
- `algorithmic-scaling`'s report was not read; complexity items in F9 are
  pointers only.
