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
