# Adversarial review, round two: synthesis

Review of osoe at merge `d542c63` (phase 1, step 2, slice 1), run 2026-09-20
against `phase1-step2-slice1-review-prompt.md`. Eleven investigations; their
deliverables and reports are listed under Sources.

**How this round fell short of its prompt — read first.** The review ran under a
usage limit that stopped every investigator mid-work; each was resumed and told to
write up what it had. Consequences: (1) the **CPU-load flake survey was run
last and narrowly** — one preset, one load level (result under "What is sound");
(2) the **refutation pass by fresh agents was not run** — instead, a finding rated
*design change needed* or worse is kept here only if two or more agents found it
independently, or the lead re-read the cited lines, or both, and each says which;
(3) several planned models and mutations are listed as owed, not done. Every
timing anywhere in this review is **measured under contention** on a shared 4-core
VM over a shared-memory wire, and is order-of-magnitude only.

The work is already merged, so the verdict is about what comes next.

The author added goals beyond the prompt's six on the day of the review —
intranode and internode algorithms linear or logarithmic in time and memory, a
RoCE path to ~1M PEs that no interface may block, and a successor API to follow.
They are scored alongside. Two scale tiers were agreed: the Ethernet
implementation at 500 × 256; the seam and everything an RoCE build inherits at
4,096 × 256.

## Verdict

**Proceed to step 3 after specific changes; do not rethink.** The foundation is
better than a reviewer has a right to expect of code and tests written by one
process: the atomics discipline is kept to the letter, the wait/wake protocols are
sound, the reservation protocol passes a model written from the C, ten recorded
model runs reproduce exactly, the launcher cannot pass a failing job, and the tree
passes 131/131 on a 16 KiB-page, 47-bit-VA kernel like the Pi 5's and under
clang. The data-path half of §4.10 is the right seam for RoCE and blocks no RDMA
feature anyone tried against it.

But three things must change before a socket is put under this. **The
acknowledgement protocol is wrong twice** and the defects hide each other (B1).
**The internode failure paths are silent or slow** — a dropped get request, waits
that never sweep, a 60 s bootstrap timer on every `quiet` (D1–D3) — and on a real
network those paths stop being hypothetical. **The receive side trusts lengths
from the wire** (D4). Separately, and more important for where the author wants
to go: **the decision that closed `osoe_transport_sync` rests on reasons the code
contradicts**, and what it leaves in place is a flat star at node 0, not the tree
the design assumes (D5). That is cheap to fix now and gets dearer with every
collective written on top of it.

## The goals, scored

**The prompt's six.**

| Goal | Score | Strongest evidence |
|---|---|---|
| 1. An OpenSHMEM library in C, widely portable | **Good for what exists; the subset is narrow and unstated.** | 511 of 526 routines do real work, but 467 are the type cross product of two families; only 7 of the spec's 53 examples run as written (no symmetric statics, no collectives beyond barrier/sync). No abort lands on spec-defined behaviour. 131/131 on 16 KiB pages, clang 19, headers clean as C99/C11/C++17. Under Slurm it cannot start yet: launch is refused, and the rendezvous is a shm object (portability F5). |
| 2. RoCE by porting a lower part | **Data path: yes. Collectives, lifecycle, bootstrap: not behind a seam.** | Every `transport.h` function maps to verbs or to a stated emulation; no capability bit was needed even against mlx5's two atomic modes. But `barrier.c`, `init.c`, `agent.c` call `progress-host.h` directly; "a backend implements §5's internode phase" is a sentence with no interface. |
| 3. Correctness at every level | **Sound at the bottom, two real defects in the middle, thin at the top.** | See `…-correctness.md`. |
| 4. Reasonable performance on Ethernet | **Unknowable until step 3; nothing alarming, one thing good.** | Intranode put is at the floor (13–17 ns over a raw store, no syscalls). Aggregation works: ~1.2 µs per small put batched vs ~320 µs with quiet-per-put. Internode medians are the 250 µs poll, so no floor should be pinned on step-2 numbers. |
| 5a. Conformance | Good; 11 findings, none blocking. | `…-conformance.md`. |
| 5b. Diagnosability | **Excellent above §4.10, blind below it.** | `osoe-stat` alone named the culprit PE and node for barrier and `wait_until` hangs. For internode quiet/get/AMO hangs it showed `blocked in: -`, `sweeps 0`, and no AM-layer metric exists (89 metrics, none per connection). |
| 5c. Scale | **AM layer meets its claim; two private arrays and the star do not.** | Below. |
| 6. Deferrals | **Nothing structurally foreclosed; four are costlier than stated.** | Collectives (barrier-shaped star, no payload), nonblocking gets (one landing slot and token per PE), teams beyond two (named `world_*`/`shared_*` fields, identity comparisons), sockets (three 20 µs polls have no wake word). |

**The author's five.**

| Goal | Score | Why |
|---|---|---|
| Good Ethernet performance | Open | As goal 4. The two risks visible now are B1's echo storm and the head-of-line bound ignoring the kernel's TCP send buffer (2.1 ms designed vs ~33 ms with a 4 MiB autotuned `SO_SNDBUF` at 1 GbE, *arith*). |
| Intranode ≤ linear, ideally log | **Linear today, with serial bottlenecks; structure permits better.** | `notify` is O(1); quiet is O(communication degree). But arrival is L-way `fetch_add` on one line, release is one O(L) `futex_wake` on the lead's progress thread, reservation is L-way CAS on one `head`, and §5.5's lead-combines-everything is O(L·m) serial. No trace of §5.3's two-level tree in code or layout — though nothing in the layout prevents it. |
| Internode ≤ linear, ideally log | **Linear with a single hot node; the design assumes log.** | Barrier is a star at node 0: 2(N−1) messages (optimal) but O(N) serial work in one thread — ~2.5–7 ms at 500 nodes, ~20–25 ms at 4,096 on TCP (*arith*, per-message costs *recalled*) vs 0.3–1 ms for a tree. The wire format and handlers already permit a tree; per-team topology is missing. Progress passes visit every connection; every wake is an O(N) pass. Full mesh opened eagerly. |
| RoCE at ~1M PEs, nothing blocked | **Data path unblocked; three things inherited that fail the bar.** | (i) `local_of_rank` and `gtids`: O(P) private per PE → ~2 GiB per node at 4,096 × 256. (ii) Collectives pinned to AM-over-TCP star beside an RDMA data path. (iii) Default AM sizing refuses to start above ~1,025 nodes — loud, by design, but an RoCE build inherits the mesh because collectives stay on AM. Also: XRC implies ~2.1M QPs per NIC at that tier, so DC is the only option (below the seam, fine). |
| Successor API | Well fed | Every deliverable ends with lessons; collected in `docs/design/successor-api-stage-setting.md`. |

**Least well served:** of the prompt's goals, **2 as it concerns collectives**,
and of the author's, **internode scaling** — the same defect seen from two sides.

## Findings

Fields are abbreviated here; location, scenario, evidence and confidence are in
the named deliverable. "×N" = found independently by N agents. "LV" =
lead-verified in source.

### Blocking (before step 3)

- **B1. The acknowledgement protocol: every ack requests an ack, and the ack
  request can attach to the wrong segment; the first defect masks the second.**
  `progress.c:283,442,467,492-523`. After the first quiet a node pair exchanges
  ~2,500 header-only segments/s per direction forever; with the obvious fix,
  `quiet` hangs (4 of 5 under timing injection). Fix together: the request
  carries "acknowledge through S". Model it first. ×2, LV. *Correctness C1, C2.*

### Design change needed

- **D1. A get/AMO request finding the reply pool empty is silently dropped.**
  `progress.c:61-72`; pool `peers × C` lacks §3.8's teams term. ×3, LV;
  reproduced 3/3 at `OSOE_AM_CREDITS=2`. Rated blocking by one agent;
  reachability at defaults is disputed. *Correctness C3.*
- **D2. AM blocking loops never sweep, publish no blocked record.**
  `transport-am.c`, four loops, vs `transport.h:48-55`. ×2 (one measured), LV.
  *Correctness C4.*
- **D3. Data-path waits use the 60 s bootstrap timeout and abort.** A legitimate
  large quiet on 1 GbE exceeds it. ×3. *Correctness C5.*
- **D4. The receive side trusts a get reply's offset/token/total, message `len`,
  and `message_count`.** A forged reply wrote past a landing zone 3/3, test
  exited 0. ×2. Pair with **stating the trust boundary**, which appears nowhere.
  *Portability F1, F2.*
- **D5. `osoe_transport_sync` was closed on reasons the code contradicts.** What
  crosses at `barrier.c:61` and `progress-host.c:99` is already `(team, epoch)`
  down and one release upcall back; nothing node-addressed leaks and the backend
  needs no futex knowledge. §10's "4–6×" compares an RDMA tree with SHARP (not on
  RoCE switches — *documented*) when the real baseline is a TCP star. And §10's
  closed row "every place the SHMEM layer can block is a backend function" is now
  false (`barrier.c:69`). Make the internode phase a bound interface —
  `arrive(team, epoch)` down, `released(team, epoch)` up — with AM as one
  implementation; tens of lines now. ×3, LV: `barrier.c:61` passes
  `(t->number, k + 1)`, `progress-host.c:99` is `release(context, team, epoch)`,
  and `barrier.c:10` includes `progress-host.h` directly.
  *RoCE seam F1, F5; premises 1, 2.*
- **D6. The built internode barrier is a star; the design everywhere assumes a
  tree.** Build the tree on AM now, since reductions, scans and §5.8 depend on it.
  ×3. *Algorithmic-scaling 2.*
- **D7. `local_of_rank` (`node.c:391`, LV) and `gtids` (`bootstrap.c:81`) are
  O(P) private per PE.** ~250 MiB/node at 500 × 256, ~2 GiB at 4,096 × 256.
  Share one copy per node or represent as ranges; the job map also cannot give a
  remote PE's node-local index, which an RDMA address needs. ×4.
- **D8. A lost barrier wake is invisible to tests, invariants and `osoe-stat`.**
  200× slowdown, 22 tests green. *Correctness C6.*
- **D9. `osoe_fatal` → `exit()` → `start_pes`'s atexit `shmem_finalize` barrier
  → hang.** 3/3. Step 4's `global_exit` inherits it. *Conformance F1.*
- **D10. No AM-layer metrics; no way to find the one node that has not arrived
  without visiting every node.** *Diagnosability D-3, D-5.*
- **D11. `osoe-stat --gc` leaves `conn`/`job`/`wire` objects after SIGKILL; the
  next job with that id dies with `File exists`.** ×2. *Diagnosability D-4.*
- **D13. There is no signal design, and a cancelled job leaks everything.** The
  design never mentions `scancel`, SIGTERM, SIGINT or SIGPIPE. *Measured*: SIGTERM
  to all PEs leaves all six named shm objects; with the 256 MiB default
  connection-area budget about eight cancelled jobs fill a 4 GiB Pi's `/dev/shm`.
  `osoe-launch` has no signal handling or `PR_SET_PDEATHSIG`: SIGTERM to the
  launcher leaves every PE running. With D11 this is the first thing a Slurm
  cluster will hit. *Omissions 2, 6.*
- **D14. A PE that calls `exit(0)` without finalizing hangs a release-build job
  until its timeout** (*measured*); the documented answer, `srun -K1`, acts only
  on nonzero exit (*recalled*). Under `start_pes` the dead PE's atexit finalize
  makes `shmem_barrier_all` *return* on survivors that never matched it; they hang
  one collective later. Same family as D9. *Omissions 3.*
- **D15. Shared-memory objects are opened by name with only a size check**
  (`node.c:304`, `wire.c:198`, `job.c:121`, `connection-area.c:209`). *Measured*:
  a file pre-created by another user blocks the job, and the message blames a
  crashed job. A hijack via a fake control segment is argued, not demonstrated.
  When `/dev/shm` fills, the process that dies (SIGBUS, no osoe message) is the
  node lead applying *another* PE's put. Belongs with D4's trust statement.
  *Omissions 1, 5.*
- **D12. Progress work discovery is O(N) per wake, and every remote op does a
  locked add + futex syscall on one node-wide word.** By arithmetic. §3.10 must
  say how step 3 finds work. ×2. *Algorithmic-scaling 5; performance 4.*

### Clarification needed

- **Force-remote does not do what §8.1, `job.h` and commit `56dedff` say**: a put
  to a node-mate aborts "no connection toward node" (×3, up to 4/4); dispatch is
  per PE on one predicate, not per (pe, operation class) as §10 says was "folded
  in"; and a node-sized `SHMEM_TEAM_SHARED` whose members get NULL violates spec
  §7's "must return a non-null pointer" — commit `de540da` calls it legal.
- **Promises with no test under them**: FENCED (one producer, no consumer),
  the DRAINING guard (no production caller, and broken), quiet-means-applied
  (asserted through a race), the watchdog regex. *Correctness C7.*
- **4-byte rmw emulated by CAS on the containing 8-byte word corrupts a legal
  program** (a neighbour's CPU store is overwritten). On stock verbs the 4-byte
  class must run on the target CPU. *RoCE seam F2.*
- **Hardware premises are stale in the design's favour**: mlx5 extended atomics
  and a PCIe-AtomicOp CPU-coherent mode exist (*observed* in rdma-core/UCX
  source). *RoCE seam F3.*
- **"Notification: exact" cannot be kept for unhinted words on RoCE**
  (`transport.h:242`, `wait.c:105-108`). *RoCE seam F8.*
- **Probe checks never moved into init** (§1 says they do): with futex2 denied
  the job dies on SIGABRT mid-run and leaks segments. *Portability F3.*
- **Slurm**: `SLURM_NODEID` step-relativity unconfirmed; refuse `SLURM_HET_SIZE`;
  no hostlist parser; `OSOE_STEP_NUM_NODES` undocumented. *Portability F5, F6.*
- **§3.2's head-of-line bound ignores the kernel send buffer.** ×2.
- **§1 "cores are over-provisioned" contradicts §3.11/§3.12**; code follows §3.11.
- **`max_teams` is job-wide** and sizes per-node state: a 2-D split at 1M PEs is
  feasible (2,048 teams), P/4 small teams is not. Design team identity before
  more arrays are indexed by team number.
- **§5.5's lead-combines-everything is O(L·m) serial**; a chunk-partitioned
  combine keeps the single-writer argument at O(m) depth.
- **Under Slurm's `job_container/tmpfs` each job has a private `/dev/shm`**
  (*recalled*), so `osoe-stat` run from outside sees nothing — which undermines
  §3.14's premise on exactly the target cluster. Verify on the Pi cluster first.
- **Core files contain every sibling heap, non-sparse** (513 MiB for a 2-PE job
  touching 64 MiB each); no mapping uses `MADV_DONTDUMP` or `MADV_DONTFORK`.
- **CPU binding on a 4-core node has no launch recipe** and no affinity line in
  `SHMEM_DEBUG` output, though §3.12 depends on it. §4.2 still claims
  heterogeneous nodes while the rendezvous checks no endianness or ABI word.
  Generic `shmem_put`/`shmem_g` are absent (spec Examples 5 and 7 do not compile).
  No address-family or interface-selection rule for step 3.
- **No licence, no version identity** (0.0.0, no build id in `SHMEM_VERSION`,
  nodes do not compare library versions), and the installed tree cannot launch a
  job. Note that without a licence nothing derived from this tree — including a
  lessons document — can be reused by anyone else.
- **Subset scope is stated only in the design doc** while `SHMEM_VERSION` prints
  1.6; the rendezvous has no slot for a backend's addressing blob; §3.16's shape
  taxonomy and "four orders"; the aarch64 serializing store.

### Minor

Collected in the deliverables: `SMA_SYMMETRIC_SIZE` ignored; `test_any` always
returns the lowest index (against §9.11.3's fairness sentence); pre-init calls
segfault; 1.4-deprecated AMO names and `<mpp/shmemx.h>` missing; `pcontrol` only
in `pshmem.h`; static `libosoe.a` omits `osoe_base`; no `.pc`/`oshcc`/installed
launcher; tree needs gnu11, not C11; tsan unusable on 47-bit VA; fatal messages
that omit the peer; agent-label race moved, not removed; `OSOE_SEGMENT_HAS`
correct both ways but pinned by no test; fence re-flushes acknowledged peers;
`pe_quiet` allocates per call; 3 of 9 models under ctest.

## What is sound

With the evidence that convinced us; detail in the deliverables.

- **§3.16 kept to the letter** — 162 sites, greps validated. Store-buffer litmus:
  0 in 1M for the tree's pattern vs 68–203 for the control.
- **Reservation protocol** — new model from the C, 7/7 broken variants caught.
- **Ten model runs reproduce exactly**; every check but one arm is reachable.
- **The launcher and test registration** — no way found to pass a failing job.
- **The suite under load** — release preset, 16 spinners on 4 cores: 131/131 in
  5 of 5 full runs; 51 of 52 multi-process tests 0 failures in 30 isolated runs
  each. The exception, `shmem.wait.L2` (~4 %), is a test asserting what the code
  does not promise (a PE reaching its first check within 5 ms of its neighbour
  leaving a barrier), made into a 60 s hang by a `CHECK` that continues into a
  wait that can no longer be satisfied. Test defect; *Correctness C9.*
- **AM shared state really is O(N) per node with no PE term** (compiled sizing
  functions); **no deadlock at any limit** (no handler reserves outbound space).
- **The data-path seam**: `FENCED` as a per-op flag, counters not handles, `MORE`,
  `set_range`, `direct` as a store-ordering contract — all survived DC,
  multi-rail, ODP, completion moderation and doorbell batching. No-capability-bits
  survived a harder test than round one gave it.
- **Portability of what exists** — 16 KiB pages, 47-bit VA, clang, musl syntax,
  C++17 headers; shm objects 0600 + `O_EXCL`; §6.1's reading of Slurm matches the
  documentation.
- **Diagnosability above the seam** — the self-describing segment, by-name reads,
  the blocked record plus `pe.sweeps`, `arrival_posted` vs `released`.
- **The teardown fix** fixed a real defect and is stated where it should be.

## Where the evidence is weakest, and the cheapest strengthening

1. **No ack-protocol model** — one afternoon; it should fail the current tree on
   quiescence and the naive fix on termination.
2. **The load survey is narrow** — repeat under `debug` and `asan`, and at 8×.
3. **No refutation pass** — owed for B1, D1–D12.
4. **Tests that assert outcome but not latency** cannot see lost wakes (D8). A
   wall-clock ceiling on barrier loops, or D8's invariant counter, is cheap.
5. **Nothing has crossed a socket, a second machine, a Pi, or Slurm.** A
   two-process TCP loopback wire behind `wire.h` would retire a large class of
   unknowns before hardware.
6. **The aarch64 litmus ran on Apple cores under a hypervisor.** Re-run on an A76.

## Sequencing steps 3 and 4

1. **Before step 3 (small, in order):** model then fix B1; D1 (size the pool per
   §3.8 and make exhaustion a named fatal); D2+D3 together (sweep in all four
   loops, publish the blocked record, separate the timers); D4 and a stated trust
   boundary; D8's counter; D11.
2. **With step 3, not after:** D5 and D6 — bind the internode phase behind an
   interface and make the AM implementation a tree — because step 3 rewrites the
   progress loop anyway and D12 (work discovery) must be designed there. Fix D7
   while touching bootstrap for the TCP rendezvous, and add the backend blob slot.
3. **Pull D9 and D14 forward from step 4** — small, and they hang jobs today.
   **Before the first run on the Pi cluster:** D13 (signals, launcher, cleanup of
   all six object kinds) with D11 and D15, and check whether the cluster's Slurm
   gives each job a private `/dev/shm`.
4. **Step 4 otherwise as planned**, once D2 exists: abort propagation is
   pointless while four loops ignore the abort word.
5. **Before any more collectives:** team identity and the split protocol (§5.7),
   since per-team arrays are already being indexed by a number nobody has defined.

## Refuted or narrowed

No formal refutation pass ran. Claims that did not survive cross-checking between
agents:

- *"The idle lead spins"* (diagnosability) vs *"sleeps on a futex, 3–4 % of a
  core"* (performance): both describe the same 250 µs receive poll; neither is
  wrong, and after any quiet B1 makes it moot.
- *"An unaligned 64-bit atomic faults on aarch64"* (design §3.4): *measured* not
  to fault on aarch64 Linux in three codegen variants (portability F13). The
  alignment rule remains right; the stated reason is not.
- *"The separate connection area keeps 'does AM stay hot' open"*: narrowed —
  closing `sync` keeps the area, wires and engine alive in every build, so only
  its *size* is open.
- *Page-size risk on the Pi 5*: looked for, not found (131/131 × 3 presets).
- *`<stdbit.h>` portability*: moot; nothing includes it.
- *Reply-pool drop "unreachable today"* (protocols) vs *"ordinary at 256 PPN"*
  (performance): unresolved; needs more cores than the VM has.

## Sources

Deliverables, all under `docs/reviews/`:
`phase1-step2-slice1-conformance.md`, `-correctness.md`, `-portability.md`,
`-roce-seam.md`, `-performance-and-scale.md`,
`-diagnosability-and-deferrals.md`.
Reports under `evidence/phase1-step2-slice1/`: `atomics/report.md`,
`protocols/report.md`, `reports/evidence-audit.md`,
`reports/algorithmic-scaling.md`, `reports/premises.md`, `reports/omissions.md`,
`reports/load-survey.md`.
Each evidence directory holds its programs, patches, models and logs. External
sources (Slurm documentation, rdma-core and UCX source, NVIDIA documentation)
are listed with URLs in `roce-seam/sources.md` and in the portability deliverable.
