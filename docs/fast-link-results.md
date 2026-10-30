# The fast link's results

What `docs/fast-link.md`'s kit measured on a link above a gigabit, session by
session, in the form of `docs/pi-cluster-results.md`. The evidence for each is in
`docs/design/evidence/v0.2-step4/<date>-fast-link/`; the decisions the numbers
feed (A2 to A6, A11) are the author's, after Part 4's sessions.

## The first session (2026-10-12)

**The machines.** Eight Google Cloud `c4-standard-16` - an Intel Xeon Platinum
8581C at 2.30 GHz, 16 vCPUs on eight cores, 60 GB - and a head node,
`slurm-controller` (`e2-standard-4`), in `us-central1-a` under a compact placement
policy, obtained once the project's C4 quota allowed the eight (the first
`terraform apply` placed one node: `CPUS_PER_VM_FAMILY` was 24). Debian 13, Linux
6.12, the gVNIC interface (`gve`) at 23,000 Mbit/s, the VPC's MTU 8896; the TCP
settings a site tuning its nodes sets (`netdev_max_backlog` 250000, the buffer
ceilings at 64 and 128 MiB, `tcp_slow_start_after_idle` 0, `somaxconn` 65535),
which §2's facts record on every node. The tree was `ab4425c`. The author's
Terraform made the machines; the lead reached them through IAP, by the head node.

**Setup (§1, H1).** `slurm-setup.sh` from the head node with `CONTROLLER` set:
`SlurmctldHost=slurm-controller`, `TaskPlugin=task/none`, eight node lines of 16
CPUs and 59,760 MB, and a job of a task a node on all eight. `build.sh`'s first
run left `slurm-node-01` without the tree - its copy skipped the list's first
node, which a head node is not - and the fix went in during the session
(`ab4425c`); then all eight built without a warning. `floor.sh`'s second
direction ran, the first time on two machines.

**The probe and the suite (§3, H2).** The probe passed on every node, thirteen
facilities each; tier 2 passed 764 of 764 on one node in 83 seconds.

**The floor and the bare stream (§4, H3).** The fastest get, put-and-quiet and
fetch-add took 1.48, 1.48 and 1.50 times the floor's round trip (medians of the
pairs); a put-and-quiet or a fetch-add cost 1.02 times a get; osoe's stream ran
at 0.999 of the bare stream's rate. `iperf3` both ways at once summed 5,436 MiB/s
- 2,718 MiB/s a node each way, the egress Google allows a `c4-standard-16` - and
one stream each way already reached it.

**The latency underneath.** `ping` between nodes took 0.27 to 0.30 ms on average,
0.23 ms at best, the same to every node. With the interface's interrupt
coalescing (20 µs receiving, 50 sending) off, the best fell to 0.14 ms and the
average not at all; and osoe's start-up model put a hop at 148 µs on the floor's
nearly idle nodes but at 45 to 52 µs in the sections that kept every PE
running. So a sparse message on an idle node waits on more than the wire - a
vCPU's wake among it - which a site tuning for latency attacks with busy polling
and limits on idle states (open item H27).

**The full verification, native x86-64.** Round nine's verification split into
parts, a part a node at once (`cluster-verify.sh`), each in a copy of the tree:

| Part | Result |
|---|---|
| debug at 0, 2 and 3 helpers | 871 of 871 at each, no warning |
| release with the floor's pins | the static checks 13 of 13; 871 of 871 at each helper count |
| AddressSanitizer | 856 of 856 at each |
| ThreadSanitizer | 612 of 612 at each; the gate passed at each - no report at no helpers, 78 in 24 tests at two and 97 in 25 at three, each in a program's heap or at the internal region's named places |
| tier 3 | 114 of 114 |
| the single-node build | no warning; 405 of 405 |
| `osoe-pacer` | mix, mailbox, collectives and ten minutes of mix at 0 and at 2 helpers: `failures=0`, no deadlock line |
| the models | 27 of 40, every one passed; node 8's half stopped after its seventh at the author's word, the models being Python whose result no machine changes (all 40 passed in the VM in round nine) |

The first five took five minutes a node, where the VM takes some three hours for
all of them; `model.reservation` took 1,113 s here against 811 in the VM.

**Gets and the landing zone (§8, H9).** `multistream get` in 1 MiB blocks at 8 PEs
a node over every node:

| Zone | The job's MiB/s | A node's | Peers in calls | Share of time in quiets |
|---|---|---|---|---|
| 256 KiB | 13,601 | 1,757 | 4.78 | 0.03 |
| 1 MiB | 11,468 | 1,356 | 2.36 | 0.30 |
| 4 MiB | 9,359 | 1,099 | 1.17 | 0.40 |
| 16 MiB | 8,786 | 1,031 | 1.02 | 0.41 |

The larger the zone, the fewer peers a node's PEs pull from at once, and the
slower the whole. `get-pieces`, one PE a node: with a 256 KiB zone a get took
1.58 times a put at 1 MiB and 1.9 times from 4 MiB; from a zone of 1 MiB a get
took what a put did at every size from 1 MiB, 23.7 ms for 64 MiB, the link's
rate. A nonblocking get took what a blocking one did throughout.

**Fan-in (§9, H10).** Every PE of seven nodes putting into node 1's PE 0, MiB/s:

| PEs a node | 0 helpers, 64 buffers | 0 helpers, 256 | 2 helpers, 64 | 2 helpers, 256 |
|---|---|---|---|---|
| 1 | 4,121 | 3,774 | 6,536 | 5,776 |
| 4 | 4,330 | 4,015 | 6,970 | 7,116 |
| 16 | 4,146 | 3,962 | 7,150 | 6,320 |

More than the 2,718 MiB/s a node may send: Google limits what a machine sends,
not what it receives, and here the receiving node's CPU set the rate, two
helpers raising it by 50 to 70%. 256 receive buffers cut the re-arms about five
times (12,527 to 2,182 at one PE a node and no helpers) without raising the rate.

**The ladder (§6, H5).** A fetch-add's round trip behind a stream at the link's
rate, microseconds (the stream at 2,702 to 2,706 MiB/s in every case):

| Setting | Alone, median | Behind the stream, median (p99) |
|---|---|---|
| unsent limit 0, the system's | 85 | 1,960 (2,756) |
| unsent 4 MiB | 85 | 1,376 (1,995) |
| unsent 1 MiB | 85 | 789 (1,019) |
| unsent by the rule (about 417 KB here) | 86 | 618 (787) |
| unsent 256 KiB | 88 | 582 (751) |
| unsent 64 KiB | 85 | 524 (700) |
| connection buffer 128 KiB / 1 MiB / 4 MiB | 86 to 88 | 516 / 747 / 756 |
| receive buffers 16 KiB x 256 / 64 KiB x 64 | 85 / 88 | 586 / 567 |

The rule takes two thirds off the system's delay at no cost to the stream; a
lower limit took a little more off on this link.

**OSU's benchmarks (7.5.2), beside the Pi's release run.** Point to point, two
nodes, a PE each; the collectives here at 8 nodes of 16 PEs (128 PEs), the Pi's at
4 of 4 (16):

| | Here | The Pi |
|---|---|---|
| put, 8 B / 64 KiB / 1 MiB, µs | 80 / 145 / 483 | 174 / 702 / 9,058 |
| get, 8 B / 64 KiB / 1 MiB, µs | 87 / 137 / 699 | 168 / 884 / 11,588 |
| put bandwidth, 1 MiB, MB/s | 2,887 | 121 |
| get bandwidth, 1 MiB, MB/s | 1,454 | 91 |
| fetch-add, µs | 92 | 164 |
| barrier, µs | 279 | - |
| broadcast, 4 B / 1 MiB | 179 µs / 4.4 ms | 191 µs / 27.5 ms |
| reduce, 4 B / 1 MiB | 184 µs / 7.5 ms | 194 µs / 29.3 ms |
| fcollect, 4 B / 1 MiB | 260 µs / 110 ms | 234 µs / 118 ms |
| collect, 4 B / 64 KiB / 1 MiB | 516 µs / 6.0 ms / 111 ms | 404 µs / 7.8 ms / 119 ms |

A get's bandwidth is half a put's at 1 MiB, as §8's 256 KiB zone has it. At 4 B
a collect took twice an fcollect, at 64 KiB 1.05 times: at 128 PEs its lengths
cost a round, not bytes.

**What the numbers point at**, for the decisions to come, not taken here: the
helpers' worth on a receiving node (A2, beside §5's sweep); the unsent rule's
target, which a lower limit beat a little (A4); the landing zone, whose 256 KiB
serves many PEs' gets and whose 1 MiB serves one PE's (A5); 64 receive buffers
enough (A6); and `collect`'s round at small sizes, which P29's smaller vector
leaves as it is (A11, H24 and H25 to measure small sizes).

**Left for the next sessions:** §5's helper sweep (H4), the ladder's rungs 1 and 4
(H6), io_uring's options (H7), the zero-copy send (§7, H8), the cluster at size
(§10, H11), many PEs a node (H12), `collect`'s crossover (H25), the helpers under
a site's binding (H26), latency tuning (H27), and B8's rest - SOS at 4 x 2, the
single-node build on Rocky 9, and the 13 models not run here.

## The second session (2026-10-20)

**The machines.** The first session's again, made anew by the author's
Terraform: eight `c4-standard-16` and the head node in `us-central1-a`, and §2's
facts the same on every node - the Xeon Platinum 8581C, Linux 6.12.111, `gve` at
23,000 Mbit/s, the MTU 8896, the TCP settings, interrupt coalescing at 20 µs
receiving and 50 sending, liburing 2.9, `RemoveIPC=no`, locked memory unlimited
- so that the two sessions' numbers compare. New beside them: each machine's
startup script, the author's, installs the runbook's packages and shares
`/shared` from the head node by NFS, which the kit does not use; and
`kernel.perf_event_paranoid` is 3, so §13's profile needs `PERF="sudo -n
perf"`. The tree was `9929797`, P9 merged.

**Setup (§1 to §3).** About six minutes of work: the packages and the clone in
12 s, `slurm-setup.sh` in 58 s (eight nodes of 16 CPUs and 59,760 MB),
`build.sh` in 29 s (all eight without a warning), the facts, the probe on
every node (thirteen facilities each, the zero-copy send's opcode supported)
and a job of 256 PEs, 32 a node, whose slowest PE started in 110 ms; tier 2
passed 885 of 885 on one node in 117 s. Nine minutes more went to waiting on
the machines' startup scripts by a `pgrep -f` that matched itself; §1 now says
how to wait for them.

**The floor and the bare stream (§4).** As the first session's: `iperf3` both
ways at once 5,436 MiB/s, 2,718 a node each way, which one stream each way
reaches; osoe's stream at 0.999 of the bare stream's rate; the fastest get,
put-and-quiet and fetch-add 1.61, 1.51 and 1.48 times the floor's round trip
(1.48, 1.48 and 1.50 in the first), a put-and-quiet or a fetch-add 1.02 and
1.03 times a get. Seventeen minutes.

**P9: leads claiming team blocks at once (§18).** Every PE of one node splitting
a team of its own at a start PE 0 set, 1,000 times, the tree before P9
(`821a6dfb`) beside it, three rounds - microseconds, the median of the rounds'
medians:

| PEs on the node | Claims a PE | A split, before → after | The last PE done, before → after |
|---|---|---|---|
| 16 | 1 | 1.56 → 1.47 | 2.1 → 1.9 |
| 16 | 8 | 1.35 → 1.33 | 12.6 → 12.3 |
| 32 | 1 | 1.38 → 1.38 | 18.9 → 20.1 |
| 32 | 8 | 1.31 → 1.22 | 40.7 → 40.2 |
| 64 | 1 | 1.50 → 1.50 | 42.6 → 40.1 |
| 64 | 8 | 1.47 → 1.41 | 81.9 → 79.2 |

Where the most claims meet at once - one PE a vCPU each claiming once, and
eight claims a PE at two a vCPU - the change takes about a tenth of a
microsecond off a split, 6 to 7%, the rounds' ranges apart; elsewhere the two
builds match within their ranges, and past one PE a vCPU the scheduler sets
the last PE's time. Nothing costs more, and the 257 leads of the finding are
not to be had on 16 vCPUs.

**H14: the link's measurement on two and three nodes (§19).** The start-up
probe with the link measured and set to what a first job measured, five rounds
- the fastest PE's `shmem_init`, milliseconds, median:

| Nodes | PEs a node | Measured | Set |
|---|---|---|---|
| 2 | 1 | 47.3 | 44.0 |
| 2 | 8 | 55.2 | 49.7 |
| 3 | 1 | 53.9 | 50.5 |
| 3 | 8 | 52.5 | 47.7 |

The measurement costs 3 to 5 ms of a start here (27 ms on the Pi's gigabit).
What it measures is less steady than what it costs: the hop 43 to 53 µs every
time, but the bandwidth 1.9 to 5.1 GB/s from job to job on a link that gives
2.85 - its streams of 128 and 512 KiB too short to time 23 Gbit/s - and every
data wire's unsent limit is set from it; and the message cost 0 on two nodes,
whose root has one child to time it by, and 0 to 159 µs on three, where
Träff's choice reads it.

**H4: the progress helpers (§5).** 180 jobs, three rounds, in 34 minutes - the
whole job's rate over eight nodes, MiB/s, whose ceiling is Google's egress,
8 × 2,718 = 21,744; the median of the rounds:

| PEs a node | Put 1 MiB at 0 / 1 / 2 / 4 helpers | Get 1 MiB at 0 / 1 / 2 / 4 |
|---|---|---|
| 8 | 17,407 / 17,520 / 21,502 / 21,680 | 15,100 / 14,897 / 19,770 / 19,967 |
| 16 | 15,777 / 16,040 / 21,343 / 21,678 | 14,009 / 13,828 / 18,906 / 19,391 |
| 32 | 14,567 / 14,887 / 21,204 / 21,696 | 11,308 / 11,494 / 18,536 / 19,992 |

The 64 KiB put stream as the 1 MiB one, within 4%. Two helpers bring every
case to 97 to 99% of the ceiling, where one thread a node held 67 to 80% of it,
less the more PEs a node; one helper is no helper, every data wire still one
thread's; four add 1 to 2% to the puts and up to 8% to the gets. The progress
threads' CPU a GiB: 0.46 to 0.52 s at none, 0.59 to 0.65 at two, 0.77 to 0.82 at
four.

The collectives, microseconds, at 0 and 2 helpers (4 beside where it differs):

| PEs a node | Barrier | Reduce 64 KiB | Broadcast 64 KiB | Alltoall 8 B | Alltoall 4 KiB |
|---|---|---|---|---|---|
| 8 | 132 → 130 | 453 → 401 | 344 → 313 | 477 → 398 (364) | 1,321 → 1,014 (896) |
| 16 | 146 → 147 | 478 → 439 | 353 → 327 | 760 → 644 (525) | 4,562 → 3,323 (2,917) |
| 32 | 184 → 186 | 566 → 516 | 351 → 345 | 1,493 → 1,133 (898) | 17,314 → 13,210 (10,817) |

The 8-byte reduction and broadcast and the fcollects within a few percent. On
the Pi two helpers made a 64 KiB broadcast and a 4 KiB alltoall slower; here,
unbound, nothing is slower and the alltoalls are a quarter faster.

The mailbox, messages a second, at 0 / 1 / 2 / 4 helpers:

| PEs a node | Messages a second |
|---|---|
| 8 | 920,000 / 916,000 / 1,255,000 / 1,370,000 |
| 16 | 1,213,000 / 1,223,000 / 1,277,000 / 979,000 |
| 32 | 1,371,000 / 1,349,000 / 617,000 / 597,000 |

The one loss: with more PEs than vCPUs, helpers halve the mailbox, and four
already take a fifth off it at one PE a vCPU.

**H8: the zero-copy send (§7).** 48 jobs in nine minutes, at 8 PEs a node, the
send off and on from 16 KiB (the default threshold), 64 KiB and 256 KiB - the
job's MiB/s and the progress threads' CPU seconds a GiB, the median of three
rounds:

| Helpers | Blocks | Off | On, 16 KiB | On, 64 KiB | On, 256 KiB |
|---|---|---|---|---|---|
| 0 | 1 MiB | 17,326, 0.470 | 18,568, 0.439 | 18,500, 0.441 | 18,636, 0.437 |
| 0 | 64 KiB | 16,708, 0.483 | 18,508, 0.443 | 18,665, 0.439 | 18,025, 0.454 |
| 2 | 1 MiB | 21,504, 0.602 | 21,451, 0.612 | 21,456, 0.614 | 21,506, 0.609 |
| 2 | 64 KiB | 21,656, 0.622 | 21,593, 0.643 | 21,652, 0.639 | 21,632, 0.625 |

Where one thread a node is what holds the rate back, the zero-copy send raises
it 7 to 12% and takes 6 to 9% off its CPU; where two helpers already reach the
ceiling it changes neither the rate nor - by 1 to 3%, the wrong way - the CPU.
Every send by reference had its notification, none was copied after all, and
none was refused for locked memory, which is unlimited on these nodes; the
threshold matters little between 16 and 64 KiB. Nothing here asks for a third
buffer: no case stalled behind its notifications.

**H27: latency tuning (§15).** Interrupt coalescing off (`rx-usecs` and
`tx-usecs` 0; `gve` has no adaptive coalescing to turn off, and refused the
flags) and busy polling on (`busy_read` and `busy_poll` 50 µs) on every node,
against the image's settings, three rounds in four minutes, the floor left out
(`FLOOR=0`); every node's settings read back afterwards as the image's:

| Case | The image's | Tuned |
|---|---|---|
| `ping`'s average round trip, ms | 0.229 | 0.211 |
| osoe-bench put and quiet, get, fetch-add, 8 B, µs | 88.6, 87.4, 84.2 | 87.0, 87.1, 86.4 |
| osoe-bench barrier, two PEs, µs | 87.3 | 88.0 |
| hol's fetch-add alone, µs | 86 | 87 |
| hol's fetch-add behind a stream, median (p99), µs | 605 (865) | 587 (932) |
| the mailbox, 8 PEs a node, messages a second | 936,000 | 840,000 |

The tuning takes 8% off a `ping` and nothing off osoe's small operations, and
costs the mailbox a tenth. The likely reason: the two sysctls govern a
socket's blocking reads and `poll`/`epoll`, and osoe's receives go through
io_uring, which polls a device only when a ring registers for it
(`IORING_REGISTER_NAPI`, Linux 6.9 and later) - a code change, not a setting.

**H25: `collect`'s places (§12).** 45 jobs, three rounds - a `collect` over the
world by each way forced and by the default, `link`, microseconds, the median of
the rounds; each node's 8 bytes (`equal`), PE 0's 64 KiB beside the others' 8
(`large`), and every sixteenth PE's alone (`zeros`):

| PEs a node | Lengths | Members | Totals | Scan | Exchange | `link` |
|---|---|---|---|---|---|---|
| 8 | equal | 450 | 440 | 434 | 408 | 412 |
| 8 | large | 616 | 636 | 602 | 575 | 595 |
| 8 | zeros | 426 | 440 | 429 | 411 | 406 |
| 16 | equal | 461 | 433 | 445 | 433 | 424 |
| 16 | large | 648 | 635 | 628 | 594 | 590 |
| 16 | zeros | 439 | 447 | 423 | 418 | 409 |
| 32 | equal | 520 | 471 | 481 | 460 | 454 |
| 32 | large | 692 | 678 | 661 | 650 | 649 |
| 32 | zeros | 524 | 462 | 470 | 442 | 453 |

On eight nodes the leads' exchange is the fastest way in every case and the
default takes it, within the rounds' ranges but at 8 PEs a node with one
large block (595 against 575); the slowest way is within a fifth of the
fastest, the members' vector the slowest at 32 PEs a node. P35's cap of 16 nodes
stands at eight; past eight nodes this cluster cannot say.

**H13: the pipeline's window and the chooser (§14).** 42 jobs, three rounds,
8 PEs a node - a reduction's and a broadcast's microseconds by the chooser
(`link`), by its trees alone (`tree`) and by the best of the twelve fixed
shapes (fan-outs 2, 4 and 8, windows 1, 2, 4 and 8):

| Bytes | Reduce: `link`, `tree`, best fixed | Broadcast: `link`, `tree`, best fixed |
|---|---|---|
| 64 KiB | 515, 507, 489 (fan-out 8, window 8) | 353, 365, 357 (8, 4) |
| 256 KiB | 1,626, 1,625, 1,343 (4, 8) | 1,182, 1,174, 934 (4, 8) |
| 1 MiB | 6,256, 6,499, 4,788 (4, 8) | 4,326, 4,397, 3,359 (4, 8) |
| 4 MiB | 25,125, 25,990, 19,081 (4, 8) | 17,261, 17,571, 12,825 (4, 8) |

From 256 KiB a fixed fan-out of 4 with a window of 8 is 17 to 26% faster than
what the chooser takes, and the chooser within 4% of its trees alone - and
the fixed shapes ran with each data wire's unsent limit at its 16 KiB floor,
since no link is measured under `fixed`, where the chooser's jobs had about
335 KB. The chooser's costs were fitted to the Pi's table (`shape_timing.py`):
on this link they are off, most likely in the window.

**H6, H15 to H19, H26, H28, H11 and B8's rest** did not run: the author's window
closed after H13 (the session ran from 00:27 to 02:13 UTC, about $13).

**What the numbers point at**, for the decisions to come, not taken here: the
helpers' default (A2) - two pay everywhere but in the mailbox at more PEs than
vCPUs, which H26 is to check under a site's binding; the zero-copy send's
(A3), which pays only where helpers do not run; the chooser's costs, refitted
to a fast link; the link's measurement, steadier on a fast link; and
io_uring's own device polling, which a site's latency tuning cannot reach.

## The third session (2026-10-28)

**The machines.** The second session's again, made anew by the author's
Terraform: eight `c4-standard-16` and the head node in `us-central1-a`, and §2's
facts the same on every node - the Xeon Platinum 8581C, Linux 6.12.113 (6.12.111
in the second), `gve` at 23,000 Mbit/s, the MTU 8896, the TCP settings,
interrupt coalescing at 20 µs receiving and 50 sending, liburing 2.9,
`RemoveIPC=no`, locked memory unlimited, `kernel.perf_event_paranoid` 3. The tree
was `2dd3a623`, layout 42 and H23's data wires on demand merged into it, cloned
on the head node from a bundle of `main`. The session was one chain,
`docs/fast-link/session-3.sh` (§21), the author's plan
(`docs/prompts/done/cluster-session-3-plan.md`) section by section, one round each
where the plan named no other, from 22:38:56 to 23:23:05 UTC: **44.1 minutes**
against the plan's 52, every section done, 357 jobs, two of them failed (H11,
below). The machines' start-up scripts were done before it began, so its wait
took 0.1 of its four minutes.

**Setup (§1 to §3).** 4.2 minutes: the packages on every machine at once,
`slurm-setup.sh` (eight nodes of 16 CPUs and 59,760 MB), `build.sh` (all eight
without a warning), the facts, the probe passed on every node, and a job of 256
PEs, 32 a node, whose slowest PE started in 122 ms; tier 2 passed 1,071 of 1,071
on one node in 148 s, T35's litmus, `shmem.quiet.litmus.L2`, among them -
natively on x86-64 for the first time, in 7.1 s (T36). SOS was fetched and built
on the head node meanwhile: 161 of its 163 programs, the C11
`test_all_any_some` and `wait_until_all_any_some` not building, as in the VM's
dry run.

**The floor (§4, `iperf3` alone).** As both sessions': both ways at once 5,423
to 5,431 MiB/s from one stream each way to sixteen, 2,712 to 2,715 a node each
way. Under a minute.

**P38: the link's measurement on two and three nodes (§19).** The start-up
probe with the link measured and set to what a first job measured, five rounds -
the fastest PE's `shmem_init`, milliseconds, the median, and what PE 0
measured, the rounds' range:

| Nodes | PEs a node | Measured | Set | Its hop, µs | Its bandwidth, GB/s | Its message cost, µs |
|---|---|---|---|---|---|---|
| 2 | 1 | 55.3 | 46.2 | 37 to 112 | 1.38 to 2.30 | 0 |
| 2 | 8 | 58.8 | 48.6 | 40 to 98 | 0.95 to 2.78 | 0 |
| 3 | 1 | 70.0 | 51.3 | 92 to 111 | 1.79 to 2.89 | 1.1 to 151 |
| 3 | 8 | 68.8 | 48.6 | 98 to 120 | 0.88 to 5.68 | 0 to 36 |

P38's longer streams and untimed first fan (`docs/decisions/2026-10-20.md`,
"Batch 1") cost a start 9 to 20 ms here, where the second session's
measurement cost 3 to 5, and steady nothing: the bandwidth 0.88 to 5.68 GB/s
from job to job on a link that gives 2.85, as H14's 1.9 to 5.1; the hop 37 to
120 µs, where H14's was 43 to 53; the message cost still 0 on two nodes and 0
to 151 µs on three.

**P37: the pipeline's window and the chooser (§14).** P37's four cases, five
rounds, 8 PEs a node - a reduction's and a broadcast's microseconds by the
chooser at a window of 4 and of 8, by its trees alone at 8, and by the fixed
fan-out of 4 at 8, the median of the rounds:

| Bytes | Reduce: chooser at 4, at 8, trees at 8, fixed (4, 8) | Broadcast: the same four |
|---|---|---|
| 64 KiB | 474, 474, 471, 556 | 350, 358, 359, 416 |
| 256 KiB | 1,620, 1,390, 1,302, 1,302 | 1,148, 949, 945, 940 |
| 1 MiB | 5,962, 5,932, 5,778, 4,651 | 4,377, 3,358, 3,256, 3,310 |
| 4 MiB | 25,525, 26,909, 25,399, 20,202 | 16,917, 12,374, 12,699, 12,690 |

A window of 8 against 4: the broadcasts 17 to 27% faster from 256 KiB, the
chooser's then within 3% of the fixed shape's, which at 4 it trailed by the
second session's 17 to 26%; the 256 KiB reduction 14% faster; the 1 and 4 MiB
reductions within 6% either way, and still 24 to 33% behind the fixed fan-out
of 4; 64 KiB alike.

The chooser's jobs were two kinds again, as in the second session
(`docs/decisions/2026-10-20.md`, "P37, for the author"). By the model -
`shape_timing.py`'s `choose_shape` given each job's measured link - nine of the
ten took the tree of 7 children for every reduction from 256 KiB, and one, the
window of 8's third round, took Träff's: its link read 0.55 GB/s and a message
cost of 21 µs, where the others read 1.78 to 3.02 GB/s and 23 to 45 µs. Its 256
KiB, 1 MiB and 4 MiB reductions took 1,019, 2,479 and 10,459 µs - 1.4, 2.4 and
2.6 times faster than the median of that window's five, the tree's, and 1.3 to
1.9 times than the fixed fan-out of 4 - its broadcasts no faster. At the link's
2.85 GB/s the closed form takes Träff's on eight nodes only while the message
cost reads under 5.5 to 10.5 µs, by the size and the hop (at 256 KiB and a hop
of 100 µs, never); at the 44 µs most rounds read it prices Träff's at 2.5 to
3.9 times the tree.

**H12: many PEs a node (§20).** On two nodes, one round, no helpers: §5's
streams and the mailbox at 16, 32 and 64 PEs a node; the wake's point, 256 PEs a
node on a 16 MiB heap, profiled once; and `OSOE_CHECK_COLLECTIVES` off and on at
each count. The job's MiB/s over the two nodes, whose both ways at once give
5,429; the progress threads' CPU seconds a GiB; the PEs' sleeps a 64 KiB moved
by the puts; the mailbox's messages a second:

| PEs a node | Get 1 MiB | Put 1 MiB | Put 64 KiB | CPU a GiB | Sleeps a 64 KiB | Mailbox |
|---|---|---|---|---|---|---|
| 16 | 3,936 | 4,116 | 4,335 | 0.45 to 0.48 | 4.0 | 697,000 |
| 32 | 3,626 | 3,567 | 3,985 | 0.50 to 0.58 | 8.0 | 697,000 |
| 64 | 3,648 | 3,159 | 3,139 | 0.50 to 0.66 | 16.0 | 372,000 |
| 256 | - | 1,698 | 1,817 | 1.15 to 1.22 | 63.5 to 64.0 | - |

Every writer sleeps once a turnover: the puts' sleeps a 64 KiB are a quarter of
the PEs a node at every count - S1's herd, each turnover's wake of all its
writers - and at 256 the progress threads' CPU a GiB doubles and the job moves
31 to 33% of the link; the profile's progress thread has the kernel's
`_raw_spin_unlock_irqrestore` for its top symbol, 5.8% of the node's samples, 3.7
of them waking PEs from `futex_wake`, the node 58 to 60% idle. The collectives,
microseconds, the check off:

| PEs a node | Barrier | Reduce 64 KiB | Broadcast 64 KiB | Alltoall 8 B | Alltoall 4 KiB |
|---|---|---|---|---|---|
| 16 | 125 | 296 | 200 | 306 | 814 |
| 32 | 157 | 359 | 206 | 504 | 2,741 |
| 64 | 221 | 466 | 227 | 1,203 | 11,129 |
| 256 | 672 | 1,287 | 368 | 13,916 | 262,727 |

`OSOE_CHECK_COLLECTIVES` costs nothing one round can see from 16 to 64 PEs a
node - each collective's time with the check over its time without, 0.84 to
1.07 - and at 256 the same within 3.4% but for the barrier, 672 µs to 975, one
round at 16 PEs a vCPU. The round is `osoe-bench quick`, 200 trips after 200
of warm-up, and the barrier's is mostly a start-up cost, as the tenth review
found (scale S2): a sync is checked by whichever PE of the node arrives last,
and the first time a PE checks one on a team it reads a page of each of its 255
node-mates' regions, a minor fault each, on the barrier's path; in the
development VM, in this shape, the check cost a barrier 3% from the 1,000th on.

**H28: gets and the per-node landing budget (§8).** One round on eight nodes:
`multistream`'s gets of 1 MiB at 1, 4, 8 and 16 PEs a node with the zone unset -
the budget's, which gave 1 MiB at 1 and 4 PEs a node, 512 KiB at 8 and 256 KiB at
16 - beside 256 KiB and 1 MiB fixed, the job's MiB/s:

| PEs a node | The budget's zone | 256 KiB | 1 MiB |
|---|---|---|---|
| 1 | 13,115 | 8,232 | 12,975 |
| 4 | 16,119 | 13,278 | 16,207 |
| 8 | 15,654 | 14,527 | 13,129 |
| 16 | 14,609 | 14,531 | 11,022 |

The budget's zone is the best or within 1% of it at every count: a fixed 256
KiB costs one PE a node 37% and four 18%; a fixed 1 MiB costs eight PEs a node
16% and sixteen 25%, the nodes then unequal (1,553 to 2,257 MiB/s at eight
against the budget's 1,774 to 2,085) and their PEs inside calls toward 2 to 2.6
peers at once, against the budget's 5.5 to 6.4. Where the budget's zone and the
fixed one are the same, 256 KiB at 16 PEs a node, the two jobs ran within 0.5%.
Between two nodes, one PE each, a get's time over a put's: with the budget's
zone 0.95 to 1.00 from 1 MiB to 64 MiB; with 256 KiB, 1.49 to 1.80 past the zone.

**H15 to H19: behind a stream (§13).** One round on eight nodes, 8 PEs a node,
at 0 and 2 helpers. The stream, `multistream`'s puts of 1 MiB: 17,760 MiB/s at
none, 21,256 at two, the progress threads' CPU a GiB 0.46 and 0.58. An
operation's microseconds alone and behind a stream at the link's rate, 2,678 to
2,702 MiB/s (2,306 to 2,374 beside the 64 KiB collectives), median (p99):

| Operation | Alone, no helpers | Behind | Alone, two helpers | Behind |
|---|---|---|---|---|
| Barrier | 99 (422) | 499 (902) | 94 (266) | 121 (263) |
| Fetch-add | 77 (98) | 663 (893) | 72 (93) | 598 (927) |
| Broadcast 8 B | 112 (349) | 710 (1,104) | 113 (286) | 649 (2,006) |
| Reduce 8 B | 123 (409) | 763 (1,186) | 122 (328) | 643 (2,333) |
| Broadcast 64 KiB | 293 (523) | 1,034 (1,708) | 271 (531) | 1,028 (1,495) |
| Reduce 64 KiB | 389 (601) | 1,243 (1,703) | 331 (587) | 1,242 (2,388) |

Behind the stream a fetch-add takes 8.6 times its time alone at no helpers and
8.3 at two, the 8-byte collectives 5.3 to 6.3 times, the 64 KiB ones 3.2 to 3.8;
the barrier alone escapes it with two helpers, 121 µs against 499 at none.
TCP's retransmissions over the stream's window (`nstat`, H18): at no helpers 9
to 46 segments on four nodes, none on the rest; at two helpers, the stream at
Google's egress, 286 to 518 on seven nodes and one on the eighth - fast
retransmits and a few SACK recoveries, one tail-loss probe a node, none timed
out. The progress threads profiled on x86-64 (H19, node 1): at no helpers the
node 89% idle and its progress thread's most the kernel's copies to and from the
sockets (`_copy_from_iter` 2.0%, `_copy_to_iter` 1.3% of the node's samples);
at two the node 81% idle, the copies split about evenly between the helpers.

**H6: the ladder's code rungs on the earlier trees (§16).** One round between
the first two nodes, two PEs each, every tree built there by its own `oshcc` -
`multistream`'s puts, the job's MiB/s and the PEs' sleeps a 64 KiB at 1 MiB and
64 KiB blocks; the mailbox's messages a second; `hol`'s fetch-add alone and
behind a stream, median µs:

| Tree | Put 1 MiB | Put 64 KiB | Sleeps a 64 KiB | Mailbox | Fetch-add alone, behind |
|---|---|---|---|---|---|
| `3ee3357`, before rung 1 | 3,482 | 3,793 | 1.72, 1.91 | 93,652 | 77, 1,323 |
| `de3dc42`, rung 1 | 3,860 | 4,186 | 0.94, 0.98 | 100,200 | 74, 1,724 |
| `49c62d3`, rungs 2 and 3 | 4,070 | 4,237 | 0.52, 0.54 | 97,709 | 74, 507 |
| `2dcf8d6`, rung 4 | 4,166 | 4,444 | 0.53, 0.54 | 93,926 | 73, 525 |
| `main` | 4,002 | 4,569 | 0.52, 0.53 | 99,091 | 87, 523 |

Rung 1, a put filling the room its open buffer has, raised the puts 10 to 11%,
halved the PEs' sleeps and took 12 to 14% off the CPU a GiB, a fetch-add behind
the stream no faster in this one round; rungs 2 and 3, the
kernel's unsent queue limited by the link and buffers of 256 KiB, settings, cut
a fetch-add behind the stream from 1,724 µs to 507 and the sleeps by half again;
rung 4, the pass parsing each provided buffer where the kernel put it, 2 to 5%
more with up to 5% less CPU. The mailbox within 7% across them all.

**H11: the cluster at size (§10), and B8's SOS.** 173 jobs in 9.6 minutes.
`osoe-bench` at 8 and 16 PEs a node, 64 and 128 PEs, no helpers -
microseconds, the median of 200 trips:

| PEs | Barrier | Reduce 8 B | Reduce 64 KiB | Broadcast 64 KiB | Fcollect 4 KiB | Alltoall 8 B | Alltoall 4 KiB |
|---|---|---|---|---|---|---|---|
| 64 | 117 | 161 | 434 | 337 | 480 | 469 | 1,283 |
| 128 | 139 | 170 | 466 | 345 | 685 | 756 | 4,527 |

A remote put-and-quiet and get of 8 bytes 73.8 and 71.2 µs at 64 PEs (41.3 and
41.2 the fastest), 84.9 and 76.0 at 128, and a fetch-add 77.5 there; a stream of puts
2,709 MiB/s from 64 KiB blocks, the link's. Start-up, the slowest PE's
`shmem_init` and the fastest's, milliseconds, the median of three: 102 and 70.1
at one PE a node, 122 and 69.4 at 8, 117 and 69.5 at 32 (256 PEs). `mix`,
`osoe-pacer` for 300 s at 32 PEs, 4 a node: no failure. SOS at 2 PEs a node, 16
PEs: 159 of its 161 programs passed, and the two that did not are refused by
osoe's argument checks, as on the Pi (`docs/pi-cluster.md`):
`shmem_team_b2b_collectives` gives `shmem_long_collect` a destination of 80
bytes for a result of 1,280, and `shmemalign` asks `shmem_align` for an
alignment of 2. The VM's dry run failed both so, and `bigget` and `bigput` only
for its 32 MiB heap.

**H26: the helpers under a site's binding (§5, `binding.sh site`).** Slurm's
`task/affinity`, 8 PEs a node, 0 and 2 helpers, two rounds each way: every PE
bound to one CPU (`-K1 --cpu-bind=threads`), which Slurm gave as CPUs 0 to 3 and
8 to 11, four cores' two hyperthreads, the other four cores idle; and every PE
given two (`--cpus-per-task=2`), one core a PE. The job's MiB/s over eight
nodes, the mailbox's messages a second and two collectives' microseconds, the
median of the rounds, beside the second session's unbound (H4):

| | One CPU a PE, 0 → 2 helpers | One core a PE, 0 → 2 | Unbound, 0 → 2 |
|---|---|---|---|
| Get 1 MiB | 14,386 → 15,153 | 14,423 → 17,539 | 15,100 → 19,770 |
| Put 1 MiB | 16,701 → 17,115 | 17,247 → 20,821 | 17,407 → 21,502 |
| Put 64 KiB | 16,639 → 16,754 | 16,849 → 20,535 | - |
| Mailbox | 98,000 → 123,000 | 925,000 → 456,000 | 920,000 → 1,255,000 |
| Reduce 64 KiB | 389 → 391 | 462 → 356 | 453 → 401 |
| Alltoall 4 KiB | 1,297 → 1,489 | 1,375 → 1,196 | 1,321 → 1,014 |

Bound to one CPU, a PE's helpers share it: two buy the streams 1 to 5%, where
unbound they bought 23 to 31%, and the mailbox a quarter, and cost the
collectives up to 15% (the 4 KiB alltoall; the 8-byte ones 5 to 6%); the
mailbox itself runs at a tenth of its unbound rate, helpers or none. With a core
a PE, two helpers buy the streams 21 to 22% and the 64 KiB reduction and the
alltoalls 13 to 23%, nearly as unbound, the small collectives within 5% either
way, but halve the mailbox, as H4 found unbound past one PE a vCPU.

**What the numbers point at**, for the decisions to come, not taken here
(`docs/decisions/2026-10-28.md`, "For the author"): the window of 8, decided,
pays for every broadcast and tree; the reductions' choice of Träff's turns on a
message cost P38 has not steadied, read at 23 to 45 µs where the closed form
takes Träff's only under 5.5 to 10.5; the helpers' default (P36) - under a
site's binding they pay with a core a PE but halve the mailbox, and with one CPU
a PE buy little; the herd's
wakes (P2), a quarter of the PEs a node at every count; and the collectives'
check, which costs nothing seen up to 64 PEs a node.
