# Algorithmic scaling: every algorithm, as designed versus as built

Review of phase 1, step 2, slice 1 (merge `d542c63`). Agent `algorithmic-scaling`.
Scope: the author's goals 2–4 — intranode algorithms linear or better
logarithmic in PEs per node (`L`), internode algorithms linear or better
logarithmic in node count (`N`), in time and memory, and nothing standing in the
way of ~1M PEs on RoCE. The sibling report `performance-scale` owns byte-level
arithmetic; this one owns asymptotic complexity.

Symbols: `N` nodes, `L` PEs per node, `P = N·L`, `T` teams, `m` message bytes,
`B` live heap blocks, `C` reply credit per connection. Two tiers, per the common
brief: the Ethernet/AM build at 500 × 256 (`P` = 128,000) and inherited
structures at 4,096 × 256 (`P` ≈ 1.05 M).

Claim labels: *documented* (design doc or spec text), *observed in source*,
*measured* (this review, in the shared 4-core VM, always **under contention**),
*arithmetic* (derived here from stated assumptions), *recalled* (from memory,
unverified — all per-message and per-wake CPU costs below are recalled typical
values and are only used as stated assumptions).

Evidence directory: `docs/reviews/evidence/phase1-step2-slice1/algorithmic-scaling/`.
All builds and runs used a private copy under
`build/scratch/review-algorithmic-scaling/tree`; the one source mutation (a
`fprintf` of the progress engine's private counters at stop) exists only there
and is recorded as `instrument-progress-counters.patch`.

---

## Summary table

"Bar" is the author's: **log** / **linear** / **fails** (super-linear, or a
serial O(N) or O(P) hot spot where a logarithmic algorithm is standard).

| # | Algorithm / structure | Designed | Built | Intranode bar | Internode bar | Structure permits the fix? |
|---|---|---|---|---|---|---|
| 1a | Barrier arrival, intranode | flat `fetch_add` "to start"; two-level tree "chosen at init" (§5.3) | flat `fetch_add`, one line per team (`barrier.c:45`) | linear, tiny constant (~25 µs at 256, *arithmetic*) | — | Yes, with a layout bump: counters are named fields, not arrays (`node.h:247-255`). No trace of the tree in code or layout |
| 1b | Barrier release, intranode | "wake all local waiters", on the progress thread (§5.3) | `futex_wake(INT_MAX)` on the lead's agent (`progress-host.c:115`) | linear, serial **on the progress thread** (~0.25–0.5 ms at 256) | — | Yes, no layout change: chained wake on the same futex word |
| 1c | Barrier, internode | "internode tree", per-team topology (§5.2, §5.5, §3.8, §5.8, §10) — but §5.3 itself says "the root counts" | **star** rooted at node 0: N−1 arrivals in, N−1 releases out, all serial in one thread (`progress.c:200-238`, `:660-676`) | — | **fails**: O(N) serial at one node, depth 1. 2.5–6.7 ms at 500, ~20 ms at 4,096 on TCP vs 0.3–1 ms for a tree | Yes: the wire message (`team, epoch, from_node`) and the handler seam are tree-ready; per-team topology is missing |
| 2a | Small reduction / broadcast, intranode | lead combines all `L` sources serially (§5.5) | not built | linear in work, but depth O(L·m) with L−1 cores idle; two-level "sub-leads" give O(√L·m) at best | — | Yes: a chunk-partitioned combine keeps single-writer and needs no new counters |
| 2b | Reductions, internode | accumulator tree; ring / Rabenseifner above a measured crossover (§5.8) | not built | — | log latency, bandwidth-optimal at size — **as designed**; depends on 1c's tree existing | Yes (`OSOE_REGION_INTERNAL` is additive) |
| 2c | Scans | parallel prefix over runs, O(log runs) rounds (§5.8) | not built | linear | log (contiguous) … log P (cyclic) | Run table is O(P) per team per PE under cyclic placement unless stored as a rule |
| 2d | collect / fcollect / alltoall | undesigned (§5.5, §10) | not built | — | — | Node-pair aggregation suits them (N² messages, not P²) |
| 3a | Rank→node map | one table, broadcast by the server (§6.2) | shared `uint32_t[P]` per job object (`job.c:98`), one lookup function (`job.h`) | — | O(P) per node; O(N·P) bytes broadcast from one node | Yes: every reader goes through `osoe_node_of()` |
| 3b | World rank → local id | `base_of[L]` table (§4.1) | **private `int32_t[P]` per PE** (`node.c:391-397`) | **fails memory**: O(P) per PE, O(P·L) per node — 131 MB/node at 500×256, 1.05 GB/node at 1M | — | Yes, ~10 lines |
| 3c | Rendezvous and mesh set-up | leads only, all-to-one; eager full mesh, lower connects to higher (§6.2) | shared-memory stand-in, eager (`progress-host.c:244-246`) | O(L²) sibling mmaps per node (fine) | N−1 per node, N(N−1)/2 job-wide, eager; accept incast at high-index nodes | Lazy connect is not provided for, not precluded |
| 4a | Locality dispatch, address translation, team→world | O(1) | O(1): three dependent loads (`transport-am.c:132-144`) | O(1) | O(1) | — |
| 4b | `notify` after a local put / rmw | one watch slot per PE, indexed | direct index, one load (`watch.c:83-97`) | **O(1)** — sound | — | — |
| 4c | Dirty set, `quiet` | O(communication degree) (§4.6) | `order[]` list of touched slots; O(degree) (`transport-am.c:149-158, 594-613`) | — | **O(degree)** time — sound; O(N) memory per context per PE | Sparse form possible; struct is private |
| 4d | `fence` | flush the dirty set (§4.6) | re-requests an ack from **every** peer touched since the last quiet, acknowledged or not (`transport-am.c:506-517`) | — | O(peers since last quiet) segments per fence | Yes: skip peers already acknowledged |
| 4e | `pe_quiet` | "close to free" (§4.6) | `calloc(N−1)` per call (`transport-am.c:582`) | — | O(N) per call; should be O(npes + degree) | Yes |
| 4f | Progress pass | io_uring, completion-driven (§3.10); step 2 polls | every pass visits all N−1 connections, 3 phases each, including a locked exchange per connection (`progress.c:575-584, 467`); wake carries no peer identity | no `L` term — sound | O(N) per wake | Yes: `osoe_progress_host_wake_lead()` call sites all know the connection |
| 4g | Lead wake on every remote op | flag + futex wake; optional `blocked` hint (§3.10, §3.12) | unconditional `fetch_add` + `futex_wake` syscall on **one word for the whole node** (`progress-host.c:142-149`, called from `transport-am.c:267, 403, 444`) | one cache line and one futex bucket shared by all `L` writers, on the data path | — | Yes |
| 4h | Reservation | CAS on one `head` per node pair (§3.5) | same (`connection.c:33-81`) | worst case O(L²) CAS attempts per round on one line when all `L` PEs target one peer | — | Trade-off, see finding 6 |
| 4i | Acknowledgements | ack-only segment when nothing to piggyback on (§3.4) | **ack requests echo forever** (`progress.c:283-284, 442`) | — | every connection that ever saw a fence/quiet ping-pongs indefinitely (*measured*) | Yes, 3 lines |
| 4j | Watchdog sweep | development only | each blocked PE `kill(pid,0)`s all `L` slots per 100 ms sweep (`watchdog.c`) | O(L²) syscalls per sweep node-wide; negligible and dev-only | — | Step 4's pidfd design is O(L) on the lead |
| 5 | `shmem_malloc` / `free` | deterministic, world-collective barrier (§4.4, §4.5) | first-fit over a sorted array: O(B) scan + O(B) `memmove` + O(B) levels recompute per call (`allocator.c:95, 123, 218`), then a barrier | no `L` or `N` term locally | one barrier = finding 1c | O(log B) structure is a local change |
| 6 | Teams | `max_teams` job-wide, bounding three structures; allocator and split protocol undesigned (§5.7) | two predefined teams, compile-time `OSOE_TEAMS`, arrays indexed by team number; agent scans every team's handoff word per pass (`progress-host.c:197`) | per-node state O(T_job), not O(teams present on the node) | — | Wire carries a 32-bit team id, so a per-node map is possible; must be designed before splits land |
| 7 | Full mesh | one TCP connection per node pair (§3.1) | eager; buffers demand-paged | — | O(N) per node, O(N²) job-wide; default AM sizing refuses to start above ~1,000 nodes | Lazy connect permitted by the seam (`osoe_connection_to`), not designed |

---

## Findings

### 1. Acknowledgement requests echo forever: every connection that has carried one `fence` or `quiet` ping-pongs ack-only segments for the rest of the job

- **Location:** `shmem/src/progress.c:283-284` (`segment_applied`: an incoming
  `ACK_REQUESTED` sets `c->owe_ack`), `:439-447` (`finalize`: the outgoing
  segment's flags are `c->owe_ack ? OSOE_AM_SEGMENT_ACK_REQUESTED : 0`), `:515-517`
  (an owed ack seals a header-only segment). Design §3.4 "Ack-only segments".
- **Failure scenario:** `owe_ack` holds two different facts — "a local writer
  wants the peer to acknowledge" and "I owe the peer an acknowledgement" — and
  the seal turns either into the `ACK_REQUESTED` flag. So the segment that
  *carries* an acknowledgement also *asks* for one. A sends a put with
  `ACK_REQUESTED`; B applies it, owes an ack, sends a header-only segment whose
  flag is again `ACK_REQUESTED`; A applies that, owes an ack, sends a
  header-only segment with the flag set; and so on, indefinitely. Only teardown
  stops it (`osoe_progress_forget_acks`, whose comment — "so that a final drain
  does not manufacture ack-only segments" — describes the symptom at the one
  place it was noticed).
- **Evidence (*measured*, reproduced 4 times, control clean):**
  `idle-after-quiet.c` — two logical nodes, one PE each; PE 0 issues one 8-byte
  put and one `shmem_quiet`, then both PEs sleep 2 s. With the private counters
  printed at stop (`idle-after-quiet-instrumented.log`):

  | Run | `segments_sent` | `acks_only` | `messages_applied` |
  |---|---|---|---|
  | one put + quiet (3 runs) | 5,972 / 6,546 / 6,373 | 5,966 / 6,540 / 6,367 | 5–6 |
  | control, no put (3 runs) | 5 | **0** | 5 |
  | three nodes, PE 0 → PE 1 only | node 0: 6,794, node 1: 6,788, **node 2: 5** | 6,783 / 6,783 / 0 | — |

  About 3,000 header-only segments per second per direction, paced only by
  step 2's 250 µs poll sleep — which is also why CPU time does not show it (the
  first version of the test measured CPU and saw nothing) and why the suite is
  green. Over step 3's sockets the pacing is the round trip: on the order of
  5,000–10,000 segments per second per connection, and the lead's agent never
  sleeps. After any all-to-all pattern followed by a quiet, every one of a
  node's N−1 connections does this at once: ~2.5 M segments per second per node
  at N = 500 (*arithmetic*, 200 µs RTT), which is more than the lead can send.
- **Why it belongs in a scaling report:** prompt item 7 asks whether any
  protocol touches every connection periodically and so defeats lazy
  connections and idle sleep. This one does, by accident, and its cost is
  O(connections ever quieted) forever.
- **Fix shape:** two flags. `want_ack` (from `osoe_connection_take_ack_request`)
  sets the outgoing `ACK_REQUESTED`; `owe_ack` (from an incoming
  `ACK_REQUESTED`) only forces a seal. `owe_ack` must never set the flag.
- **Severity:** *blocking* for step 3 (harmless-looking in step 2, a permanent
  packet storm on TCP). **Confidence:** high. A test asserting
  `acks_only <= fences + quiets` would pin it; the counter is not exported to
  the control segment today, so `osoe-stat` cannot see it either.

### 2. The internode barrier is a star rooted at node 0; five sections of the design, and §10's closing argument, assume a tree

- **Location:** `shmem/src/progress.c:200-238` (root counts every node's
  arrival, then loops `slot = 0 .. nodes-2` enqueueing one release per peer),
  `:652-692` (`osoe_progress_arrive`: every non-root reserves onto *the root's*
  connection), `shmem/src/progress-host.c:130-138` (`participants()` returns
  `node_count`; `OSOE_COLLECTIVE_ROOT 0u`). Design §5.3 ("The root counts …
  the root releases only when every participating node has arrived") against
  §5.2 ("the internode tree spans only nodes with at least one member"), §5.5
  ("forwarded down the internode tree"), §3.8 ("bounded by tree degree ×
  concurrent teams"), §5.8 ("handlers that forward down a tree"), and §10 /
  `docs/reviews/archive/roce-transport.md:613` ("a tree over 7812 nodes ≈ 13 levels at
  ~1.8 µs ≈ 23 µs … roughly 4–6×").
- **What is built (*observed in source*):** total messages 2(N−1) — optimal.
  Critical path: depth 1 up, depth 1 down, but **all 2(N−1) message events are
  serial in one thread on one node** — the lead's agent of node 0, which is
  also that node's entire progress engine. Per-participant work O(1); root work
  O(N) per barrier per team. Root memory: N−1 reply descriptors held at once.
  The release loop also addresses all `nodes − 1` connections rather than the
  team's participating nodes (correct today only because the world team is the
  only one that reaches it).
- **Cost (*arithmetic*; assumptions *recalled*):** per small TCP message
  through io_uring, `t_rx` ≈ 3 µs and `t_tx` ≈ 2 µs of CPU on a server core,
  8 µs and 5 µs on a Pi 5 core; one-way latency `h` ≈ 100 µs on Pi 1 GbE, 20 µs
  on a fast Ethernet. Releases go to N−1 different sockets, so §3.9's
  aggregation cannot batch them.

  | | Star: 2h + (N−1)(t_rx+t_tx) | Tree, fanout 8: 2d·h + d·9·(t_rx+t_tx) | Ratio |
  |---|---|---|---|
  | Pi, N = 500 | 0.2 + 499 × 13 µs = **6.7 ms** | d = 3: 0.6 + 0.35 = **0.95 ms** | 7× |
  | server TCP, N = 500 | 0.04 + 499 × 5 µs = **2.5 ms** | d = 3: **0.26 ms** | 10× |
  | server TCP, N = 4,096 | 0.04 + 4,095 × 5 µs = **20.5 ms** | d = 4: **0.34 ms** | 60× |
  | RoCE-class (1 µs per message, h = 2 µs), N = 7,812 | **~7.8 ms** | §10's own figure: **23 µs** | ~340× |

  The last row is the important one for goal 4. §10 closed
  `osoe_transport_sync` with "the 4–6× estimate at 10^6 PEs is unchanged" — but
  that estimate compares an in-network barrier to a *tree*. Against what is
  built the gap is two further orders of magnitude, so the premise under that
  closure is not the code's. Throughput consequence at 4,096: node 0 can
  complete about 50 barriers per second job-wide, and every `shmem_malloc`,
  `shmem_free`, non-last `shmem_finalize`, and (in bale) two per collective is
  one.
- **Measured:** `barrier-scaling.c` at one PE per logical node, 2–16 nodes
  (`barrier-scaling-run-{1,2}.log`): 0.4–1.2 ms per `sync_all` with no usable
  trend — the cost is two 250 µs poll sleeps plus scheduler noise on 4 cores.
  **The star's O(N) term cannot be seen on this host**; the finding rests on
  the source and the arithmetic.
- **Does the structure permit the tree?** Yes, and cheaply, *now*.
  `struct osoe_am_barrier` already carries `team, epoch, from_node`;
  `arrivals[team]` / `participants[team]` generalise to "arrivals expected at
  this node = children + self"; an interior node forwards one `ARRIVE` to its
  parent instead of releasing, and on `RELEASE` enqueues to its children before
  applying locally — all inside the two `case` arms and
  `osoe_progress_arrive`. The full mesh means any tree shape is available.
  What is missing is §5.2's per-team topology (parent, children) — two
  functions of `(team, node)`, trivial for the world team. The later it is
  left, the more rests on the star: §3.8's control-queue bound
  (`degree × max_teams` — with a star, degree is N−1, and
  `progress-host.c:233` sizes the pool as `peers × C` with no team term at
  all), §6.4's teardown argument, and reductions, whose "partial result merged
  at a child" rules (§5.5) have no code to attach to.
- **Severity:** *design change needed* (the document must say which it is; the
  code should become the tree before reductions are written on top of it).
  **Confidence:** high on what is built and on the doc inconsistency; medium on
  the absolute milliseconds (recalled per-message costs) — a two-host TCP
  measurement of small-message send and receive cost would firm them up.

### 3. `local_of_rank` is a private O(P) array in every PE: the one built structure that breaks §1's "never PE count"

- **Location:** `shmem/src/node.c:391-397` (`malloc(sizeof(int32_t) × world_size)`
  per process, every entry written), read on every fast path:
  `transport-am.c:140`, `watch.c:85`, `progress-host.c:59, 75`, `team.c`. Also
  `bootstrap.c` (`b->gtids = malloc(4 × world_size)` for a list of `L`; never
  touched past `L`, so virtual only).
- **Arithmetic:** 4·P bytes resident per PE. 500 × 256: 512 KB per PE,
  **131 MB per node** — larger than the 125 MB connection area the design
  budgets so carefully (§3.4). 4,096 × 256: 4.2 MB per PE, **1.05 GB per
  node**, 4.4 TB job-wide. Init work O(P) per PE, O(P²) job-wide.
- **Evidence:** *observed in source*; design §1 *documented* ("Nothing may
  scale with PE count where it can scale with node count"); §4.1 specifies
  only the O(L) `base_of` table.
- **Fix shape:** the information is `gtids[]`, O(L). After
  `osoe_node_of(pe) == my node` (already O(1) from the shared map), local id is
  a binary search of `gtids` (8 steps at 256), or one shared `int32_t[P]` in
  the node segment (4·P per *node*), or arithmetic for block and cyclic
  layouts. An RoCE build inherits this table as it stands, since `resolve`,
  `notify` and `osoe_transport_direct` all use it.
- **Severity:** *minor* to fix, but it is a direct miss of goal 2's memory bar
  and should not survive to step 3. **Confidence:** high.

### 4. Intranode barrier: no trace of §5.3's two-level tree, and the larger linear term is the release, which runs on the progress thread

- **Location:** `shmem/src/barrier.c:45` (flat `osoe_epoch_bump_eq`),
  `progress-host.c:99-116` (`release` → `osoe_epoch_wake_all`, i.e.
  `futex_wake(INT_MAX)`, called from the progress loop's handler),
  `node.h:247-255` (per-team counters are six named fields plus
  `arrival[OSOE_TEAMS]`). Design §5.3, §3.11.
- **As designed / as built:** identical — the flat counter "to start". A grep
  for fanout, sub-lead or two-level finds nothing in `shmem/` or `base/`; the
  layout has no per-subgroup counters, and per-team counters are not an array,
  so both split teams and the intranode tree need a layout version (cheap
  here; layout is at 7).
- **Complexity:** arrival — L locked increments on one line: total work O(L),
  serialized latency O(L) × one cache-line transfer ≈ 256 × ~0.1 µs ≈ 25 µs on
  one socket (*recalled* transfer cost), perhaps 100 µs across two. Release —
  one thread issues a wake of L sleepers across L processes:
  O(L) × ~1–2 µs per `try_to_wake_up` with an IPI (*recalled*) ≈ 0.25–0.5 ms,
  serial, in the waker. Memory O(1) per team per node (four lines). Both meet
  "linear"; neither is logarithmic.
- **Calibration:** §5.3 says the counter "dominates the intranode phase
  entirely" at 64–256. By the arithmetic above the wake-all is an order of
  magnitude larger, and unlike the counter it is charged to the **lead's
  progress thread**, where every one of the node's N−1 connections waits
  behind it — the same objection §5.3 raises against O(members × count)
  copying on that thread. *Measured* (`barrier-scaling` logs, one node, 4 cores,
  oversubscribed, under contention): mean `sync_all` 1.7–5.7 µs at L = 2,
  45–48 µs at 8, 57–64 µs at 16, 95–105 µs at 32 — roughly linear at 2–3 µs per
  added PE; this host cannot separate the two terms.
- **Does the structure permit better?** The release does, with no layout
  change: wake k, and have every woken member wake k more on the same word
  before returning — O(log L) depth, O(1) on the progress thread. Spurious
  wakes of next-epoch waiters are harmless under `wait_eq`. The arrival tree
  needs the layout bump above.
- **Severity:** *clarification needed* — reorder the priority (release first)
  and record that the chained wake needs no redesign. **Confidence:** medium;
  a run on a ≥ 64-core host would replace the recalled constants.

### 5. The lead's wake word is a node-wide hot spot on the data path, and a wake does not say which connection

- **Location:** `progress-host.c:142-149` (`osoe_progress_host_wake_lead`:
  unconditional `fetch_add` on `slots[0].agent_wake` then a `futex_wake`
  syscall), called once per remote put, get and rmw
  (`transport-am.c:267, 403, 444`) and in every back-off loop;
  `progress.c:575-584` (one pass = `receive`, `drain`, `flush` over all N−1
  connections; `flush` does a locked `atomic_exchange` on every connection's
  `ack_requested`, `:467`); `progress-host.c:35, 197` (static `consumed[]`, a
  scan of every team per pass); `state.h` (one `osoe.progress`). Design §3.10
  (optional `blocked` hint), §3.12 ("perhaps four" progress threads "sharded by
  peer node").
- **Complexity:** per remote operation, writer side: one locked RMW on a line
  shared by **all L PEs of the node regardless of destination**, plus one
  syscall taking one futex hash bucket's lock. This is §5.3's ping-pong moved
  onto the data path, and it is wider than the reservation `head`, which is
  at least per node pair. The syscall (~0.3–1 µs, *recalled*) exceeds the rest
  of a small put (reserve + copy + commit, ~0.1 µs) several times over. Lead
  side: a wake identifies no connection, so each wake costs an O(N) pass —
  ~500 × 3 phases at N = 500, ~4,095 at 4,096, on the order of 0.1–0.3 ms per
  pass at the latter (*arithmetic*, ~30–60 ns per connection per phase) even
  when one connection is active. There is **no `L` term and no per-PE queue in
  the pass** — that part of the design holds (*observed in source*).
- **As designed for step 3:** receive and send completions become CQE-driven,
  which removes two of the three scans. §3.10's `submit_sealed_buffers()` /
  `work_pending()` do not say how the lead learns *which* open buffers gained
  bytes; as written that is still a scan of every connection per wake.
- **Structure permits:** a per-node pending bitmap over peer slots
  (N/8 bytes; `fetch_or` by the writer, which knows its connection at every
  call site), a published "lead asleep" word so an awake lead costs no
  syscall, a wake only by the first writer into an empty open buffer or into a
  sealed one, and one wake word per shard. None touches a wire format. The
  singletons (`consumed[]`, `osoe.progress`, "slot 0 is the lead") are what
  sharding will have to unpick; §3.8 already records that the control queue
  becomes multi-producer then.
- **Severity:** *clarification needed* (say how step 3 finds work in
  O(active connections), and build the `blocked` hint). **Confidence:** medium;
  a many-writer small-put microbenchmark on real cores would settle the
  constant.

### 6. Reservation under L concurrent writers: a CAS loop on one line is O(L²) attempts per round in the worst case

- **Location:** `connection.c:33-81`; design §3.5 ("CAS loop, not
  `fetch_add`"), §3.4 ("every local PE contends on the same open buffer … 256
  PEs on a node cost exactly what one PE costs").
- **Scenario:** a bulk-synchronous halo exchange: all 256 PEs of a node put to
  the same neighbour node at the same moment. One CAS succeeds per round and
  the rest reload and retry: L(L+1)/2 ≈ 32,900 attempts for 256 reservations,
  each an exclusive cache-line transfer — about 3 ms for the round at ~0.1 µs
  per transfer (*arithmetic*, *recalled* constant), ~13 µs per reservation
  against ~0.1 µs uncontended. `filled` is a second contended line per
  message (`connection.h:122-127`), but a `fetch_add`, so O(L).
- **Calibration:** memory is exactly as claimed — no PE dimension
  (*observed*: `connection-area.h`, 9 cache lines of shared words per peer).
  The copy between reserve and commit spreads writers out, and on 1 GbE the
  wire (~1 M small messages per second per link) saturates first. It matters
  on faster Ethernet, and it is the design's deliberate price for a
  failure-free reservation and a single ordered stream. It is a trade-off to
  measure, not a defect.
- **Severity:** *clarification needed* — §3.4's "cost exactly what one PE
  costs" is true of memory and should not be read as true of time.
  **Confidence:** low-medium; needs ≥ 32 real cores.

### 7. Teams: a job-wide `max_teams` sizes per-node state by teams in the *job*, and the undesigned parts are the ones that decide whether 1M PEs works

- **Location:** design §5.7, §4.5 ("a team is a private descriptor: member
  list, tree topology, counters"), §5.8 (runs), §10; `node.h:48-52, 255`,
  `progress.h:106-108`, `progress-host.c:35, 197-207`, `team.c`.
- **As built:** two predefined teams; `OSOE_TEAMS` is a compile-time
  enumerator, not §5.7's runtime `max_teams`; per-team state is indexed
  directly by team number in the node header, the progress engine and the
  agent; the agent scans every team's handoff word on every pass — O(T) per
  pass. Team → world translation is O(1) (*observed*, `team.c`), and a strided
  team keeps that.
- **As designed:** `max_teams` "counts teams in existence job-wide at once"
  and bounds the counter pool, the control queue and the range allocator.
  Per-node cost is then O(T_job): 4 cache lines = 256 B per team per node, plus
  an O(T_job) scan per progress pass.
  - `shmem_team_split_2d` of the world at 1M PEs with `xrange` = 1,024 creates
    1,024 + 1,024 = 2,048 teams job-wide; each PE is in two, each node hosts
    at most 2·L = 512. Job-wide bound: 512 KB per node and 2,048 words scanned
    per pass — **workable; the bound does not make 2-D decompositions
    impossible.**
  - Many small teams — P/4 four-PE neighbourhood teams, 262,144 job-wide, at
    most 256 on any node — cost 64 MB per node and a 262 k-word scan per pass
    under a job-wide bound, a thousand times what the node's own teams need.
    That is where it fails.
- **What the undesigned parts must decide to stay within the bar:**
  (i) per-node state keyed by *teams with a member on this node* — the wire
  already carries a 32-bit team id, so a per-node id → slot map is possible;
  (ii) how a job-unique id is agreed by a collective over the *parent* only —
  hierarchical ranges exhaust with split depth, and the alternative (a
  reduction of free-slot bitmaps over the parent) is an O(max_teams)-bit
  collective per split; (iii) membership stored as `(start, stride, size)`,
  never the "member list" §4.5 mentions, and §5.8's runs stored as a rule —
  under Slurm's cyclic distribution a literal run table is O(P) per team per
  PE; (iv) handoff words found by a summary word or bitmap, not a scan;
  (v) per-team internode topology (finding 2).
- **Severity:** *clarification needed* now; it becomes *design change needed*
  the day split teams are scheduled, and (i) and (iv) are cheaper to settle
  before more code indexes arrays by team number. **Confidence:** medium.

### 8. §5.5's "the lead combines every local contribution itself" is a serial O(L·m) bottleneck, and the argument against parallelising it does not hold for a chunk-partitioned combine

- **Location:** design §5.5 ("parallel combines would ping-pong the same cache
  lines anyway"; "At high local member counts the §5.3 tree applies
  unchanged"), §5.8 rule 2 and "the intranode phase is unchanged".
- **Complexity as designed:** work O(L·m), which is optimal, but depth O(L·m)
  on one core while L−1 cores sleep; then L−1 parallel copy-outs, depth O(m).
  At L = 256 with 1 M doubles the lead streams 2 GB through one core — about
  0.2 s at 10 GB/s (*arithmetic*) — before the internode phase starts. The
  two-level sub-lead variant gives depth O((f + L/f)·m), O(√L·m) at best,
  not logarithmic.
- **Counter-proposal that keeps every §5.5 safety argument:** once
  `arrived == (k+1)·L`, member *i* combines element range *i* of every source
  into range *i* of the lead's `dest`. Each range has exactly one writer (no
  lost updates); nothing is read before every member has called (push, not
  pull; the in-place case stays safe because only member *i* touches range
  *i*); the lead still leaves last; every element is combined in the same
  member order, so results are bit-identical to the serial loop. Depth O(m),
  bounded by node memory bandwidth instead of one core's. For small `m` — the
  common case (§8.6 U7) — the serial lead remains right; select on `m`.
  Cache-line ping-pong occurs only at the L−1 range boundaries.
- **Structure permits:** yes — the same three counters, with `arrived` waited
  on by every member (sound by the same bounded-skew argument, since no member
  can arrive for k+2 before release).
- **Internode:** tree for small `m` gives O(log N) latency; ring or
  Rabenseifner above a measured crossover gives bandwidth-optimal large
  reductions (prefer the latter at thousands of nodes: a ring is 2(N−1)
  latency steps). Scans are O(log runs) rounds. All meet the bar **as
  designed; none is built, and all of it presumes the tree that finding 2
  shows is not there.** Alltoall and collect are undesigned; node-pair
  aggregation makes an alltoall N² wire messages rather than P², and a push
  into a symmetric `dest` needs no scratch.
- **Severity:** *clarification needed*. **Confidence:** medium-high on the
  argument; nothing to measure yet.

### 9. Bootstrap and the full mesh: fine at 500, unexamined at 4,096, and an RoCE build inherits the mesh because collectives stay on AM

- **Location:** design §6.2, §3.1, §3.4 (budget), §2.2 / §10 (collectives stay
  on AM); `job.c`, `progress-host.c:222-252`, `connection-area.c`,
  `base/node-map.c`.
- **As designed:** N−1 leads connect to node 0's lead (all-to-one, O(N)
  connections — the right shape, and correctly *not* O(P)); the server
  broadcasts the rank→node map: 4·P bytes to each of N−1 leads, serially, from
  one NIC. 500 × 256: 255 MB, about 2 s on 1 GbE — acceptable. 4,096 × 256:
  16.4 GB — seconds on a 100 Gb fabric, but O(N²·L) bytes through one node for
  information that is O(1) under block or cyclic placement and O(runs) in
  general. Then an **eager** full mesh: N(N−1)/2 connects (124,750 at 500;
  8.4 M at 4,096), and the highest-index node accepts N−1 inbound connections
  at once, which is a listen-backlog overflow and a 1 s SYN retransmit unless
  paced. §6.2's `RLIMIT_NOFILE` note covers 500.
- **As built:** a shared-memory stand-in of the same shape; every PE (not
  only leads) scans all P entries (`job.c:236`) — O(P) per PE at init, fine.
  The lead opens every wire and initialises every connection eagerly.
  `base/node-map.c` has an O(N·P) check and an O(rank) `local_id`, launcher and
  test only. Connection *memory* is already lazy in effect — the area is
  demand-paged and only the 576 B of shared words per peer are touched at init
  (*observed*). Put fast path: `pe → node` one load from the shared map;
  `pe → local id` one load (finding 3); `→ heap base` one load; O(1).
- **At 4,096 nodes:** the default AM configuration refuses to start
  (2 × 4,095 × 128 KiB ≈ 1 GiB against a 256 MiB budget whose floor is the
  default — `connection-area.h`, by design). An RoCE build still needs AM
  between nodes for every collective (§10's closure), so as designed it
  carries 4,095 TCP connections per node to run barriers. With a tree
  (finding 2) a node needs connections only to its tree neighbours plus the
  peers it actually sends AMs to — lazy establishment is worth most exactly in
  the RoCE build.
- **Does anything defeat laziness?** (a) finding 1's echo keeps every quieted
  connection busy for ever; (b) the step-2 pass touches every connection's
  shared words each pass — private state only, harmless to laziness in step 3;
  (c) teardown announces and waits on all N−1 wires
  (`progress-host.c:305-308`) — must skip never-opened ones; (d) the star
  touches only the root's connections; (e) §6.2's "lower index connects to
  higher" rule presumes eager set-up — on-demand needs an always-armed accept
  on every lead and a tie-break for simultaneous opens. Nothing in a wire
  format or in the §4.10 seam prevents it; `osoe_connection_to()` is the one
  place a connection is looked up.
- **Severity:** *clarification needed* (state that the mesh is eager by choice
  and what on-demand would need; compress the map). **Confidence:** medium.

### 10. `fence` re-flushes every peer touched since the last quiet, acknowledged or not

- **Location:** `transport-am.c:506-517` (`flush_dirty`), `:530-536`; design
  §4.6 ("does NOT clear the dirty set"), §10 "fence eagerness".
- **Scenario:** the PRK / Graph500 pattern §10 cites — put to a random PE,
  `fence`, repeat, `quiet` rarely. The set only grows between quiets, so by the
  k-th fence `flush_dirty` sets `ack_requested` on min(k, N−1) connections; the
  lead seals each (a header-only segment where the buffer is empty) and each
  peer answers. Up to 2(N−1) segments per fence for one new put: ~1,000 at
  N = 500. With finding 1 present, each also starts an echo.
- **Fix shape:** skip a slot whose published `acked` already covers
  `high_segment` — one load — or drop it from the set, which is safe because
  those operations *are* acknowledged. Cost returns to the peers with
  something outstanding.
- **Severity:** *minor* (*design change needed* if fence-per-operation codes
  are a target workload). **Confidence:** high on the source reading; not run.

### 11. Smaller items

- `shmem_pe_quiet` allocates and zeroes N−1 bytes per call
  (`transport-am.c:582`) where O(npes + degree) needs no allocation: walk the
  targets, test `touched[slot]`, wait, clear. *Minor*, high.
- The allocator is O(B) per `malloc` and `free` (first-fit scan, `memmove` of
  the block array) plus an O(B) `osoe_allocator_levels` on every call
  (`allocator.c:95, 123, 218`): O(B²) to build a heap of B blocks. No N or L
  term, deterministic, private — sound in kind; a balanced tree makes it
  O(log B) without touching determinism. *Minor*, high.
- The reply-descriptor pool is `peers × C` (`progress-host.c:233`), omitting
  §3.8's `degree × max_teams` term, and `enqueue_collective` aborts on an empty
  pool. Unreachable while the world team is the only one with an internode
  phase (every PE is inside the barrier, so no get is outstanding); reachable
  once a sub-team can barrier while other PEs hold get credits. With a star
  the term is (N−1) × T at the root. *Minor*, medium.
- Per-PE private arrays linear in N: connection handles 24 (N−1) bytes
  (`connection-area.c:256`) and the dirty set 13 (N−1) bytes **per context**
  (`transport-am.c:94-96`). At 4,096 nodes and 64 contexts per PE that is
  3.4 MB per PE, ~870 MB per node. The time cost is O(degree) as claimed; the
  memory is O(N × contexts × L) per node. A sparse set keyed by touched slot
  restores O(degree). *Minor*; an RoCE backend owns this structure anyway.
- A blocking get is one round trip per chunk, serially
  (`transport-am.c:393-416`): O(m / chunk) RTTs — 16 for 1 MB, ~3 ms on Pi
  Ethernet before bandwidth. Documented as "slow mode first". *Accepted.*
- The watchdog is O(L) syscalls per sweep per blocked PE, O(L²) per sweep
  node-wide when the whole node waits in a barrier — about 0.1 % of a core at
  L = 256 (*arithmetic*), development builds only. *Accepted.*

---

## What is sound, with the evidence

- **`notify` is O(1).** `watch.c:83-97`: world rank → local id → that PE's
  slot → one load of `watch_rng`; no scan of the L watch slots. The line is
  written only on watch and unwatch, so L concurrent putters share it
  read-only. A `shmem_p` to a node-mate is a range check, three loads, a
  `memcpy` and one more load.
- **`quiet` is O(communication degree), not O(N).** `transport-am.c:149-158,
  594-613` keeps an insertion-ordered list of touched peer slots beside the
  membership flags; a wait walks only the list, and a quiet with nothing dirty
  returns after one branch (`:563`). §4.6's claim that a halo exchange scans
  six peers at any job size is true of the code.
- **No PE dimension below the boundary.** Connection buffers, shared words,
  wires, reply descriptors and the progress pass are all indexed by peer node
  (*observed*: `connection-area.h`, `progress.h`, `progress.c`). Landing zones
  are O(L) (`node.c:237-240`), epoch counters four cache lines per team per
  node. §1's rule holds everywhere except finding 3.
- **The locality decision really does live in one function** (`job.h`:
  `osoe_node_of`, `osoe_is_local`), so compressing the node map for regular
  layouts later is a change to one inline function and one table.
- **Team-relative → world translation is O(1)** and stays so for strided
  teams.
- **Rendezvous is leads-only**, so its connection count is O(N), not O(P)
  (§6.2; mirrored in `job.c:186`).
- **Barrier message count is optimal** — 2(N−1), one arrival per node not per
  PE (`barrier.c:45-61`) — and the arrival handoff is one word per team with no
  queue. The defect in finding 2 is where the work is serialised, not how much
  of it there is.

## Ranking: how much redesign each defect costs if left until after steps 3 and 4

1. **The star (finding 2) — most expensive to leave.** Reductions, broadcast,
   §3.8's queue bound, the control-queue sizing, the teardown argument and
   §10's RoCE estimate all take the internode topology as given. Written on
   top of a star, each bakes in "the root counts" and "degree = N−1". A day's
   work now; afterwards a rewrite of every collective's internode phase plus
   the models that assume it.
2. **Team identity and per-node team state (finding 7).** Every array indexed
   by a job-wide team number, and every per-pass scan over teams, is a site to
   revisit; the control-segment layout is involved. Undesigned today, so the
   cost is a constraint written into §5.7 now rather than a retrofit.
3. **Work discovery and the lead wake (finding 5).** Step 3 replaces this loop.
   If it arrives without "which connection" in the wake and without a
   sleeping-lead hint, an O(N) scan per wake and a syscall per put become part
   of the io_uring loop's shape, and sharding (§3.12) later has to unpick the
   singletons.
4. **The ack echo (finding 1).** Trivial to fix at any time, but it should not
   reach step 3: on TCP it is a permanent storm, and it will contaminate every
   measurement §10 is waiting for.
5. **Lazy connections and the compressed map (finding 9).** Left late, the
   cost is the connect rule, teardown, liveness, and any code that comes to
   assume every connection exists; the seam itself is not at risk.
6. **`local_of_rank` (3), `fence` pruning (10), `pe_quiet` (11), the chained
   wake (4), the chunked combine (8), allocator complexity (11).** All local,
   all cheap at any time. Finding 3 should be fixed immediately simply because
   it is the largest per-node allocation in the library at the design's own
   target scale.

## Could not determine

- Every absolute time above: there is no multi-core host, no second machine
  and no TCP in this slice. The per-message, per-wake and per-transfer
  constants are recalled typical values and labelled as such.
- Whether step 3's loop is meant to scan connections per wake — §3.10 leaves
  `submit_sealed_buffers()` and `work_pending()` unspecified.
- The constant on the L-way reservation CAS (finding 6); the 4-core VM cannot
  produce the contention.
- The default value of `max_teams`; the design gives none.

## Not investigated

Planned or plausible, and not done for lack of budget:

- `wait.c` (`shmem_wait_until_any/all/some`): the loops over `nelems` at
  `wait.c:233, 264` were seen by grep only; their per-wake cost was not read.
- `wire.c` and `reply.c` were not read beyond signatures; the wire is a step-2
  stand-in, and the reply pool's take/give complexity was assumed O(1).
- `metrics.c`'s counter-block claim (`:199, 270` scan the pool) — cost per
  thread start, not per operation; not analysed.
- `tools/launch/launch.c`'s own complexity in P (test-only).
- The O(L²) sibling-heap mappings per node (§4.1) and their page-table cost:
  noted as inherent and left to `performance-scale`'s byte arithmetic.
- No model or test was written for finding 1's fix; no mutation run was made
  for findings 2–10, which rest on reading and arithmetic.
- The barrier-scaling experiment was run twice only, at ≤ 16 logical nodes and
  ≤ 32 PEs, on 4 shared cores; it supports the intranode shape and nothing
  about the star.

---

## Lessons for a successor API

Places where OpenSHMEM's semantics, rather than this implementation, force an
unscalable or serialising structure:

- **World-collective allocation.** `shmem_malloc` and `shmem_free` are
  collective over *every* PE and imply a barrier (spec §9.3; design §4.5). One
  allocation is a job-wide synchronisation — O(log N) at best — for a purely
  local effect under a deterministic allocator. A successor should allocate
  per team (or "memory space"), and let a deterministic allocator skip the
  barrier by stating that the object is usable once the caller has returned.
- **PE-addressed everything.** Every routine names a target by dense integer
  rank, so every implementation needs a rank → (node, local index) function on
  the fast path, and for arbitrary placements that is an O(P) table somewhere.
  An API whose endpoints are `(node, local index)`, or that guarantees a
  regular placement, makes the map O(1) and removes the per-PE `{base, rkey}`
  temptation §4.1 warns about.
- **Teams are created by a collective over the parent only, but need
  job-unique identity.** That is the entire difficulty in §5.7 and the reason
  `max_teams` became a job-wide count. Either make identity the application's
  (a caller-supplied tag, as MPI sessions do) or make team creation cheap and
  id-free by deriving the id from `(parent, start, stride, size)`.
- **`split_2d` and arbitrary strides put many teams on one node pair** while
  each PE is in two, so any per-connection resource bounded per PE is wrong
  (review C10) and one bounded per job is wasteful (finding 7). A successor
  should let the implementation bound team resources *per node*.
- **Teams × contexts.** A context is a completion domain, and `quiet` must
  know which peers it has outstanding work with — O(N) dense or a sparse set —
  per context. Contexts created per thread per team multiply that
  (finding 11). A completion *handle* returned by a batch of operations is
  O(1), where a long-lived per-context dirty set is not.
- **`barrier_all` = `quiet` + `sync`, and every finalize is a barrier.**
  Conflating completion with synchronisation makes the most-called collective
  pay for both. Keep them separate, as `shmem_team_sync` already does.
- **Reductions to every PE with a symmetric `dest`** are what allow §5.5's
  accumulate-in-place trick, which is good; but "`source` need not be ready
  until the PE calls" rules out pull-based collect and alltoall, so large
  collectives must be push-with-rendezvous. A successor could allow a PE to
  declare a source ready before it calls.
- **Global locks** (`shmem_set_lock`) name a symmetric address and promise
  job-wide mutual exclusion with no notion of locality or owner; every
  implementation becomes a distributed queue rooted at one PE — a single-PE
  hot spot by construction. Not built here yet; a successor should drop them
  or scope them to a team with an explicit home.
- **`shmem_wait_until` on arbitrary symmetric words** forces every write path
  — local put, remote put, every AMO — to check for waiters. osoe keeps that
  O(1) (one watch slot per PE), but only because `SERIALIZED` allows one wait
  at a time; `MULTIPLE` makes it a per-PE bitmap scan (§4.11). Waitable
  objects declared at allocation, as the `SIGNAL_REMOTE` hint half-does, would
  confine the check to memory that can have waiters.
