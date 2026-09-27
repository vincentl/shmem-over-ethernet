# The Pi cluster's results

What each session on the Raspberry Pi cluster measured and found, newest last.
How to run a session is the runbook, `docs/pi-cluster.md`; the raw outputs are
under `docs/design/evidence/pi-cluster/`, a directory a session.

## The second session (2026-09-26)

Main `b8d7ad5` (plus that day's documentation), `release`, driven over
`ssh pi-login` from the development Mac, about 45 minutes of cluster time.
Raw outputs: `docs/design/evidence/pi-cluster/2026-09-26/`; the day's record
and the decisions it fed: `docs/decisions/2026-09-26-day.md`. Everything the
first session left ran, and the runbook's expectations held, with two
findings.

**The floor (§5)**, seven pairs on two nodes: the echo's fastest round trip
153-156 µs; the library's get 1.09× of it, put and quiet 1.08×, fetch-add
1.12× (fastest of each pair; the medians 1.09, 1.08, 1.13); the stream 111
against the echo's 112 MiB/s. Tier 3's `pi` link had predicted 1.01-1.06× on
a fast CPU; the Pi's cores cost a further 3-7%, and the 170 µs round trip
every table shows is the Pi's network stack, not the library.

**`osoe-bench` (§6)**, all four lines: 8-byte put and quiet 169-175 µs, get
168-181, fetch-add 162-176; a 1 MiB put 9.1 ms and get 11.2 ms (the get gap of
the first session, unchanged); every stream 111.5 MiB/s from 64 KiB up; a
barrier 178-184 µs at 2, 4 and 16 PEs; sixteen PEs streaming pairwise 440
MiB/s in all, four links at line rate. **Bound against unbound at 4 × 4**:
168.6 against 170.5 µs for the put, every line within 5%. The agent thread
sharing a core with its PE costs nothing measurable here.

**The test suite, split as §2 has it.** Tier 1 on the head node: 95 of 95,
1,485 s of which 1,300 were the three model checks. Tier 2 on one compute node
with `-c4`: 347 of 347 in 44 s, the floor pins and `echo-uring`'s included.
`shmem.get-no-progress.0-1` alone, five repeats: pass, about 7 s each.

**Start-up.** `hello` at 1 × 1 in 0.07-0.10 s, at 4 × 1 in 0.14-0.25 s, at
4 × 4 in 0.23-0.26 s, against 0.18-0.20 s for `srun /bin/true` at 4 × 4: the
library adds about 60 ms at 16 PEs. The first session's guess that a node took
over a second to connect is refuted; that test's 1 s limit was simply tight.

**SOS at 4 × 2**: 149 pass, 2 do not build, 12 exit nonzero, each as §8 names
it - `bigget` new among them, for its loop count at gigabit speed alone.

**The pipelined collectives**, 1 MiB on four nodes, one PE a node
(`osu-*-k*-d*.txt`; a fan-out of 3 is a fan-out of 4 on four nodes):

| | D = 1 | D = 2 | D = 4 | D = 8 |
|---|---|---|---|---|
| broadcast, k = 4 | 36.6 ms | 27.4 | 27.2 | - |
| broadcast, k = 2 | 38.8 | 22.3 | 18.4 | 18.2 |
| reduce, k = 4 | 48.5 | 28.2 | 28.3 | - |
| reduce, k = 2 | 58.4 | 33.5 | 23.0 | 19.4 |

At k = 4 the root sends three copies over one link, 27 ms at 111 MiB/s, and
no D goes below it; at k = 2 it sends two, 18.8 ms, and D = 4 reaches it. So
**over Ethernet the fan-out is a bandwidth decision for large messages**: k =
2 is 1.5× faster than the default at 1 MiB, and 1.45× for the reduction at
D = 8 (design §10, the fan-out item; L48). Small messages pay a hop per level;
the sweep started at 64 KiB, so the crossover is the next session's.

**The allgather.** fcollect of 1 MiB a PE: 28.0 ms at 4 × 1 and 120 ms at
4 × 4, against the first session's tree at 190 and 773 - 6.8× and 6.4×, the
second within 10% of the twelve MiB a node must send at line rate. Of 8 bytes
a PE: 243 µs at 4 × 1 and 269 at 4 × 4 against 190 and 197 - 1.28× and 1.37×,
under the 1.5× at which the two-slot staging would have been built; the author
accepted the cost (`docs/decisions/2026-09-26-day.md`).

**PEs per node.** A barrier 181, 202 and 185 µs at 1, 2 and 4 PEs a node; an
8-byte reduce 184, 193, 193; an 8-byte fcollect 246, 254, 270. Shared memory,
two PEs on one node: a put of 1 byte 0.16 µs, a get 0.04; 1 MiB 103 and 99 µs.

**Finding F1: fcollect at 4 × 2 with 16 KiB blocks takes 63-109 ms a call**
(five runs: 69, 63, 88, 109 ms, and 43.6 under a cyclic layout), against
0.8 ms at 8 KiB and 2.5 ms at 32 KiB; every other shape is smooth, 4 × 1 and
4 × 4 at 16 KiB included, and 4 × 4 at 8 KiB has the same 32 KiB share a
node. On any three of the four nodes the row is normal (1.17 ms without
`pi-002`, 1.11 without `pi-004`). The job makes progress throughout - the
no-progress rule never fires - the NICs count no errors and no drops, and
TCP's `TcpExtTCPTimeouts` on one node climbed 482 → 492 in eight seconds of
the run while the others held at 2-4; every node has fast-retransmitted about
0.03% of its segments since boot. A retransmission timeout is 200 ms or more,
so the average is one every few calls. Not a cable: it needs all four nodes.

**Finding F2** is the fan-out, above.

## The third session (2026-09-26, late)

Main `a4ca6c4` (contexts and the scans), `release`, about 25 minutes of cluster
time, then a second half hour for one experiment on F1. Raw outputs:
`docs/design/evidence/pi-cluster/2026-09-26-late/`, the scripts beside them.

**The build, the probe and tier 2**: clean; 375 of 375 in 32 s.

**SOS's context and scan programs, now that both exist**, at 4 × 2:
`shmem_team_context`, `many-ctx`, `shmem_ctx_get_team` and
`shmem_ctx_pipelined_reduce` pass, and so do `in_scan`, `ex_scan` and
`scan_in_place` under block, cyclic and 4 × 4 layouts (their `shmemx_` names
renamed to the 1.6 ones).

**F1 again, with its levers.** The 16 KiB fcollect at 4 × 2, 200 calls each:

| Run | a call |
|---|---|
| default | 194 ms, then 237 |
| `OSOE_COLLECTIVE_PIPELINE=1` | 61 ms |
| `OSOE_AM_CHUNK=16384` | 1.23 ms |
| `OSOE_COLLECTIVE_FANOUT=2` | 1.33 ms |
| default at 8 KiB and 32 KiB | 0.78 and 2.6 ms |

Only `pi-002` counts retransmission timeouts - 30 to 48 a run at the default,
none with the smaller chunk or the fan-out of 2 - and only it: every node
retransmits about as often in all, and `pi-002` alone recovers by timeout.
Its kernel, TCP settings, NIC offloads and link (gigabit, full duplex) are
identical to the others', and no NIC counts an error, a drop, an overrun or a
pause frame (`netcfg-*.txt`). So the segments are lost in the switch, and
`pi-002`'s place in this one exchange is what puts them in its flows.

**The experiment: is the 200 ms minimum timeout what makes a loss cost so
much?** No. A scratch build set `TCP_RTO_MIN_US` on osoe's own sockets (the
Pis' 6.18 kernel has it; `rto/`). At 5 ms the calls took 50 and 34 ms against
80 and 12 ms at the default in the same half hour, with as many timeouts as
before; 1 ms the kernel refuses. The losses themselves are the cost, and
they depend on the size of the bursts: a 16 KiB chunk or a fan-out of 2 makes
none. The session also shows how noisy F1 is - 12 to 237 ms a call on the
same shape.

**F2's crossover**, four nodes, one PE each, from 4 bytes to 64 KiB (`f2-*`):

| | 8 B | 1 KiB | 8 KiB | 16 KiB | 32 KiB | 64 KiB |
|---|---|---|---|---|---|---|
| broadcast, k = 4 | 187 µs | 209 | 260 | 447 | 1,036 | 1,838 |
| broadcast, k = 2 | 367 | 391 | 418 | 607 | 1,307 | 1,785 |
| reduce, k = 4 | 188 | 228 | 450 | 913 | 1,459 | 2,333 |
| reduce, k = 2 | 371 | 443 | 512 | 883 | 1,405 | 2,387 |

On four nodes a fan-out of 4 is one hop and 2 is two, and the hop is what
small messages pay: k = 4 is twice as fast up to 1 KiB and still ahead at 8 KiB. The two meet between
16 and 64 KiB, and at 1 MiB k = 2 wins by 1.5× (the second session). So the
fan-out should fall with the message size, somewhere near 32 KiB here -
a per-call choice of k by size, which the next measurement at more than four
nodes would have to confirm before it is built.

**Would a 16 KiB chunk cost anything?** (`chunk/`, the runbook's §11 item 2,
run the same evening without root.) Two rounds, the default 64 KiB chunk and
`OSOE_AM_CHUNK=16384` alternating, agree within a few percent:

| | 64 KiB chunk | 16 KiB chunk |
|---|---|---|
| get, 32 KiB | 415 µs | 689-711 µs |
| get, 256 KiB | 2.95 ms | 4.44-4.51 ms |
| get, 1 MiB | 11.3-11.4 ms | 17.3-17.6 ms |
| put and quiet, 1 MiB | 9.10 ms | 9.10 ms |
| put stream | 111.5 MiB/s | 111.5 MiB/s |
| pairs at 2 × 1 | 175-185 MiB/s | 155 MiB/s |
| pairs at 4 × 4 | 437 MiB/s | 436-442 MiB/s |
| fcollect 4 × 1, 32 KiB blocks | **122-138 ms** | 1.2 ms |
| fcollect 4 × 4, 16 KiB blocks | 2.7 ms | 2.15 ms |
| broadcast and reduce, 64 KiB-1 MiB | within 5% | within 5% |

So **F1 is not only 4 × 2**: at 4 × 1 the default chunk stalls on 32 KiB
blocks in both rounds, the same 32 KiB a node as 4 × 2 at 16 KiB. And a
16 KiB chunk is not free: every get past a chunk pays about 1.5×, for a
reason not yet looked into. Whether to trade the gets for
the collectives - or give collectives a chunk of their own - is the author's
decision; nothing was changed. (`osu_oshm_put_bw` needs a `heap` argument the
script did not pass; the bench's put stream stands in for it.)

**Threads, after step 8 slice 2** (main `bc5fbb6`, `threads/`; the runbook's
§11 item 4). The thread, context, get and lock tests of tier 2 on a compute
node, 47 of 47. `shmem_test_multiple` under Slurm at 4 × 2 and 4 × 1, with
the landing zone at 8 KiB and at its default: all four pass. SOS's threaded
programs at 4 × 2: `mt_a2a`, `mt_contention`, `mt_membar`, `threading`,
`thread_wait` and `query_thread` pass; `mt_lock_test` needs build flags from
SOS's makefile that this session's script did not pass (it passes in the
VM's sweep, which does).

**Step 8 slice 3's timing** (`threads-bench/`): `osoe-bench threads`, PE 0's
threads sharing the default context, 8-byte puts and nonblocking gets toward
the other node with a quiet every 32 pairs, two Pis, three runs each:

| threads | under slice 2's mutex | slice 3, lock-free issue |
|---|---|---|
| 1 | 327-379 k ops/s | 334-388 k |
| 2 | 325-330 k | 348-370 k |
| 4 | 317-320 k | 454-565 k |

Four threads went from 0.89 of one thread's rate to about 1.4, which is the
brief's claim. In the VM the four-thread run shares four cores with both PEs'
agents and shows no change; issue alone there rose 10-25% at two and four
threads.

## F1, investigated (2026-09-26, night)

Main `96b2e0f`, about an hour of cluster time, no root. Raw outputs and
every script: `docs/design/evidence/pi-cluster/2026-09-26-night-f1/`; a
probe, `f1probe.c`, times `shmem_fcollectmem` at any block size.

**What F1 is.** Rare, enormous stalls - one to four calls in 200, each 0.5 to
36 seconds, the retransmission timeout doubling six or seven times - at four
nodes only: at 4 × 1 with blocks of 24 to 36 KiB, at 4 × 2 with 16 KiB. On
any three of the four nodes, osoe never stalls.

**It follows pi-002.** Across four placements of the ranks on the nodes,
pi-002 held ranks 1, 0, 3 and 2, and took 90 to 110 new timeouts every run,
the other nodes at most 13 once. Its retransmissions are spurious - every one
is reported back as a duplicate - so its data arrives and the
acknowledgements come back late. No NIC counts an error, drop, overrun or
pause frame; no kernel counts a receive drop; the four nodes' kernels, TCP
settings, offloads and links are identical.

**Plain TCP does not trigger it.** A Python program doing the same four-node
exchange (`tcp-bruck.py`), steps one after the other or both at once, ran
1,500 rounds at 24 and 32 KiB without a round over 2 ms. So osoe's traffic
triggers something on pi-002's path that this simpler traffic does not.

**No software setting cures it; each moves it.**

| tried | result |
|---|---|
| `TCP_RTO_MIN_US` 5 ms | stalls of 34-50 ms a call on average, as many timeouts |
| pacing at 100 and 60 MB/s | stalls stay at one shape or the other; 60 MB/s halves the stream |
| send buffer 64 and 32 KiB | stalls stay |
| Reno instead of Cubic | stalls stay |
| linear retransmission timeouts | a 27 s stall still |
| the allgather's puts in 16 KiB pieces | stalls stay |
| a 32 KiB chunk; a 256 KiB buffer | stalls stay |
| a 16 KiB chunk | **the stalls move**: 4 × 1 at 24-28 KiB (up to 20 s), 4 × 2 at 12 KiB (up to 18 s) |

The session's earlier "a 16 KiB chunk ends F1" held only at the sizes it
measured.

**After moving pi-002** to another switch port with another cable (the
author, the same night; `retest-new-port/`): **no change.** pi-002, rebooted,
went from 0 timeouts to 149 over the trigger map run twice, the others
gaining at most 8; single stalls reached 52 s, at the same block sizes
(4 × 1 at 28-36 KiB, 4 × 2 at 12-16 KiB). What else was compared, all alike
on every node: the bootloader (2026-05-11, with the same 2026-05-26 update
offered to all four), the RP1 firmware, the kernel and its command line,
`config.txt`, the loaded modules, the NIC driver, energy-efficient Ethernet
(enabled, inactive), and the supply - no node has throttled or dipped
(`get_throttled` 0x0, 5.14-5.22 V). Two nodes' kernels, pi-001's and
pi-004's, logged one NIC transmit-queue hang each earlier that day
(`NETDEV WATCHDOG: transmit queue 0 timed out` after about 5.5 s); none
happened during the retest.

**With pi-002's segmentation offload off** (`sudo ethtool -K eth0 tso off gso
off` on pi-002 only, the author; `retest-tso-off/`): **no stalls.** Both
rounds of the trigger map and the placement with rank 0 on pi-002 are clean
- no call over 20 ms at any shape or size, the slowest 2.8 ms - and no node
counted a retransmission timeout. It costs nothing measurable: with pi-002
sending, an 8-byte put and quiet 171 us, a 1 MiB put 9.05 ms, the stream
112.1 MiB/s, against 170 us, 9.10 ms and 111.5 MiB/s with pi-001 sending.
**One confound:** all four nodes had rebooted a few minutes before, and two
of them had logged a NIC transmit hang earlier that day, so a fresh boot of
the peers could also explain it. pi-002 alone rebooted earlier still
stalled at once; offload back on, on these same fresh nodes, is the control
that separates the two.

## The pacer's first runs (2026-09-27)

`task-stress-phase1` at `4a96354` (`osoe-pacer`, phase 1 of the stress tester),
`release`, 4 × 4, the runbook's §6 run by `pi-runs.sh` under `nohup`, about 45
minutes of cluster time. Raw outputs, the script and the nodes' state:
`docs/design/evidence/pi-cluster/2026-09-27/pacer/`; the day's record:
`docs/decisions/2026-09-27.md`. The four nodes had rebooted at 10:09 and all
four ran with TCP and generic segmentation offload **off** - set at boot now,
it seems, where F1's record left it off on `pi-002` alone - and the
`ondemand` governor.

**Every run ended `failures=0`**: every key of every round in its owner's
range, sorted, and counted into the oracle; every word of every alltoall
variant; for ten minutes each.

**The sort**, 16 PEs:

| run | keys a second | what a round costs |
|---|---|---|
| default: rounds of 4 M keys a PE, fetch-add and put, `shmem_ptr` to node-mates | 16.5 M | exchange 1.1 s, the pacer's own sort 2.8 s |
| `--no-ptr`: puts to node-mates too | 16.2 M | exchange 1.1 to 1.2 s, sort 2.8 s |
| `--exchange alltoall`: counts by alltoall, bins pulled by nonblocking gets | 12.1 M | exchange 3.3 to 6.0 s, sort 2.8 s |
| `--sort accumulate --memory 1g`: 1 M keys a round into 1 GiB a PE | 39.7 M | every 113 rounds, 118 M keys a PE sorted in place in 8.7 s |

Two readings. The rounds' rate is the tool's more than the library's: its
16-bit LSD sort takes 2.8 s of every 4, on one Pi core a PE, while the
exchange moves the job's 512 MiB in 1.1 s - about 465 MiB/s, the four links
near line rate; `exchange_s` is the library's number. Stores through
`shmem_ptr` buy nothing measurable here over puts: three keys in sixteen go to
a node-mate. And **the pull exchange
is three to five times slower than the push**: bins of about 2 MiB, gotten
nonblocking from twelve remote PEs at once, move at 90 to 150 MiB/s where
puts of the same bins move 465 - the gap earlier sessions saw between one
1 MiB get and one 1 MiB put, much wider under load.

**The alltoall**, the world's 16 PEs, the slowest PE's median in µs (whole
five-minute run, 415 exchanges of each variant at each size):

| block | `alltoall` | `alltoallmem` | `alltoalls` | put, quiet, sync | sync, get | `putmem_signal`, `wait_until_all` | `iput` | `ibput` |
|---|---|---|---|---|---|---|---|---|
| 8 B | 372 | 362 | 362 | 362 | 461 | 285 | 358 | 359 |
| 512 B | 609 | 612 | 1,985 | 613 | 884 | 502 | 1,820 | 655 |
| 4 KiB | 2,877 | 3,054 | 9,297 | 3,008 | 3,442 | 2,834 | 8,992 | 3,020 |
| 64 KiB | 32,163 | 34,759 | 137,250 | 32,780 | 33,442 | 30,456 | 135,298 | 32,576 |

What the library's collective buys over the same exchange written by hand -
puts, a quiet, a sync - is **nothing measurable at 4 × 4**: within 4% either
way at every size. Put-with-signal beats both, since it waits for its own
data and no barrier: 23% at 8 B, 5% at 64 KiB. The strided forms cost three
to four times as much from 512 B, one put an element (the ledger's "no
strided primitive at the transport boundary"). `osoe-bench`'s alltoall line at
the same shape, run last (PE 0's median, with every other PE in the call too):
343 µs at 8 B and 2,840 µs at 4 KiB, against the pacer's slowest-PE medians of
372 and 2,877 - the same collective, measured two ways.

**Retransmission timeouts**, every node's count before and after each run:
the default run 10 in all (2, 2, 5, 1), the accumulating sort 38 (1, 22, 7,
9) - `pi-002`, F1's node, most - the pull run none, the `--no-ptr` run 4
(0, 3, 1, 0). No round
stalled for them: the checkpoints came every five seconds throughout.

**The author's first run** was the planted one, `--plant print-hash print`,
and read as a fault; it was the plant caught, 56 failures a round as
designed, and every case PE 1 cannot disturb right on 16 PEs over four
nodes. It showed one failure split over several lines, now fixed, and that a
planted run should say so, now done (`4a96354`; the record's item 8).

## The pacer's second phase (2026-09-27)

`task-stress-phase2`, 4 × 4, the runbook's §6 run by `pi-runs-phase2.sh`,
`pi-runs-phase2b.sh` and `pi-cpu-bind.sh` under `nohup`, about two hours of
cluster time. `mailbox` and `teams` ran ten minutes each in `release`,
`threads` an hour in a `debug` build with the library's two test hooks on
and a landing zone of 8 KiB. Outputs, scripts and the nodes' state:
`docs/design/evidence/pi-cluster/2026-09-27/pacer/phase2/`, a directory a
commit; the record: `docs/decisions/2026-09-27.md`, from item 11. `teams`
and `threads` ran at `80c13ac`, whose exercises are those that merged;
`mailbox` ran there and again at `01d003f`, after its round trip across nodes
was added. The nodes had been up since 10:09, TCP and generic segmentation
offload off, the `ondemand` governor, as for the first runs.

**Every run ended `failures=0`**, and the hour of `threads` printed no
`OSOE_CHECK_QUIET:` line: 1.71 × 10⁹ operations, private, serialized, default
and team contexts in turn, four workers a PE, the check that a quiet on a
shared context completes each thread's puts made inside the library on every
one of them. No node's retransmission timeouts moved in any counted run.

**`mailbox`**, 16 PEs, four messages a PE a round, a waiting form a round:

| | 10 min at `80c13ac` | 10 min at `01d003f` |
|---|---|---|
| messages a second | 41,500 | 40,500 |
| time in the mailbox between node-mates, median | 259 µs | 260 µs |
| the same, 99th percentile | 10.5 ms | 11.3 ms |
| round trip across nodes, four words each way, median | - | 175 µs |
| the same, 99th percentile | - | 211 µs |

The round trip is typed put-with-signal and a wait each way, against a bare
io_uring echo's 153 to 156 µs (the floor, above). The tail between
node-mates, forty times the median, is **the core a PE's threads share**:
two minutes each way, back to back, at `01d003f`:

| | messages a second | node-mates, median | node-mates, 99th percentile |
|---|---|---|---|
| Slurm's default binding, a core a task | 39,600 | 262 µs | 11.7 ms |
| `--cpu-bind=none` | 249,500 | 183 µs | 0.47 ms |

Six times the rate and a tail twenty-five times shorter, with nothing else
changed. Each PE's main thread shares its bound core with its agent thread,
and the node lead's agent drives the whole node's network traffic (design
§3.12); the record's item 17 has the likely mechanism, and what to do about
it is open. The `osoe-bench` pair above measured the same sharing on bare
operations; on a pipelined pattern it is the largest single factor measured
on this cluster.

**`teams`**, 16 PEs: twelve team numbers spent - four strided teams and a
4 × 4 grid's rows and columns - and 36,600 checked collectives a second over
the job, 511,118 rounds of a collective on every PE's row and column at once
and one on each strided team it belongs to.

**`threads`**, 16 PEs of four workers each, for an hour: 474,000 operations a
second from the first minute to the last, within 1%. Of every five-second
step, a PE's workers spent 6.6 to 6.7 thread-seconds on the default and team
contexts - the shared ones - and 4.3 on the private and serialized, for the
same work: half again as much, which the record leaves to the project
review.

## The pacer's third phase (2026-09-27)

`task-stress-phase3`, 4 × 4, the runbook's §6 run by `pi-runs-phase3.sh` and
`pi-coverage-phase3b.sh` under `nohup`, about an hour of cluster time.
`atomics`, `rma`, `locks`, `memory` and `collectives` ran ten minutes each in
`release` at `23f0667`, which differs from the merged tree by names and by
`rma`'s choice of spelling (the record's item 8); the counted build's
coverage ran at `075b4da`, the merged code. Outputs, scripts and the nodes'
state: `docs/design/evidence/pi-cluster/2026-09-27/pacer/phase3/`, a directory
a commit; the record: `docs/decisions/2026-09-27-afternoon.md`. The nodes had
been up since 10:09, as for phase 2's runs.

**Every run ended `failures=0`.** Ten minutes each:

| Exercise | Work | A second |
|---|---|---|
| `atomics`, 71 cases on three contexts | 103.0 M updates | 171,700; 43,100 to node-mates and 129,300 across nodes at the last checkpoint |
| `rma`, the halo and 140 transfer cases | 4.53 M transfers | 7,540, 694 MiB/s |
| `locks`, eight locks | 4.44 M acquisitions | 7,410; the longest wait at a checkpoint 80 ms |
| `memory`, to 96 MiB and back | 1.68 M operations | 2,770, 2.2 GiB/s filled and checked |
| `collectives`, 314 typed cases and the sweep | 184,000 calls | 300 |

The runs of the first four moved no node's retransmission timeouts; the
collectives' sweep added four and each of the two coverage runs two - their
1 MiB alltoall puts 48 MiB a node through each link at once, F1's incast.

**The collectives at 1 MiB a PE**, 16 PEs, the slowest PE's median: broadcast
28.0 ms and reduce 29.7, the default fan-out's three copies over the root's
link (27 ms at line rate; `osu_bcast` gave 27.2 above); fcollect 148.5 ms,
118 its fastest call, against 107 ms for twelve MiB into each node at line
rate; alltoall 575 ms against 429 ms for 48 MiB each way a node, 1.34×. The
scan is the outlier: **134 ms on 16 PEs and 136 on 8**, since the internode
scan moves its 4 KiB pieces one at a time up and down its tree (the record's
measurements). At 8 bytes every family costs 130 to 530 µs, a team sync 170.

**The counted build**: 560 rounds of every exercise called 1,404 of the
1,794 routines the library declares; the record lists the 390 it did not.
