# Step E's merge point, question 3: does osoe scale, and perform, as the README says?

Review of `main` at `375973a` (the team redesign's step E, both parts, on step
D's merge point), run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, Question 3, with the lead's changes:
round five (`phase1-step7-slice3-scale-and-performance.md`, at `d8a651d`: T1 to
T3, six minors and three notes for step E) is the baseline and its fixes are
checked as claims; step E (`docs/design/team-resources-proposal.md` §4; the
records `docs/decisions/2026-10-04-team-lanes.md` and `2026-10-04-team-scans.md`;
design §5.7 and §5.8) is the heart, and the grant's cost the claim tested
hardest. One investigator; the lead runs the refutation pass. Every claim is
labelled *documented*, *observed in source*, *measured* or *recalled*.

**How it was checked.** Source read at `375973a` in the read-only worktree
`build/scratch/wt/review-e`: `internal-region.h` whole; in `internode-generic.c`
the file's comment, `in_pieces`, the grant (`osoe_internode_generic_reduce_enter`,
`take_grant`, `take_child_lanes`), the broadcast's pieces, the scan
(`osoe_internode_generic_scan`) and the number's zeroing; `collectives.c`'s
`data_collective_at` and the split's AND; `bootstrap.h`'s sizes; `rank-map.h`.
The README's scaling section, its settings rows and its team limits; design
§1's release point, §5.7 and §5.8; both step E records, the author's decisions of
2026-10-04 (items 3 and 5 to 9), round five's synthesis and the record of its
fixes, `docs/release-checklist.md`; the two timing models,
`tools/modelcheck/grant_timing.py` and `scan_grant_timing.py`, read and run.
The Pi evidence: every raw file under
`docs/design/evidence/pi-cluster/2026-10-04-step-e/` (three sessions, the probes,
the hour of `mix`), and the lead's merge-point run against D
(`build/scratch/e-measure/pi-merge/`, 21:51 to 22:05). Executable evidence in
the `osoe` VM (Debian 13, Linux 6.12, aarch64, 4 vCPUs, 8 GiB), every build and
run under `/tmp/osoe-vm.lock`, the VM idle before each (load at most 1.6, no
other job in `ps`) and the host's other two VMs idle: release builds of E (a
copy of the worktree) and of D (`158c13e`, the records' baseline, from `git
archive`) in the VM's `/tmp`, removed since; three probes of this review's -
`q3e-grant` (a large reduction and scan under five kinds of entry, below),
`q3e-loop` (back-to-back calls) and `q3e-split` (splits whose AND is large) - and
a random-skew driver for the two timing models. Sources, drivers and every output
are in the session's scratchpad (Sources); nothing else was written in the
repository.

**What the VM's timings are worth.** Whole runs are bimodal: the same line comes
out about twice as long in some runs as in others, whichever tree (D's 8-byte
reduce entering together: 104.5, 188.7 and 224.6 µs in three runs) - the host
placing the VM's vCPUs, by every sign. Within one run the quartiles are tight.
So every comparison below is taken **within a run** - a large collective against
the same run's 8-byte reduction (small, never granted), and then E's difference
against D's - and absolute times are not compared across runs. The VM's hop with
its wakes, as an 8-byte reduction with a leaf late less one with the root late,
was 26 to 87 µs from run to run: the VM can show the shape of a cost, not count
it in hops. That is the Pi's to do (below).

## Verdict

**In this question's area the tree can be tagged 0.1.0 as it is.** The README's
scaling section is true of the code: every figure step E changed - a team
number's 13.5 to 15.5 KiB, the 160 KiB lanes, the region's 6.03, 6.28 and 6.47
MiB, each row of what one more of a setting costs - comes out exactly from
`internal-region.h`'s formulas, and nothing step E added grows with the job's PE
count. **The grant's cost is what the README says**: a bound, once a collective -
up to a hop when a parent and its child enter together, up to a round trip when
the parent's lead enters after its children, nothing when a leaf is late. The
project's own timing models keep that bound under any entry skew, not only the
single late lead their tables try (27,000 random skews, never past +2.02 hops);
the Pi measured all four of the scan's cases as the models have them; and in the
VM a one-piece large reduction shows the same shape, its extra cost vanishing
when it is put back in the small class. What the records say of the reductions
is more than they measured (U2): the Pi's "nothing moved on the reductions" was
measured only right after a barrier, which gives the node tree's root the hop of
head start that hides the grant, and the records credit the intranode phase
instead; a reduction with its root's lead late was never measured on hardware.
One performance defect predates step E and is worth fixing after the release
(U1): a reduction's or broadcast's piece holds 16,368 bytes, so a 32 KiB one is
three pieces against a window of two, the third waiting a round trip of the
tree - 1.57 to 1.95 times as long as 32 bytes less, after a barrier in the VM. **Nothing blocks the release.** Before the tag, if the
author wants hardware behind the reductions row: one Pi run, described below.

## Findings, ranked

### U1. A reduction's or broadcast's piece is 16,368 bytes, so every power-of-two size from 16 KiB carries one piece more, and at 32 KiB the third waits a round trip of the tree

- **Location**: `shmem/src/internode-generic.c:762-773` (`piece_bytes`:
  `(OSOE_INTERNAL_SLOT_BYTES - HEADER_BYTES) / width * width`, and
  `bytes_per_piece`), `:1080` (the piece count), `:1623` (the broadcast's, width
  1); `internal-region.h:128` (the 16 KiB slot) and `:139` (the 16-byte header
  inside it); README line 1151, `OSOE_COLLECTIVE_PIPELINE`'s row: "a reduction's
  and a broadcast's 16 KiB each" (*observed in source*).
- **Scenario**: a large piece's header shares its 16 KiB slot, so a piece carries
  16,368 bytes of data. A 16 KiB reduction is then 16,368 + 16 bytes, two pieces
  where one would hold it - both inside the window of D = 2, so an extra message
  each way; 32 KiB is three, and a leaf's third piece goes up only once it holds
  the first's release - a round trip of the tree it would not wait with two; 64
  KiB is five pieces where four would do, 1 MiB 65. Results are right. The scans
  had the same shape in step E's second part (2,049 longs cut 1,024 + 1,024 + 1)
  and were given the fix - pieces counted so that D pieces are D and not D + 1
  (`:2010-2017`; the record's adversarial read, item 1); the reductions and
  broadcasts were not, and the sizes they meet are the powers of two.
- **Evidence** (*measured*, `q3e-grant`, four logical nodes of one PE, release
  builds): a sum reduction of 32,768 bytes against one of 32,736 (two full
  pieces), in the same run - in all 14 runs, D's and E's, at fan-outs 2 and 4,
  32,768 took 1.57 to 1.95 times as long after a barrier and 1.17 to 2.39 times
  entering together (+45 to +372 µs for 32 bytes more). The same on D's tree:
  the defect is old, from the pipelined pieces of phase 1 step 5 slice 5. On the
  Pi's gigabit at four nodes a 32 KiB reduction is bound by the root's link
  (*arithmetic*: three leaves' 96 KiB in and out at about 113 MiB/s, some 0.8
  ms each way, against 1,849 and 1,899 µs measured at the merge point), which
  may hide the third piece's round trip; on a faster link it would not (round
  four's S2, the window on fast links, is the neighbouring question).
- **Severity**: **after release** - or before, if the author wants the common
  sizes right in 0.1.0: a slot whose data is 16 KiB (the header outside it, 160
  bytes more a lane), a layout version, and a test that pins two pieces at 32
  KiB the way `most_pieces_in_flight` pins the window. The row's "16 KiB each"
  can say "16,368 bytes, a 16 KiB slot less its header" now (minor).
- **Confidence**: high on the mechanism and on the VM's ratio; the Pi's
  magnitude unmeasured (the run below).

### U2. The band's reductions were measured only where the grant is hidden by construction, and the records give that a cause nobody measured

- **Location**: `docs/decisions/2026-10-04-team-scans.md:164-166` ("Nothing moved
  on the reductions and broadcasts: at four PEs a node a leaf's intranode phase
  takes about a hop, and `grant_timing.py` says the grant then costs nothing"),
  `:187-193` (the VM's "+21 to +29 µs ... the grant's hop"); `docs/pi-cluster-results.md:664-666`;
  the probe, `docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c` (each
  call between two barriers) (*documented*).
- **Scenario** (*observed in source*, and the timing model): a barrier's release
  comes down the node tree, so the root's node leaves it a hop before its
  children - and the reduction's tree is that tree. The root's lead enters the
  reduction first and grants as it enters (`collectives.c:179-187`), so its grant
  reaches each child about as the child enters. The e-band probe therefore sees
  only what is left of the together case after a hop of head start, at any PEs
  a node. The same head start comes, in a program, from the previous collective's
  release; the grant shows where a reduction follows anything that releases
  parents and children together, and where a parent's lead is late - a node 0
  whose PE 0 also writes the output. The scans paid in the band because the
  scan's tree is over ranks: rank 5 on node 1 and its child rank 8 on node 2 leave
  the barrier together.
- **Evidence** (*measured*; `q3e-grant` makes every logical node enter at one
  instant of the VM's one clock - entry spread 0.1 to 0.2 µs - or one PE 1 ms
  late, spread 1,000.0 to 1,000.1 µs; the time is the last exit less the last
  entry; four nodes of one PE at the default fan-out of 4, the Pi's tree; the
  median over three runs a side of E's (large - 8 B) less D's, within each run,
  µs):

  | | after a barrier | together | root's lead late | a leaf late (PE 1) | a leaf late (PE 3) |
  |---|---|---|---|---|---|
  | 4 KiB reduction, one large piece | +17 | +100 | +110 | +17 | -9 |
  | 2 KiB scan, one large piece | +16 | +93 | +121 | +11 | +4 |
  | 64 KiB reduction, five pieces | +26 | +73 | +97 | +3 | -60 |
  | 4 KiB and 2 KiB at `OSOE_SMALL_COLLECTIVE_BYTES=4096` (small) | -16, -17 | +2, +6 | +20, +24 | +17, +8 | +12, +4 |

  After a barrier E pays +17 to +26 µs - the record's own VM figure, +21 to +29
  µs - and entering together +73 to +100, with the root late +97 to +121: the
  barrier hides about five sixths of the together case. Put back in the small
  class, the 4 KiB reduction and the 2 KiB scan cost what D's do in every mode:
  the extra is the grant. Back to back without barriers (`q3e-loop`, 50 calls a
  loop, four runs a side), E less D was +1.6, +1.2, -5.5 and +1.6 µs for the 4
  KiB reduction and +6.8, +1.9, -5.9 and +0.2 for the scan: in a loop the grant
  is hidden. The 64 KiB line pays once, not a piece at a time. At a fan-out of 2
  (a middle lead) the runs were too bimodal to read and are not used.
- **What it does not show**: whether the together case is one hop or two. The VM's
  ratio of the together cost to the root-late cost, run by run, was 0.57 to 1.05,
  where the model has 0.5; its hop, 26 to 87 µs, cannot count them. The Pi can:
  its scan with nobody late, the members entering together where the tree
  crosses sibling nodes, paid +119 µs against a hop of 115 to 135 - one hop, as
  the README says.
- **Severity**: **minor**. The README's reductions row states bounds, not the
  band's 4%, and the bounds hold (below); what is loose is the records' cause and
  the hardware evidence for reductions. The records' sentence wants "after a
  barrier, which releases the tree's root node first and so hides the grant";
  and the Pi run below would give the reductions row a measured late-lead cost
  beside the scans'.
- **Confidence**: high on the shape and on the attribution (the small-class
  control); medium on its size on the Pi until it runs there.

### U3. The job object's rank map is silent on plane and arbitrary distributions, where its runs grow with the job's PEs

- **Location**: README line 630 ("24 a run of the rank map - n of them under a
  block distribution, P under a cyclic one") and the list of what does not scale;
  `shmem/src/rank-map.h:12-20` ("more only under plane or arbitrary ones"; the
  rendezvous's root expands every node's progressions to O(N) to check every
  rank is claimed once) (*observed in source*).
- **Scenario**: under `--distribution=plane=4` a run is four ranks, so a job of
  128,000 PEs has 32,000 runs - 768 KB in every node's job object, carried down
  the rendezvous tree - and under an arbitrary one up to one a rank, 3 MB.
  Lookups stay O(log runs). Not measured: the VM cannot start such a job.
- **Severity**: **minor**: a clause in the row ("and up to one a rank under plane
  or arbitrary distributions") or a line in what does not scale.
- **Confidence**: high on the counts; how often plane is used at this scale is
  the author's call.

## The grant's cost, tested

The claim (README lines 582, 585 and 1152; design §5.8; `internode-generic.c`'s
comment): a large reduction's or scan's child waits for its parent's lane once -
up to a hop when the two enter together, nothing once the child's own node takes a
hop, up to a round trip when the parent enters after it, nothing when a leaf is
late.

- **The code does what the models assume** (*observed in source*): the local lead
  grants before it waits for its node's members (`collectives.c:179-187`,
  `osoe_internode_reduce_enter`); a scan's member grants before anything else
  (`internode-generic.c:1978-1992`); a child waits for the grant only before its
  first piece up (`:1086-1094`, `:2030-2034`), and once taken never again in the
  collective. Every grant is a put and a fenced add, the last child's without
  `MORE`, so it leaves at once. A broadcast takes none (`:1086`).
- **The bound holds beyond the models' tables** (*measured*, the models as they
  are, driven with random entries): the tables try one late lead at a time; I
  gave every lead (member) its own entry, uniform over half a hop, two and six
  hops - reductions on 5, 21 and 85 nodes at fan-out 4 and 21 at fan-out 2, 1 and
  8 pieces, intranode time 0, 0.3 and 1 hop; scans of 16 and 64 members at fan-out
  4 and 16 at 2, 1 and 4 pieces; 300 skews each, 27,000 in all. The extra over
  step D never passed **+2.02 hops** - a round trip and one combine step - in any
  of them. With entries within half a hop of each other and no intranode time it
  averaged +0.85 to +1.01 hops and reached +1.03 to +1.47 (a parent a little
  after its child is between the README's two cases); with an intranode time of a
  hop it averaged +0.00 to +0.02, as the README says.
- **On the Pi** (*documented*, the adversarial read's `scan-skew`, raw files
  read): a 2 KiB scan, E less D, +119 µs with nobody late, +259 with a member with
  children late, +228 with the root late, -6 with a leaf late; a hop there 115 to
  135 µs. One, two, two and none. The lead's merge-point run on the merged tree
  (raw files read): the 2 and 4 KiB scans +137 and +81 µs, the README's "120 to
  160" holding for 2 KiB.
- **In the VM** (U2's table): the shape again for a reduction, the cost once, the
  grant its cause, nothing in a loop.
- **Not measured anywhere**: a large *reduction* with its root's lead or a middle
  lead late, on hardware - the README's "up to a round trip" for reductions rests
  on the model and the VM. The run that measures it is below.

## For the Pi (the lead's runs)

1. **A reduction's late lead** (U2). `reduce-skew`: the adversarial read's
   `probes/scan-skew.c` with both calls of `shmem_long_sum_inscan` replaced by
   `shmem_long_sum_reduce`; built with each tree's `oshcc` (D, `158c13e`, under
   `~/osoe/build/compare/before`; E, `main`). Alternating, three runs a side:
   - `srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 ./reduce-skew 512 L 1000 300`
     with L = -1 (nobody), 0 (the root node's lead), 1 (a PE of the root's node
     that does not lead it: the grant leaves with PE 0, so a control) and 12
     (node 3's lead, a leaf);
   - the same with `OSOE_COLLECTIVE_FANOUT=2` exported and L = 4 (node 1's lead,
     a middle lead, its child node 3);
   - and at `--ntasks-per-node=1`, L = -1, 0 and 3.

   The README holds if, E less D, L = 0 and the middle lead pay about two hops
   (about 230 to 270 µs) at both shapes, L = 1, 12 and 3 nothing beyond noise,
   and nobody late between nothing and a hop (less at 4 × 4 than 4 × 1). It is
   refuted if a late lead costs well past a round trip (above about 300 µs), or a
   late leaf or non-lead anything.
2. **The extra piece** (U1). `e-band.c` with its sizes `{ 32736, 32768, 65472,
   65536 }`, E alone, at 4 × 4 under `--cpu-bind=none` and at 4 × 1, three runs.
   If 32,768 bytes cost more than 32,736 by more than a few percent (the bytes
   differ by 0.1%), the third piece's round trip shows on gigabit too.

## The README's table, row by row (what step E touched, and spot checks)

| Row | At `375973a` |
|---|---|
| barrier, sync, small collectives: ⌈log_k n⌉ levels, a hop each way | holds (*observed in source*): a small collective is one piece through the number's own rows; the Pi's barrier and 8-byte lines within their run spread at the merge point (below) |
| larger reductions and broadcasts: lanes, the grant once, "up to a hop ... up to a round trip" | holds as a bound (above); "16 KiB each" in the pipeline row is 16,368 bytes (U1) |
| `collect`: the lengths O(N) bytes, 1 MB at 128,000 PEs | holds; the lengths reduction is large past 128 members and so granted (`collectives.c:471`), which the large row covers |
| prefix scans: O(log N) rounds, the grant, the Pi's 120 to 160 and 230 to 260 µs | holds (*documented* and raw files read: +119, +137, +148, +158; +228, +259) |
| `alltoall`: 32.7 million messages a call at 500 × 256 | holds: 256 × (128,000 - 256) = 32.7 million |
| a team: the reduction of `OSOE_MAX_TEAMS` / 64 + 1 words, about a broadcast and a barrier | holds: 5 words at 256; *measured*, at the extremes - 1,024 numbers, `OSOE_SMALL_COLLECTIVE_BYTES=128`, so 136 bytes and a large, granted AND (`collectives.c:299-309` goes through `data_collective_at`) - 200 splits, each used by a sum and destroyed, on four logical nodes of two PEs: every sum right, status 0 |
| memory: a PE's heap object, 6.03 / 6.28 / 6.47 MiB of region; a number 13.5 KiB at 4 nodes, 15.3 at 500 | holds exactly (*observed in source*, `internal-region.h`'s formulas computed: 13.50, 14.50, 15.25 and 15.50 KiB a number at 4, 64, 500 and 1,000 nodes; 6.03, 6.28, 6.47 and 6.53 MiB) |
| one more of each setting: `OSOE_MAX_TEAMS`, `OSOE_MAX_THREADS` (a 160 KiB lane), `OSOE_SMALL_COLLECTIVE_BYTES` (ten bytes a byte on 5.25 KiB: 6.5, 15.25 and 165 KiB), `OSOE_COLLECTIVE_FANOUT` (2.5 KiB and 32 KiB), `OSOE_COLLECTIVE_PIPELINE` (80 KiB, 1.3 MiB across 17) | hold exactly (the same formulas) |
| the job object: runs n under block, P under cyclic | holds for those two; silent on plane and arbitrary (U3) |
| the limits paragraph's Pi figures for the fan-out (1.2, 19%, 1.5, 1.2 to 1.45, twice as fast to 1 KiB) | trace to `docs/pi-cluster-results.md:56-66` and `:136-145` (1.23, 1.19, 1.49 to 1.51, 1.23 to 1.45, 1.87 to 1.97), measured before step E moved the large pieces into lanes; nothing in E changes the window or the tree |

**Nothing step E added grows with the job's PE count** (*observed in source*): a
number's state is O(k + log n) a PE, a lane O(k D) a thread record, a grant O(k)
messages a lead (a member, for a scan), a scan's call O(k) pieces of scratch.
The lanes of a large scan are every member's, so its pages are written on every
PE of a node - linear in the node's PEs, as the rule allows.

## The previous round's findings, checked as claims

| | At `d8a651d` | What the fix claims | At `375973a` |
|---|---|---|---|
| T1 | the pool binds before the numbers at 256 PEs a node | a README sentence; the default put to the author | **holds**: README 724-735 gives the reach (P + 1 blocks a grid, three at 256 PEs, "where the numbers would allow 126") and a block's cost; the author kept 1,024 (`2026-10-04.md` item 3) |
| T2 | a 32-bit generation | after release | **on the list** (`release-checklist.md:124-126`) |
| T3 | a dry pool said by every PE in lines that interleave | one write a line | **holds** (`team.c:1235-1250`, `osoe_notice`) |
| minor 1 | the team row left out the closing sync | "about a broadcast and a barrier" | **holds** (README 587) |
| minor 2 | `OSOE_TEAM_BLOCKS` costs more than "a kilobyte" | the row | **holds** (README 1141: a kilobyte of `/dev/shm` and about 600 bytes on the first PE) |
| minor 3 | `bootstrap.h`'s numbers | fixed | **holds**: "about 18 MiB" at 1,024 numbers (computed 16.2 to 18.2 MiB), the lanes 2.7 MiB, "about 5.2 GiB" at every ceiling (computed 5.18) |
| minors 4 to 6 | a busy round spins; 257 claims on one word; no test past 16 nodes | after release | **on the list** (`release-checklist.md:111-112`, `:133-136`) |
| For E, 1 | the pool beside 256 numbers | a decision in E's brief | **answered**: the author kept 1,024 knowing it binds first (item 3); the README says so |
| For E, 2 | the 1 to 16 KiB band | measure it on the Pi | **measured** at 4 × 4 - after barriers only (U2) |
| For E, 3 | the lanes' cost in `OSOE_MAX_THREADS`'s row | a sentence | **holds** (README 1143: 160 KiB a place, 2.7 MiB a PE) |

## Defaults

| Setting | Default | Justified by | On hardware unlike the Pi |
|---|---|---|---|
| `OSOE_SMALL_COLLECTIVE_BYTES` (new) | 1,024 | the author's answer to E2, a memory dial: a number 15.25 KiB at 1 KiB, 45.25 at 4 KiB (at 500 nodes) - 7.5 MiB more address space a PE at 256 numbers, counted P times by the capacity check. The Pi: scans of 2 and 4 KiB +81 to +158 µs, reductions in the band -1 to +14% after a barrier | safe: the cost is a bounded latency, smaller in absolute terms on a faster link. The README's row says when to raise it, and the remedy works (*measured*, U2's table: at 4,096 a 4 KiB reduction costs what D's does) |
| `OSOE_MAX_TEAMS` (now 256) | 256 | the author (E10); a number about 14 KiB | safe: 3.5 to 4 MiB of address space a PE, counted P times; half the region step D's 64 numbers took |
| `OSOE_TEAM_BLOCKS` | 1,024 | the author, knowing its reach (item 3) | binds first past 4 PEs a node for teams that share no PE, failing alike and named |
| `OSOE_COLLECTIVE_FANOUT`, `OSOE_COLLECTIVE_PIPELINE`, `OSOE_GET_LANDING_BYTES`, `OSOE_AGENT_SLICE_US`, `OSOE_AGENT_CPUS` | 4, 2, 256 KiB, 100, `task` | unchanged; round five's table stands. The slice row's percentages are still the 2026-09-28 tree's - round five's open question, unanswered and unmeasured since | as round five found; U1 and round four's S2 for the pipeline on fast links |

## Measurements and methodology

- **The Pi's band, by session** (*measured* from the raw files): D's lines are
  stable across the three sessions (within about 1%), so the record's "D the
  median of its nine runs" against E's third session moves no figure: E less D
  within the third session is +148 and +104 µs for the 2 and 4 KiB scans against
  the record's +158 and +108. The 4 KiB reduction was +16 to +19 µs in every
  session - consistent, small, and after a barrier (U2). The record reports the
  reductions at 8 B, 4 KiB, 16 KiB and 1 MiB; the 1, 2, 8, 32 and 64 KiB lines
  it printed say the same.
- **The merge-point run** (the lead's, `pi-merge/`, read): the band as in the
  third session - reductions +6 to +87 µs from 2 to 64 KiB (1.01 to 1.14
  times), the scans +137 and +81 at 2 and 4 KiB and 1.01, 1.00, 0.94, 0.79 and
  0.67 times D from 8 KiB to 1 MiB. `osoe-bench`'s 65 lines, three runs a side:
  nine had E's three runs wholly outside D's, all within 0.3% of D; the medians
  that differ more - the local put stream 0.83 to 0.85 times D from 64 KiB to 4
  MiB, a local 256 KiB put and quiet 1.18, two pairs' 1 MiB streams 1.07 and
  1.15 - are on paths step E did not touch, and bimodal from run to run on both
  trees in every session (the local 1 MiB stream 5,400 to 5,800 or 6,700 to
  7,240 MiB/s). The record's "no line moved beyond its run-to-run spread" holds.
  The band's barrier and 8-byte reduce medians, +12% and +15%, are the cluster's run-level
  bimodality - runs at about 181 to 185 or 204 to 210 µs, E with two of three in
  the slow mode and D one - which three runs a side cannot separate from a few
  percent; the third session had +2% and +0.4%.
- **Which tree the evidence is of** (*documented*, the commits' times against
  `session-3/run.txt`): the third session, the band's E column and the hour of
  `mix` ran on the tree of `ba2153c` (20:30), before the adversarial read's code
  changes (`cf624e7`, 20:58: pieces counted in elements, a scan's slot cleared
  once read). The records say "the third tree", which is accurate; but the
  checklist's "an hour of `mix` on the tree to be tagged" is not yet met by it.
  The probes and the merge-point run are of the merged code.
- **The VM**: bimodal between runs (above); every figure here is a difference
  within a run. The first two versions of `q3e-grant` were not used: PEs that
  slept to their deadline entered 57 to 99 µs apart (a sleeping vCPU wakes late),
  and PEs that spun between calls starved the agents of the PEs still in one
  (upper quartiles of 2 to 4 ms); the probe used spins with a yield to the
  deadline and quiets with a barrier after each timed call.
- **Not measured by anyone**: a large reduction with a late lead on hardware (the
  run above); more than 4 PEs a node on real cores for any of step E's lines; a
  link faster than gigabit.

## What is sound, and what convinced me

- **The scaling section's figures are the code's**: every step E number in the
  memory tables and the "one more" table reproduced from `internal-region.h`'s
  formulas to the rounding, and the time table's step E rows match the code paths
  they describe.
- **The grant's cost is a bound, and the bound is kept**: by the code's order (the
  grant before the intranode wait; the wait before the first piece only), by the
  models under 27,000 random skews (never past +2.02 hops), by the Pi's four scan
  cases (one hop, two, two, none), and by the VM's reduction (the same shape, once
  a collective, gone in a loop, gone when the collective is small again).
- **Step E moved nothing it should not have**: on the Pi the broadcasts within
  1%, `osoe-bench` within its runs' spread (the lines whose runs did not overlap
  within 0.3%), the scans from 8 KiB equal or faster and 1 MiB a
  third faster; in the VM the 8-byte control the same on both trees within each
  run's noise.
- **The settings at their extremes work**: 1,024 numbers with a 128-byte small
  line make the split's AND large and granted, and 200 splits ran right.
- **Round five's fixes in this area hold**, and its notes for step E were taken.

## Sources

`README.md` (lines 482-692, 1140-1173) and the code at `375973a`, cited by line
above; design §1, §5.7 and §5.8; `docs/design/team-resources-proposal.md` §4;
`docs/decisions/2026-10-04-team-lanes.md`, `2026-10-04-team-scans.md`,
`2026-10-04.md`, `2026-10-04-step-d-review-fixes.md`; `docs/release-checklist.md`;
`docs/pi-cluster-results.md`; round five's `phase1-step7-slice3-scale-and-performance.md`
and `phase1-step7-slice3-review.md`; `tools/modelcheck/grant_timing.py` and
`scan_grant_timing.py`; the evidence under
`docs/design/evidence/pi-cluster/2026-10-04-step-e/` and the lead's
`build/scratch/e-measure/pi-merge/` and `pi-mix/`. This review's probes,
drivers and outputs, in the session's scratchpad
(`<scratchpad>/`):
`q3e-grant.c`, `q3e-loop.c`, `q3e-split.c`; `models/random_skew.py` (the two
timing models, copied, driven with random entries); `analyze.py` and
`analyze2.py`; `out/` - `f4-{D,E}-{1,2,3}.txt` (fan-out 4), `f2-*` (fan-out 2,
not used), `trial3-*`, `loop-{D,E}-{1..4}.txt`, `s4k-E-{1,2,3}.txt`
(`OSOE_SMALL_COLLECTIVE_BYTES=4096`). The VM's build copies are removed.
