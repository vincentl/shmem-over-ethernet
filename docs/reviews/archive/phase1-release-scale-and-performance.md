# Round three, question 3: does osoe scale, and perform, as the README says?

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. Investigators: `scaling` (the
README's table against the code, with footprints measured in the VM and two
small simulations), `defaults` (every default against the records that justify
it), `pi` (the cluster's numbers reproduced on the tree under review), and the
lead's own runs: the four presets, tier 3 and the suite under load in a fresh
VM. Working files are under `build/scratch/review-r3/` (`scaling/`, `defaults/`,
`pi/`, `load/`), which git ignores. Each claim is labelled *documented*,
*observed in source*, *measured* (with where) or *recalled*; "lead-verified"
means the lead re-read the cited lines or re-ran the command.

## Verdict

**The memory claim holds; the throughput claim is one thread wide; two rows of
the table and one paragraph of the Limits need correcting before a stranger
relies on them.** No array the size of the job's PE count survives init in any
PE or node - the second review's `local_of_rank` and `gtids` are gone,
replaced by runs and an O(L) index - and connection state is O(n) a node, as
the table says (*observed in source*, checked allocation by allocation, and
the control-segment formula matches the measured 651,264 bytes at two PEs on
two nodes). The trees between nodes are the logarithmic ones the table names.
What the table hides is a job-wide resource spent once per node (team
numbers: finding P1), one thread carrying every node's network traffic (P2),
and a budget that cannot do what the README says it does (P3). The defaults
that matter are measured on the Pi; the ones that are guesses fail safe. The
Pi's headline numbers are corroborated across sessions, and the ones this
round re-measured are in the section "Measurements".

## Findings, ranked

### P1. Team numbers are claimed from one job-wide counter whatever the parent team, so a node-local split costs one number per node and fails past 61 nodes at the default

- **Location**: `shmem/src/team.c:813-840` (`claim`: every split's parent
  rank 0 compare-and-swaps the counter on world PE 0, for any parent,
  `SHMEM_TEAM_SHARED` included; lead-verified), `team.c:964-999`; README
  "Scaling with nodes, not PEs" row "a team ... O(1) a member" and Limits "Team
  numbers are never reused"; design §5.7 and §10 "Team numbers for `split_2d`".
- **Scenario**: a program splits `SHMEM_TEAM_SHARED` once on every node - a
  node-local team for a hierarchical algorithm, the natural use of that team.
  Each node's split is a separate collective and claims its own number. With
  `OSOE_MAX_TEAMS=64` (61 creatable) on 500 nodes, 61 nodes get a team and 439
  get `SHMEM_TEAM_INVALID` and a nonzero return, and which nodes win is a race.
  Raising the variable to fit costs every PE about 208 KiB of address space a
  number (P-minor 3): at 500 nodes, `OSOE_MAX_TEAMS` of at least 503 makes each
  PE's internal region about 105 MB, 26 GB of the `/dev/shm` capacity check per
  node of 256 PEs; past 1,021 nodes no setting works.
- **Evidence**: *measured* (VM, `scaling/split_shared.c`: every node splits its
  `SHMEM_TEAM_SHARED` once, `osoe-launch -n 6 -m 0,0,1,1,2,2`): `OSOE_MAX_TEAMS=4`
  gives node 0 a team and nodes 1 and 2 "no team numbers are left"; `=5` two
  nodes succeed; `=6` all three. *Observed in source*, lead-verified: `claim()`
  targets PE 0 through `osoe_transport_rmw(..., 0, OSOE_REGION_INTERNAL, ...)`
  for every parent. *Documented*: design §10 already says "a number needs to be
  unique only among the teams that share a node" and defers a per-node
  colouring to a RoCE backend.
- **Severity**: **fix before release**. As the README stands a reader is misled
  inside the range it promises ("hundreds of nodes"). One sentence in the team
  row and the Limits ("every split claims job-wide numbers; a split made on
  each node - of `SHMEM_TEAM_SHARED`, say - spends one per node") is enough to
  release; numbering single-node teams from a per-node range, or §10's
  colouring, is the fix proper and is after release. The failure is loud, not
  silent.
- **Confidence**: high.
- Related, minor: concurrent claims contend in a CAS loop on world PE 0; c
  simultaneous claimers cost up to c(c + 1)/2 remote CASes on node 0's
  progress thread where a fetch-add would cost c. Moot while P1 stands.

### P2. One progress thread a node moves every internode message of all its PEs, and the README does not say what that bounds

- **Location**: `shmem/src/progress-host.c:382-388, 642-676` (only `osoe_am.lead`
  runs the engine), `shmem/src/agent.c` (`osoe_transport_agent_pass` on the
  lead's agent only; every other agent "is answered with false and sleeps";
  lead-verified); design §3.12 ("At 256 PEs perhaps four, all in the lead
  process, sharded by peer node"); README "How osoe works" and the table's
  second row ("connection state O(n) a node").
- **Scenario**: every remote put, atomic, get request and reply for a node's
  256 PEs is parsed, applied and acknowledged by one thread, and every segment
  sealed and sent by it, so each PE's share of the node's message rate falls as
  1/L. An 8-byte `alltoall` at 500 × 256 sends L × (P - L) = 32.7 million
  messages into each node. The Pi measured the rate of that thread at 243,000
  to 257,000 messages a second for 16 PEs (*documented*, design §10). The same
  bound applies to bandwidth: in the lead's fresh VM the library streamed
  511-1,636 MiB/s a pair over loopback where the bare echo streamed 5.3-8.6
  GiB/s (*measured*, `load/release-test.log`, `shmem.floor`'s seven pairs).
  "Streams run at line rate" is true of gigabit Ethernet and not shown, or
  likely, for 10 GbE and above.
- **Evidence**: *observed in source*, lead-verified; *documented* (§3.12's plan
  for four sharded threads, which §3.8's "multi-producer once a node runs more
  than one progress thread" also assumes; §10's "small-message rate bound"
  still open); *measured* as above; the per-message cost at 256 PEs on x86-64 is
  unmeasured (no x86-64 host has run osoe: design §10).
- **Severity**: **fix before release** (README): the Limits should say that a
  node's internode message rate and bandwidth are one thread's, whatever the
  PE count, that "line rate" was measured at 1 Gbit/s, and that the sharded
  engine §3.12 describes for 256 PEs is not built. Building it is after release.
- **Confidence**: medium on magnitude (the structural fact is certain; a
  put-rate run with 1, 4 and 16 senders on a host with more cores than the VM's
  four would pin it).

### P3. The connection-buffer budget cannot shrink at the defaults; a job of more than 1,025 nodes is refused, and the README says the opposite

- **Location**: `shmem/src/connection-area.c:107-125` (`osoe_am_choose_buffer`:
  `while (b > floor && peers * 2 * b > budget) b >>= 1;` - at the defaults b is
  already the floor, 2 × `OSOE_AM_CHUNK`, so the loop never runs; lead-verified),
  `:212-217` (the fatal); README reference row `OSOE_AM_BUFFER_BUDGET` ("the
  buffers shrink, saying so, to fit").
- **Scenario**: defaults (chunk 64 KiB, buffer 128 KiB, budget 256 MiB): 1,025
  nodes fit exactly; 1,026 are refused at init with "raise the budget or lower
  the chunk". The shrink the README describes happens only when `OSOE_AM_BUFFER`
  was raised above its floor, and then the notice prints once per PE
  (`connection-area.c:207`, before the creator check): 128,000 identical lines
  at 500 × 256.
- **Evidence**: *measured* (VM, `scaling`): three logical nodes with
  `OSOE_AM_BUFFER_BUDGET=262144` and the default buffer end with the fatal; with
  `OSOE_AM_BUFFER=524288 OSOE_AM_BUFFER_BUDGET=1048576` they print "connection
  buffers reduced from 524288 to 262144 bytes". *Observed in source*,
  lead-verified. `connection-area.h:47-58` describes the budget as "a guard
  rail ... not reached until about a thousand nodes", consistent with this.
- **Severity**: **fix before release** (README: at the defaults the budget is
  a ceiling of 1,025 nodes and names the two variables that raise it; print
  the notice once a node). The 500-node working point is unaffected.
- **Confidence**: high.

### P4. The intranode arrival is one cache line and the lead's combine is one thread, unmeasured past 4 PEs a node

- **Location**: `shmem/src/barrier.c:59` (every member's locked add on one
  line), `shmem/src/collectives.c:189-191` (the lead combines L vectors
  serially), `:362-369` (fcollect's lead copies L blocks serially); design §5.3
  "Still flat, deliberately" (lead-verified: "there is no measurement at large
  L to size a fan-out from ... What would trigger it: on a host with 64 or more
  cores, `sync_all` on one node growing faster than linearly in L"); README
  "within a node, linear or better in the PEs there".
- **Scenario**: L serialized cache-line transfers per arrival is linear, as the
  goal allows, and about 25-50 µs at L = 256 (*recalled* transfer times); a
  1 MiB reduction at L = 256 is 256 MiB combined on one core, about 50-100 ms
  (*recalled* rates) against about 4 ms for the internode pipeline at 10 GbE.
  The README's large-collective row has an empty cost cell where this is the
  dominant term at 256 PEs. Nothing has run osoe with more than 16 PEs a node
  (the VM has four cores; the Pi four a node).
- **Evidence**: *observed in source*; *documented* (§5.3's own admission);
  *recalled* rates, unverified.
- **Severity**: **after release**, with one sentence in the README now: the
  intranode claim is measured to 16 PEs a node, and the lead's combine is
  O(L × bytes) on one thread. The `defaults` investigator rated it fix before
  release; the lead lowered it because an L-way add on one line is linear, and
  the design names only a risk that it grows worse.
- **Confidence**: high that it is unmeasured; medium on impact.

### P5. TCP send and receive buffers are left to autotuning; the second review's head-of-line finding (F7) was never closed

- **Location**: `shmem/src/wire.c` (only `TCP_NODELAY` and keepalive are set;
  no `SO_SNDBUF`, `SO_RCVBUF` or `TCP_NOTSENT_LOWAT` anywhere in `shmem/src` or
  `base/`; *observed in source*); design §3.2's "roughly two segments"
  head-of-line bound; `docs/reviews/archive/phase1-step2-slice1-performance-and-scale.md`
  F7 ("needs step 3").
- **Scenario**: a barrier arrival queued behind a stream waits for the kernel's
  send buffer to drain, not for two segments: the second review's worked
  example was about 33 ms at 1 GbE with a 4 MiB autotuned buffer (*recalled*
  numbers) against the design's 2 ms. At higher bandwidth an autotuned buffer
  drains proportionally faster, so the effect need not grow, but nothing has
  measured it above 1 Gbit/s, and a cluster with `tcp_wmem` tuned up for bulk
  transfers reintroduces it.
- **Severity**: after release (a latency-model gap, not a hang); close it by
  measurement or by `TCP_NOTSENT_LOWAT`/`SO_SNDBUF` before any latency claim
  beyond gigabit Ethernet.
- **Confidence**: high that it is unaddressed; medium on magnitude.

### P6. `shmem_collect` reduces a vector of all P lengths through the tree before any data moves

- **Location**: `shmem/src/collectives.c:411-462` (`collect_varying`); decisions
  README "collect's lengths through the reduction tree" (author, 2026-09-26).
- **Scenario**: at 500 × 256 every `shmem_collect`, even of one element a PE,
  first reduces 8 × 128,000 = 1 MB through the k-ary tree - about 31 tree round
  trips at 16 KiB pieces with two in flight, and about 5 MB out of each
  interior lead - where an allgather of the lengths (Bruck, as `fcollect` uses)
  would move about 1 MB a lead in 9 steps. The result is O(P) a PE by
  definition; the constant is about five times a gather's.
- **Evidence**: *observed in source*; *arithmetic*.
- **Severity**: after release (a README line under Limits or a `collect` row
  now). **Confidence**: high on traffic, the time estimated.

### P7. Per-context completion state is O(n) a context a PE

- **Location**: `shmem/src/transport-am.c:151-168` (`dirty_set_allocate`: 21
  bytes a peer node a live context, plus a bit); README row "connection state
  O(n) a node, not O(n × P)".
- **Arithmetic**: 10.5 KB a context at 500 nodes; with every one of the 64
  contexts in use on every PE, 172 MB a node at 500 nodes and 1.4 GB at 4,096,
  resident only as touched. The row's claim holds for the connections; the
  ordering state beside them is per context, and an RDMA backend with a queue
  pair per context and target node inherits the shape.
- **Severity**: after release (a note). **Confidence**: high.

### P8. Heaps are 4 KiB pages although design §6.2 says hugetlbfs where available

- **Location**: `shmem/src/node.c:224-270`, `shm-object.c:37` (`shm_open` on
  `/dev/shm`; no hugetlbfs or `MAP_HUGETLB` path); design §6.2 "Where hugetlbfs
  is mounted and has pages, heaps use it".
- **Arithmetic**: every process maps all L node-mates' heaps and the lead's
  progress thread writes into all of them: page tables cost touched bytes /
  512, up to 134 MB in a process that touches all of 256 default heaps.
  *Measured* baseline VmPTE 64-80 kB a PE in a 4-PE VM job.
- **Severity**: after release (correct the design text, or build the path).
  **Confidence**: high on the code, medium on how often programs reach the
  large numbers.

### P9. The README's "16-PE barrier about 180 µs" holds only under the binding the README tells users to avoid (fix before release, README; high; *measured* on the Pi, `pi`)

Under Slurm's default binding 180.8-182.7 µs; under `--cpu-bind=none`, the
README's recommended launch, 196-208 µs over four runs, with small collectives
4-17% slower unbound. The README gives binding's cost to point-to-point
patterns and not unbinding's cost to small collectives. The 2026-09-26 record's
"under 5% on every line" is contradicted by its own raw files.

### P10. The record's "no difference in small operations" for the agent's 100 µs slice does not reproduce (fix before release, documentation; the default is the author's call; high; *measured*, three runs each way alternating)

Bound, the slice makes the barrier 8% slower and small reduce and broadcast
6-16% slower, bound or unbound; its gains reproduce (a 64 KiB reduce 2,388
against 3,130 µs, alltoall 8-10% faster, the bound mailbox 1.7×). Say both
sides where the README and the record give one.

### Minor

1. **Start-up**: the row says "⌈log₈ n⌉ levels"; the rendezvous is that, and
   then every lead makes n - 1 connections serially (`wire.c:169-216`: accept
   every lower node, then dial every higher one). A discrete-event model
   (`scaling/mesh_sim.py`) completes the mesh in about 2n connection times:
   997 at 500 nodes, 8,189 at 4,096 - at about two round trips each, 0.1-0.4 s
   and 1-3 s. Not a problem; not the row's cost either.
2. **Large reductions**: "about twice the bytes of a ring or Rabenseifner's" is
   (k + 1)/2 the *time* on the busiest links - 2.5× at the default fan-out -
   and the same total bytes as a ring. The Pi's four nodes measured 28.2 ms at
   k = 4 against about 13.5 ms for a ring, 2.1× (*documented*, design §10, and
   *arithmetic*).
3. **Per-team-number cost**: 208,384 bytes a number at k = 4, D = 2, n = 500,
   not the README's "about 160 KiB"; the internal region is 12.7 MiB a PE at
   the default and 203.5 MiB at 1,024, where `bootstrap.h:72-77` says 160 MiB
   (*measured*, `scaling/sizes.c` against the headers, and the 4-PE job's
   mappings). The scans' 2.6 MiB of it is written by every member of a
   scanning team, not "only a team's leads".
4. **Scans under a cyclic layout**: the tree is over PEs (depth 9 at 128,000),
   and with a cyclic distribution 127,972 of 127,999 tree edges cross nodes:
   about 1,024 messages a node a piece, and 2L × V bytes out of each node for a
   scan of V bytes (`scaling/scan_tree.py`). Inherent to that layout, not to
   the design; the row should say per-node traffic is O(L) under cyclic
   distributions.
5. **Deprecated active-set collectives** on a proper subset run per-PE
   algorithms (`active-set.c`): `shmem_barrier` on half of 500 × 256 is 16
   rounds of an atomic from every member, about 2,300 network messages a node,
   where the team path sends one arrival and at most k releases. The row should
   say "on teams".
6. **`docs/decisions/README.md` line 70** says `OSOE_AGENT_CPUS` "defaults to
   `node`"; the code (`agent.c:127-136`), the README and the record it cites
   (`2026-09-28-overnight.md`, "For the author" 2) say `task`.
7. **Design §4.11** (line ~4044) still says `OSOE_MAX_THREADS` defaults to the
   node's CPU count; the code's 16 was chosen against that on purpose
   (`metrics.c:402-412`; `2026-09-26-evening.md` B1).
8. **Fixed constants that read like settings**: the rendezvous fan-out 8
   (`rendezvous.h:50`), the scan piece of 4 KiB (`internal-region.h:81-84`,
   justified only by "no program in the corpus scans much"), the collectives'
   16 KiB piece (`:72`), `OSOE_LANDING_EXTENT_BYTES` 1 KiB (`node.h:867`), and
   `OSOE_SWEEP_NS` 100 ms (`base/deadline.h:16`). None is an environment
   variable; a line in the reference would save a reader the search.
9. **The listen backlog** is min(n, 4096), capped by `net.core.somaxconn`; on a
   host with the old default of 128 a 500-node start-up may see SYN drops
   during the rendezvous. Undocumented; unmeasured.
10. **"7 s" for `osoe-pacer --quick` on 16 PEs** rests on one recorded run
    (`docs/pi-cluster-results.md:442`); the lead's own baseline in the VM ran
    `--quick` in 1.4 s on 4 PEs and 2.5 s on four logical nodes.

## Defaults: measured, reasoned or guessed

From the `defaults` investigator's table (`reports/defaults.md`), condensed;
"elsewhere" is the judgement for hardware unlike the Pi (10-100 GbE, 64-128
cores with SMT, NUMA, a kernel before 6.12, 500 nodes).

| Setting | Default | Justified by | Elsewhere |
|---|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | 4 | reasoned from an h/o model whose "server TCP" numbers the design itself labels *recalled*; measured only on the VM's loopback and the Pi (`docs/pi-cluster-results.md:44-59, 127-141`) | safe: only shapes the tree; unknown on 10/100 GbE, where h/o was never measured |
| `OSOE_COLLECTIVE_PIPELINE` | 2 | a timing simulation and Pi measurements at D = 1, 2, 4, 8 (broadcast, reduce, scan) | safe |
| `OSOE_GET_LANDING_BYTES` | 256 KiB | measured: a get-heavy sort round 5.6 s at the default, 4.3 s at 4 MiB (`pi-cluster-results.md:487-491`); refill recommended, not built | safe (backpressure); leaves more on the table on a faster link |
| `OSOE_AGENT_SLICE_US` | 100 | measured extensively (mailbox 38,900 → 57,500-62,900 a second bound) | safe: a pre-6.12 kernel ignores it; unknown under SMT |
| `OSOE_AGENT_CPUS` | `task` | measured: `node` made small collectives 7-18% slower | safe; `node` on a two-socket node is unmeasured |
| `OSOE_DEADLOCK_SECONDS` | 10 | a round number; the protocol is model-checked and tested at 1 s | safe (ends only a job proved idle); the root's O(n) rounds unmeasured at scale |
| `OSOE_KEEPALIVE` | 60,10,6 | conventional; the mechanism tested with short values | safe |
| `OSOE_AM_CHUNK` | 64 KiB | measured on the Pi, with an unresolved trade-off against 16 KiB (F1, `pi-cluster-results.md:100-166`) | safe; the trade-off was switch-specific |
| `OSOE_AM_BUFFER` | 2 × chunk | reasoned; measured on loopback only: 0.18× the floor at the default, 0.49× at 256 KiB, 0.56× at 512 KiB-2 MiB (design §8.4) | safe, and plausibly 2-5× bandwidth left on the table on any link faster than gigabit; the README does not say to raise it |
| `OSOE_AM_BUFFER_BUDGET` | 256 MiB | reasoned as a guard rail | P3 |
| `OSOE_AM_CREDITS` | 64 | guessed ("tunable because the only way to exercise exhaustion in a test is to make it small", `connection-area.c:161-164`) | safe; exhaustion under 256 PEs' gets unmeasured |
| `OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS` | 16 KiB × 64 | a test-shaping default; exhaustion is a documented, tested re-arm (`-ENOBUFS`), a stall not a drop | unknown whether 1 MiB of receive buffering suffices at 100 Gbit/s |
| `OSOE_MAX_TEAMS`, `OSOE_MAX_CONTEXTS` | 64, 64 | sized to address-space cost and to the pacer's own exercise, not to a survey of applications | fail safe; P1 for teams |
| `OSOE_MAX_THREADS` | 16 | reasoned against a CPU-count default on purpose | safe by design; a 32-thread OpenMP region ends the job by name - an operator moving to many-core nodes should raise it first |
| `SHMEM_SYMMETRIC_SIZE` | 256 MiB | reasoned | safe; the `/dev/shm` check refuses rather than faults |

## Measurements

**The lead's fresh VM** (Debian 13 arm64, kernel 6.12, four cores, made for
this review as `osoe-load`; the tree copied from the worktree): debug 598 of
598, asan 597 of 597, tsan 387 of 387, release 597 of 598, and the tier-3
preset - the whole release suite with every model and the 103 shaped-link
tests - 725 of 725 (*measured*, `load/*-test.log`). The one failure is
`shmem.floor`'s stream pin: the library's stream over loopback was 0.069-0.223×
the bare echo's across seven pairs, median 0.0999, against a pin of "at least
0.10×"; the fastest round trips were 1.5-1.7× the floor's (pinned at most 3×)
and the medians 1.95-1.99×. The pin is machine-dependent, and it fails the
README's first command on a machine unlike the author's: readiness finding R2.

**Under load** (the same VM, eight busy loops on four cores): the release suite
without the models 596 of 598, the two `shmem.strict-c11` (a nested build,
76 s idle, past its 180 s timeout) and `shmem.reply` (0.06 s idle; 60 s
timeout, reproduced once in five repeats). The reply test's four threads spin
with `sched_yield` for a credit the library would sleep for
(`shmem/tests/test-reply.c:104-106`), and under oversubscription each handoff
can wait a scheduling period; the ticket field is 21 bits, so no wrap is
involved (lead-verified). A test-robustness defect, not a library one:
correctness finding C-minor. The debug suite and the newest tests repeated
under load are in the correctness deliverable.

**The Pi cluster** (`reports/pi.md`; 116 runs on 2026-09-28, 06:40-08:07, on
`~/osoe-f561e2d`, source-identical to `b0e0b53` by sha256 of all 357 tracked
source files; four Pi 5, kernel 6.18, 16 KiB pages, 1 Gb/s with interrupt
coalescing at 49 µs; `/dev/shm` empty before and after; every output under
`pi/out/`):

| Claim | Recorded | Measured on this tree |
|---|---|---|
| smoke test, 16 PEs | 7 s, `failures=0` | README command 6.8-7.3 s, bound 7.1-7.3 s; 37 runs over five shapes, all `failures=0` |
| floor ratios | 8-13% | 9.0-10.5% (three runs each way, spread at most 0.006); stream 0.999× |
| put streams | line rate | 112.1 MiB/s, TCP's line rate at MTU 1500 |
| bound pipelined pattern against unbound | 6× (4× with the slice) | 6.9× (4.0×) |
| `OSOE_AGENT_CPUS=node`'s cost | 7-18% | 7-18%; a 64 KiB reduce 2,819 against 2,388 µs |
| `mix`, unbound, detector on | an hour, `failures=0` | 18 minutes over two seeds, 90 periods, 159 GB, `failures=0`, nothing declared |
| README ring example | - | correct at 2 × 1 and 2 × 4 |
| SOS at 4 × 2 (release-point condition 3 on hardware) | 159 in the VM | 163 of 165 built, 161 pass (`bigget -l 100` in 22.2 s); the same four fail for the documented reasons; nothing that passed in the VM fails here |

Two claims did not reproduce, both **fix before release** as documentation
(P9, P10 below): the README's "a 16-PE barrier takes about 180 µs" holds under
Slurm's default binding (180.8-182.7 µs) and not under the `--cpu-bind=none`
the README tells users to run with (196-208 µs over four runs), where small
collectives cost 4-17% more (fcollect 4 KiB +16%, 64 KiB reduce +17%); the
2026-09-26 record's "under 5% on every line" between bound and unbound is
contradicted by its own raw files (barrier 7.8%, fcollect 12-19%). And the
record's "no difference in small operations" for the 100 µs agent slice does
not hold: bound, the slice makes the barrier 8% slower (180.8-182.7 against
167.2-167.5 µs) and small reduce and broadcast 6-16% slower bound or unbound,
while its gains reproduce (64 KiB reduce 2,388 against 3,130 µs, alltoall
8-10% faster, bound mailbox 1.7×). The default is the author's call; the
record and README should carry both sides.

Methodology, after release: the floor is an idle-link echo of about 157 µs
inflated by interrupt coalescing (echo minima reach 106 µs; inside `mix`, on
a busy link, the library's own round trip has a median of 134 µs, below the
idle floor), so the absolute overhead of about 14-17 µs a round trip would be
a more portable claim than the percentage, and `test-floor.c`'s "fastest"
statistic is unreliable on this hardware (per-pair ratios 0.89-1.30 where the
medians are 1.09-1.11); `osoe-bench` times each collective on PE 0 alone, back
to back, so its numbers are PE 0's wait rather than latency (an 8-byte reduce's
"fastest" was 17.9 µs over a 157 µs link); a 1 MiB get runs at 86 MiB/s,
about 77% of line rate, where puts run at line rate; the soak records say
"504 MB" and "605 MB" where it should be GB (half of this run sent 81 GB); the
2026-09-28 measurements behind the slice and widening numbers have no raw
output under `docs/design/evidence/`; 7 of 15 pacer runs took one or two
extra quanta of about 5.2 s outside the program's own clock, unexplained and
not reproduced in 28 later timestamped runs; `unit/spam` (83.5 s) and
`unit/mt_membar` (50.6 s, hand-offs by plain stores found at the 100 ms sweep)
run close to the runbook's 120 s timeout. The captured 4 × 2 Slurm environments
the lead asked for (block, cyclic, uneven `-N4 -n6`) are in
`pi/out/slurm-env-*.txt`, 22 per-task files.

**The records' methodology** (from `defaults`): the floor ratios are seven
pairs of 2,000 rounds; the small collectives two or three alternating runs; the
mailbox rates one 60-second run per configuration; every headline README
number but the "7 s" is re-measured in at least two sessions on different
commits and agrees. That is better evidence than most projects keep.

## The README's table, row by row

| Row | Verdict |
|---|---|
| put, get, atomic to a node-mate: no system call | holds (`transport-local.c`; notify loads one bitmap word and wakes only a matching waiter) |
| across nodes: connection state O(n) a node | holds for connections (125 MiB of area and 3 MB of engine at 500 nodes); per-context state O(n) a context (P7); throughput one thread a node (P2) |
| barrier, sync, small reductions, broadcast: ⌈log_k n⌉ steps | holds: 5 levels at 500 nodes; a node handles one arrival and at most k releases; the intranode arrival flat on one line (P4); active-set subsets excepted (minor 5) |
| larger data collectives: pipelined, two in flight | holds; the empty cost cell hides the O(L × V) lead combine (P4) and (k + 1)V on interior links (minor 2) |
| `fcollect`: ⌈log₂ n⌉ steps | holds: 9 at 500 nodes, each share arriving once |
| prefix scans: O(log N) rounds | holds (9 at 128,000 PEs); per-node traffic O(L) under cyclic layouts (minor 4) |
| `alltoall`: O(N) a PE | holds; all through one thread a node (P2) |
| a team: O(1) a member after O(S log S) | per member holds; per job misleading (P1, minor 3) |
| waiting: O(1) a waiter | holds; the waker's `futex_wake(INT_MAX)` on turnover and acknowledgement words is O(waiters) on the progress thread |
| start-up: ⌈log₈ n⌉ levels | the rendezvous holds; the mesh is about 2n serial connections (minor 1) |
| `SHMEM_THREAD_MULTIPLE`: O(1) an operation | holds: issue is one CAS and a conditional OR; quiet O(n/64) words plus dirty peers plus landing extents |

## Memory at 500 × 256 (defaults; from `scaling`, struct sizes measured)

| Structure | Per | Bytes | Grows with |
|---|---|---|---|
| Control segment (PE slots, counter blocks, landing zones, contexts, thread records) | node | 70.6 MiB | L (+ n for connection metrics) |
| Connection area, 2 × 128 KiB a peer | node | 125.4 MiB (refused above 1,025 nodes: P3) | n |
| Internal region (team slots, scans, gather words) | PE | 12.7 MiB (3.2 GiB a node) | T, log n |
| Engine: descriptors, wires, ring, receive buffers | lead | about 3 MB | n |
| Per-context dirty set | context × PE | 10.5 KB | n × contexts |
| Sockets / file descriptors | node | 499 / about 760 on the lead | n |
| Virtual address space | PE | about 67 GiB (every node-mate's heap) | L × heap |
| `/dev/shm` capacity check | node | about 67 GiB | L × (heap + region) |

No structure of size P remains after init except the root's one-time rank-map
check, `collect`'s per-call places (P6) and a cyclic split's one-time pairs.

## What is sound

- **No O(P) state per PE or per node after init** (*observed in source*: every
  `malloc`/`calloc` sized by `world_size`, `node_count` or `local_count` in
  `shmem/src`, and the control-segment formula against the measured segment).
- **The internode trees** are what the table says: the barrier's k-ary tree
  with O(k) state a team a node (`collective-tree.c`, `progress.c:271-311`),
  the pipelined reduction and broadcast (`internode-generic.c:713-760`), Bruck's
  allgather (`:1134-1184`), the rendezvous's fan-out-8 tree.
- **The engine's per-pass cost follows activity**: dirty, readable and busy
  bitmaps read 64 connections a word; the arrival scan is bounded by team
  numbers published, not by `OSOE_MAX_TEAMS`.
- **Kernel resources are sized and checked**: `RLIMIT_NOFILE` raised to the hard
  limit before any socket; io_uring queues sized to their bounds and refused by
  name otherwise; the listen backlog min(n, 4096).
- **Periodic duties are cheap at 500 nodes**: `TCP_INFO` on n - 1 sockets once
  a keepalive interval; `poll` of L - 1 pidfds a sweep; the abort O(connections)
  once; a detector sample about 3.3 µs a `/proc` read (*measured*), so a few
  milliseconds at L = 256.
- **The defaults that carry weight are measured** (pipeline depth, agent slice
  and CPUs, chunk, landing zone), on the Pi, in records that name commit and
  session; the guessed ones fail loud and by name.
- **The Pi numbers the README quotes are corroborated across sessions**, and
  the README repeats none of the record's single-run multipliers.

## Sources

`reports/scaling.md` and its scripts (`sizes.c`, `footprint.c`,
`split_shared.c`, `procread.c`, `scan_tree.py`, `mesh_sim.py`),
`reports/defaults.md`, `reports/pi.md`, `load/*.log` (the lead's baseline and
load runs), `docs/pi-cluster-results.md`, `docs/decisions/2026-09-27*.md` and
`2026-09-28-overnight.md`, design §3.12, §5.3, §5.7, §5.8, §6.2, §8.4, §10,
`docs/reviews/archive/phase1-step2-slice1-performance-and-scale.md` (F7).
