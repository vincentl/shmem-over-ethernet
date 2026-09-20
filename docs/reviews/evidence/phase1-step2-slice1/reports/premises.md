# Premise audit — phase 1, step 2, slice 1

Agent `premises`. Reviewed: `main` at `7ff9343` (slice merge `d542c63`), all 31
slice commit messages read in full, design doc §1, §2, §3.1–3.2, §3.9, §3.11–3.12
(parts), §4.10, §5.1–5.3, §6.4, §8.1, §10; source named per finding.

Evidence labels: *documented* (design doc, header, commit message, spec),
*observed in source*, *measured* (run by me in a private copy,
`build/scratch/review-premises/tree`, debug preset, VM shared with other agents,
so any timing is "measured under contention"), *recalled*.

Experiment logs: `docs/reviews/evidence/phase1-step2-slice1/premises/`
(`force-remote-node-mate-put.log`, `agent-ready-label-window.log`,
`segment-has-mutation.log`). No tracked file was modified; mutations were made
only in the private copy and reverted there.

Findings established independently by sibling agents are **cited, not
re-derived**, and marked *(sibling)*: (a) the built internode barrier is a flat
star rooted at node 0, and §10's "4–6×" compares against a tree that does not
exist; (b) `OSOE_FORCE_REMOTE=1` with node-mates aborts; (c) the `owe_ack` echo
at `progress.c:283/442`, which also masks a quiet-liveness hazard. I reproduced
(b) myself before learning it was already established; the log is kept.

---

## Summary table

| # | Premise | Verdict |
|---|---|---|
| P1 | `osoe_transport_sync` closed "no" for two concrete obstacles | **No longer holds** as reasons; the decision is defensible for this backend, the *closure* is not |
| P2 | "The differential test is now two node maps"; the flag "denies the pointer without changing the route" | **Holds with a narrower scope** (differential); **no longer holds** (flag behaviour) |
| P3 | Separate connection area keeps "does AM stay hot" open | **Holds with a narrower scope**: it keeps the *size* open; P1's closure settled *existence* |
| P4 | `OSOE_TRANSPORT_MORE` defers the wake, bounded by one sweep | **Holds**; the bound in step 2 is 250 µs, and the test observes the branch, not the withheld wake |
| P5 | `OSOE_SEGMENT_HAS` makes append-only two-directional | **Holds in code (measured); not pinned by any test (measured)** |
| P6 | Two flakes fixed by making a promise real | Teardown: **holds**. Agent-in-table: **holds with a narrower scope** — the race moved, the test still depends on it (measured) |
| P7a | Wire latency dominates memory bandwidth by 100× | **Holds with a narrower scope** (small messages on 1 GbE); flips on 200 GbE; its consequences are below the seam, with one leak |
| P7b | "Cores are over-provisioned" (§1) | **No longer holds**; the doc contradicts itself (§3.11, §3.12) |
| P7c | One ordered stream per node pair; never a second socket | **Holds**, and is isolated below the seam; §3.2's HOL bound is **narrower than stated** |
| P7d | Equality waits are sound because skew is bounded | **Holds** for the two predefined teams; **unestablished** for split teams (rests on an undesigned item) |
| P7e | The allocator is deterministic, so no coordination | **Holds**, but **unfalsifiable at run time**: nothing detects a program that breaks it; one comment overclaims |
| P7f | Every place the SHMEM layer can block is a backend function | **No longer holds** |
| P7g | `shmem_ptr` may return NULL | **Holds** in normal mode; **no longer holds** under force-remote with `SHMEM_TEAM_SHARED` unchanged |
| P8 | Commit messages vs code | Eleven overclaims or stale statements listed; measurement inventory given |

---

## P1. `osoe_transport_sync`: closed, and the answer is no

**Claim** (§10, lines 4291–4316; repeated in §5.3 lines 2932–2936, commit
`1cb1770`, and `shmem/src/progress.c:623–651`): the internode phase does not
cross §4.10 because of (1) *addressing* — "§4.10's model is `(pe, region,
offset)` … while this phase is addressed to a *node*" — and (2) *the release* —
"Expressing the release as an rmw would need the backend to know that this
particular word has futex waiters … which `osoe_transport_notify` does not
mean." Conclusion: "a backend wanting a native group barrier implements §5's
internode phase rather than growing a new primitive."

**What rests on it.** §2.2's placement of every future collective (reduce,
broadcast, collect, alltoall, scans) inside the AM engine; the RoCE tier's
largest recorded loss (`docs/reviews/roce-transport.md` §8.4, lines 607–630:
in-network reduction, multicast, triggered operations all "require the backend
to see `shmem_barrier_all` as a barrier"); P3.

**Check.** Read what actually crosses between `barrier.c` and the engine.

- *Observed in source*: the SHMEM layer's whole interaction with the internode
  phase is one call down and one callback up.
  `barrier.c:61` calls `osoe_progress_host_post_arrival(t->number, k + 1)` — the
  arguments are **(team number, epoch)**. `progress-host.c:99–116` is the
  `release(context, team, epoch)` callback, which does
  `osoe_epoch_bump_eq(released, epoch)` then `osoe_epoch_wake_all(released)`.
  The member then waits at `barrier.c:69` exactly as on one node. *(sibling (a)
  reached the same observation.)*
- Against reason 1: nothing node-addressed crosses. The group is implied by the
  team; the root and participant set are computed *below* the call
  (`progress-host.c:130–138`). And `transport.h` already has operations that are
  not `(pe, region, offset)`: `osoe_transport_set_range`, `osoe_transport_load`,
  `osoe_transport_watch*` and `osoe_transport_wait(context, pes, npes)` are
  self- or set-addressed. "A second addressing model" describes put/get, not
  what the header already contains.
- Against reason 2: the release is already delivered without the backend
  knowing anything about futexes or watches — it makes one upcall and the SHMEM
  side bumps and wakes its own word. The objection is to one *implementation
  strategy* (release-as-rmw-plus-notify), which nobody needs.
- So both reasons are descriptions of how a rejected draft would have been
  written, not reasons the seam cannot carry a group operation. The code that
  was then written *is* a two-function group interface; it is simply declared in
  `progress-host.h` (an AM-layer header) instead of `transport.h`.

**Is "a backend implements §5's internode phase" an extension point or a
sentence?** A sentence. *Observed in source*:

- `barrier.c:10` includes `progress-host.h`; `init.c:130,160` call
  `osoe_progress_host_start/stop`; `agent.c:70–72` calls
  `osoe_progress_host_poll/idle_ns`. `transport.h:5–7` says "The SHMEM layer
  (§4) calls only what is declared here. It never calls the AM layer (§2.1)
  directly" — that is now false in four files. §4.10's headline ("It never
  calls the AM layer") is false for `barrier.c` on the narrow reading that §5 is
  outside §4, and false for `init.c`/`agent.c` on any reading.
- The internode phase itself lives *inside* the AM engine: `OSOE_AM_BARRIER_*`
  kinds in `am.h`, `OSOE_REPLY_ARRIVE/RELEASE` in `reply.h:76–84`,
  `arrivals[]/arrival_epoch[]/participants[]` in `struct osoe_progress`,
  `osoe_progress_arrive` at `progress.c:652–692`, root hard-wired
  (`progress-host.c:138`), the team number travelling in a field named
  `origin_pe` (`progress.c:669,689`). There is no interface a second
  implementation could be bound to; a porter edits `barrier.c` and `init.c`.

**Cost now versus later.** Now: move two declarations into `transport.h`
(`osoe_transport_sync_post(team, epoch)` down; an exported
`osoe_collective_release(team, epoch)` up), move `release()` from
`progress-host.c` to `barrier.c`/`team.c`, and give lifecycle and poll a
transport-named entry — tens of lines, no protocol change, the arrival model
and §5.4's wait untouched. Later: every data-carrying collective written per
§2.2 adds handler kinds, reply kinds and per-team engine state to `progress.c`;
an RoCE build must then either keep TCP + the AM engine alive for all
collectives (what `roce-transport.md` §8.4 called "every barrier over TCP while
the data path runs on the NIC") or re-implement each inside its own engine with
no contract to implement against. The cost grows with each collective written.

**Finding P1-F1 — the closure rests on reasons the code refutes.**
- Location: §10 lines 4291–4316; `progress.c:623–651`; `barrier.c:61`;
  `progress-host.c:99–116,163–179`; `transport.h:5–7`.
- Failure scenario: a RoCE backend with SHARP-style or multicast barrier has
  nowhere to bind it; §4.10's promise ("replaces the implementation of this
  header without touching §4") fails for `barrier.c`, `init.c`, `agent.c`. At
  4,096 nodes the built phase is a flat star *(sibling (a))*: the root enqueues
  4,095 releases serially (`progress.c:668–673`).
- Evidence: *observed in source* (above); *documented* (`roce-transport.md`
  §8.4); flat-star and the 4–6× baseline *(sibling (a))*.
- Severity: **design change needed** (small now; judged at the 1M-PE tier).
- Confidence: high on the source facts; medium on the cost estimate — a
  patch in a private copy that makes `barrier.c` compile against `transport.h`
  alone would raise it.

---

## P2. Force-remote lost its purpose; the differential is two node maps

**Claims.** §8.1 lines 3796–3827 and commit `56dedff`: "running a program 'both
ways and diffing' is `-m 0,0,0,0` against `-m 0,1,2,3`"; the flag "denies the
*pointer* to a node-mate without changing the route … the traffic itself still
takes the intranode path." §8.1 lines 3853–3858: "It becomes a three-way switch
… the moment a second network backend exists, so the differential test extends
to it for free."

**What rests on it.** The only end-to-end evidence that the two paths behind
§4.10 agree (`shmem.differential`), and the plan for validating a third backend.

**Check 1 — does the two-map differential test what the flag was for?**
Largely yes. *Observed in source* (`test-differential.c`,
`cmake/Differential.cmake`, `shmem/tests/CMakeLists.txt:314–322`): six ranks,
four maps, value-only transcript, sorted and compared. Every *PE pair* is
intranode in one run and AM in another, so per-pair divergence is compared
directly, not indirectly. Commit `c8a247c`'s mutation (drop the final partial
chunk) is the right kind of proof. What it does **not** cover:

1. **No run is all-AM with more than one PE per node.** *Computed*: among the
   four maps only `0,1,2,3,4,5` sends everything over AM, and there L = 1. The
   slice's own repeated lesson (`d503dbb`: "Every breakage passes with one PE
   per node"; `2541a33`: 3 of 5 mutations passed at L = 1) applies: the
   routed-flag would have given all-AM at L = 3. With the maps used, 4 of 30
   ordered pairs never cross AM from a multi-PE origin node, 4 never to a
   multi-PE target, 10 never with both ends multi-PE. Mixed maps cover
   contention, but only with part of the traffic.
2. **Only fully synchronized observations are compared.** Every print is
   fenced by `quiet` + `barrier_all` on both sides (the test's own comment,
   lines 119–127). That is necessary for determinism, and it means the diff
   cannot see the class the flag's other half exists for: code that works only
   because the intranode path is *stronger* than the contract (a put applied
   before return). A collective written later that reads after a put without a
   wait is right on map 1 and racy on map 2, and the diff will flake rather than
   fail.
3. `put_signal` is 200 bytes against a 1,024-byte chunk: put and signal always
   share a segment. The `FENCED` ordering across a chunk/segment boundary is not
   in the transcript.
4. Not run under tsan (documented, accepted).

**Check 2 — "without changing the route".** *Measured*, 4 of 4 runs
(`force-remote-node-mate-put.log`): `OSOE_FORCE_REMOTE=1 osoe-launch -n 4 -m
0,0,1,1 -- shmem_test_differential` dies with `osoe: PE 0: no connection toward
node 0, which holds PE 1`; the same command without the flag prints 62 lines and
exits 0. Cause, *observed in source*: `osoe_is_local` (`job.h:95–100`) returns
false for a node-mate under the flag, and `osoe_transport_put/get/rmw`
(`transport-am.c:462,475,491`) dispatch on that same function, so the operation
goes to `remote_put`, which finds no self connection (`transport-am.c:222–224`).
The locality function answers two questions — "may I hand out a pointer" and
"which path" — and the flag was meant to change only the first. *(sibling (b)
established the same abort.)* `shmem/tests/CMakeLists.txt:288–292` still calls
this "an open decision", which contradicts `56dedff`'s "decided with the user";
the only force-remote tests run `shmem_test_multinode`, which performs no RMA
to a node-mate, so nothing in the suite sees it.

**Check 3 — "three-way switch … for free".** No longer holds. With the flag
reduced to a pointer denial, the only routing input is the node map, which has
two states per pair (same node / different node). A third backend needs a new
selector. §10 line 4275–4278 still lists "§8.1's three-way switch" under "Folded
in already".

**Check 4 — "a feature no production run will ever use".** Contradicted by
§4.10 itself (lines 2263–2272): a NIC whose atomics are not CPU-coherent "must
do exactly that" — "on-node atomics go to the NIC by loopback". Same-node
traffic over the network is the production path for rmw on exactly the hardware
goal 4 names. The AM layer need not grow a self connection for it, but the
reasoning used to reject the routed flag is not true of the seam, and
`transport-am.c` dispatches per PE only, with no operation-class dimension
(§10 line 4275 lists "dispatch per `(pe, operation class)`" as folded in; it is
documented, not built).

**Findings.**
- **P2-F1** Flag behaviour contradicts §8.1/`56dedff`/`job.h` comment.
  Location above. Scenario: any program doing RMA to a node-mate under
  `OSOE_FORCE_REMOTE=1` aborts at its first put. Evidence: *measured* 4/4.
  Severity: **clarification needed** (test-only flag, loud failure; but the doc,
  the header and a commit message all state the opposite, and the promised
  "program cannot depend on the shortcut" check cannot be run on any real
  program). Confidence: high. Fix is one predicate: route on `node_of == node`,
  answer `direct` on `osoe_is_local`.
- **P2-F2** Differential coverage gaps 1–3. Severity: **minor** (1, 3),
  **clarification needed** (2, because it bounds what the test can ever show for
  collectives). Confidence: high (1 computed, 2–3 read from source).
- **P2-F3** "Three-way switch for free" and "dispatch per (pe, operation class)
  folded in" are stale. Severity: **clarification needed**. Confidence: high.

---

## P3. The connection area is separate, which keeps §10's question open

**Claim** (commit `445b31c`; what-changed note): "Folding them into the control
segment would settle that open question by accident, in the direction of
'always allocated' … Separate, a build that does not keep the AM path hot never
creates it."

**What rests on it.** §10's open item "Does the AM data path stay hot in an
RDMA build?" (lines 4317–4328).

**Check.** *Observed in source*: `osoe_progress_host_start`
(`progress-host.c:212–255`) opens the area, all N−1 wires, the descriptor pool
and the engine whenever `osoe.internode`, unconditionally. Everything below
depends on that path existing: the barrier's arrival and release
(`progress.c:652–692`, control queue → connection buffer); therefore
`shmem_malloc/free/realloc` (barrier on exit/entry, `malloc.c:96,174,207,246`)
and `shmem_finalize`; acknowledgements and hence `quiet`; teardown
(`flush_before_close`, wire shutdown handshake). P1's closure ("§2.2's placement
of collectives on AM stands") therefore settles the *existence* half of the
question in the direction "always created, always polled" — the same direction
the separation was meant not to pre-empt. §10's own text anticipated it ("what
gets exercised is bootstrap and control"), but the two §10 items are not
cross-referenced and `445b31c`'s "never creates it" is no longer achievable
without reopening P1.

What separation still buys: the *size* is a free parameter. Arithmetic from
`connection-area.c:67–93`: area = 2 × (nodes−1) × buffer. Defaults (64 KiB
chunk, 128 KiB buffer = floor, 256 MiB budget) give 128 MiB/node at 500 nodes;
at **more than 1,025 nodes the default configuration is fatal at init**
(`OSOE_AM_SIZE_NO_ROOM`, since the buffer already sits on the floor) — loud,
with a message naming both knobs. At 4,096 nodes a control-only AM path fits
with `OSOE_AM_CHUNK` ≤ 16 KiB (4,095 × 2 × 32 KiB = 255.9 MiB) and is ~16 MiB at
a 1 KiB chunk, demand-paged. Also inherited at that tier: N−1 wires per lead
opened eagerly (`progress-host.c:244–246`), i.e. a full mesh of ~8.4 M
connections job-wide at 4,096 nodes, for a path that in an RDMA build carries
barriers.

**Verdict.** Holds with a narrower scope. **Finding P3-F1**: severity
**clarification needed** — record in §10 that closing `transport_sync` answered
half of "does AM stay hot", and what the remaining half is. Confidence: high.

---

## P4. `OSOE_TRANSPORT_MORE` defers the wake rather than the seal

**Claim** (§4.10 lines 2323–2369; `transport.h:125–166`; commit `02279fa`).

**Check.** *Observed in source*:
- `transport-am.c:249–267`: bytes are copied and committed; with the flag the
  function counts `am.wakes_deferred` and returns without
  `osoe_progress_host_wake_lead()`. Nothing else differs. ✔
- The seal is independent of the wake: `flush()` seals the open buffer on any
  pass where `used > header` (`progress.c:512–524`). ✔
- The bound: the lead's agent sleeps `osoe_progress_host_idle_ns()` =
  **250 µs** in a multi-node job (`progress-host.c:28,330–333`), not
  `OSOE_SWEEP_NS`. So in step 2 the deferred wake costs ≤ 250 µs + scheduling;
  "at most one safety-sweep interval" is a statement about step 3's blocking
  loop, which does not exist. It also means that in step 2 the wake itself is
  close to unobservable either way.
- `signal.c:93–98`: the only caller; the signal rmw that follows wakes the lead,
  so the saving is exactly one futex syscall per `put_signal`.
- The test (`test-internode.c:346–348`) asserts the counter is > 0 when the
  partner is remote and 0 otherwise. The counter is incremented *before* the
  early return, so a mutation that counts and then wakes anyway passes. The
  assertion proves the branch is reached, not that the wake is withheld.
  (*Reasoned from source; not run.*)
- Purpose served? For an RDMA backend, put + signal under one doorbell is a
  real, if small, use. The pattern the flag exists for — runs of small
  application puts ("`shmem_p` is the hottest pattern in the corpus", §10) — can
  never set it, because OpenSHMEM below 1.6 sessions gives the library no way to
  know more follows. The session routines are not in the tree.
- Adjacent observation: `osoe_progress_host_wake_lead`
  (`progress-host.c:142–149`) is an unconditional locked add plus a
  `futex_wake` **syscall on every internode put**, with no "lead is asleep"
  test. On 1 GbE that is noise; it is the first thing P7a's premise protects.

**Verdict.** Holds. **Finding P4-F1**: severity **minor** — the doc's bound
names the wrong interval for the built code, and the test cannot see a broken
deferral. Confidence: high (source), medium on the mutation claim until run.

---

## P5. Layout 7 and `OSOE_SEGMENT_HAS`

**Claim** (commit `951747a`; `tools/stat/segment.h:79–92`): asking per field
makes §3.14's append-only rule hold in both directions.

**Check** (*measured*, `segment-has-mutation.log`):
1. Reverted `segment.c:126` in the private copy to the pre-fix whole-structure
   length check. `stat.segment`, `stat.absent`, `stat.help`, all `shmem.stat.*`
   and `shmem.landing` **pass** (9/9). The defect the commit fixed is invisible
   to the suite: `test-segment.c:154` always writes
   `directory_bytes = sizeof d`, and every tier-2 test reads a segment written
   by the same build.
2. Changed the test (private copy only) to write an *older* directory
   (`directory_bytes = offsetof(…, landing_offset)` = 112): the fixed reader
   passes; the pre-fix reader fails at `test-segment.c:206` (`table_usable`) and
   every read after it. So one constant pins the rule.
3. A *newer* directory (`directory_bytes = sizeof d + 64`): fixed reader passes.

Other direction checks, *observed in source*: `segment.c:98` refuses a file
smaller than 136 bytes — harmless, any real segment is larger;
`metric_descriptor_bytes != 64` makes the table unusable, which is the
documented freeze, not an append rule; `stat.c` reads no landing field, and
`test-landing.c:46–47` asks `OSOE_SEGMENT_HAS` first.

**Verdict.** Genuinely two-directional in code; **not pinned by any test**.
**Finding P5-F1**: location `tools/stat/test-segment.c:154`. Scenario: the next
append re-introduces a whole-structure check and `osoe-stat` silently shows no
table for every older job. Severity: **minor**. Confidence: high (measured).

---

## P6. The three flake fixes

**Teardown — "a node must not stop its engine while it owes a peer bytes".**
The library change is in `cc0b0bd` (`flush_before_close`,
`osoe_progress_idle`); `f47bed7` changed only the two tests to use the same
predicate. *Observed in source*: the predicate (`progress.c:586–608`) covers
in-flight, sealed, both reply queues and committed bytes in the open buffer; the
argument that no `quiet` can be waiting on a forgotten ack holds, because a
non-root node is released only after every PE in the job has passed the quiet
that opens `barrier_all` (`barrier.c:97–98`). The promise is stated in §6.4
(lines 3514–3525) and `progress.h:139–152`. It is a real defect really fixed,
not a moved race. Two caveats: its only test is probabilistic (commit: "removing
the drain again reproduces it, 4 failures in 12"; not re-run, by instruction);
and the ack side interacts with *(sibling (c))*'s `owe_ack` echo. **Verdict:
holds.**

**Agent in the metric table before `shmem_init` returns.** Claim (`f47bed7`,
`agent.c:18–31`): "'after shmem_init, every thread this PE started is in the
table' is true rather than usually true."

*Observed in source*: `agent_main` publishes `agent_ready` at `agent.c:47`,
makes a `futex_wake` **syscall** at `:48`, and first touches `agent.loops` at
`:52`. `osoe-stat -T` labels a block "agent" iff `agent.loops != 0`
(`tools/stat/stat.c:393,405`), and `test-stat.c:95` asserts the string "agent".
So the delivered promise is "the block is claimed"; the asserted one is "the
block is labelled", and between them sits a syscall — a natural preemption
point on a loaded machine.

*Measured* (`agent-ready-label-window.log`): with a 3 s sleep inserted after
`agent.c:48` in the private copy, `shmem.stat.L1` fails **3 of 3** at
`test-stat.c:95`. The race was narrowed (from thread start-up to one syscall),
not removed; the commit's "0 in 120" says the window is now rare, not closed.
During the window a reader sees the agent listed as `application` with no
liveness signal — the early-hang case the commit says it fixed. The promise
appears nowhere in the design doc (*grep*: §3.14 has no such sentence).

**Finding P6-F1**: location `agent.c:44–52`, `stat.c:393–405`,
`test-stat.c:95`. Fix: bump `agent.loops` (or publish an explicit kind) *before*
`agent_ready`. Severity: **minor**. Confidence: high (measured, deterministic
with the delay).

**Wire-pair deadline** (`c55e4a5`): test-only, reasoning sound on reading; the
general lesson (a `CHECK` that counts but does not stop lets the failing node
corrupt its peer's stream) is worth a sweep of other two-process tests. Not
investigated further.

---

## P7. Load-bearing premises never revisited

### P7a. "Wire latency dominates memory bandwidth by two orders of magnitude" (§10 lines 4252–4256)

Rests on it: copy counts (put: `src` → connection buffer → wire/socket → target,
`transport-am.c:240–242`, `progress.c:473–476`; get adds landing zone → `dst`),
one futex syscall per internode put (P4), 20 µs `nanosleep` back-offs in
`reserve_waiting`, `take_credit` and `osoe_transport_wait`, the 64 KiB chunk.

Arithmetic (link rates exact; host figures *recalled*, unverified):
- 1 GbE: a 64 KiB chunk occupies the wire 524 µs. A Pi 5 copies 64 KiB in
  roughly 15 µs; three copies ≈ 45 µs → **~12×**, not 100×, for bulk. For an
  8-byte operation (RTT ~100–200 µs against a ~1–2 µs syscall) 100× is about
  right.
- 200 GbE: 64 KiB is 2.6 µs on the wire against ≥ 6 µs per copy. The premise
  inverts.

Isolation: every consequence listed is in `transport-am.c`, `progress.c`,
`connection*.c` — below §4.10 — so an RoCE build does not inherit them. One
leak: the landing zones live in the control segment "because every backend
needs them" (`951747a`) but each slot is sized to **one AM chunk** from
`OSOE_AM_CHUNK`; an AM tuning parameter sizes a structure documented as
backend-independent. **Verdict: holds with a narrower scope; minor.** Still
unmeasured, as §10 says.

### P7b. "Cores are over-provisioned" (§1 line 25)

*Documented* contradiction: §3.11 line 795 "Under oversubscription — the Pi's
normal case"; §3.12 lines 894–897 "On a Pi with one core per task this is the
oversubscription case". A Pi 5 running 4 PEs has 4 application threads, 4 agent
threads, and in step 2 the lead's agent waking 4,000 times a second
(`progress-host.c:28`). The decisions actually taken (no spin before block, no
RT priority, every wait a futex) are the ones oversubscription wants, so the
code follows §3.11, not §1. What rests on §1's version: "remote memory *is* the
progress thread" (§1 Framing) — every internode get needs the lead's agent
scheduled on two nodes, each competing with a compute-bound application thread.
**Verdict: no longer holds as written; clarification needed** — strike or
qualify §1, and make agent scheduling latency on a full node a named §8.4
measurement. Confidence: high on the contradiction; the performance consequence
is unmeasured.

### P7c. "A single ordered stream per node pair … destroyed by any second data socket. Do not add one." (§3.1)

Isolation check: the seam does not inherit it. §4.10 states the weak ordering
contract with `OSOE_TRANSPORT_FENCED`, and nothing above `transport.h` relies on
stream order (`signal.c:98` uses the flag; the dirty set and segment sequence
are private to `transport-am.c`). Multi-rail / multi-QP RoCE is therefore not
blocked. **Holds.**

§3.2's bound ("roughly two segments"), computed for 1 GbE: a segment is at most
one buffer = 128 KiB = 1.05 ms, so ~2.1 ms plus §3.6 for a barrier message
behind bulk traffic. That bounds *user-space* queueing only. Step 3 puts a TCP
socket under it: in the VM `net.ipv4.tcp_wmem = 4096 16384 4194304` and
`tcp_notsent_lowat` is unlimited (*measured*, `sysctl`), so an autotuned send
buffer can hold up to 4 MiB = **33.5 ms at 1 GbE** — the very figure §3.2 opens
with as the problem. The design mentions small `SO_SNDBUF` only as a *test*
mitigation (§8.2 line 3901). **Finding P7c-F1**: §3.2/§3.3 need a production
rule (`SO_SNDBUF` or `TCP_NOTSENT_LOWAT` sized to about one segment) or the
bound is off by ~16×. Severity: **clarification needed** (step 3 is unbuilt).
Confidence: medium — would be high after a two-socket measurement in step 3.

### P7d. "Equality waits are sound because skew is bounded" (§5.4; `base/epoch.h`)

*Observed in source*: counters are per team (`barrier.c:35–39`), the epoch is
per PE per team, the handoff word is per team (`node.h`, layout 7), and the
one-word argument is model-checked per team. A PE in several overlapping teams
has independent counters, and blocking collectives on one team are totally
ordered among its members, so the argument carries over. Under thread-multiple
the spec forbids concurrent collectives on one team (*recalled*, unverified);
different teams from different threads touch disjoint words. The stated breaker
— nonblocking collectives — is documented at the type.

What is *not* established: the engine indexes `arrivals[team]` by a **team
number carried on the wire** (`progress.c:655–664`), with `OSOE_TEAMS = 2`, root
fixed at node 0 and `participants()` fatal for anything but world
(`progress-host.c:130–138`). Split teams need job-wide agreement on team
numbers among members while non-members allocate nothing — §10's "team range
allocator and split protocol. Undesigned." The equality argument is sound; what
it will be indexed by does not exist. At the 1M-PE tier the per-team node-header
array and per-team engine arrays are O(max_teams) per node, which is fine.
**Verdict: holds for the built teams; unestablished beyond; deferral is safe
but should be listed as a dependency of §5.4.**

### P7e. "The allocator is deterministic so no coordination is needed" (§4.4, invariant 6)

*Observed in source*: failure is symmetric by construction — "no block fits"
returns NULL on every PE and still takes the exit barrier (`malloc.c:87–96`);
private-metadata exhaustion, the one asymmetric failure, is fatal
(`malloc.c:91–93`). Hinted blocks grow from the other end (`allocator.c:92`),
`realloc` is a function of identical state. `set_range` inherits the property.
**Holds.**

Two qualifications:
- **Unfalsifiable at run time.** Nothing detects a program that calls
  `shmem_malloc` with different sizes or hints on different PEs; heaps diverge
  silently and the first symptom is corrupted data. The exit barrier already
  exchanges one message per node; a debug-mode digest of (call count, offset)
  carried in the arrival would convert it to a loud failure, in the project's
  own style. Severity: **minor**.
- `malloc.c:151–160` (`shmem_align`): "every PE reaches it identically because B
  is the same number on all of them". B is chosen **per node** by each lead
  (`node.c:412–428`, candidates at `node.c:39–44`); nothing in `job.c`/`job.h`
  mentions a heap base (*grep*), so nodes may hold different B. All four
  candidates are multiples of 2^36, so PEs can disagree only for an alignment
  above 64 GiB. Harmless in practice; the comment is wrong job-wide. Severity:
  **minor**. Confidence: medium (I did not read `job.c` end to end).

### P7f. "Every place the SHMEM layer can block is already a backend function … so a backend pumps its own progress there" (§10 line 4239)

*Observed in source*: `barrier.c:69` blocks in `osoe_epoch_wait_eq`
(`base/epoch.h`), a futex wait on a node-segment word, outside `transport.h`.
Every `barrier_all`, `sync`, `team_sync`, allocation, free and finalize blocks
there. On this backend that is fine because an agent thread exists to deliver
the release. But the §10 row is recorded as "a real advantage of the current
shape", and it is no longer true: a backend that makes progress only inside its
own blocking calls (polling a completion queue from the waiting thread — the
usual lowest-latency choice on the hardware goal 4 names) can never deliver a
release to a PE parked on a futex it does not own. The seam now *requires* an
asynchronous progress thread per node for collectives. **Verdict: no longer
holds.** Severity: **design change needed**, and it is the same change as P1:
if the wait is reached through a transport function, the row becomes true
again. Confidence: high.

### P7g. "`shmem_ptr` may return NULL" (§4.8)

Spec §9.1.9 *documented*: "Otherwise, a null pointer is returned" — returning
NULL across nodes conforms. Corpus (*grep*, read-only): real use is thin and
node-local — bale's convey `putport.c:308–313` needs non-NULL and has a
configure-time fallback (`shmemptr.c`); hclib UTS only under `VERBOSE`;
examples print; the rest are `nvshmem_ptr`-family device code, and PyTorch's
comment says "If a peer is over network, `nvshmem_ptr` returns null". osoe's
normal mode returns a pointer for node-mates, which serves all of them.
**Holds.**

Exception: commit `de540da` and §8.1 lines 3845–3851 say a node-sized
`SHMEM_TEAM_SHARED` whose members answer NULL "is legal" because returning NULL
unconditionally conforms. Spec §6 (Library Constants, `SHMEM_TEAM_SHARED`)
*documented*: "`shmem_ptr` and `shmem_team_ptr` **must return a non-null
pointer** to the local PE for all symmetric heap objects on all target PEs in
the team." Unconditional NULL conforms only with SHARED = {self}. Under the
flag osoe is non-conforming on that constant. It is a test flag, so severity
**minor**, but a program that trusts SHARED and dereferences will crash exactly
when run under the flag meant to test it. Confidence: high.

---

## P8. Commit messages and doc sentences that claim more than the code delivers

| # | Where | Claim | What the code does | Basis |
|---|---|---|---|---|
| 1 | `56dedff`; §8.1 l.3819–3821; `job.h:79–84` | flag denies the pointer "without changing the route" | node-mate RMA aborts: "no connection toward node" | measured 4/4; sibling (b) |
| 2 | `shmem/tests/CMakeLists.txt:288–292` | force-remote data path is "an open decision" | contradicts #1's "decided with the user"; nothing tests either | observed |
| 3 | `de540da`; §8.1 l.3845–3851 | node-sized SHARED answering NULL "is legal" | spec §6 says members "must return a non-null pointer" | documented |
| 4 | `f47bed7`; `agent.c:26–30` | every thread is in the table "rather than usually true" | block is claimed; label and liveness counter still race across a syscall | measured 3/3 with delay |
| 5 | `445b31c` | "a build that does not keep the AM path hot never creates it" | area, wires, engine created whenever internode; barrier, malloc, finalize depend on it | observed |
| 6 | `1cb1770`; `transport.h:5–7`; §4.10 headline | SHMEM layer calls only `transport.h`; never the AM layer | `barrier.c:61`, `init.c:130,160`, `agent.c:70–72` | observed |
| 7 | `1cb1770`; §10 | two "concrete obstacles" | what crosses is `(team, epoch)` down and one release upcall | observed; sibling (a) |
| 8 | §10 l.4275–4278 | "Folded in already: dispatch per (pe, operation class) … §8.1's three-way switch" | dispatch is per PE only; the flag no longer routes | observed |
| 9 | `951747a` | per-field check is "what 'appended, never reordered' actually promises" | true, and untested: reverting it passes 9/9 | measured |
| 10 | `02279fa` | "`am.wakes_deferred` counts them and the test asserts it" | asserts the branch is reached; count-then-wake passes | reasoned, not run |
| 11 | `malloc.c:156–159` | B "is the same number on all of them" | per node; no job-wide agreement | observed |
| 12 | `445b31c` / §4.10 | MORE costs "at most one sweep interval" | 250 µs in the built code; the sweep bound belongs to unbuilt step 3 | observed |
| 13 | §10 l.4239 | "every place the SHMEM layer can block is a backend function" | `barrier.c:69` | observed |

**Measurements recorded only in commit messages or the what-changed note.**

Cheap to re-derive (deterministic, minutes each in a private copy): the
"broken on purpose" catches in `80bb3d0` (2), `aa4fc27` (3), `3f82b2f` (3 of
4), `f761fae` (M4 + 4 malformed), `951747a` (3), `ed4fbc6` (4), `92e8cda`/
`cc0b0bd` (`members` vs `local_members`; arriver bumping `released`; root
releasing on any arrival), `de540da` (self case), `56dedff` (unchunked get;
credit never returned at `OSOE_AM_CREDITS=2`), `c8a247c` (dropped final chunk);
all model verdicts (`d503dbb`, `2b8c504`: 17 cases) — seconds; sizing
arithmetic in `445b31c` and `eafbb6b` (I confirm 64 nodes × 63 × 2 × 128 KiB =
1.06 GB and 250,000 × 48 B = 12 MB; for the 8 KiB-chunk case I get 132 MB =
126 MiB for buffers alone against the commit's "129 MB" — not reconciled;
`shmem.amsize` pins whatever the code computes).

Not cheap (rates; need repetition and, for most, load — left to the load
agent): `3f82b2f` consumed-early 4/8; `99b66cd` seal-bit drop asan 2/2, debug
1/3; `eafbb6b` racy credit 0/6 at C = 3 vs 6/6 at C = 1; `1cb1770` early
release 3/3 at three nodes vs 0/3 at two, and the 1-in-20 `shmem.progress`
flake; `cc0b0bd` teardown 3/12, 4/12 on revert, 0/25 + 0/16 tsan after, and the
arrival-table false failure 2/8 tsan; `a1a5a10` 1 in 30; `f47bed7` 2/60 → 0/120
and 1/30 → 0/30 full-suite + 0/120; `c55e4a5` 2/80 → 0/120 at 12× load; the
what-changed note's "30 times clean under 8x load".

---

## What is sound

- The **MORE** design: the source-bytes rule is kept by construction, the seal
  is independent of the wake, and a missing wake is bounded by the lead's own
  pass (P4).
- The **teardown rule**: a real defect, a predicate that matches its
  justification, stated in §6.4 and the header (P6).
- **Append-only directory**: correct in both directions when exercised (P5).
- **Ordering is isolated below the seam**: the "one stream" simplification is an
  AM-internal convenience and the header states the weak contract (P7c).
- **Allocator failure symmetry** is handled deliberately, including the one
  asymmetric case being fatal (P7e).
- **The differential test** compares each PE pair across both paths and its
  proof mutation is the right one (P2).
- The commit messages are unusually honest about their own tests' defects; most
  overclaims in P8 are doc sentences left behind by a later decision, not
  inflated results.

## Not investigated

- No load experiments (by instruction); none of the failure *rates* in P8 were
  re-measured.
- The cost estimate in P1 was not proven by a patch.
- P4's count-then-wake mutation was not run.
- `job.c` was not read end to end (P7e heap-base claim rests on a grep).
- The 129 MB figure in `445b31c` was not reconciled.
- §4.6/§4.7 accepted trade-offs, §3.5/§3.8 protocols, `pe_quiet`, the node-map
  scale at 1M PEs, rendezvous scale: other agents' areas.
- Spec text on hints symmetry (§9.3.5) and on concurrent collectives under
  `THREAD_MULTIPLE` was recalled, not looked up.
- First-parent step-1 commit messages were not read; the audit covers the 31
  slice commits.
- Web access was not used; all host-performance figures in P7a are *recalled*.

---

## Lessons for a successor API

1. **Collectives should be nameable to the transport.** OpenSHMEM's collectives
   are library routines over teams; nothing in the API forces them above a
   transport seam, but nothing offers a group handle below it either. A
   successor should define a *group* (set of endpoints, with an epoch) as a
   first-class transport object so barrier/reduce can bind to in-network
   implementations without a second, accidental seam (P1, P7f).
2. **Allocation as an implicit barrier is expensive and couples memory to
   collectives.** `shmem_malloc/free/realloc` being world-collective with
   barrier semantics makes every allocation depend on the whole internode
   collective path (P3), and its "same arguments everywhere" rule is
   uncheckable without extra traffic (P7e). Prefer explicit, team-scoped
   region creation that returns a handle, with agreement checked once.
3. **Batching must be expressible by the caller.** The library could set its
   batching hint honestly in exactly one routine (P4). A successor should carry
   "more follows"/flush in the operation or context API from the start (what
   1.6 sessions retrofit), since doorbell batching is where small-message rate
   is won.
4. **`shmem_ptr` and `SHMEM_TEAM_SHARED` entangle locality with addressing.**
   One query answers both "which path" and "may I dereference", and SHARED is
   defined in terms of `shmem_ptr`'s answer. That is what made a test flag
   unable to vary one without the other (P2, P7g). Separate "memory domain"
   (a team property) from "pointer grant" (a per-object capability).
5. **Blocking-only completion forces a progress thread.** `quiet`/`barrier` as
   the only completion points, with no way for the caller to lend its thread to
   progress explicitly, pushes every implementation toward hidden threads — a
   poor fit for oversubscribed small nodes (P7b) and for polling NICs (P7f). An
   explicit, optional `progress()`/completion-object model would let both ends
   of the scale choose.
6. **"Undefined behaviour" on asymmetric arguments should be "detectable in
   debug mode" by specification**, so an implementation is licensed to spend a
   few bytes per collective on agreement checks (P7e).
