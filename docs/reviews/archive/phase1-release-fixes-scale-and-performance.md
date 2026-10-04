# Release fixes, question 3: does osoe scale, and perform, as the README says?

Review of `main` at `08b3da0` (the release fixes, Parts 1 to 6, merged), run
2026-10-03 against `docs/reviews/adversarial-review-prompt.md`, Question 3, with
the lead's changes: the previous round (`phase1-release-scale-and-performance.md`,
at `b0e0b53`; its findings P1-P10 and minors 1-10) is the baseline, its fixes
are checked as claims, and the team redesign (`docs/design/team-resources-proposal.md`,
`docs/prompts/phase1-step7-slice3-prompt.md`) is not built. One investigator;
the lead runs the refutation pass. Every claim is labelled *documented*,
*observed in source*, *measured* or *recalled*.

**How it was checked.** Source read at `08b3da0` in the read-only worktree
`build/scratch/wt/review-fixes`: the diff from `b0e0b53` (2,802 lines added in
the library, `base/`, the launcher, the probe and `osoe-stat`) read whole where
it touches start-up, global exit and the transport (`listener.c`, `wire.c`,
`abort.c`, `liveness.c`, `progress*.c`, `connection-area.c`, `node.c`'s
segment and capacity check), and every allocation and loop it adds elsewhere
checked for what sizes it; every allocation in `shmem/src` at `08b3da0` checked
the same way. Executable evidence in the `osoe` VM (Debian 13, Linux 6.12, aarch64, 4
vCPUs, 8 GiB), every build and run under `/tmp/osoe-vm.lock`: the release preset's
library, `osoe-launch`, `osoe-bench`, `shmem_test_floor` and `osoe-stat`, built
from a copy of the tree in the VM's `/tmp/q3/src`; a second copy,
`/tmp/q3/src-wake1`, with one line of `connection.c` changed for S1's
experiment; and ten probe programs of this review's (`/tmp/q3/probe/q3-*.c`:
`exit`, `start`, `split`, `deadlock`, `herd`, `herd2`, `a2a`, `pipe`, `pipe2`,
`wakecost`), run under `osoe-launch` over up to 254 logical nodes, their outputs
in `/tmp/q3/out/`. The two copies of the tree are removed; the probes, the run
scripts (`/tmp/q3/run*.sh`, `common.sh`, `build.sh`) and their outputs stay in
the VM's `/tmp/q3` (1.4 MB), and the sources also in this session's scratchpad
on the host. To run them again: copy the tree to `/tmp/q3/src` as the brief
says, then `bash /tmp/q3/build.sh`, then a `run*.sh` (`runW.sh` makes the
wake-one copy, which `runX.sh` then uses). Nothing else was written in the repository. The README's numbers rest on the finished check
`build/scratch/release-fixes/review-briefs/readme-numbers-check-08b3da0.md`
(every number against its record and raw files; none contradicted, six on
records alone), which this review relied on and spot-checked - the Pi's floor
raw file (`docs/design/evidence/pi-cluster/2026-09-28-review/03-floor-1.txt`:
172.7-175.5 µs against 157.9-158.8) and the VM's (`docs/design/evidence/release-fixes/floor-idle.txt`) -
rather than redid.

**What the VM's timings are worth.** The host (8 cores) ran the other review
VMs at the same time - load average 3.5 to 15, two VMs at 190-280% CPU during
the first half of the runs - which the `osoe` lock does not cover; Lima's guest
agent took up to a core for some seconds after each job of hundreds of
listening ports; and a stray process from an earlier review (`completion-probe`,
PID 649886, eleven days old, from `build/scratch/review-conformance/`) took 4-9%
of a core throughout. Counts - wakeups, statuses, lines, descriptors, bytes -
are unaffected; absolute times are upper bounds, and where a time carries a
conclusion it is a median of many calls, with series alternated.

## Verdict

**The README's scaling claims are true of the code, the previous round's
fixes in this area hold, and the start-up and global-exit code Parts 2 and 3
rewrote scales to the 254 logical nodes one machine can hold - further than
any test goes. What the table does not say is how two of the library's
mechanisms behave beyond the Pi's four PEs a node and its gigabit link.** Every
row that states a cost matches the code (*observed in source*) and, where one
machine can show it, the measurement: a PE that is not its node's lead keeps
the same private memory at 16 and 63 nodes, a lead grows by 3.5 KB a peer, and
nothing grows with the job's PE count; 61 nodes of 62, 64 and 100 get a
node-local team, exactly as the Limits say. New: a reopened connection buffer
wakes every writer waiting for it, though at the defaults it has room for one
64 KiB message (S1) - the thundering herd at 256 PEs a node that design §3.11
itself rules out, measured growing linearly in the writers and costing the
progress thread, on this VM, 0.7-0.9 ms a wake at 255 sleepers, more than a
64 KiB message takes on the gigabit link; and a large collective's window of
two 16 KiB pieces an edge, at the default pipeline depth, bounds it on links
faster than gigabit as much as the link does - over the VM's loopback, alone (S2).
**Nothing here blocks the release.** S1 wants a README sentence before the tag
and its small code fix soon after; S2 a sentence and a measurement on a faster
link; the rest are minor.

## Findings, ranked

### S1. Every reopening of a connection's buffer wakes every writer waiting for it, though at the defaults it holds one 64 KiB message: O(PEs) of wakes on the node's one progress thread per 64 KiB

- **Location**: `base/wait-word.h:70-76` (`osoe_wait_word_wake_all`:
  `futex_wake(..., INT_MAX)`); `shmem/src/connection.c:95-123`
  (`osoe_connection_open` wakes `turnover_wait` on every reopening, `:122`);
  `shmem/src/transport-am.c:671-703` (`reserve_waiting`: a writer with no room
  sleeps on the turnover word, and retries when woken), `:714-727`
  (`payload_limit`: a message is at most `OSOE_AM_CHUNK`) and
  `connection.h:360-363` - with the default buffer of twice the chunk less its
  segment header, a reopened buffer holds one chunk-sized message. The trigger
  in the library's own code: `shmem/src/collectives.c:522` (alltoall visits
  `(me + 1 + step) % members`, so at each step a node's PEs all target the
  same one or two nodes). Design §3.11 (lines 1050-1056): buffer turnover
  "divisible ... → wake many", with the example "192 KB free, 30 waiters
  needing 200 B each", against "indivisible → wake one. Waking more is a
  thundering herd; at 256 local PEs, 255 threads wake and immediately
  re-sleep". README, the table's waiting row ("O(1) a waiter", line 452) and
  Limits, "One thread a node moves every message between nodes" ("a node's
  message rate and its bandwidth to other nodes are one thread's, whatever its
  PE count", lines 658-674).
- **Scenario**: the PEs of a node put 64 KiB or more each toward one other node
  at once - an alltoall of 64 KiB blocks at any step, every PE sending its
  result to PE 0, a halo whose neighbours share a node. Each reopening admits
  about one writer, and wakes every writer then asleep: the progress thread
  makes one `futex_wake` of all of them, each wakes, fails to reserve, and
  sleeps again. §3.11's divisibility holds for small messages; for chunk-sized
  ones the room is indivisible, and the design's own rule says wake one.
- **Evidence**:
  - *Measured*, the stream (`q3-herd.c`: W writers on logical node 0 put 1 GiB in
    all, 1 MiB a call, to node 1's one PE; default chunk and buffer; the
    shipped library against `src-wake1`, whose `osoe_connection_open` wakes one
    writer instead of all - an experiment only, not a proposed fix; the two
    alternated, three series):

    | writers on the node | 1 | 16 | 32 |
    |---|---|---|---|
    | writers' sleeps per 64 KiB, shipped | 0.75, 0.76, 0.78 | 8.39, 9.07, 8.74 | 19.4, 15.8, 22.8 |
    | the same, waking one | 0.79, 0.77, 0.78 | 1.00, 1.00, 1.00 | 1.01, 1.01, 1.01 |
    | progress thread's CPU per GiB (ms), shipped | 210, 200, 200 | 170, 190, 180 | 220, 210, 240 |
    | the same, waking one | 190, 200, 200 | 170, 170, 170 | 180, 180, 170 |
    | stream (MiB/s), shipped | 2,889, 3,048, 3,246 | 4,262, 4,455, 4,341 | 3,152, 3,333, 2,534 |
    | the same, waking one | 3,287, 3,083, 3,168 | 4,338, 4,224, 4,273 | 3,748, 3,757, 4,206 |

    An earlier series of the shipped library at 1, 2, 4, 8, 16 and 32 writers
    gave 0.9, 1.3-1.5, 2.2-2.6, 4.1-4.3, 7.5-8.5 and 10-19 sleeps per 64 KiB:
    about W/2, every turnover waking the writers then asleep. The sleep counts
    are robust; the CPU and stream rows move with the host's load (a later
    pair at 16 writers gave the shipped library 3.3 GiB/s and 230 ms a GiB and
    the experiment 1.7 GiB/s and 310), and only at 32 writers did the shipped
    library fall behind in every series. With the Pi's shape, two nodes of
    four PEs and node 0's four writing (`q3-herd2.c`, below): 2.08 sleeps per
    64 KiB shipped, 1.00 waking one.
  - *Measured*, the library's alltoall (`q3-a2a.c`, `shmem_alltoallmem` of 64 KiB
    blocks on two logical nodes, ten calls, both libraries alternated, two
    series): sleeps per 64 KiB that crossed between the nodes 1.85-2.15, 4.1-4.8
    and 8.7-11.7 at 2, 8 and 16 PEs a node, shipped; 1.8, 1.3-1.4 and 1.17
    waking one. At 16 PEs a node (32 in all) an alltoall took 8.6 and 9.2 ms
    shipped, 7.9 and 8.7 waking one.
  - *Measured*, what one such wake costs its caller in this VM (`q3-wakecost.c`:
    N processes asleep on one shared word, the parent's `futex_wake(INT_MAX)`
    timed, median of 30, two series): 11-17 µs for 1 sleeper, 122-230 µs for
    32, 215-384 for 64, 360-677 for 128 and **678-933 µs for 255**. At 256 PEs a
    node the progress thread would spend more on one turnover's wake than a 64
    KiB message takes on the Pi's gigabit link (about 520 µs), and more than
    ten times what it takes at 10 Gbit/s. Bare metal will be cheaper than this VM; the
    shape (linear in the sleepers, on the one thread that moves every message)
    is the kernel's.
  - *Not measured*: 256 PEs on one node, which needs a host with that many
    cores. The 4-vCPU VM understates the herd (33 processes cannot all run to
    re-sleep) and overstates a wake's cost.
- **Severity**: **fix before release** for one README sentence - in Limits, "One
  thread a node...": "and when many PEs of a node send large messages to one
  other node at once, each buffer's turnover wakes all of them, so the
  thread's own rate falls as they rise" - since the paragraph's "whatever its
  PE count" says otherwise and the design's 256-PE target is the README's
  first paragraph. **After release** (the first thing after the tag, or with the
  team work if the author prefers) for the code: wake one writer per
  reopening and have a writer that reserved and left room wake the next - a
  change to a modelled protocol, so `wait_word_model.py` first - or wake one
  when the waiters' messages exceed half the buffer; and visit an alltoall's
  destinations so that a node's PEs spread over nodes (start each PE its local
  id × members / L further on). Correct §3.11's example in the design.
- **Confidence**: high on the mechanism and on its growth to 32 writers (the
  one-line experiment removes it); medium on its magnitude at 256 PEs a node.

### S2. On a link faster than gigabit, a large collective is bounded by the default pipeline's window of two 16 KiB pieces an edge, not only by the link

- **Location**: `shmem/src/internal-region.h:72` (a piece is
  `OSOE_INTERNAL_SLOT_BYTES`, 16 KiB, a constant) and its layout comment (each
  tree edge has a row of D slots each way); `shmem/src/bootstrap.h:111`
  (`OSOE_COLLECTIVE_PIPELINE_DEFAULT` 2); README reference row (line 865: "Pieces
  of a data collective each node keeps in flight up and down that tree") and
  Limits paragraph "Where the design admits a limit of its own" (lines
  456-468).
- **Scenario**: a 1 MiB broadcast or reduction is 64 pieces, and an edge of the
  tree carries D of them at a time, so it moves about D × 16 KiB per
  application round trip whatever the link can take. On the Pi that is more
  than the link (32 KiB per 170 µs is 190 MB/s, against 117), so D = 2 was right
  there. At 10 Gbit/s the two bounds meet (*arithmetic*, with *recalled* round
  trips of 60-90 µs for a piece's cycle on such a link: 360-550 MB/s an edge
  against the root's link split four ways, about 300); at 25 Gbit/s and over
  the VM's loopback the window binds alone, and raising D or lowering k
  helps only together.
- **Evidence** (*measured*, `q3-pipe2.c`: four logical nodes of one PE over the
  VM's loopback, where the library streams 1.1-4.9 GiB/s; the median of 50
  calls of the slowest PE's time; two medians where the configuration ran
  twice):

  | 1 MiB | k=4, D=1 | **k=4, D=2 (defaults)** | k=4, D=4 | k=4, D=8 | k=2, D=1 | k=2, D=2 | k=2, D=4 | k=2, D=8 |
  |---|---|---|---|---|---|---|---|---|
  | broadcast (ms) | 13.7 | **7.4, 6.5** | 6.2 | 5.6 | 24.9 | 8.7 | 6.7 | 3.3 |
  | reduce (ms) | 15.8 | **11.2, 8.8** | 8.4 | 5.4 | 26.4 | 15.1, 8.1 | 10.0 | 4.9 |

  At the defaults a 1 MiB broadcast moves about 150 MiB/s on a link that streams
  more than a gigabyte a second; a fan-out of 2 at D = 8 is about twice as fast
  - the Pi's direction (README: 1.5 times at 1 MiB, at D = 4 to 8), and further.
  The arithmetic agrees: 32 windows of two pieces at about 200 µs each is the
  6.5 ms measured. A first series timed as means of 20 back-to-back calls
  spread by 2-10 times between identical runs a minute apart (host
  contention), so only the medians are used. The README gives the Pi's figure
  and says no faster link has been measured; it does not say that the depth
  bounds a large collective, nor how large a piece is (the previous round's
  minor 8).
- **Severity**: **after release**, with a sentence in the reference row now if
  wanted ("each edge of the tree carries D pieces of 16 KiB at a time, which on
  a link faster than gigabit bounds a large broadcast or reduction; raise it
  there"); the default wants a measurement on a faster link, by the project's
  own rule for speed claims.
- **Confidence**: high that the window binds over the VM's loopback (two series,
  and the arithmetic); medium on the numbers, which the host's load spreads,
  and on where it starts to bind on real links, which nobody has measured.

### S3. The README's round-trip ratio is the Pi's, and the library's overhead is roughly fixed, so the ratio does not travel (minor; documentation)

- **Location**: README, "How osoe is tested", the Pi paragraph (lines 813-825:
  "within 9 to 11% of the echo"), and `shmem.floor`'s description (lines
  792-805).
- **Evidence**: the Pi: 172.7-175.5 µs against an echo of 157.9-158.8, so 14-17 µs
  added (*documented*, raw file above), on a floor inflated by the NIC's 49 µs
  interrupt coalescing (the previous round's methodology note); the VM's
  loopback: 54-56 µs against 27-28, 1.95 times (*documented*,
  `floor-idle.txt`, five runs), and 1.92-1.93 times today (*measured*, three
  runs of `shmem_test_floor`). On a 10 Gbit/s network with a round trip of
  about 25 µs a reader should expect about twice the echo, not a tenth more.
  The README's stream ratio over loopback ("a fifth") also varies with the
  host: 0.096-0.118 today in `osoe` under the host's load, against 0.20-0.21
  in `osoe-load`'s committed run.
- **Severity**: minor (one sentence: "about 15 µs more than the echo on the
  Pi, 27 µs over the VM's loopback - twice its floor"). **Confidence**: high.

### Minor

1. **No test runs past 16 logical nodes** (`shmem.work-discovery`'s
   `-n 16`; *observed in source*, every `-n` in `shmem/tests`). This review's
   runs at 64-254 nodes (below) took seconds each; a tier-2 test of a global
   exit and of a proved deadlock at 64 nodes would keep Parts 2 and 3 honest
   at the scale they were written for. After release.
2. **`bootstrap.h:72-77`** still says each PE's internal region is "about 160 MiB"
   at 1,024 team numbers; it is 203.5 MiB (the previous round's minor 3, left in
   this one comment; the README's row is right).
3. **F1's lower bound**: the README's "0.5 to 52 s" stalls; the probe prints only
   each size's maximum, and the smallest maxima over 100 ms are 0.2 s
   (numbers check, 9a). "Up to 52 s" says what the raw files hold.
4. **The VM's own hygiene**, for the lead rather than the README: the stray
   `completion-probe` above has run in the shared `osoe` VM for eleven days.

## The previous round's findings, checked as claims

| | The finding at `b0e0b53` | What the fix claims | At `08b3da0` |
|---|---|---|---|
| P1 | team numbers job-wide; a node-local split spends one a node | Limits, "Teams": 61 nodes get their team, the rest `SHMEM_TEAM_INVALID` and nonzero, which nodes a race, collectives on it no-ops, status 0; temporary | **holds**, *measured* (`q3-split.c`): 61 nodes all split; at 62, 64 (× 2 PEs) and 100 nodes exactly 61 get a team, the rest's sync and reduction return nonzero, the job exits 0, and the losers differ by run (PEs 58, 62, 116, 124 named) |
| P2 | one progress thread; "line rate" only at 1 Gbit/s | Limits paragraph with loopback figures | **holds**, with S1 added to what it bounds; the loopback figures are one machine's (S3) |
| P3 | the budget cannot shrink at the defaults; notice per PE | "at most 1,025 nodes"; notice once a node | **holds**, *observed in source* (`connection-area.c:224-235`, `local_id == 0`); `test-am-size.c` pins 1,025 and 1,026 |
| P4 | intranode unmeasured past 4 | tested to 15 a node; barrier timed to 32 | **holds**; *measured* again: the one-node barrier's median 13-18, 26-44, 38-56, 68-70, 105-116 µs at 2, 4, 8, 16, 32 PEs (three series, `osoe-bench barrier`) - linear, and 2.5 times §5.3's 42 µs at 32 under today's load |
| P5 | TCP buffers left to autotuning | after release | unchanged, as recorded: no `SO_SNDBUF`, `SO_RCVBUF` or `TCP_NOTSENT_LOWAT` in `shmem/src` or `base/` |
| P6 | `collect`'s O(P) lengths | the row | **holds** (O(N) bytes, 1 MB at 128,000 PEs) |
| P7 | per-context ordering state O(n) | the row | **holds**: 21 bytes = 8 + 4 + 1 + 8 a peer a context, and a bit (`transport-am.c:99-108`) |
| P8 | heaps on 4 KiB pages; design said hugetlbfs | design corrected | **holds** (design §4.1's costs, line 2384: "designed and is not built") |
| P9 | the barrier's figure holds only bound | 176-183 bound, 196-208 unbound, the trade stated | **holds** (numbers check, 3c) |
| P10 | the slice's "no difference" false | both sides in the row | **holds** (numbers check, 10a-c; "1.3" rests on the overnight record alone) |
| minor 1 | the mesh's n - 1 connections, serial | the start-up row | **holds**; *measured*: `shmem_init` 0.04, 0.06, 0.15, 0.28 and 1.1-1.2 s at 2, 16, 64, 128 and 254 logical nodes on four vCPUs (CPU-bound at the end: 32,131 connections on one machine) |
| minor 2 | the large reduction's bytes | Limits paragraph | **holds** (k + 1 vectors from the busiest lead) |
| minor 3 | a number's 208 KiB; scans touched by all members | reference row | **holds** in the README; one stale comment (minor 2 above) |
| minor 4 | scans' per-node traffic O(L) under cyclic | - | left, as minor |
| minor 5 | active-set collectives per PE | design goals | **holds** |
| minor 6, 7 | decisions index; design §4.11 | corrected | **hold** (index line 73; §4.11 line 4185) |
| minor 8 | fixed constants that read like settings | - | left; S2 is where it costs a reader |
| minor 9 | listen backlog against `somaxconn` | - | moot on the kernels osoe accepts (`somaxconn` 4,096 since 5.4); 254 nodes started with a backlog of 254 |
| minor 10 | "7 s" on one run | - | **holds** (numbers check: 6.8-6.9 s, three runs) |

## What Parts 2 and 3 changed, at scale

*Measured* with `osoe-launch` on one machine, the default chunk lowered to 1 KiB
and the heap to 4 MiB so that 254 nodes fit the VM's `/dev/shm`:

- **A global exit's caller leaves last** (`q3-exit.c`: every other PE asleep in a
  barrier the caller never enters; the caller calls `shmem_global_exit(7)` 300 ms
  after the first; the launcher without `-k`, which ends the rest at the first
  bad exit, as `srun -K1`): 16 × 2 (caller rank 9), 64 × 2 (65), 127 × 2 (129),
  254 × 1 (128 and 0), 2 × 127 (200 and 0) and 1 × 200 (150 and 0) all
  returned 7, wrote every PE's buffered line, ran the caller's handler once,
  and had no rank killed; each ended 0.03-1.0 s after the call. The goodbyes
  relayed among 254 nodes, the carrier's lead's wait for 253 ends of stream,
  and the carrier's pidfd batches past 64 PEs (`liveness.c`'s
  `OUTLAST_BATCH`) all fit the 20 s and 30 s bounds with room.
- **A proved deadlock** (`q3-deadlock.c`, every PE waiting for a word nobody
  writes, `OSOE_DEADLOCK_SECONDS=2`): ended with status 1 on every PE 3.1, 5.3
  and 5.4 s after the wait began at 4, 64 and 254 nodes, each node naming where
  its PE waited (952 lines of standard error at 254 nodes; at 500 × 256 that
  report would be some 128,000 lines).
- **Start-up with `OSOE_JOB_SECRET`** (the proof before the claims; callers kept
  in a set of 16 per listener, the rest in the backlog): 254 nodes started as
  fast with the secret as without (`shmem_init` 1.09 against 1.19 s at the
  slowest PE); node 253's mesh listener took 253 callers through the set.
- **Footprint against the node count** (`q3-start.c`, one PE a node unless
  said): a lead's private memory grows by about 3.5 KB a peer (RssAnon 244 KB at
  2 nodes, 464 at 64, 688 at 128, 1,132 at 254), its descriptors are n + 6 (8,
  22, 70, 134, 260), and a PE that is not its node's lead holds about 190 KB of
  private memory at 16 and at 63 nodes of four PEs. *Observed in source*: the
  new code allocates by the node's PE count (`local_rank`'s walk of the
  node's ranks, `osoe_liveness_outlast`'s batches of 64 on the stack) or by the
  node count (`osoe_progress_peers_ended`, O(n) a pass), and never by the job's.

## The README's table, row by row

| Row | At `08b3da0` |
|---|---|
| put, get, atomic to a node-mate: no system call, a futex wake if the target waits | holds (unchanged code; the previous round's check) |
| across nodes: the connections' state O(n) a node; ordering state O(n) a context, 21 bytes a node | holds, *observed* and *measured* (above) |
| barrier, sync, small collectives: ⌈log_k n⌉ steps | holds (unchanged code) |
| larger data collectives: pieces pipelined, two in flight; O(P × bytes) to combine | holds as to steps and work; the window of D × 16 KiB an edge bounds their throughput on a fast link (S2) |
| `fcollect`: ⌈log₂ n⌉ steps | holds |
| `collect`: the lengths O(N) bytes up and down | holds |
| prefix scans: O(log N) rounds | holds; per-node traffic O(L) under cyclic layouts left (minor 4 of the last round) |
| `alltoall`: O(N) a PE | holds; its order puts a node's PEs on one or two connections at each step, S1's trigger |
| a team: O(P) a member, O(M) on each lead once, S leads kept | holds (`team.c` `build`, the node set); the temporary limits measured exactly as stated |
| waiting: O(1) a waiter | true of each waiter; the waker's cost on a connection's turnover is O(waiters), on the progress thread (S1) |
| start-up: ⌈log₈ n⌉ levels, then n - 1 connections a lead | holds; measured to 254 nodes |
| `SHMEM_THREAD_MULTIPLE`: O(1) an operation | unchanged since the last round (question 2's) |

## Defaults

| Setting | Default | Justified by | On hardware unlike the Pi |
|---|---|---|---|
| `OSOE_COLLECTIVE_FANOUT` | 4 | the Pi: 4 twice as fast as 2 to 1 KiB; 2 faster at 1 MiB with a deeper pipeline (`docs/pi-cluster-results.md`) | safe; on the VM's loopback 4 and 2 are close at D = 2, and the large-size choice is the depth's (S2) |
| `OSOE_COLLECTIVE_PIPELINE` | 2 | the Pi at D = 1, 2, 4, 8 and a timing simulation | safe, but it caps a large collective near D × 16 KiB a round trip: S2 |
| `OSOE_GET_LANDING_BYTES` | 256 KiB | the Pi: the sort's pull 5.61 s a round at the default, 4.85 at 1 MiB, 4.26 at 4 MiB (`pi-cluster-results.md:494-496`); the size is a memory trade, 256 KiB × L of the control segment | safe (backpressure, never an error); it bounds a PE to 256 small gets in flight; not a bandwidth cap at 10 Gbit/s for large gets (256 KiB a round trip) |
| `OSOE_AGENT_SLICE_US` | 100 | the Pi, both sides measured and now both in the row (D5) | safe: ignored before Linux 6.12; the same weight, only sooner preemption; unmeasured under SMT |
| `OSOE_AGENT_CPUS` | `task` | the Pi: `node` 7-18% slower on small collectives | safe; `node` on two sockets unmeasured |
| beside them: `OSOE_AM_BUFFER` | 2 × chunk | reasoned (§3.4's floor) | one chunk-sized message a buffer, which is what makes S1's turnover indivisible; raising it trades the 1,025-node ceiling (P3) |

## Measurements and methodology

- **Reproduced here** (VM, host loaded as above): the loopback round trip at 1.92-1.93
  times the floor (record: 1.95); the loopback stream at 0.096-0.118 of the
  bare stream (record: 0.20-0.21 in `osoe-load` "running nothing else"); the
  one-node barrier linear to 32 PEs at 105-116 µs (§5.3 gives 42 µs; the
  committed logs of a contended run, 95-105: numbers check, 4b). The Pi's numbers were not re-run (not this investigator's to use);
  the numbers check found every one supported by its record and raw file but
  six that rest on records (build and model times, §5.3's barrier, the
  `shmem_ptr` round trip, the VM's SOS count, the overnight mailbox rates).
- **Methodology**: (1) S3 - ratios against a floor travel only with the floor;
  the absolute overhead is the portable figure. (2) The VMs share a host the lock
  does not cover, and `floor-idle.txt` records its VM's load average but not the
  host's: a loopback figure in the README should name both, or be taken with
  the other VMs stopped. (3) `osoe-bench` times collectives on PE 0 alone
  (the last round's note, unchanged): `q3-pipe2.c`'s slowest-PE time per call is
  the measure S2 needed. (4) Nothing measures a node with more PEs than cores
  beyond 32, or more than four PEs on real hardware: S1's magnitude at 256 is
  extrapolated from a per-wake cost measured here.
- **A run for the lead** (the Pi, not this investigator's): S1 at four PEs a node
  is small (three waiters), but its shape shows. `/tmp/q3/probe/q3-herd2.c` in
  the VM (writers are the PEs on PE 0's node; the target is the last PE), built
  with `oshcc` on the Pi and run as `srun -K1 --cpu-bind=none -N2
  --ntasks-per-node=4 ./q3-herd2`: confirmed if the line's
  `switches_per_64KiB` is near 2 (the VM's 2 × 4 gave 2.08), refuted if it is
  near 1 (the one-line change's 1.00). Not needed for the README sentence,
  which rests on the mechanism.

## The proposal and step D's brief (kept apart)

- **D and E touch neither S1 nor S2**: the transport's waits and the pipeline's
  piece are outside both (the brief's "Not in this slice"). The proposal's "nothing
  on any hot path" holds by reading: a collective's arrive and release still go
  through a pointer, and the engine reads the map once per team.
- **D, Part 1 item 6**: "a bitmap of posted blocks behind one summary word, so
  that a pass with nothing posted costs one load". One 64-bit word summarizes
  64 words, 4,096 blocks; `OSOE_TEAM_BLOCKS` goes to 65,536. The brief should say
  that a summary bit covers several words above 4,096, or that the summary has
  two levels. Minor.
- **E9, a lane per thread record**, ties `OSOE_MAX_THREADS` to each PE's internal
  region and so to the `/dev/shm` check: about 160 KiB a thread at k = 4, D = 2 -
  2.7 MiB a PE at the default 16, 160 MiB at the maximum 1,023. The previous
  round told operators of many-core nodes to raise `OSOE_MAX_THREADS` for
  OpenMP; once E lands the README's row for it should give that cost. Minor;
  for E's brief.
- **D's acceptance case** "the per-node split of `SHMEM_TEAM_SHARED` on 64 logical
  nodes, every node valid" fails today exactly as the README says (above);
  `/tmp/q3/probe/q3-split.c` counts the nodes with and without their team and
  the collectives that returned nonzero, which is the check the case needs.

## What is sound, and what convinced me

- **No state grows with the job's PE count**: every allocation in the changed
  code read, sized by the node's PEs or the node count; per-process footprints
  measured from 2 to 254 nodes, flat but for the lead's 3.5 KB a peer; the
  rank map stays runs in a node-shared object (`rank-map.c`, `job.c`).
- **Parts 2 and 3 scale**: the global exit, the start-up with and without a
  secret, and the deadlock proof, run at 64 to 254 logical nodes and with 127
  and 200 PEs on a node, did what the README says, within seconds.
- **The previous round's ten findings and ten minors in this area**: fixed or
  stated as the record says, each re-read or re-run above; the team limits
  behave to the node as the Limits describe them.
- **The internode trees are what the table says**; their logarithms hold, and
  the one-node barrier is linear to 32 PEs.
- **The README's numbers** match their records and raw files (the numbers
  check, spot-checked), and the README now says what each was measured on.

## Sources

`README.md` and the code at `08b3da0` (cited by line above); design §3.11,
§3.12, §4.1, §5.3, §5.8; `docs/decisions/2026-10-02.md`,
`2026-10-02-release-fixes.md`; `docs/prompts/phase1-release-fixes-plan.md`;
`docs/pi-cluster-results.md`; `docs/design/evidence/release-fixes/floor-idle.txt`,
`docs/design/evidence/pi-cluster/2026-09-28-review/03-floor-*.txt`; the
previous round's `phase1-release-scale-and-performance.md` and
`phase1-release-review.md`; `build/scratch/release-fixes/review-briefs/readme-numbers-check-08b3da0.md`;
the probes and outputs in the `osoe` VM under `/tmp/q3/` (`probe/`, `out/`,
`run*.sh`, `common.sh`, `src-wake1/`).
