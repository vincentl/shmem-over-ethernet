# Step D's merge point, question 3: does osoe scale, and perform, as the README says?

Review of `main` at `d8a651d` (the team redesign's step D, both parts, merged on
the round-four review's fixes), run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, Question 3, with the lead's changes:
the previous round (`phase1-release-fixes-scale-and-performance.md`, at
`08b3da0`: S1 to S3 and four minors) is the baseline and its fixes are checked
as claims; step D (`docs/design/team-resources-proposal.md` §3; the records
`docs/decisions/2026-10-03-team-storage.md` and `2026-10-03-team-allocation.md`;
design §5.2 and §5.7) is the heart; step E is not built, and what this review
has to say about the proposal's §4 is kept apart at the end. One investigator;
the lead runs the refutation pass. Every claim is labelled *documented*,
*observed in source*, *measured* or *recalled*.

**How it was checked.** Source read at `d8a651d` in the read-only worktree
`build/scratch/wt/review-d`: `team.c`, `internal-region.h` and `internode-am.c`
whole; in `node.h` and `node.c` the pool, the map, the bitmaps, the block's life
and `create_control`'s sizing; in `progress.c`, `progress-host.c` and
`internode-generic.c` everything sized or walked by `OSOE_TEAM_BLOCKS`; the
split's reduction in `collectives.c`; and the diff `08b3da0..d8a651d` of every
file under `shmem/src`, `base/` and `tools/stat`, read for what each change
sizes or walks. The records, the proposal, D's brief and design §5.2 and §5.7
read whole; step E's brief (`docs/prompts/phase1-step7-slice4-prompt.md`, added
after this tree by `c306221`) read for the section on E only. The merge
point's Pi runs (the lead's: twelve `osoe-bench` runs at 4 × 4,
`build/scratch/merge-d/pi/`) and verification (`build/scratch/merge-d/main/verify.txt`:
debug 690/690, release 690/690 with the floor pins, asan 687/687, tsan 465/465,
tier 3 113/113, models 28/28) used as evidence. Executable evidence in the
`osoe` VM (Debian 13, Linux 6.12, aarch64, 4 vCPUs, 8 GiB), every build and run
under `/tmp/osoe-vm.lock`: the release preset's library, launcher and
`shmem_test_floor`, built from a copy of the tree in `/tmp/q3d/src` (removed
since), and eight probe programs of this review's - `q3d-split` (a split's cost
against the collectives the README compares it with), `q3d-pool` (what a node's
pool holds at many PEs a node), `q3d-churn` (teams made and destroyed for as
long as a program runs), `q3d-maps` (what the pool costs a node and a process),
`q3d-nodeset` (a copy of the node-set walk's loops at the design's scale),
`q3d-gen` (the generation limit, against a patched copy), `q3d-band` (a baseline
for step E) - and the previous round's `q3-split` and `q3-start`. Sources,
scripts (`run*.sh`) and every output stay in the VM's `/tmp/q3d` (1.2 MB);
nothing else was written in the repository.

**What the VM's timings are worth.** The runs fell in two sessions. From 12:54
to 13:06 the host ran the two other review VMs at 230 and 390% CPU (load
average 80), which the `osoe` lock does not cover; from 14:56 to 14:59 the host
was quiet (load average 3, the other VMs idle). Absolute times differ two to
three and a half times between the two; every ratio taken within one run - a
split against a reduction and a barrier in the same job - agrees within 0.05,
and counts (blocks, numbers, lines, bytes) do not move. Tables below give the
quiet session where there is one. The VM cannot show a logarithm in the node
count: its 4 vCPUs time-slice n PEs and n progress threads, so every
collective's time grows about linearly past eight logical nodes.

## Verdict

**The README's scaling table, with its new row for a team, is true of the code
at `d8a651d`, and step D's costs are what the row says: a split is one
reduction over the parent - about a broadcast - plus the sync that closes it,
at every size from one node to 128 logical nodes; nothing D added grows with
the job's PE count; a destroyed team gives back its memory as well as its
number; and D moved no line on the Pi or the VM's floor.** The previous round's
fixes in this area hold, and its case against the team limits (P1) is closed to
254 logical nodes. What the README does not say is how far a node's default pool
of team blocks reaches at its own target of 256 PEs a node: three whole-job
`split_2d` grids, each PE then holding nine of its numbers (T1, measured) - and
after step E's 256 numbers the pool, not the numbers, is what binds at any node
of more than four PEs whose teams share none of them. **Nothing here blocks the
release.** T1 wants a sentence and a decision on the default, both of which fit
step E, which precedes the tag; the rest are minor.

## Findings, ranked

### T1. At the README's 256 PEs a node, a node's default pool of 1,024 team blocks binds long before a PE's numbers: three whole-job `split_2d` grids, each PE then holding 9 of its 64

- **Location**: README, Limits, "Teams", the second bullet (lines 498-509) and
  the `OSOE_TEAM_BLOCKS` row (line 890), beside the first bullet's "a program can
  hold 61 teams of its own on every PE" (line 486); `shmem/src/bootstrap.h:108`
  (`OSOE_TEAM_BLOCKS_DEFAULT 1024u`); `shmem/src/team.c:1281-1296` (every
  would-be local lead of every team a split creates claims a block,
  `leads()` at `:1036`) and `:1448-1454` (`split_2d`'s columns, stride
  `xrange`); the default's rationale, proposal D3 ("253 whole-job teams and
  three layers of one-PE-per-team splits on a 256-PE node"), and the first
  adversarial read's item 5 (teams of one PE; `2026-10-03-team-allocation.md`,
  "The adversarial read", item 5).
- **Scenario** (*observed in source*, and arithmetic): a node's pool holds a block
  for every team with a member on the node. A whole-job `split_2d` whose rows are
  at least a node long - the usual two-dimensional grid at this scale - gives
  each of a node's P PEs a column of its own, a column with one member on every
  node: P + 1 blocks on every node (P + 2 where a row straddles two). A family of
  teams "the PEs of local rank i on every node", one `split_strided` per i, costs
  P blocks a node and one number a PE. So a node of P PEs holds ⌊1,021 / (P + 1)⌋
  grids: 204 at the Pi's 4, 15 at 64, 7 at 128, 3 at 256 - where a PE's own 61
  numbers would allow 30. The pool binds before the numbers for grids from about
  34 PEs a node, and for local-rank families from 17. Step E's default of 256
  numbers moves both down: for teams that share none of a node's PEs the pool
  binds first at any node of more than four (1,021 / 253).
- **Evidence** (*measured*, `q3d-pool`, two logical nodes, `OSOE_TEAM_BLOCKS` at its
  default; every team kept was used by a sync and destroyed at the end):

  | PEs a node | whole-job grids made | numbers a PE then held | local-rank families made | numbers a PE then held |
  |---|---|---|---|---|
  | 128 | 7, the 8th dry | 17 | 7, and 125 splits of the 8th | 11 at most |
  | 256 | 3, the 4th dry | 9 | 3, and 253 splits of the 4th | 7 at most |
  | 256, `OSOE_TEAM_BLOCKS=8192` | 6, the 7th out of numbers (`OSOE_MAX_TEAMS` 16 in this run) | 15 | - | - |

  The failing split returned nonzero on every PE and named `OSOE_TEAM_BLOCKS`
  and the node, as the README says - so a user who meets it is told the remedy.
  The Pi cannot show this: at four PEs a node its pool holds 204 grids and its
  numbers 30.
- **The trade, and why it is worth reopening**: the author chose the pool over a
  per-PE table "for scaling", the table costing 14 MiB of a 256-PE node's segment
  at 64 numbers and 56 MiB at 256 (proposal D3, *documented*). A pool that never
  binds first is the same size: the worst case is L × (`OSOE_MAX_TEAMS` − 3) + 3
  blocks, 15,619 at 256 PEs and 64 numbers, and a block costs, *measured* below,
  1,024 bytes of `/dev/shm` and about 570 bytes resident on the node's lead - 15
  MiB and 8.5 MiB, against the 64 GiB of heaps 256 PEs have at the default
  `SHMEM_SYMMETRIC_SIZE`. The default was sized counting whole-job teams (a block
  a node each) and teams of one PE (the follow-up of item 5, which step E's brief
  leaves until after the release); the column team, one member on every node, is
  between the two and is what a grid makes. The README states the rule
  correctly ("a team of members on several nodes counting once on each") and
  gives no case of it.
- **Severity**: **fix before release, with step E** (which precedes the tag): a
  README sentence - "a whole-job `shmem_team_split_2d` whose rows are a node long
  or longer gives every PE of a node a column team of its own, so at 256 PEs a
  node it takes 257 of the node's 1,024 blocks" - and the default decided in E's
  brief with E's 256 numbers: scaled with the node's PE count, or kept and its
  reach stated. Nothing in the code is wrong.
- **Confidence**: high on the counts and the arithmetic; how often programs at
  256 PEs a node hold more than three grids is the author's call.

### T2. A PE may take one team number at most 4,294,967,295 times in a session, and a loop of split and destroy takes the same number every time: two hours in a one-PE job

- **Location**: `shmem/src/team.c:1069-1079` (`finish`: `if (generation ==
  UINT32_MAX) osoe_fatal(...)`), `team.h:145` (`uint32_t generation`); the
  README's Limits, which say they list "every limit a program can meet" (line
  479), list none of this; the proposal's table promises "no limit" on the teams
  a job makes in its life (*documented*).
- **Scenario** (*observed in source*): a split takes the lowest number free on every
  member, so a program that makes and destroys a team in a loop - a library that
  makes a team per call - takes one number every time, and that number's
  descriptor counts the takes for its handles (D6). The 4,294,967,296th ends the
  job.
- **Evidence** (*measured*): a copy of the library whose descriptors start four
  takes short of the limit: the fourth split of a loop ended the job, status 1,
  `this PE has taken team number 3 4294967295 times, every generation a handle
  can name`, on one PE, on two PEs of a node and on two nodes (`q3d-gen`,
  `runG.sh`). How long the real limit takes, from the quiet VM's split-and-destroy
  times: 1.7 µs in a one-PE job (or any split whose parent is a team of one), so
  2.0 hours; 80 µs at four PEs of a node, 4.0 days; 214 µs on two nodes, 10.6
  days. *Observed in source*, beside it: a block's generation, 32 bits stepped twice
  a team, wraps after 2^31 teams on one block, and the loop above reuses the
  pool's top block every time. I found no harm in the wrap - the engine and the
  generic phase compare a generation only with the one they learnt last - except
  in a schedule that brings an internode team back to a block after exactly
  2^31 − 1 teams with no internode phase, where the agent's stale `consumed` word
  could match a post; for question 2 to judge, noted only.
- **Severity**: **minor** (after release): a 64-bit generation - the handle's
  arithmetic has room for about 2^45 at 1,021 descriptors of some 160 bytes
  (*observed in source*, the struct's fields) - or the limit listed.
- **Confidence**: high.

### T3. A dry pool is said by every PE whose claim failed, not "a PE of that node", in lines that interleave mid-sentence

- **Location**: `team.c:1234-1241` (`pool_dry`, an `fprintf` to stderr) and
  `:1341-1344`; README line 501 ("a PE of that node saying why"); `fatal.c:16-20`
  ("One buffer, one write ... separate writes interleave inside each other's
  lines"), which this message does not follow.
- **Evidence** (*measured*): one failed `split_2d` at 2 × 64 with
  `OSOE_TEAM_BLOCKS=8` printed the message from 120 PEs, 60 a node; at 2 × 128 at
  the default, 21; at 2 × 256, 14 (each node's shortfall). One 261-byte `fprintf`
  to stderr is three `write`s under the VM's glibc 2.41 (128, 128 and 5 bytes,
  by `strace`), and in the job's stderr the concurrent lines interleave
  mid-sentence ("... OSOE_TEAMosoe: PE 14: ..."). A `split_2d` against an empty
  pool at 500 nodes of 256 PEs would print up to 128,500 such lines.
- **Severity**: **minor**: one line a node - the PE of lowest local id among those
  whose claim failed, or the parent's rank 0 naming the dry nodes, as
  `numbers_used_up` does for the numbers - formatted into one buffer and written
  once. (`SHMEM_DEBUG`'s per-PE line, `init.c:423`, has the same shape; question
  5's.)
- **Confidence**: high.

### Minor

1. **The team row leaves out the split's closing sync.** "The reduction of
   `OSOE_MAX_TEAMS` / 64 words - about what one broadcast costs" (README line
   459) is right of the reduction (*measured*: an 8-byte reduction and broadcast
   within 3% of each other between nodes, below), but a split also syncs its
   parent before it returns, and costs 1.5 to 1.7 broadcasts across nodes, 0.82
   to 0.96 of a reduction plus a barrier, and a destroy one barrier of the team. "About a
   broadcast and a barrier" says what a reader will pay. (The words are
   `OSOE_MAX_TEAMS` / 64 and one more of marks.)
2. **`OSOE_TEAM_BLOCKS` costs more than its row's "about a kilobyte each"**
   (*measured*, `q3d-maps` at 4 × 4, 1,024 against 65,536 blocks): 1,024 bytes of
   `/dev/shm` a block, allocated at start; on each node's lead about 570 bytes
   resident a block - some 330 of them a `memset` (`progress.c:1141`) of an array
   that `calloc` has zeroed already (`progress-host.c:460`), the rest the reply
   pool's free list (`progress-host.c:452-458`, 48 bytes × the degree); and about
   480 bytes of untouched address space a block in every process
   (`internode-generic.c:381`, `internode-am.c:65`). At the default, 0.55 MiB a
   lead; at the 65,536 the row allows, 35 MiB a lead and 30 MiB of address space
   a process. It matters for T1's remedy; dropping the `memset` would take off
   more than half of the lead's share (the 570 less the reply pool's 240).
3. **`bootstrap.h`'s numbers**: line 80 still says a node's team blocks come to
   about a megabyte at `OSOE_MAX_TEAMS` 1,024 - since D they are
   `OSOE_TEAM_BLOCKS`'s, and the map at 1,024 numbers is 4 KiB a local PE - and
   line 123 gives the scans 1.3 MiB at the defaults, where `internal-region.h:74`
   and `:212` give 2.6 (D = 2; 1.3 is D = 1). The previous round's 160 MiB is
   fixed.
4. **A busy round runs again at once** (`team.c:1311-1313`, `continue`): a split
   whose pool is empty while a block is on its way back runs back-to-back
   reductions over its parent until the block is back - on a parent within one
   node, intranode reductions of a few microseconds each, so its PEs spin, on the
   oversubscribed node the design expects, against the member whose leave they
   wait for. Rare (a dry pool) and bounded by that member's next turn on a core.
   *Observed in source*, not measured; a pause between busy rounds, as the start-up
   waits back off, would cost nothing.
5. **The P + 1 claims of a whole-job `split_2d` all start at the same word**
   (`node.c:1258-1280`: from the top word down, a failed exchange retried on the
   returned value): when a node's leads claim at once, each success can fail
   every other claimant's exchange, O(P²) exchanges on a few cache lines in the
   worst case - about 33,000 at 256 PEs a node, a few milliseconds of one line's
   traffic on a rare call. *Observed in source*; not measurable on 4 vCPUs. A start
   word chosen by local id spreads them.
6. **No test runs past 16 logical nodes** (the previous round's minor 1,
   unchanged, *observed in source*): D's acceptance at 61 to 64 nodes and this
   review's to 254 were run by hand.

## The previous round's findings, checked as claims

| | At `08b3da0` | What the fix claims | At `d8a651d` |
|---|---|---|---|
| S1 | a reopened buffer wakes every writer | README sentence; §3.11's example; two comments; the hand-on wake after release | **holds** (*observed*): README 696-702, design §3.11 ("As built the room is divisible for small messages only"), `wait-word.h:24-30`, `transport-am.c:661-668`; the code unchanged, as recorded |
| S2 | the window of two 16 KiB pieces an edge bounds a large collective on a fast link | after release; a sentence if wanted | **unchanged**, no sentence taken; still after release |
| S3 | the round-trip ratio does not travel | README sentence | **holds** (README 851-855); *measured* again: round trips 1.95 times the echo on the quiet host, the stream 0.18-0.21 of a bare one ("a fifth"); with the host at load 80, 1.91-1.97 and 0.08-0.10 - the VM's stream figure is the host's |
| minor 1 | no test past 16 logical nodes | after release | unchanged (minor 6 above) |
| minor 2 | `bootstrap.h`'s 160 MiB | "about 200 MiB" | **holds** (*measured*: 203 KiB a number, 202 MiB of region a PE at 1,024 numbers); two other numbers there stale (minor 3) |
| minor 3 | F1's lower bound | "up to 52 s" | **holds** (README 844) |
| minor 4 | a stray process in the VM | killed | **holds**: none |
| D, Part 1 item 6 | one summary word covers 4,096 blocks | - | **answered**: sixteen summary words (`bootstrap.h:113`, `node.h:842-862`); the agent loads one at the default pool (`internode-am.c:180-206`) |
| D's acceptance | the per-node split of `SHMEM_TEAM_SHARED` on 64 logical nodes | every node valid | **holds**, *measured* past it: every node got its team at 64, 128 and 254 nodes of one PE and 64 of two, no sync or reduction on it nonzero, status 0 (`q3-split`) - round three's P1 closed |

## Step D at scale, measured

**What a split costs** (`q3d-split`, the quiet session: 200 iterations each, every
one after a barrier, the slowest PE's median in µs; the 128-node line from the
loaded session):

| job | barrier | 8 B reduce | 8 B broadcast | split of the world | its destroy | split ÷ (reduce + barrier) |
|---|---|---|---|---|---|---|
| 1 PE | 0.7 | 0.6 | 0.6 | 1.5 | 0.2 | - |
| 1 × 4 | 25.5 | 44.7 | 49.4 | 57.6 | 22.5 | 0.82 |
| 2 × 1 | 65.2 | 88.9 | 86.3 | 147.5 | 65.9 | 0.96 |
| 4 × 1 | 79.9 | 107.9 | 109.3 | 179.4 | 78.9 | 0.96 |
| 8 × 1 | 126.3 | 196.1 | 199.2 | 309.5 | 124.4 | 0.96 |
| 16 × 1 | 181.9 | 256.2 | 261.7 | 397.5 | 182.6 | 0.91 |
| 32 × 1 | 289.5 | 391.3 | 385.0 | 587.7 | 290.9 | 0.86 |
| 64 × 1 | 635.0 | 797.8 | 786.4 | 1,189.2 | 647.1 | 0.83 |
| 16 × 4 | 236.2 | 339.1 | 339.1 | 506.8 | 233.9 | 0.88 |
| 128 × 1, loaded | 3,856 | 4,726 | 4,822 | 7,361 | 4,178 | 0.86 |

A split of two PEs out of the world costs what the world's does, within 3% - the
parent is what the split is collective over - and a `split_2d` within 7%. Under
`SHMEM_THREAD_MULTIPLE` a split took 1.62 times the default level's at 8 × 1 and
1.51 at 16 × 4: the second reduction, as the README and the record say. At 1,024
numbers (17 words) against 64 (2 words) at 16 × 1 the ratio was 0.92 against
0.91: the words cost nothing measurable. The record timed PE 0 alone (196 and
220 µs at 5 and 8 nodes, the same before D and after); these are the slowest
PE's, which is what a program waits for.

**The one non-logarithmic step** (`q3d-nodeset`, a copy of
`created_node_set`'s two paths with the rank map's search, on the VM's core):
a whole-job team of 128,000 members on 500 nodes takes 1.6 ms on each node's
local lead when the walk meets the nodes in order (a block map) and 4.8 ms when
it must gather and sort (a cyclic map) - about what the split's reduction and
sync would take at that depth on gigabit, and documented as O(M) and O(M log M);
I see no reason to change it, and the row's figure would help a reader.

**Memory** (*measured*): the control segment grows by 1,024 bytes a block and the
lead's resident memory by about 570 (minor 2); the previous round's start-up
probe on this tree against its own outputs at `08b3da0`: each node's lead holds
about 540 KiB more after D, at 16 × 4 (296 → 840 KiB anonymous) and at 63 × 4
(464 → 1,000) alike, the non-leads' 180-190 KiB and every descriptor count
unchanged - a constant a node, nothing a peer, nothing a PE of the job.
**Given back means given back**: three teams a cycle (`split_2d`'s rows and
columns and a split of every other PE), each used by an `fcollect`, a reduction
and a sync and then destroyed - 10,000 cycles at 4 × 2, 10,000 at 4 × 1 with the
pool at 8 blocks so that blocks cycle, 20,000 at 1 × 4 - and every PE's VmSize,
VmRSS and anonymous memory identical from the 2,000th cycle to the last
(`q3d-churn`; 4 × 2's lead: 115,984, 3,408 and 1,064 kB throughout).

## The README's table, row by row

| Row | At `d8a651d` |
|---|---|
| put, get, atomic to a node-mate | holds (code unchanged since the last round) |
| across nodes: connections O(n) a node; ordering state 21 bytes a node a context | holds (the review fixes touched `connection-area.c`'s creator check only) |
| barrier, sync, small collectives: ⌈log_k n⌉ steps | holds: D changed how the engine finds a team (two loads a message) and how it finds a post (one load a pass at the default pool), not the tree; the Pi's barrier unmoved |
| larger data collectives | holds; S2 stands |
| `fcollect`, `collect`, prefix scans, `alltoall` | hold (unchanged) |
| **a team** (new) | holds in every term, *measured* above: the reduction about a broadcast, the second under `SHMEM_THREAD_MULTIPLE`, O(P) a member (`shape`), O(M) or O(M log M) on each lead, the leads kept and freed at destroy; the closing sync unmentioned (minor 1); what "a block from each member node's pool" reaches at 256 PEs a node is T1 |
| waiting: O(1) a waiter | holds: D's one new wait, a pending word under `SHMEM_THREAD_MULTIPLE`, is a futex word a number in the process |
| start-up | holds: D adds a constant a node (the pool, 1 MiB allocated; the lead's arrays), *measured* the same at 16 and 63 nodes |
| `SHMEM_THREAD_MULTIPLE`: O(1) an operation | holds (unchanged) |

## Defaults

| Setting | Default | Justified by | On hardware unlike the Pi |
|---|---|---|---|
| `OSOE_COLLECTIVE_FANOUT`, `OSOE_COLLECTIVE_PIPELINE`, `OSOE_GET_LANDING_BYTES`, `OSOE_AGENT_SLICE_US`, `OSOE_AGENT_CPUS` | 4, 2, 256 KiB, 100, `task` | unchanged since the previous round, whose table stands; the slice row's percentages are the 2026-09-28 session's (below) | as the previous round found; S2 for the pipeline |
| `OSOE_TEAM_BLOCKS` (new) | 1,024 | reasoned, not measured (proposal D3: whole-job teams and teams of one) | safe - a dry pool fails a split alike and names the setting - but at 256 PEs a node it binds before `OSOE_MAX_TEAMS` for grids and local-rank families (T1) |
| `OSOE_MAX_TEAMS` | 64, *temporary* | the author's choice through D | three SOS programs skip 9 of 70 iterations at 70 PEs, as the README and the record say |

## Measurements and methodology

- **The Pi** (the lead's runs, not repeated): at 4 × 4 under Slurm's binding, D
  against the tree before it within 1% on every line but one run before D
  (barrier 175.6-175.7 µs after against 175.6-182.6 before; 8-byte reduce
  197.6-197.8 against 198.3-207.9; 64 KiB reduce 2,387-2,389 against
  2,389-2,398; put stream 112.1 MiB/s both), and the 2026-09-28 records' tree
  measuring the same as D. The README's bound barrier, 176-183 µs, holds
  (175.4-182.6). Its percentages for the slice and for unbinding were measured on
  2026-09-28, when an 8-byte reduce took 211 µs bound with the slice
  (`pi-cluster-results.md:591`) where both trees now take 198: whether the
  percentages still hold is unmeasured - the question the lead has put to the
  author.
- **The VM**: the floor reproduces the README on a quiet host (above, S3); the
  two sessions agree on every ratio; the VM shows nothing about 256 PEs a node on
  real cores (T1's counts do not depend on cores; minor 5's contention does).
- **Not measured by anyone**: a node of more than 32 PEs on real cores; more than
  254 logical nodes; a link faster than gigabit; a split at the design's 500
  nodes (the node-set walk above is a copy of the loops, not the library).

## For step E (kept apart)

E's brief (`c306221`, after this tree) builds the proposal's §4. Three things it
would get wrong or leave unseen:

1. **The pool, beside 256 numbers.** T1 above: E10 raises a PE's numbers to 256
   and leaves the node's pool at 1,024, so for any team that shares none of a
   node's PEs - columns, local-rank families - the pool, not the numbers, is the
   limit at any node of more than four PEs, and the README's 253 teams a PE
   after E will not be what a 256-PE node gets. The brief leaves the pool's default
   alone and defers only teams of one; the column case needs a decision in it.
2. **The measurements skip the band where E costs most.** By the proposal's own
   simulation (`grant_timing.py`, *documented*) a large reduction pays up to a hop,
   once, and the whole hop when the leaf's intranode time is small - on five
   nodes or fewer, the Pi's four. Over 64 pieces the hop is amortized; on one
   piece, a reduction of more than 1 KiB and up to 16 KiB, it is not. The brief's
   measurements (8 B, 1 KiB, 64 KiB and 1 MiB, Part 2 item 4) and `osoe-bench`'s
   lines (8 B and 64 KiB) have nothing in that band, so its stop-and-ask - "the
   Pi showing more than the one hop on any line" - cannot fire there. On the Pi
   at 4 × 1 a reduce takes 228 µs at 1 KiB and 450 at 8 KiB
   (`pi-cluster-results.md:140`), and a hop is about 80 µs (half the echo's 158):
   a fifth to a third more (*arithmetic*). NPB IS sums 1,029 ints - 4,116 bytes -
   every iteration (`corpus/openshmem-npbs/C/IS/is.c:607`). A baseline on D now
   (*measured*, `q3d-band`, quiet VM, slowest PE's median): a reduction of 1, 2, 4,
   8 and 16 KiB took 194, 210, 209, 225 and 233 µs at 4 × 1 and 310, 320, 325, 333
   and 358 at 4 × 4, against 181 and 311 for 8 bytes. *For the lead, on the Pi*:
   build `/tmp/q3d/probe/q3d-band.c` with `oshcc` on D's tree and run `srun -K1
   -N4 --ntasks-per-node=4 ./q3d-band` three times, and again on E's, alternating;
   E holds its claim if the 8-byte and 32 KiB lines do not move and the 2-16 KiB
   reductions move by at most about 80 µs. The small slot's default (E2, the
   author's dial) is best chosen from those lines: at 4 KiB a number would cost
   about 44 KiB a PE where it costs 15 at 1 KiB (*arithmetic*, E2's layout).
3. **The lanes' cost in the README** (the previous round's note on E9): the brief
   now measures the lanes against E9's 2.7 MiB a PE; the `OSOE_MAX_THREADS` row,
   which operators of many-core nodes are told to raise, should give that cost
   too.

## What is sound, and what convinced me

- **Nothing D added grows with the job's PE count**: every array and walk it
  added is sized by the node's PEs, the node's pool or a team's own members, read
  in full; *measured*, the lead's 540 KiB more at 16 and at 63 nodes alike, the
  non-leads unchanged.
- **A split costs what the row says**, from one node to 128 logical nodes, at
  both thread levels and at 1,024 numbers, with the ratio to a reduction and a
  barrier the same in a loaded and a quiet host.
- **Giving back is complete**: memory flat over 40,000 cycles of teams made, used
  and destroyed, blocks cycling through a pool of eight.
- **The per-node split works at any node count**: 254 logical nodes each got
  their team, where round three found 61.
- **The posted bitmap** costs one load a pass at the default pool and sixteen at
  the largest, which answers the previous round's note on D's brief.
- **D moved nothing it should not have**: the Pi's lines within 1% (the lead's
  runs), the VM's floor and stream at the README's figures on a quiet host, the
  previous round's fixes in this area all standing.

## Sources

`README.md` and the code at `d8a651d` (cited by line above); design §3.11, §5.2,
§5.7; `docs/design/team-resources-proposal.md`; `docs/prompts/phase1-step7-slice3-prompt.md`;
`docs/decisions/2026-10-03-team-storage.md`, `2026-10-03-team-allocation.md`,
`2026-10-04-review-fixes.md`; `docs/prompts/phase1-step7-slice4-prompt.md` at
`c306221` (the section on E only); `docs/pi-cluster-results.md`; the previous
round's `phase1-release-fixes-scale-and-performance.md` and
`phase1-release-fixes-review.md`; the lead's `build/scratch/merge-d/main/verify.txt`
and `build/scratch/merge-d/pi/`; `corpus/openshmem-npbs/C/IS/is.c`; in the `osoe`
VM's `/tmp/q3d`: the probes (`probe/q3d-split.c`, `q3d-pool.c`, `q3d-churn.c`,
`q3d-maps.c`, `q3d-nodeset.c`, `q3d-gen.c`, `q3d-band.c`, and the previous
round's `q3-split.c` and `q3-start.c`), the scripts (`build.sh`, `common.sh`,
`runS.sh` split cost, `runP.sh` pool, `runC.sh` churn, `runF.sh` footprint,
`runH.sh` per-node split, `runN.sh` node set, `runG.sh` generation limit,
`runT.sh` floor, `runB.sh` band) and every output (`out/`; the loaded session's
split runs in `out/loaded/`, its floor runs as `floor-r*-loaded.out`); the
previous round's outputs in `/tmp/q3/out` for the start-up comparison.
